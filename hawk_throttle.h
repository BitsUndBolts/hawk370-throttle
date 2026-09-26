/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — STPCLK# throttle (RMT hardware loop + glitch-free switching)
//
// Threading: the web server runs on the AsyncTCP task. Its handlers only post
// requests (throttleRequest*), which are cheap and thread-safe. All pin, RMT
// and NVS work happens in throttleTick(), called from loop().
// =============================================================================

#pragma once

#include <Arduino.h>
#include "hawk_pattern.h"

enum CpuFamily : uint8_t {
  FAMILY_MENDOCINO    = 0,
  FAMILY_COPPERMINE   = 1,
  FAMILY_TUALATIN     = 2,
  FAMILY_VIA_SAMUEL   = 3,
  FAMILY_VIA_EZRA     = 4,
  FAMILY_VIA_NEHEMIAH = 5,
  FAMILY_COUNT        = 6
};

struct CpuFamilyInfo {
  const char*         name;
  hawk::PatternTiming timing;
  bool                benchVerified;   // false until confirmed on real hardware
};

extern const CpuFamilyInfo CPU_FAMILIES[FAMILY_COUNT];

struct ThrottleState {
  uint8_t  family;
  uint32_t baseMhz;
  float    requestedPercent;   // what the user asked for (0 = Pause, 100 = full)
  float    deliveredPercent;   // what the pattern really gives the CPU
  bool     paused;
};

// Call as the very first thing in setup(): drives the 2N7002 gate low
// (STPCLK# released) before anything else runs.
void throttleEarlyPinInit();

// Loads the saved CPU family / base clock, creates the RMT channel and starts
// at full speed. Power-on is always full speed; the speed is not saved.
bool throttleBegin();

// Applies pending requests and persists config changes. Call from loop().
void throttleTick();

// Thread-safe, callable from web handlers. Return the resulting state.
ThrottleState throttleRequestSpeed(float speedPercent);
ThrottleState throttleRequestConfig(uint8_t family, uint32_t baseMhz);

ThrottleState throttleGetState();
uint32_t      throttleStateVersion();   // bumps on every change, for SSE sync
bool          throttleHardwareOk();
