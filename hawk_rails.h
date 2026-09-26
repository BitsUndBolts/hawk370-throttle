/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — VCORE / VTT monitoring (ADC1, eFuse calibrated)
//
// A rough, averaged view of the rails for the dashboard — not a scope. Samples
// every 100 ms and averages over each 500 ms push window.
// =============================================================================

#pragma once

#include <Arduino.h>

struct RailReading {
  float vcore;   // volts, NAN if unavailable
  float vtt;     // volts, NAN if unavailable
};

bool        railsBegin();          // false if the ADC could not be set up
void        railsTick();           // call from loop()
RailReading railsLatest();
bool        railsOk();
bool        railsCalibrated();     // eFuse calibration active
