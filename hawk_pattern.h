/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — STPCLK# pattern builder (pure logic, no Arduino / IDF dependency)
//
// Kept free of hardware headers on purpose so test/pattern_test.cpp can build
// and sweep it on a PC. hawk_throttle.cpp turns the result into RMT symbols.
//
// Timing model — taken from Intel's own STPCLK# throttling, not guessed:
//   The PIIX4 on every 440BX board throttles these CPUs itself by toggling
//   STPCLK# with a 244 us period in 12.5% duty steps, so its shortest phase is
//   30.5 us (Intel, "Pentium III Processor Active Thermal Management
//   Techniques", AP-273405). Stop Grant exit takes only 10 bus clocks because
//   the PLL keeps running.
//
//   * Between ~13% and ~87% speed the pattern is a fixed 244 us period with
//     continuous duty — the chipset's own regime, minus its 12.5% steps.
//   * Outside that band the short phase is pinned at wMinUs (32 us, just above
//     the PIIX4's 30.5 us) and the long phase stretches, up to one RMT
//     symbol (32.767 ms). That gives ~0.1% .. ~99.9% speed.
//   * 0% is Pause (STPCLK# held asserted), 100% is released.
//
//   Each cycle is emitted RUN first, then STOP. PATTERN_CYCLES cycles are
//   dithered (error diffusion) so the average hits the request to ~0.03%.
// =============================================================================

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace hawk {

// Logical levels. The pad polarity is applied separately (STPCLK_INVERTED).
static const uint8_t  LVL_RUN  = 0;   // STPCLK# released, CPU runs
static const uint8_t  LVL_STOP = 1;   // STPCLK# asserted, CPU in Stop Grant

static const uint16_t PATTERN_MAX_PHASE_US = 0x7FFF;  // 15-bit RMT duration at 1 MHz
static const uint8_t  PATTERN_CYCLES       = 16;      // 16 symbols + EOF fit one 48-word block

struct PatternTiming {
  uint16_t wMinUs;     // shortest allowed run or stop phase
  uint16_t periodUs;   // nominal period in the middle band
};

// Defaults for every CPU family (see header comment for the source).
static const PatternTiming DEFAULT_TIMING = { 32, 244 };

enum PatternKind : uint8_t {
  PATTERN_FULL      = 0,   // 100%: STPCLK# released
  PATTERN_PAUSE     = 1,   // 0%:   STPCLK# held asserted
  PATTERN_MODULATED = 2    // RMT loop of `cycles` run/stop pairs
};

struct PatternCycle {
  uint16_t runUs;
  uint16_t stopUs;
};

struct Pattern {
  PatternKind  kind;
  uint8_t      cycles;
  PatternCycle cycle[PATTERN_CYCLES];
  float        deliveredPercent;  // what the CPU actually gets, 0..100
};

// Builds the pattern for a requested speed (0..100 percent of base clock).
void buildPattern(float speedPercent, const PatternTiming& timing, Pattern& out);

// Lowest / highest speed that is still modulated (outside: Pause / full speed).
float minModulatedPercent(const PatternTiming& timing);
float maxModulatedPercent(const PatternTiming& timing);

}  // namespace hawk
