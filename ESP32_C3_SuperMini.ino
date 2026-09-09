/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 Wireless ThrottleBlaster — ESP32-C3 SuperMini Firmware
// Firmware Version: 0.1.0  (pre-bring-up — see TODO(bench) / TODO(bring-up))
//
// Wireless Socket 370 CPU throttle controller (Mendocino / Coppermine /
// Tualatin / VIA C3), built for the HAWK 370 board. Web server, Wi-Fi
// provisioning, mDNS and OTA subsystems are adapted from the author's
// AirMeter project; the HC-12/meter-registry logic that project used has
// been removed entirely since this board has no RF meter link.
//
// Architecture:
//   Browser  <-- SSE (voltage) / REST (throttle + system) -->  ESP32-C3
//   ESP32-C3 --> RMT peripheral --> 2N7002 driver --> STPCLK# (Socket 370)
//   ESP32-C3 <-- ADC1 (GPIO0/GPIO1, 10k/10k dividers) <-- VTT / VCORE rails
//
// Pin Map:
//   GPIO 7  — STPCLK# drive (via onboard 2N7002), RMT TX
//   GPIO 0  — VCORE sense, ADC1_CHANNEL_1  (10k/10k divider + 100nF)
//   GPIO 1  — VTT  sense, ADC1_CHANNEL_0   (10k/10k divider + 100nF)
//
// IMPORTANT — items to verify on hardware bring-up (see plan Section 8):
//   - STPCLK_ASSERT_LEVEL polarity (assumed HIGH = NMOS on = STPCLK# pulled
//     low = CPU stopped; matches the original ThrottleBlaster's 2N7000
//     wiring, but confirm against the HAWK 370 schematic before power-up).
//   - GPIO0/GPIO1 <-> VTT/VCORE assignment (assumed GPIO0=VTT, GPIO1=VCORE;
//     plan doesn't pin this down explicitly).
//   - THROTTLE_PRESETS carrier/w_min values are PLACEHOLDERS. Do not run
//     real hardware at these settings until Phase A-D bench calibration
//     (plan Section 3) has produced real numbers per CPU family.
//   - RMT and ADC code targets the new IDF5 drivers (`driver/rmt_tx.h`,
//     `esp_adc/adc_oneshot.h`), matching arduino-esp32 core 3.x. If you
//     ever move to a 2.x (IDF4) core, these would need porting back to
//     the legacy `driver/rmt.h` / `esp_adc_cal.h` APIs.
// =============================================================================

#include <WiFi.h>
#include <Update.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <math.h>
#include "esp_bt.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/rmt_tx.h"

// =============================================================================
// FIRMWARE METADATA
// =============================================================================

const char* FIRMWARE_VERSION = "0.1.1";

// =============================================================================
// HARDWARE CONFIGURATION — STPCLK# / RMT
// =============================================================================

static const gpio_num_t STPCLK_GPIO = GPIO_NUM_7;

// GPIO level that asserts STPCLK# (i.e. stops the CPU), given the onboard
// 2N7002: gate high -> FET conducts -> drain (STPCLK#) pulled low.
// TODO(bring-up): confirm against schematic; flip if inverted.
static const uint8_t STPCLK_ASSERT_LEVEL = HIGH;

// RMT channel resolution: 1 MHz (1 tick = 1 us), which keeps the
// tick-to-microsecond math trivial everywhere below.
static const uint32_t RMT_RESOLUTION_HZ = 1000000;

static rmt_channel_handle_t rmtChannel = NULL;
static rmt_encoder_handle_t rmtEncoder = NULL;

// =============================================================================
// HARDWARE CONFIGURATION — VOLTAGE SENSE
// =============================================================================

static const adc_channel_t VCORE_ADC_CHANNEL = ADC_CHANNEL_0; // GPIO0
static const adc_channel_t VTT_ADC_CHANNEL   = ADC_CHANNEL_1; // GPIO1

static const adc_bitwidth_t ADC_WIDTH = ADC_BITWIDTH_12;
static const adc_atten_t    ADC_ATTEN = ADC_ATTEN_DB_12; // full ~0-3.3V range (IDF5 renamed DB_11 -> DB_12)
static const float          DIVIDER_RATIO = 2.0f;        // 10k/10k -> Vrail = Vadc * 2

static const uint32_t VOLTAGE_SAMPLE_INTERVAL_MS  = 100; // raw ADC read cadence
static const uint32_t VOLTAGE_PUSH_INTERVAL_MS    = 500; // SSE push / display cadence

// =============================================================================
// CPU FAMILY / THROTTLE PRESETS
// =============================================================================

enum CpuFamily : uint8_t {
  FAMILY_MENDOCINO    = 0,
  FAMILY_COPPERMINE   = 1,
  FAMILY_TUALATIN     = 2,
  FAMILY_VIA_SAMUEL   = 3,
  FAMILY_VIA_EZRA     = 4,
  FAMILY_VIA_NEHEMIAH = 5,
  FAMILY_COUNT        = 6
};

struct ThrottlePreset {
  const char* name;
  uint32_t    carrierHz;   // PDM slot/tick rate. TODO(bench): Phase B, plan Section 3.
  uint16_t    wMinTicks;   // minimum run length, in ticks. TODO(bench): Phase A/B.
  bool        calibrated;  // false until real bench numbers are entered here.
};

// PLACEHOLDER VALUES — see file header. VIA entries are intentionally more
// conservative (lower carrier, higher w_min) given the plan's note that VIA
// C3 parts have a track record of behaving differently from the P6-derived
// Intel parts; still just a guess until Phase A bench characterization.
static const ThrottlePreset THROTTLE_PRESETS[FAMILY_COUNT] = {
  /* MENDOCINO    */ { "Mendocino",    20000, 4, false },
  /* COPPERMINE   */ { "Coppermine",   20000, 4, false },
  /* TUALATIN     */ { "Tualatin",     20000, 4, false },
  /* VIA_SAMUEL   */ { "VIA Samuel",   15000, 6, false },
  /* VIA_EZRA     */ { "VIA Ezra",     15000, 6, false },
  /* VIA_NEHEMIAH */ { "VIA Nehemiah", 15000, 6, false },
};

// =============================================================================
// PERSISTED SETTINGS
//
// Stored as discrete typed keys in the "throttle" and "wifi-config"
// Preferences namespaces (NVS) rather than a JSON blob — there are only a
// handful of scalar values here, so a blob would just add parse overhead
// with no benefit.
// =============================================================================

struct ThrottleSettings {
  uint8_t  family       = FAMILY_MENDOCINO;
  uint32_t baseMhz      = 300;
  float    speedPercent = 100.0f; // 100 = full speed / no throttle — SAFE DEFAULT
  bool     wifiPsNone   = false;
};

static ThrottleSettings settings;
static Preferences      preferences;

void loadSettings() {
  preferences.begin("throttle", true);
  settings.family       = preferences.getUChar("family", FAMILY_MENDOCINO);
  settings.baseMhz      = preferences.getUInt("baseMhz", 300);
  settings.speedPercent = preferences.getFloat("speed", 100.0f);
  settings.wifiPsNone   = preferences.getBool("wifiPsNone", false);
  preferences.end();

  if (settings.family >= FAMILY_COUNT) settings.family = FAMILY_MENDOCINO;
  settings.speedPercent = constrain(settings.speedPercent, 0.0f, 100.0f);
}

void persistFamilyAndBaseMhz() {
  preferences.begin("throttle", false);
  preferences.putUChar("family", settings.family);
  preferences.putUInt("baseMhz", settings.baseMhz);
  preferences.end();
}

void persistSpeedPercent() {
  preferences.begin("throttle", false);
  preferences.putFloat("speed", settings.speedPercent);
  preferences.end();
}

void persistWifiPs() {
  preferences.begin("throttle", false);
  preferences.putBool("wifiPsNone", settings.wifiPsNone);
  preferences.end();
}

// Speed-percent changes come from a UI slider, which can fire many events
// per second while dragging. We apply every change to hardware immediately
// (cheap, RAM-only) but debounce the NVS flash write so a slider drag
// doesn't hammer flash wear — only the settled final value gets persisted.
static bool          speedDirty       = false;
static unsigned long speedDirtySince  = 0;
static const uint32_t SPEED_SAVE_DEBOUNCE_MS = 1500;

void tickSettingsPersistence() {
  if (speedDirty && millis() - speedDirtySince > SPEED_SAVE_DEBOUNCE_MS) {
    persistSpeedPercent();
    speedDirty = false;
  }
}

// =============================================================================
// THROTTLE MODULATOR — RMT-driven pulse-density modulation
//
// Implements the plan's Section 2 "delta-sigma / LED dimmer" approach with a
// structural (not post-hoc) minimum run-length clamp: the PDM buffer is
// divided into fixed-width slots of exactly wMinTicks carrier-ticks each,
// and a 1-bit delta-sigma accumulator decides each slot's level (stopped or
// running) to hit the target duty. Because the slot itself IS the minimum
// run length, no assert/deassert phase can ever be shorter than w_min —
// there is no rounding/merging step that could violate it.
//
// Once armed, RMT loop mode repeats this buffer in hardware with zero
// ongoing CPU/ISR involvement, so Wi-Fi/BLE activity cannot introduce
// jitter into the STPCLK# waveform.
// =============================================================================

static const uint8_t PDM_SLOTS      = 48; // fits one ESP32-C3 RMT channel's item memory
static const uint8_t MAX_SUBPULSES  = PDM_SLOTS + 4; // headroom for overflow-splitting

struct SubPulse { uint8_t level; uint16_t ticks; };

static rmt_symbol_word_t rmtBuffer[(MAX_SUBPULSES + 1) / 2];

void rmtInit() {
  rmt_tx_channel_config_t txChanConfig = {};
  txChanConfig.gpio_num          = STPCLK_GPIO;
  txChanConfig.clk_src           = RMT_CLK_SRC_DEFAULT;
  txChanConfig.resolution_hz     = RMT_RESOLUTION_HZ;
  txChanConfig.mem_block_symbols = 48; // comfortably fits PDM_SLOTS worth of runs
  txChanConfig.trans_queue_depth = 1;  // only ever one (looping) transmission in flight

  rmt_new_tx_channel(&txChanConfig, &rmtChannel);

  rmt_copy_encoder_config_t copyEncoderConfig = {};
  rmt_new_copy_encoder(&copyEncoderConfig, &rmtEncoder);

  rmt_enable(rmtChannel);
}

// Rebuilds and re-arms the RMT output for the given target speed (0-100,
// where 100 = full base clock / no throttle). Converts to the plan's
// "duty = fraction of time stopped" internally: stopFraction = 1 - speed/100.
void applyThrottleSpeed(float speedPercent) {
  speedPercent = constrain(speedPercent, 0.0f, 100.0f);
  const ThrottlePreset &preset = THROTTLE_PRESETS[settings.family];

  const float slotUs = 1000000.0f * preset.wMinTicks / (float)preset.carrierHz;
  const uint16_t slotRmtTicks = (uint16_t) constrain((long) lroundf(slotUs), 1L, 0x7FFFL);

  const float stopFraction = 1.0f - (speedPercent / 100.0f);

  // ── 1-bit delta-sigma across PDM_SLOTS fixed-width slots ──
  bool slotStopped[PDM_SLOTS];
  float acc = 0.0f;
  for (uint8_t i = 0; i < PDM_SLOTS; i++) {
    acc += stopFraction;
    slotStopped[i] = (acc >= 1.0f);
    if (slotStopped[i]) acc -= 1.0f;
  }

  // ── Run-length encode into sub-pulses (each is a whole number of slots) ──
  SubPulse subPulses[MAX_SUBPULSES];
  uint8_t subPulseCount = 0;
  uint8_t i = 0;
  while (i < PDM_SLOTS && subPulseCount < MAX_SUBPULSES) {
    bool stopped = slotStopped[i];
    uint16_t runSlots = 1;
    while (i + runSlots < PDM_SLOTS && slotStopped[i + runSlots] == stopped) runSlots++;

    uint32_t runTicks = (uint32_t) slotRmtTicks * runSlots;
    uint8_t  level     = stopped ? STPCLK_ASSERT_LEVEL : !STPCLK_ASSERT_LEVEL;

    // Split runs that would overflow the RMT item's 15-bit duration field.
    while (runTicks > 0 && subPulseCount < MAX_SUBPULSES) {
      uint16_t chunk = (uint16_t) min<uint32_t>(runTicks, 0x7FFF);
      subPulses[subPulseCount++] = { level, chunk };
      runTicks -= chunk;
    }
    i += runSlots;
  }

  // Each RMT item carries two sub-pulses back-to-back. A duration of 0 tells
  // the RMT hardware to stop transmitting immediately — anywhere in the
  // buffer, not just at the end — which would break loop mode. If we ended
  // up with an odd sub-pulse count, split the last one instead of padding
  // with a zero-duration entry.
  if (subPulseCount & 1) {
    SubPulse &last = subPulses[subPulseCount - 1];
    uint16_t half = last.ticks / 2;
    if (half >= 1 && subPulseCount < MAX_SUBPULSES) {
      SubPulse extra = { last.level, (uint16_t)(last.ticks - half) };
      last.ticks = half;
      subPulses[subPulseCount++] = extra;
    }
  }

  uint16_t itemCount = subPulseCount / 2;
  for (uint16_t k = 0; k < itemCount; k++) {
    rmtBuffer[k].level0    = subPulses[2 * k].level;
    rmtBuffer[k].duration0 = subPulses[2 * k].ticks;
    rmtBuffer[k].level1    = subPulses[2 * k + 1].level;
    rmtBuffer[k].duration1 = subPulses[2 * k + 1].ticks;
  }

  // An infinite-loop transmission is already active after the first call
  // (loop_count = -1 below); the IDF5 RMT TX driver requires the channel to
  // be disabled before its content can be replaced mid-loop.
  rmt_disable(rmtChannel);
  rmt_enable(rmtChannel);

  rmt_transmit_config_t txConfig = {};
  txConfig.loop_count = -1; // repeat in hardware — zero ongoing CPU/ISR involvement

  rmt_transmit(rmtChannel, rmtEncoder, rmtBuffer,
               itemCount * sizeof(rmt_symbol_word_t), &txConfig);
}

// Effective-MHz readout. Linear fallback (base_MHz * speed%) per the plan's
// Section 2 model — TODO(bench): replace with the Phase D measured
// lookup-table interpolation once calibration data exists per family.
float computeTargetMhz(float speedPercent, uint32_t baseMhz) {
  return baseMhz * (speedPercent / 100.0f);
}

// =============================================================================
// VOLTAGE MONITORING — ADC1, eFuse-calibrated
// =============================================================================

static adc_oneshot_unit_handle_t adcUnit     = NULL;
static adc_cali_handle_t         adcCaliVtt   = NULL;
static adc_cali_handle_t         adcCaliVcore = NULL;

static float    latestVtt   = 0.0f;
static float    latestVcore = 0.0f;
static uint32_t vttAccumMv   = 0;
static uint32_t vcoreAccumMv = 0;
static uint16_t voltageSampleCount = 0;

void adcInit() {
  adc_oneshot_unit_init_cfg_t unitConfig = {};
  unitConfig.unit_id = ADC_UNIT_1;
  adc_oneshot_new_unit(&unitConfig, &adcUnit);

  adc_oneshot_chan_cfg_t chanConfig = {};
  chanConfig.atten    = ADC_ATTEN;
  chanConfig.bitwidth = ADC_WIDTH;
  adc_oneshot_config_channel(adcUnit, VTT_ADC_CHANNEL, &chanConfig);
  adc_oneshot_config_channel(adcUnit, VCORE_ADC_CHANNEL, &chanConfig);

  // ESP32-C3 uses the curve-fitting calibration scheme (line-fitting only
  // applies to the original ESP32).
  adc_cali_curve_fitting_config_t caliConfig = {};
  caliConfig.unit_id  = ADC_UNIT_1;
  caliConfig.atten    = ADC_ATTEN;
  caliConfig.bitwidth = ADC_WIDTH;

  caliConfig.chan = VTT_ADC_CHANNEL;
  adc_cali_create_scheme_curve_fitting(&caliConfig, &adcCaliVtt);

  caliConfig.chan = VCORE_ADC_CHANNEL;
  adc_cali_create_scheme_curve_fitting(&caliConfig, &adcCaliVcore);
}

// Called every loop() tick. Samples faster than the display update rate and
// accumulates, so short dips aren't missed even though the graph itself
// only refreshes every VOLTAGE_PUSH_INTERVAL_MS (plan Section 5).
void tickVoltageSampling() {
  static unsigned long lastSample = 0;
  const unsigned long now = millis();
  if (now - lastSample < VOLTAGE_SAMPLE_INTERVAL_MS) return;
  lastSample = now;

  int vttRaw = 0, vcoreRaw = 0;
  adc_oneshot_read(adcUnit, VTT_ADC_CHANNEL, &vttRaw);
  adc_oneshot_read(adcUnit, VCORE_ADC_CHANNEL, &vcoreRaw);

  int vttMv = 0, vcoreMv = 0;
  adc_cali_raw_to_voltage(adcCaliVtt, vttRaw, &vttMv);
  adc_cali_raw_to_voltage(adcCaliVcore, vcoreRaw, &vcoreMv);

  vttAccumMv   += (uint32_t) vttMv;
  vcoreAccumMv += (uint32_t) vcoreMv;
  voltageSampleCount++;
}

// Decimates the accumulated samples into latestVtt/latestVcore (applying the
// divider ratio to recover the actual rail voltage) and resets the
// accumulator. Returns true if new values are available to push.
bool decimateVoltage() {
  if (voltageSampleCount == 0) return false;

  latestVtt   = (vttAccumMv   / (float) voltageSampleCount) / 1000.0f * DIVIDER_RATIO;
  latestVcore = (vcoreAccumMv / (float) voltageSampleCount) / 1000.0f * DIVIDER_RATIO;

  vttAccumMv = 0;
  vcoreAccumMv = 0;
  voltageSampleCount = 0;
  return true;
}

// =============================================================================
// NETWORK & SERVER OBJECTS
// =============================================================================

// Persisted in the "wifi-config" namespace's "mode" key alongside ssid/pass.
// UNCONFIGURED means "still needs the setup decision screen" — this is what
// wipe(false) implicitly resets things back to, since it clears the whole
// "wifi-config" namespace (mode included).
enum OperationMode : uint8_t {
  MODE_UNCONFIGURED = 0,
  MODE_AP           = 1, // user chose standalone Access Point (bench mode)
  MODE_STA          = 2  // user provisioned a home/lab network
};

static const byte DNS_PORT = 53;

AsyncWebServer    server(80);
AsyncEventSource  events("/events");
DNSServer         dnsServer;

// =============================================================================
// RUNTIME STATE — NETWORK
// =============================================================================

String            wifi_ssid              = "";
String            wifi_password          = "";
bool              is_ap_mode             = false;
// True whenever the device still needs the setup decision screen (i.e. no
// Wi-Fi mode has been chosen yet). is_ap_mode is true for BOTH this state
// and the deliberately-chosen standalone-AP operating mode; provisioningMode
// is what actually decides whether "/" serves setup.html or index.html.
bool              provisioningMode       = true;
bool              littlefs_available     = false;

bool              pendingReboot          = false;
unsigned long     rebootTimer            = 0;
unsigned long     connectionAttemptStart = 0;

// =============================================================================
// IMMUTABLE ESP32 HARDWARE PROFILE — POPULATED ONCE AT BOOT
// =============================================================================

struct ESPStaticDetails {
  const char* model = nullptr;
  const char* ver   = nullptr;
  uint32_t    rev   = 0;
  uint8_t     cores = 0;
};

static ESPStaticDetails espStaticDetails;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

void   startAccessPoint();
void   setupWebServer();
void   wipe(bool fullReset);
void   factoryReset(bool fullReset, const char* source);
String getNetworksJSON();
String getContentType(const String& path);
void   handleFirmwareUpload(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool);
void   handleFileUpload(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool);
void   sendJsonResponse(AsyncWebServerRequest* request, int code, JsonDocument& doc);
void   sendStatus(AsyncWebServerRequest* request, int code, const char* status, const char* message = nullptr);
String buildTelemetryJson();

// =============================================================================
// DEOBFUSCATION — decodes hex-encoded XOR strings using a key
// (matches the setup.html Wi-Fi password obfuscation scheme)
// =============================================================================

String deobfuscate(const String& hex, const char* key) {
  if (hex.length() == 0 || hex.length() % 2 != 0) return "";
  size_t keyLen = strlen(key);
  String result = "";
  result.reserve(hex.length() / 2);

  for (size_t i = 0; i < hex.length(); i += 2) {
    uint8_t byte = (uint8_t) strtol(hex.substring(i, i + 2).c_str(), nullptr, 16);
    result += (char) (byte ^ key[(i / 2) % keyLen]);
  }
  return result;
}

/**
 * Changes the Wi-Fi power saving mode instantly on-the-fly.
 * @param enable False for ultra-low latency (<15ms), True for maximum power savings (~220ms).
 */
void setWifiPowerSaving(bool enable) {
  if (enable) {
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    Serial.println("[Wi-Fi] Power Savings ON (~220ms latency, lower power)");
  } else {
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.println("[Wi-Fi] Power Savings OFF (<15ms latency, higher power)");
  }
}

// =============================================================================
// JSON RESPONSE HELPERS
//
// Every route handler used to hand-roll its own "build JsonDocument,
// serializeJson() into a String, request->send()" sequence — or worse, a
// hardcoded literal like "{\"status\":\"success\"}". These two helpers are
// the single place that does the serialize+send dance, and the single place
// that shapes a {status[,message]} envelope, so route handlers just build
// the payload (or nothing at all) and hand it off.
// =============================================================================

void sendJsonResponse(AsyncWebServerRequest* request, int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  request->send(code, "application/json", out);
}

void sendStatus(AsyncWebServerRequest* request, int code, const char* status, const char* message) {
  JsonDocument doc;
  doc["status"] = status;
  if (message != nullptr) doc["message"] = message;
  sendJsonResponse(request, code, doc);
}

// =============================================================================
// TELEMETRY PAYLOAD — shared by the one-shot /api/telemetry GET (used for
// the initial page-load fill, before any SSE event has arrived) and the
// recurring SSE "voltage" push in loop(). Keeping one builder means the two
// can never drift out of sync in which fields they report.
// =============================================================================

String buildTelemetryJson() {
  wifi_ps_type_t psMode = WIFI_PS_NONE;
  esp_wifi_get_ps(&psMode);

  JsonDocument doc;
  doc["vtt"]    = latestVtt;
  doc["vcore"]  = latestVcore;
  doc["temp"]   = temperatureRead();
  doc["rssi"]   = is_ap_mode ? 0 : WiFi.RSSI();
  doc["mem"]    = (unsigned long) ESP.getFreeHeap();
  doc["wifiPs"] = (psMode != WIFI_PS_NONE);
  doc["ts"]     = millis();

  String out;
  serializeJson(doc, out);
  return out;
}

// =============================================================================
// WI-FI SCAN
// =============================================================================

String getNetworksJSON() {
  const int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_FAILED || n == WIFI_SCAN_RUNNING) {
    return "[]";
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() > 0) {
      JsonObject entry = arr.add<JsonObject>();
      entry["ssid"] = ssid;
      entry["rssi"] = WiFi.RSSI(i);
    }
  }

  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  return out;
}

// =============================================================================
// CREDENTIAL WIPE / FACTORY RESET
//   fullReset = true  -> wipe Wi-Fi credentials AND throttle settings
//   fullReset = false -> wipe Wi-Fi credentials only
// =============================================================================

void wipe(bool fullReset) {
  preferences.begin("wifi-config", false);
  preferences.clear();
  preferences.end();

  if (fullReset) {
    preferences.begin("throttle", false);
    preferences.clear();
    preferences.end();
    settings = ThrottleSettings{}; // back to safe defaults (100% speed)
    applyThrottleSpeed(settings.speedPercent);
  }
}

void factoryReset(bool fullReset, const char* source) {
  Serial.printf("\n[RESET] Factory Reset triggered via %s\n", source);
  wipe(fullReset);
}

// =============================================================================
// ACCESS POINT MODE — captive portal for initial Wi-Fi provisioning
//
// Brownout fix carried over from AirMeter: the SuperMini's onboard LDO
// cannot sustain the current surge from full-power radio startup right
// after a software reset. Fix: tear down prior radio state, then clamp TX
// power BEFORE softAP()/begin() rather than after.
// =============================================================================

void startAccessPoint() {
  is_ap_mode = true;

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_AP);

  const IPAddress local_IP(192, 168, 8, 1);
  const IPAddress gateway(192, 168, 8, 1);
  const IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // crucial for SuperMini — see header note

  WiFi.softAP("HAWK370-Setup", "hawk370setup");

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  Serial.println("\n[SYSTEM] Access Point Mode active.");
  Serial.print("[SYSTEM] Connect to 'HAWK370-Setup' — IP: ");
  Serial.println(WiFi.softAPIP());
}

// =============================================================================
// WEB SERVER — routes and SSE configuration
// =============================================================================

void setupWebServer() {

  // ── Root ──────────────────────────────────────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    const char* page = provisioningMode ? "/setup.html" : "/index.html";
    String gzPage = String(page) + ".gz";

    if (littlefs_available && LittleFS.exists(gzPage)) {
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, gzPage, "text/html");
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
    } else if (littlefs_available && LittleFS.exists(page)) {
      request->send(LittleFS, page, "text/html");
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "Error 404: '%s' missing from storage.", page);
      request->send(404, "text/plain", msg);
    }
  });

  // ── Wi-Fi Credentials Save ───────────────────────────────────────────
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->hasParam("ssid", true) && request->hasParam("pass", true)) {
      const String test_ssid = request->getParam("ssid", true)->value();
      const String test_pass = deobfuscate(request->getParam("pass", true)->value(), "bitsundbolts");

      if (test_pass.isEmpty()) {
        sendStatus(request, 400, "error", "Malformed password");
        return;
      }

      Serial.printf("[NETWORK] Testing connection to: %s\n", test_ssid.c_str());

      preferences.begin("wifi-config", false);
      preferences.putString("ssid", test_ssid);
      preferences.putString("pass", test_pass);
      preferences.putUChar("mode", MODE_STA);
      preferences.end();

      WiFi.mode(WIFI_AP_STA);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
      setWifiPowerSaving(false);
      WiFi.begin(test_ssid.c_str(), test_pass.c_str());
      connectionAttemptStart = millis();

      sendStatus(request, 200, "checking");
    } else {
      sendStatus(request, 400, "error");
    }
  });

  // ── API: Setup — choose standalone Access Point mode ──────────────────
  // Called from setup.html's decision screen. No reboot needed: the AP
  // radio is already up during provisioning, so we just persist the choice
  // and flip provisioningMode so "/" starts serving index.html.
  server.on("/api/setup/ap-mode", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!provisioningMode) {
      sendStatus(request, 403, "error", "Already configured");
      return;
    }

    preferences.begin("wifi-config", false);
    preferences.putUChar("mode", MODE_AP);
    preferences.end();

    provisioningMode = false;
    Serial.println("[SETUP] Standalone Access Point mode selected — dashboard now active.");
    sendStatus(request, 200, "success");
  });

  // ── API: Wi-Fi Scan ──────────────────────────────────────────────────
  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!provisioningMode) {
      request->send(403, "text/plain", "Forbidden");
      return;
    }

    static unsigned long lastScanTrigger = 0;
    const int status = WiFi.scanComplete();

    if (status == WIFI_SCAN_RUNNING) {
      sendStatus(request, 202, "scanning");
    } else if (status >= 0) {
      request->send(200, "application/json", getNetworksJSON());
    } else {
      if (millis() - lastScanTrigger > 3000) {
        lastScanTrigger = millis();
        WiFi.scanDelete();
        WiFi.scanNetworks(true, false);
      }
      sendStatus(request, 202, "scanning");
    }
  });

  // ── API: Connection Status (AP mode only) ────────────────────────────
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!provisioningMode) {
      request->send(403, "text/plain", "Forbidden");
      return;
    }

    const wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED && !pendingReboot) {
      sendStatus(request, 200, "success");
      rebootTimer   = millis();
      pendingReboot = true;
      return;
    }

    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      WiFi.disconnect();
      WiFi.mode(WIFI_AP);
      wipe(false);
      sendStatus(request, 200, "fail");
      return;
    }

    if (connectionAttemptStart > 0 && millis() - connectionAttemptStart > 10000) {
      Serial.println("[TIMEOUT] Auth threshold exceeded — forcing fail state.");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_AP);
      wipe(false);
      sendStatus(request, 200, "fail");
      return;
    }

    sendStatus(request, 200, "still_checking");
  });

  // ── API: Factory Reset (STA mode only) ───────────────────────────────
  server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (provisioningMode) {
      sendStatus(request, 403, "error", "Forbidden in setup mode");
      return;
    }
    factoryReset(false, "Web UI");
    sendStatus(request, 200, "success");
    rebootTimer   = millis();
    pendingReboot = true;
  });

  // ── API: Restart ──────────────────────────────────────────────────────
  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (provisioningMode) {
      sendStatus(request, 403, "error", "Forbidden in setup mode");
      return;
    }
    sendStatus(request, 200, "success");
    rebootTimer   = millis();
    pendingReboot = true;
  });

  // ── API: System Info ──────────────────────────────────────────────────
  server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;

    doc["firmware"]  = FIRMWARE_VERSION;
    doc["ipAddress"] = is_ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["ssid"]      = is_ap_mode ? "HAWK370-Setup" : WiFi.SSID();
    doc["rssi"]      = is_ap_mode ? 0 : WiFi.RSSI();
    // Standalone AP mode has no upstream Wi-Fi to apply power-saving
    // against, and running the radio without WIFI_PS_MAX_MODEM measurably
    // raises the module's temperature (bench: ~70C on AP vs ~45C on STA
    // with power saving on). The UI hides the toggle entirely when this
    // is true rather than showing a control that does nothing.
    doc["apMode"]    = is_ap_mode;

    JsonObject espObj = doc["esp"].to<JsonObject>();
    espObj["memory"] = ESP.getFreeHeap();
    espObj["temp"]   = temperatureRead();
    {
      wifi_ps_type_t psMode = WIFI_PS_NONE;
      esp_wifi_get_ps(&psMode);
      espObj["wifiPs"] = (psMode != WIFI_PS_NONE);
    }
    espObj["model"] = espStaticDetails.model;
    espObj["cores"] = espStaticDetails.cores;
    espObj["rev"]   = espStaticDetails.rev;
    espObj["ver"]   = espStaticDetails.ver;

    if (littlefs_available) {
      size_t totalBytes = LittleFS.totalBytes();
      size_t usedBytes  = LittleFS.usedBytes();
      doc["littlefs_total"] = totalBytes;
      doc["littlefs_free"]  = totalBytes - usedBytes;
    } else {
      doc["littlefs_total"] = 0;
      doc["littlefs_free"]  = 0;
    }

    sendJsonResponse(request, 200, doc);
  });

  // ── API: Wi-Fi Power Saving ──────────────────────────────────────────
  // POST /api/system/wifi-ps  body: { "ps": true } or { "ps": false }
  server.on("/api/system/wifi-ps", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len) || !doc["ps"].is<bool>()) {
        sendStatus(request, 400, "error", "Invalid JSON or missing ps field");
        return;
      }

      const bool psOn = doc["ps"].as<bool>();
      setWifiPowerSaving(psOn);
      settings.wifiPsNone = !psOn;
      persistWifiPs();

      Serial.printf("[Wi-Fi] Power saving set to %s via Web UI\n", psOn ? "ON" : "OFF");
      sendStatus(request, 200, "success");
    }
  );

  // ── API: CPU Family List ─────────────────────────────────────────────
  // Single source of truth for the UI's family dropdown — avoids the
  // family list drifting out of sync between firmware and browser code.
  server.on("/api/cpu-families", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < FAMILY_COUNT; i++) {
      JsonObject entry = arr.add<JsonObject>();
      entry["id"]         = i;
      entry["name"]       = THROTTLE_PRESETS[i].name;
      entry["calibrated"] = THROTTLE_PRESETS[i].calibrated;
    }
    sendJsonResponse(request, 200, doc);
  });

  // ── API: Throttle — read current config ──────────────────────────────
  server.on("/api/throttle", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["family"]       = settings.family;
    doc["familyName"]   = THROTTLE_PRESETS[settings.family].name;
    doc["calibrated"]   = THROTTLE_PRESETS[settings.family].calibrated;
    doc["baseMhz"]      = settings.baseMhz;
    doc["speedPercent"] = settings.speedPercent;
    doc["targetMhz"]    = computeTargetMhz(settings.speedPercent, settings.baseMhz);
    sendJsonResponse(request, 200, doc);
  });

  // ── API: Throttle — set CPU family + base MHz ────────────────────────
  // POST /api/throttle/config  body: { "family": int, "baseMhz": int }
  // Does not touch hardware output directly — family/baseMhz only affect
  // the preset selection and the MHz readout, not the live PWM state.
  server.on("/api/throttle/config", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        sendStatus(request, 400, "error", "Invalid JSON");
        return;
      }
      if (!doc["family"].is<int>() || doc["family"].as<int>() < 0 || doc["family"].as<int>() >= FAMILY_COUNT ||
          !doc["baseMhz"].is<int>() || doc["baseMhz"].as<int>() <= 0) {
        sendStatus(request, 400, "error", "Invalid family or baseMhz");
        return;
      }

      settings.family  = (uint8_t) doc["family"].as<int>();
      settings.baseMhz = (uint32_t) doc["baseMhz"].as<int>();
      persistFamilyAndBaseMhz();

      // Family change alone changes which preset (carrier/w_min) is active,
      // so re-arm the modulator at the current speed under the new preset.
      applyThrottleSpeed(settings.speedPercent);

      Serial.printf("[THROTTLE] Config set — family %u (%s), baseMhz %u\n",
                    settings.family, THROTTLE_PRESETS[settings.family].name, settings.baseMhz);

      sendStatus(request, 200, "success");
    }
  );

  // ── API: Throttle — set speed percent (0-100, 100 = full speed) ─────
  // POST /api/throttle/speed  body: { "speedPercent": int }
  // Applied to hardware immediately; NVS write is debounced (see
  // tickSettingsPersistence()) so a dragged slider doesn't wear flash.
  server.on("/api/throttle/speed", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len) ||
          !(doc["speedPercent"].is<float>() || doc["speedPercent"].is<int>())) {
        sendStatus(request, 400, "error", "speedPercent must be a number 0-100");
        return;
      }

      const float requested = doc["speedPercent"].as<float>();
      if (requested < 0.0f || requested > 100.0f) {
        sendStatus(request, 400, "error", "speedPercent must be 0-100");
        return;
      }

      settings.speedPercent = requested;
      applyThrottleSpeed(settings.speedPercent);

      speedDirty      = true;
      speedDirtySince = millis();

      JsonDocument resp;
      resp["status"]       = "success";
      resp["speedPercent"] = settings.speedPercent;
      resp["targetMhz"]    = computeTargetMhz(settings.speedPercent, settings.baseMhz);
      sendJsonResponse(request, 200, resp);
    }
  );

  // ── API: Telemetry — one-shot read (for initial page load, before the
  //   first SSE push arrives). Renamed from /api/voltage since the payload
  //   carries more than the voltage rails (temp, rssi, free memory, Wi-Fi
  //   power-saving state) — it's the same shape as the SSE "voltage" event,
  //   built by the same buildTelemetryJson() helper so the two can't drift
  //   apart. ───────────────────────────────────────────────────────────────
  server.on("/api/telemetry", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", buildTelemetryJson());
  });

  // ── API: List all files on LittleFS (for the OTA/Files page) ─────────
  server.on("/api/files/all", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    if (littlefs_available) {
      File root = LittleFS.open("/");
      File file = root.openNextFile();
      while (file) {
        if (!file.isDirectory()) {
          JsonObject entry = arr.add<JsonObject>();
          entry["name"] = String(file.name());
          entry["size"] = file.size();
        }
        file = root.openNextFile();
      }
    }

    sendJsonResponse(request, 200, doc);
  });

  // ── API: Delete a file on LittleFS ───────────────────────────────────
  server.on("/api/files/delete", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        sendStatus(request, 400, "error", "Invalid JSON");
        return;
      }
      if (!doc["path"].is<const char*>()) {
        sendStatus(request, 400, "error", "Missing path");
        return;
      }

      const String path = doc["path"].as<String>();
      if (path.isEmpty() || path == "/") {
        sendStatus(request, 400, "error", "Invalid path");
        return;
      }
      if (!littlefs_available) {
        sendStatus(request, 503, "error", "Storage unavailable");
        return;
      }
      if (!LittleFS.exists(path)) {
        sendStatus(request, 404, "error", "File not found");
        return;
      }

      if (LittleFS.remove(path)) {
        Serial.printf("[FS] Deleted: %s\n", path.c_str());
        sendStatus(request, 200, "success");
      } else {
        sendStatus(request, 500, "error", "Delete failed");
      }
    }
  );

  // ── API: Rename a file on LittleFS ───────────────────────────────────
  server.on("/api/files/rename", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        sendStatus(request, 400, "error", "Invalid JSON");
        return;
      }
      if (!doc["from"].is<const char*>() || !doc["to"].is<const char*>()) {
        sendStatus(request, 400, "error", "Missing from/to");
        return;
      }

      const String fromPath = doc["from"].as<String>();
      const String toPath   = doc["to"].as<String>();

      if (fromPath.isEmpty() || fromPath == "/" || toPath.isEmpty() || toPath == "/") {
        sendStatus(request, 400, "error", "Invalid path");
        return;
      }
      if (fromPath == toPath) {
        sendStatus(request, 200, "success");
        return;
      }
      if (!littlefs_available) {
        sendStatus(request, 503, "error", "Storage unavailable");
        return;
      }
      if (!LittleFS.exists(fromPath)) {
        sendStatus(request, 404, "error", "File not found");
        return;
      }
      if (LittleFS.exists(toPath)) {
        sendStatus(request, 409, "error", "Target name already exists");
        return;
      }

      if (LittleFS.rename(fromPath, toPath)) {
        Serial.printf("[FS] Renamed: %s -> %s\n", fromPath.c_str(), toPath.c_str());
        sendStatus(request, 200, "success");
      } else {
        sendStatus(request, 500, "error", "Rename failed");
      }
    }
  );

  // ── API: File Upload (Storage) ────────────────────────────────────────
  server.on("/ota/file", HTTP_POST, [](AsyncWebServerRequest* request) {
      request->send(200, "text/plain", "OK");
    },
    handleFileUpload
  );

  // ── API: Firmware Upload (OTA) ────────────────────────────────────────
  server.on("/ota/firmware", HTTP_POST, [](AsyncWebServerRequest* request) {
      // Send 200 first, then reboot — gives the browser time to receive
      // the response before the device disappears off the network.
      request->send(200, "text/plain", "OK");
      rebootTimer   = millis();
      pendingReboot = true;
    },
    handleFirmwareUpload
  );

  // ── Static Assets (LittleFS) ──────────────────────────────────────────
  if (littlefs_available) {
    server.serveStatic("/", LittleFS, "/").setCacheControl("max-age=3600");
  }

  // ── SSE Handler ───────────────────────────────────────────────────────
  events.onConnect([](AsyncEventSourceClient* client) {
    client->send("Connected to HAWK 370 Throttle Stream", nullptr, millis(), 10000);
  });
  server.addHandler(&events);

  // ── Catch-All / Dynamic HTML Rewrite ─────────────────────────────────
  server.onNotFound([](AsyncWebServerRequest* request) {
    String path = request->url();

    if (path == "/" || path == "/save" || path == "/events" ||
        path.startsWith("/api/") || path.startsWith("/ota/")) {
      request->send(404, "text/plain", "404: Not Found");
      return;
    }

    if (!littlefs_available) {
      request->send(404, "text/plain", "Error 404: '" + path + "' not found on this device.");
      return;
    }

    if (path.indexOf('.') == -1) {
      path += ".html";
    }

    const String contentType = getContentType(path);
    const String gzPath = path + ".gz";

    if (LittleFS.exists(gzPath)) {
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, gzPath, contentType);
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }
    if (LittleFS.exists(path)) {
      request->send(LittleFS, path, contentType);
      return;
    }

    request->send(404, "text/plain", "Error 404: '" + path + "' not found on this device.");
  });

  server.begin();
  Serial.println("[SYSTEM] Web server started.");
}

String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".woff2")) return "font/woff2";
  return "text/plain";
}

// =============================================================================
// OTA UPLOAD HANDLERS
// =============================================================================

struct UploadContext {
  bool   error    = false;
  String errorMsg = "";
  File   fsFile;
};

static UploadContext otaCtx;
static UploadContext fileCtx;

void handleFirmwareUpload(AsyncWebServerRequest* request, const String& filename,
                           size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    otaCtx = UploadContext{};
    Serial.printf("[OTA] Firmware upload started: %s\n", filename.c_str());

    size_t fileSize = request->contentLength();
    if (!Update.begin(fileSize > 0 ? fileSize : UPDATE_SIZE_UNKNOWN)) {
      otaCtx.error    = true;
      otaCtx.errorMsg = "Update.begin() failed: " + String(Update.errorString());
      Serial.println("[OTA] " + otaCtx.errorMsg);
      return;
    }
  }

  if (otaCtx.error) return;

  if (Update.write(data, len) != len) {
    otaCtx.error    = true;
    otaCtx.errorMsg = "Write error: " + String(Update.errorString());
    Serial.println("[OTA] " + otaCtx.errorMsg);
    Update.abort();
    return;
  }

  if (final) {
    if (!Update.end(true)) {
      otaCtx.error    = true;
      otaCtx.errorMsg = "Finalise error: " + String(Update.errorString());
      Serial.println("[OTA] " + otaCtx.errorMsg);
    } else {
      Serial.printf("[OTA] Firmware flashed successfully (%u bytes)\n", index + len);
    }
  }
}

void handleFileUpload(AsyncWebServerRequest* request, const String& filename,
                       size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    fileCtx = UploadContext{};

    String targetPath = "/" + filename;
    if (request->hasParam("path", true)) {
      targetPath = request->getParam("path", true)->value();
      if (!targetPath.startsWith("/")) targetPath = "/" + targetPath;
    }

    size_t fileSize = request->contentLength();
    if (fileSize > 0 && fileSize > LittleFS.totalBytes() - LittleFS.usedBytes()) {
      fileCtx.error    = true;
      fileCtx.errorMsg = "Insufficient LittleFS space";
      Serial.println("[OTA] " + fileCtx.errorMsg);
      return;
    }

    Serial.printf("[OTA] File upload started: %s -> %s\n", filename.c_str(), targetPath.c_str());

    fileCtx.fsFile = LittleFS.open(targetPath, "w");
    if (!fileCtx.fsFile) {
      fileCtx.error    = true;
      fileCtx.errorMsg = "Failed to open " + targetPath + " for writing";
      Serial.println("[OTA] " + fileCtx.errorMsg);
      return;
    }
  }

  if (fileCtx.error) return;

  if (len > 0) {
    size_t written = fileCtx.fsFile.write(data, len);
    if (written != len) {
      fileCtx.error    = true;
      fileCtx.errorMsg = "Write error (disk full?)";
      fileCtx.fsFile.close();
      Serial.println("[OTA] " + fileCtx.errorMsg);
      return;
    }
  }

  if (final) {
    fileCtx.fsFile.close();
    Serial.printf("[OTA] File written successfully (%u bytes)\n", index + len);
  }
}

// =============================================================================
// SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  //setCpuFrequencyMhz(80);
  Serial.println("\n--- HAWK 370 ThrottleBlaster Booting ---");

  // ── Deactivate and purge Bluetooth (RAM back to heap, ESP32-C3) ───────
  esp_bt_mem_release(ESP_BT_MODE_BLE);
  Serial.println("[SYSTEM] Bluetooth hardware purged. RAM released to heap.");

  espStaticDetails.model = ESP.getChipModel();
  espStaticDetails.cores = ESP.getChipCores();
  espStaticDetails.rev   = ESP.getChipRevision();
  espStaticDetails.ver   = ESP.getCoreVersion();

  // ── Throttle + voltage subsystems ──────────────────────────────────────
  loadSettings();
  rmtInit();
  adcInit();
  applyThrottleSpeed(settings.speedPercent);
  setWifiPowerSaving(!settings.wifiPsNone);
  Serial.printf("[THROTTLE] Loaded — family %u (%s), baseMhz %u, speed %.2f%%\n",
                settings.family, THROTTLE_PRESETS[settings.family].name,
                settings.baseMhz, settings.speedPercent);

  // ── LittleFS ──────────────────────────────────────────────────────────
  if (LittleFS.begin(true)) {
    littlefs_available = true;
    Serial.println("[STORAGE] LittleFS mounted.");
  } else {
    Serial.println("[CRITICAL] LittleFS mount failed!");
  }

  // ── Load Wi-Fi Credentials + Chosen Mode ────────────────────────────────
  preferences.begin("wifi-config", true);
  wifi_ssid           = preferences.getString("ssid", "");
  wifi_password       = preferences.getString("pass", "");
  uint8_t storedMode  = preferences.getUChar("mode", MODE_UNCONFIGURED);
  preferences.end();

  // ── Network Strategy ─────────────────────────────────────────────────
  // Three persisted states: still unconfigured (show the setup decision
  // screen), deliberately standalone-AP (bench mode, no home network), or
  // STA (join a saved network). wipe(false) clears this whole namespace,
  // which naturally drops back to MODE_UNCONFIGURED on next boot.
  if (storedMode == MODE_AP) {
    startAccessPoint();
    provisioningMode = false;
    Serial.println("[NETWORK] Configured for standalone Access Point mode.");
  } else if (storedMode == MODE_STA && !wifi_ssid.isEmpty()) {
    Serial.printf("[NETWORK] Connecting to: %s\n", wifi_ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    setWifiPowerSaving(false); // reliability during connect; re-applied once up
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 120) { // 60s max
      delay(500);
      Serial.print('.');
      retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      setWifiPowerSaving(!settings.wifiPsNone);
      provisioningMode = false;
      Serial.printf("\n[NETWORK] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

      if (MDNS.begin("hawk370")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[DNS] mDNS active: http://hawk370.local");
      }
    } else {
      Serial.println("\n[WARNING] Connection failed — dropping to setup mode.");
      startAccessPoint();
      provisioningMode = true;
    }
  } else {
    // MODE_UNCONFIGURED — first boot, or after a Wi-Fi reset.
    startAccessPoint();
    provisioningMode = true;
  }

  setupWebServer();
}

// =============================================================================
// LOOP
// =============================================================================

void loop() {
  // ── Captive Portal DNS ────────────────────────────────────────────────
  if (is_ap_mode) {
    dnsServer.processNextRequest();
  }

  // ── Pending Reboot ────────────────────────────────────────────────────
  if (pendingReboot && millis() - rebootTimer > 500) {
    Serial.println("[SYSTEM] Executing reboot...");
    ESP.restart();
  }

  // ── Wi-Fi Reconnection Watchdog ───────────────────────────────────────
  if (!is_ap_mode && !pendingReboot) {
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 10000) {
      lastWifiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NETWORK] Wi-Fi lost — reconnecting...");
        WiFi.reconnect();
      }
    }
  }

  // ── SSE Keep-Alive ────────────────────────────────────────────────────
  {
    static unsigned long lastSSEPing = 0;
    if (millis() - lastSSEPing > 25000) {
      lastSSEPing += 25000;
      events.send("", "ping");
    }
  }

  // ── Voltage sampling + SSE push ──────────────────────────────────────
  tickVoltageSampling();
  {
    static unsigned long lastPush = 0;
    if (millis() - lastPush >= VOLTAGE_PUSH_INTERVAL_MS) {
      lastPush = millis();
      if (decimateVoltage()) {
        events.send(buildTelemetryJson(), "telemetry");
      }
    }
  }

  // ── Debounced settings persistence ───────────────────────────────────
  tickSettingsPersistence();

  yield();
}
