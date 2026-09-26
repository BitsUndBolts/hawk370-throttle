/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — STPCLK# throttle
//
// Output paths
//   The STPCLK# pad is driven either by a plain GPIO level (full speed, Pause,
//   and during every switch) or by the RMT peripheral looping a pattern in
//   hardware (modulated speeds). Switching between the two is a single write
//   to the GPIO matrix, so it can be timed to the microsecond.
//
// Glitch-free switching (no phase shorter than w_min, ever)
//   1. Leaving a running pattern: inside a short critical section, wait for
//      the pad to change level, then immediately freeze that fresh level on
//      the GPIO. The phase that just began therefore lasts at least as long
//      as we hold it, and we hold it for >= w_min before changing it.
//   2. Entering a pattern: the pad holds RUN on the GPIO. The RMT first sends
//      a 10 ms RUN prefix, then the looping pattern (which also starts with
//      RUN). The pad is handed to the RMT while the prefix is still running,
//      which is checked against a timestamp taken before transmitting — a
//      guaranteed lower bound, so preemption can only cause a retry.
//   Full speed and Pause are plain GPIO levels, so they cost no RMT time and
//   survive anything the RMT driver does.
// =============================================================================

#include "hawk_throttle.h"
#include "hawk_config.h"

#include <Preferences.h>
#include <atomic>
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"

using namespace hawk;

// Same Intel-referenced timing for every family until bench data says otherwise.
const CpuFamilyInfo CPU_FAMILIES[FAMILY_COUNT] = {
  /* MENDOCINO    */ { "Mendocino",    DEFAULT_TIMING, false },
  /* COPPERMINE   */ { "Coppermine",   DEFAULT_TIMING, false },
  /* TUALATIN     */ { "Tualatin",     DEFAULT_TIMING, false },
  /* VIA_SAMUEL   */ { "VIA Samuel",   DEFAULT_TIMING, false },
  /* VIA_EZRA     */ { "VIA Ezra",     DEFAULT_TIMING, false },
  /* VIA_NEHEMIAH */ { "VIA Nehemiah", DEFAULT_TIMING, false },
};

static const char*    NVS_NAMESPACE        = "throttle";
static const uint32_t DEFAULT_BASE_MHZ     = 300;
static const uint32_t RMT_RESOLUTION_HZ    = 1000000;   // 1 tick = 1 us
static const uint16_t PREFIX_HALF_US       = 5000;      // prefix = 2 x 5 ms of RUN
static const int64_t  PREFIX_US            = 2 * PREFIX_HALF_US;
static const int64_t  HANDOVER_MARGIN_US   = 1000;      // hand over at least 1 ms before the prefix ends
static const int64_t  CUT_WINDOW_US        = 1000;      // longest critical section while waiting for an edge
static const int      CUT_MAX_ATTEMPTS     = 120;       // ~240 ms worst case at the slowest speeds
static const int      HANDOVER_MAX_ATTEMPTS = 5;

// ── Shared state (web task <-> loop task) ─────────────────────────────────────
static portMUX_TYPE     stateMux     = portMUX_INITIALIZER_UNLOCKED;
static ThrottleState    target       = { FAMILY_MENDOCINO, DEFAULT_BASE_MHZ, 100.0f, 100.0f, false };
static bool             applyPending = false;
static bool             savePending  = false;
static std::atomic<uint32_t> stateVersion{1};

// ── Hardware state (loop task only) ───────────────────────────────────────────
enum OutputMode : uint8_t { OUT_GPIO, OUT_RMT };

static rmt_channel_handle_t rmtChannel   = nullptr;
static rmt_encoder_handle_t rmtEncoder   = nullptr;
static uint32_t             rmtMatrixCfg = 0;        // GPIO matrix word routing the RMT to the pad
static bool                 hwOk         = false;
static OutputMode           outMode      = OUT_GPIO;
static uint8_t              heldLevel    = LVL_RUN;  // logical level while on the GPIO
static int64_t              heldSinceUs  = -1000000; // when that level began
static uint16_t             activeWMinUs = DEFAULT_TIMING.wMinUs;

static rmt_symbol_word_t    prefixSymbols[1];
static rmt_symbol_word_t    loopSymbols[PATTERN_CYCLES];

static portMUX_TYPE         cutMux = portMUX_INITIALIZER_UNLOCKED;

// ── Pad helpers ──────────────────────────────────────────────────────────────
static inline uint32_t padFor(uint8_t logical) {
  return (logical ^ (STPCLK_INVERTED ? 1 : 0)) & 1;
}

static inline uint8_t logicalFromPad(uint32_t pad) {
  return (uint8_t) ((pad ^ (STPCLK_INVERTED ? 1 : 0)) & 1);
}

// Route the pad to the plain GPIO output at a given logical level.
static inline void IRAM_ATTR padToGpio(uint8_t logical) {
  gpio_ll_set_level(&GPIO, STPCLK_GPIO, padFor(logical));
  esp_rom_gpio_connect_out_signal(STPCLK_GPIO, SIG_GPIO_OUT_IDX, false, false);
}

// Change the held GPIO level, keeping the current level for at least w_min.
static void setHeldLevel(uint8_t logical) {
  if (outMode == OUT_GPIO && heldLevel == logical) return;
  const int64_t earliest = heldSinceUs + activeWMinUs;
  while (esp_timer_get_time() < earliest) { /* <= 32 us */ }
  padToGpio(logical);
  outMode     = OUT_GPIO;
  heldLevel   = logical;
  heldSinceUs = esp_timer_get_time();
}

// Take the pad away from a running RMT pattern at a fresh edge (see header).
static void cutFromRmt() {
  for (int attempt = 0; attempt < CUT_MAX_ATTEMPTS; attempt++) {
    bool    caught = false;
    uint8_t level  = LVL_RUN;

    portENTER_CRITICAL(&cutMux);
    const int64_t start = esp_timer_get_time();
    const uint32_t before = gpio_ll_get_level(&GPIO, STPCLK_GPIO);
    while (esp_timer_get_time() - start < CUT_WINDOW_US) {
      const uint32_t now = gpio_ll_get_level(&GPIO, STPCLK_GPIO);
      if (now != before) {                 // a phase has just begun: freeze it
        level = logicalFromPad(now);
        padToGpio(level);
        caught = true;
        break;
      }
    }
    portEXIT_CRITICAL(&cutMux);

    if (caught) {
      outMode     = OUT_GPIO;
      heldLevel   = level;
      heldSinceUs = esp_timer_get_time() - 2;   // began just before we froze it
      return;
    }
    vTaskDelay(1);   // inside a long phase; let Wi-Fi breathe and try again
  }

  // No edge within ~240 ms: the pattern is not running (should not happen).
  // Freeze whatever the pad shows and treat it as a phase starting now.
  portENTER_CRITICAL(&cutMux);
  const uint8_t level = logicalFromPad(gpio_ll_get_level(&GPIO, STPCLK_GPIO));
  padToGpio(level);
  portEXIT_CRITICAL(&cutMux);
  outMode     = OUT_GPIO;
  heldLevel   = level;
  heldSinceUs = esp_timer_get_time();
  Serial.println("[THROTTLE] WARNING: no STPCLK# edge seen while leaving a pattern");
}

// Stops the RMT and drains any queued transaction (the pad is on the GPIO).
static void rmtFlush() {
  // rmt_enable() starts a transaction still waiting in the queue, so cycle
  // twice: the second disable recycles it. INVALID_STATE just means "already".
  for (int i = 0; i < 2; i++) {
    esp_err_t e = rmt_disable(rmtChannel);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) Serial.printf("[THROTTLE] rmt_disable: %s\n", esp_err_to_name(e));
    e = rmt_enable(rmtChannel);
    if (e != ESP_OK) Serial.printf("[THROTTLE] rmt_enable: %s\n", esp_err_to_name(e));
  }
}

static inline void setSymbol(rmt_symbol_word_t& s, uint8_t l0, uint16_t d0, uint8_t l1, uint16_t d1) {
  s.level0 = l0; s.duration0 = d0;
  s.level1 = l1; s.duration1 = d1;
}

// Starts a modulated pattern. Expects the pad on the GPIO holding RUN.
static bool startPattern(const Pattern& p) {
  for (uint8_t k = 0; k < p.cycles; k++) {
    setSymbol(loopSymbols[k], LVL_RUN, p.cycle[k].runUs, LVL_STOP, p.cycle[k].stopUs);
  }
  setSymbol(prefixSymbols[0], LVL_RUN, PREFIX_HALF_US, LVL_RUN, PREFIX_HALF_US);

  rmt_transmit_config_t once = {};
  once.loop_count = 0;
  once.flags.eot_level = LVL_RUN;
  once.flags.queue_nonblocking = 1;

  rmt_transmit_config_t forever = {};
  forever.loop_count = -1;   // repeat in hardware, no CPU involvement
  forever.flags.eot_level = LVL_RUN;
  forever.flags.queue_nonblocking = 1;

  for (int attempt = 0; attempt < HANDOVER_MAX_ATTEMPTS; attempt++) {
    rmtFlush();

    const int64_t before = esp_timer_get_time();   // the prefix cannot start earlier
    esp_err_t e1 = rmt_transmit(rmtChannel, rmtEncoder, prefixSymbols, sizeof(prefixSymbols), &once);
    esp_err_t e2 = rmt_transmit(rmtChannel, rmtEncoder, loopSymbols, p.cycles * sizeof(rmt_symbol_word_t), &forever);
    if (e1 != ESP_OK || e2 != ESP_OK) {
      Serial.printf("[THROTTLE] rmt_transmit failed: %s / %s\n", esp_err_to_name(e1), esp_err_to_name(e2));
      continue;
    }

    bool handedOver = false;
    portENTER_CRITICAL(&cutMux);
    if (esp_timer_get_time() - before < PREFIX_US - HANDOVER_MARGIN_US) {
      GPIO.func_out_sel_cfg[STPCLK_GPIO].val = rmtMatrixCfg;   // RMT is inside its RUN prefix
      handedOver = true;
    }
    portEXIT_CRITICAL(&cutMux);

    if (handedOver) {
      outMode = OUT_RMT;
      return true;
    }
    Serial.println("[THROTTLE] hand-over window missed (task preempted), retrying");
  }
  return false;
}

// Moves the output to the given pattern. Loop task only.
static float applyPattern(const Pattern& p, uint16_t wMinUs) {
  if (!hwOk) return 100.0f;

  if (outMode == OUT_RMT) cutFromRmt();

  // The phase frozen by the cut belongs to the old family's timing; honour the
  // stricter of the two minimums for this one transition.
  if (wMinUs > activeWMinUs) activeWMinUs = wMinUs;

  float delivered = 100.0f;
  switch (p.kind) {
    case PATTERN_FULL:
      setHeldLevel(LVL_RUN);
      rmtFlush();
      break;

    case PATTERN_PAUSE:
      setHeldLevel(LVL_STOP);
      rmtFlush();
      delivered = 0.0f;
      break;

    case PATTERN_MODULATED:
    default:
      setHeldLevel(LVL_RUN);
      if (startPattern(p)) {
        delivered = p.deliveredPercent;
      } else {
        Serial.println("[THROTTLE] ERROR: could not start pattern, staying at full speed");
      }
      break;
  }
  activeWMinUs = wMinUs;
  return delivered;
}

// ── Public API ───────────────────────────────────────────────────────────────

void throttleEarlyPinInit() {
  gpio_set_level(STPCLK_GPIO, padFor(LVL_RUN));
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << STPCLK_GPIO;
  io.mode         = GPIO_MODE_INPUT_OUTPUT;   // input path lets us watch the pad
  io.pull_up_en   = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&io);
  gpio_set_level(STPCLK_GPIO, padFor(LVL_RUN));
  outMode     = OUT_GPIO;
  heldLevel   = LVL_RUN;
  heldSinceUs = esp_timer_get_time();
}

static bool rmtInit() {
  rmt_tx_channel_config_t cfg = {};
  cfg.gpio_num          = STPCLK_GPIO;
  cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz     = RMT_RESOLUTION_HZ;
  cfg.mem_block_symbols = 48;               // one block: 16 pattern symbols + EOF
  cfg.trans_queue_depth = 2;                // prefix + looping pattern
  cfg.flags.invert_out  = STPCLK_INVERTED;  // flips pattern AND idle level together
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  cfg.flags.init_level  = LVL_RUN;          // older IDFs idle at 0, which is also RUN
#endif

  esp_err_t e = rmt_new_tx_channel(&cfg, &rmtChannel);
  if (e != ESP_OK) {
    Serial.printf("[THROTTLE] rmt_new_tx_channel: %s\n", esp_err_to_name(e));
    return false;
  }

  // Remember how the driver routed the RMT to the pad, then take the pad back
  // to the GPIO (still RUN) until a pattern is requested.
  rmtMatrixCfg = GPIO.func_out_sel_cfg[STPCLK_GPIO].val;
  padToGpio(LVL_RUN);
  gpio_ll_input_enable(&GPIO, STPCLK_GPIO);

  const uint32_t sig = rmtMatrixCfg & 0xFF;
  if (sig != RMT_SIG_OUT0_IDX && sig != RMT_SIG_OUT1_IDX) {
    Serial.printf("[THROTTLE] unexpected RMT matrix signal %u\n", (unsigned) sig);
    return false;
  }

  rmt_copy_encoder_config_t encCfg = {};
  e = rmt_new_copy_encoder(&encCfg, &rmtEncoder);
  if (e != ESP_OK) {
    Serial.printf("[THROTTLE] rmt_new_copy_encoder: %s\n", esp_err_to_name(e));
    return false;
  }
  e = rmt_enable(rmtChannel);
  if (e != ESP_OK) {
    Serial.printf("[THROTTLE] rmt_enable: %s\n", esp_err_to_name(e));
    return false;
  }
  return true;
}

bool throttleBegin() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  uint8_t  family  = prefs.getUChar("family", FAMILY_MENDOCINO);
  uint32_t baseMhz = prefs.getUInt("baseMhz", DEFAULT_BASE_MHZ);
  if (prefs.isKey("speed")) prefs.remove("speed");   // v0.1 stored the speed; power-on is now always full speed
  prefs.end();

  if (family >= FAMILY_COUNT) family = FAMILY_MENDOCINO;
  if (baseMhz == 0) baseMhz = DEFAULT_BASE_MHZ;

  portENTER_CRITICAL(&stateMux);
  target = { family, baseMhz, 100.0f, 100.0f, false };
  portEXIT_CRITICAL(&stateMux);

  hwOk = rmtInit();
  Serial.printf("[THROTTLE] %s — family %s, base %u MHz, full speed\n",
                hwOk ? "Ready" : "RMT INIT FAILED (STPCLK# stays released)",
                CPU_FAMILIES[family].name, (unsigned) baseMhz);
  return hwOk;
}

void throttleTick() {
  bool doApply, doSave;
  ThrottleState s;
  portENTER_CRITICAL(&stateMux);
  doApply = applyPending;
  doSave  = savePending;
  applyPending = savePending = false;
  s = target;
  portEXIT_CRITICAL(&stateMux);

  if (doSave) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("family", s.family);
    prefs.putUInt("baseMhz", s.baseMhz);
    prefs.end();
  }

  if (doApply) {
    const PatternTiming& timing = CPU_FAMILIES[s.family].timing;
    Pattern p;
    buildPattern(s.requestedPercent, timing, p);
    const float delivered = applyPattern(p, timing.wMinUs);
    if (delivered != s.deliveredPercent) {   // only differs if the hardware failed
      portENTER_CRITICAL(&stateMux);
      target.deliveredPercent = delivered;
      target.paused = (delivered == 0.0f);
      stateVersion++;
      portEXIT_CRITICAL(&stateMux);
    }
  }
}

static float previewDelivered(float speedPercent, uint8_t family) {
  Pattern p;
  buildPattern(speedPercent, CPU_FAMILIES[family].timing, p);
  return p.deliveredPercent;
}

ThrottleState throttleRequestSpeed(float speedPercent) {
  if (!(speedPercent >= 0.0f)) speedPercent = 100.0f;   // NaN guard
  if (speedPercent > 100.0f) speedPercent = 100.0f;

  portENTER_CRITICAL(&stateMux);
  const uint8_t family = target.family;
  portEXIT_CRITICAL(&stateMux);
  const float delivered = previewDelivered(speedPercent, family);   // pure, outside the lock

  portENTER_CRITICAL(&stateMux);
  target.requestedPercent = speedPercent;
  target.deliveredPercent = delivered;
  target.paused = (delivered == 0.0f);
  applyPending = true;
  stateVersion++;
  ThrottleState s = target;
  portEXIT_CRITICAL(&stateMux);
  return s;
}

ThrottleState throttleRequestConfig(uint8_t family, uint32_t baseMhz) {
  if (family >= FAMILY_COUNT) family = FAMILY_MENDOCINO;
  if (baseMhz == 0) baseMhz = DEFAULT_BASE_MHZ;

  portENTER_CRITICAL(&stateMux);
  const float requested = target.requestedPercent;
  portEXIT_CRITICAL(&stateMux);
  const float delivered = previewDelivered(requested, family);

  portENTER_CRITICAL(&stateMux);
  const bool familyChanged = (target.family != family);
  target.family  = family;
  target.baseMhz = baseMhz;
  target.deliveredPercent = delivered;
  savePending  = true;
  applyPending = applyPending || familyChanged;   // timing may differ per family
  stateVersion++;
  ThrottleState s = target;
  portEXIT_CRITICAL(&stateMux);
  return s;
}

ThrottleState throttleGetState() {
  portENTER_CRITICAL(&stateMux);
  ThrottleState s = target;
  portEXIT_CRITICAL(&stateMux);
  return s;
}

uint32_t throttleStateVersion() {
  return stateVersion.load();
}

bool throttleHardwareOk() {
  return hwOk;
}
