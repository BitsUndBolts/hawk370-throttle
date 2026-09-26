/*
 * Host-side test for the STPCLK# pattern builder.
 * Build and run on a PC (not compiled by the Arduino IDE):
 *   sh test/run_tests.sh
 */

#include "../hawk_pattern.h"

#include <math.h>
#include <stdio.h>

using namespace hawk;

static int failures = 0;

#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      failures++;                                \
      if (failures <= 20) {                      \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
      }                                          \
    }                                            \
  } while (0)

static void checkPattern(float speed, const PatternTiming& t) {
  Pattern p;
  buildPattern(speed, t, p);

  if (speed <= 0.0f) {
    CHECK(p.kind == PATTERN_PAUSE, "speed %.4f should pause", speed);
    CHECK(p.deliveredPercent == 0.0f, "pause delivers 0");
    return;
  }
  if (speed >= maxModulatedPercent(t)) {
    CHECK(p.kind == PATTERN_FULL, "speed %.4f should be full speed", speed);
    CHECK(p.deliveredPercent == 100.0f, "full delivers 100");
    return;
  }

  CHECK(p.kind == PATTERN_MODULATED, "speed %.4f should modulate", speed);
  CHECK(p.cycles == PATTERN_CYCLES, "cycle count %u", p.cycles);
  CHECK(p.cycles + 1 <= 48, "must fit one RMT memory block with EOF");

  uint32_t run = 0, total = 0;
  for (uint8_t k = 0; k < p.cycles; k++) {
    CHECK(p.cycle[k].runUs >= t.wMinUs, "run %u < wMin at %.4f%%", p.cycle[k].runUs, speed);
    CHECK(p.cycle[k].stopUs >= t.wMinUs, "stop %u < wMin at %.4f%%", p.cycle[k].stopUs, speed);
    CHECK(p.cycle[k].runUs <= PATTERN_MAX_PHASE_US, "run overflow");
    CHECK(p.cycle[k].stopUs <= PATTERN_MAX_PHASE_US, "stop overflow");
    run   += p.cycle[k].runUs;
    total += p.cycle[k].runUs + p.cycle[k].stopUs;
  }
  const float delivered = 100.0f * run / total;
  CHECK(fabsf(delivered - p.deliveredPercent) < 1e-3f, "reported delivered mismatch");

  const float expected = speed < minModulatedPercent(t) ? minModulatedPercent(t) : speed;
  CHECK(fabsf(delivered - expected) < 0.05f,
        "speed %.4f%% delivered %.4f%% (error %.4f)", speed, delivered, delivered - expected);

  // Middle band keeps the chipset's period.
  if (speed > 15.0f && speed < 85.0f) {
    const float period = (float) total / p.cycles;
    CHECK(fabsf(period - t.periodUs) <= 1.0f, "period %.2f at %.2f%%", period, speed);
  }
}

int main() {
  const PatternTiming t = DEFAULT_TIMING;

  // Sweep 0..100% in 0.001% steps.
  for (int i = 0; i <= 100000; i++) checkPattern(i / 1000.0f, t);

  // Every whole MHz on a range of base clocks, from Mendocino to overclocked Tualatin.
  const int bases[] = { 233, 300, 333, 466, 533, 700, 866, 1000, 1133, 1400, 1600 };
  float worstMhz = 0.0f;
  for (int b : bases) {
    for (int mhz = 1; mhz < b; mhz++) {
      const float speed = 100.0f * mhz / b;
      checkPattern(speed, t);
      Pattern p;
      buildPattern(speed, t, p);
      if (p.kind == PATTERN_MODULATED && speed >= minModulatedPercent(t)) {
        const float err = fabsf(p.deliveredPercent * b / 100.0f - mhz);
        if (err > worstMhz) worstMhz = err;
      }
    }
  }

  // Edge cases.
  Pattern p;
  buildPattern(NAN, t, p);
  CHECK(p.kind == PATTERN_FULL, "NaN -> full speed");
  buildPattern(-5.0f, t, p);
  CHECK(p.kind == PATTERN_PAUSE, "negative -> pause");
  buildPattern(250.0f, t, p);
  CHECK(p.kind == PATTERN_FULL, ">100 -> full speed");

  printf("modulated range %.3f%% .. %.3f%%, worst whole-MHz error %.3f MHz\n",
         minModulatedPercent(t), maxModulatedPercent(t), worstMhz);
  if (failures) {
    printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  printf("all pattern checks passed\n");
  return 0;
}
