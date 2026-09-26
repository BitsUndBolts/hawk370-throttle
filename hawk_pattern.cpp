/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

#include "hawk_pattern.h"

#include <math.h>

namespace hawk {

// Fraction of time running at the edges of the modulated range: the short
// phase is pinned at wMin and the long phase cannot exceed one RMT symbol.
static float minRunFraction(const PatternTiming& t) {
  return (float) t.wMinUs / (float) (t.wMinUs + PATTERN_MAX_PHASE_US);
}

float minModulatedPercent(const PatternTiming& t) {
  return 100.0f * minRunFraction(t);
}

float maxModulatedPercent(const PatternTiming& t) {
  return 100.0f * (1.0f - minRunFraction(t));
}

// Error-diffusion rounding to whole microseconds, never below wMin.
static uint16_t ditherPhase(float wantUs, float& err, uint16_t wMinUs) {
  const float w = wantUs + err;
  long d = lroundf(w);
  if (d < (long) wMinUs) d = wMinUs;
  if (d > (long) PATTERN_MAX_PHASE_US) d = PATTERN_MAX_PHASE_US;
  err = w - (float) d;
  return (uint16_t) d;
}

void buildPattern(float speedPercent, const PatternTiming& timing, Pattern& out) {
  out.cycles = 0;

  PatternTiming t = timing;
  if (t.wMinUs < 1) t.wMinUs = 1;
  if (t.periodUs < 2 * t.wMinUs) t.periodUs = 2 * t.wMinUs;

  // NaN or >= 100 -> released; <= 0 -> Pause.
  if (!(speedPercent < 100.0f)) {
    out.kind = PATTERN_FULL;
    out.deliveredPercent = 100.0f;
    return;
  }
  if (speedPercent <= 0.0f) {
    out.kind = PATTERN_PAUSE;
    out.deliveredPercent = 0.0f;
    return;
  }

  const float rMin = minRunFraction(t);
  float r = speedPercent / 100.0f;                 // fraction of time running
  if (r >= 1.0f - rMin) {                          // above ~99.9%: not worth a stop
    out.kind = PATTERN_FULL;
    out.deliveredPercent = 100.0f;
    return;
  }
  if (r < rMin) r = rMin;                          // below ~0.1%: slowest modulated

  // Middle band: fixed period, like the PIIX4.
  float runUs  = r * (float) t.periodUs;
  float stopUs = (float) t.periodUs - runUs;
  // Edges: pin the short phase at wMin, stretch the long one.
  if (runUs < t.wMinUs) {
    runUs  = t.wMinUs;
    stopUs = t.wMinUs * (1.0f - r) / r;
  } else if (stopUs < t.wMinUs) {
    stopUs = t.wMinUs;
    runUs  = t.wMinUs * r / (1.0f - r);
  }

  float errRun = 0.0f, errStop = 0.0f;
  uint32_t totalRun = 0, total = 0;
  for (uint8_t k = 0; k < PATTERN_CYCLES; k++) {
    const uint16_t run  = ditherPhase(runUs,  errRun,  t.wMinUs);
    const uint16_t stop = ditherPhase(stopUs, errStop, t.wMinUs);
    out.cycle[k].runUs  = run;
    out.cycle[k].stopUs = stop;
    totalRun += run;
    total    += (uint32_t) run + stop;
  }

  out.kind   = PATTERN_MODULATED;
  out.cycles = PATTERN_CYCLES;
  out.deliveredPercent = 100.0f * (float) totalRun / (float) total;
}

}  // namespace hawk
