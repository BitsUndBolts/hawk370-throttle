/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — board configuration shared by every module
// =============================================================================

#pragma once

#include <Arduino.h>
#include "driver/gpio.h"
#include "hal/adc_types.h"

#define HAWK_FIRMWARE_VERSION "0.5"

// ── STPCLK# drive ────────────────────────────────────────────────────────────
// GPIO7 drives the gate of the onboard 2N7002; its drain pulls STPCLK# to VSS.
// Gate high -> FET on -> STPCLK# asserted -> CPU in Stop Grant.
// A 10k gate-to-VSS resistor on the PCB keeps the FET off while the ESP32 is
// in reset, in the bootloader, in USB download mode or not fitted at all.
static const gpio_num_t STPCLK_GPIO = GPIO_NUM_7;

// Set to true only for a board whose driver stage inverts (gate low = assert).
// All firmware logic works in logical levels (see hawk_pattern.h) and this one
// flag flips the pad for the RMT pattern AND for every static level.
static const bool STPCLK_INVERTED = false;

// ── Rail sense (verified on the PCB) ─────────────────────────────────────────
// GPIO0 = ADC1 channel 0 = VCORE (VCC), GPIO1 = ADC1 channel 1 = VTT.
// Both through 10k/10k dividers with 100nF, so the pin sees half the rail.
static const adc_channel_t VCORE_ADC_CHANNEL = ADC_CHANNEL_0;   // GPIO0
static const adc_channel_t VTT_ADC_CHANNEL   = ADC_CHANNEL_1;   // GPIO1
static const float         RAIL_DIVIDER_RATIO = 2.0f;

// ── Network identity ─────────────────────────────────────────────────────────
static const char* const HAWK_HOSTNAME    = "hawk370";
static const char* const HAWK_AP_SSID     = "HAWK370-Setup";
static const char* const HAWK_AP_PASSWORD = "hawk370setup";
static const char* const HAWK_OBFUSCATION_KEY = "bitsundbolts";   // matches setup.html
