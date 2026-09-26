/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

#include "hawk_rails.h"
#include "hawk_config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// The pins only see half the rail: 0.6 .. 1.05 V for VTT 1.25-1.5 V and
// VCORE up to 2.05 V (Mendocino). 6 dB attenuation covers 0 .. ~1.3 V on the
// ESP32-C3 (a 2.6 V rail) with less calibration error than 12 dB.
static const adc_atten_t    ADC_ATTEN = ADC_ATTEN_DB_6;
static const adc_bitwidth_t ADC_WIDTH = ADC_BITWIDTH_12;

static const uint32_t SAMPLE_INTERVAL_MS = 100;
static const uint32_t WINDOW_MS          = 500;

static adc_oneshot_unit_handle_t adcUnit     = nullptr;
static adc_cali_handle_t         caliVcore   = nullptr;
static adc_cali_handle_t         caliVtt     = nullptr;
static bool                      ok          = false;

static uint32_t    accVcoreMv = 0, accVttMv = 0;
static uint16_t    sampleCount = 0;
static RailReading latest = { NAN, NAN };

static bool makeCali(adc_channel_t ch, adc_cali_handle_t* out) {
  adc_cali_curve_fitting_config_t c = {};
  c.unit_id  = ADC_UNIT_1;
  c.chan     = ch;
  c.atten    = ADC_ATTEN;
  c.bitwidth = ADC_WIDTH;
  const esp_err_t e = adc_cali_create_scheme_curve_fitting(&c, out);
  if (e != ESP_OK) {
    Serial.printf("[RAILS] calibration for channel %d failed: %s\n", (int) ch, esp_err_to_name(e));
    *out = nullptr;
    return false;
  }
  return true;
}

bool railsBegin() {
  adc_oneshot_unit_init_cfg_t unitCfg = {};
  unitCfg.unit_id = ADC_UNIT_1;
  esp_err_t e = adc_oneshot_new_unit(&unitCfg, &adcUnit);
  if (e != ESP_OK) {
    Serial.printf("[RAILS] adc_oneshot_new_unit: %s\n", esp_err_to_name(e));
    return false;
  }

  adc_oneshot_chan_cfg_t chCfg = {};
  chCfg.atten    = ADC_ATTEN;
  chCfg.bitwidth = ADC_WIDTH;
  if (adc_oneshot_config_channel(adcUnit, VCORE_ADC_CHANNEL, &chCfg) != ESP_OK ||
      adc_oneshot_config_channel(adcUnit, VTT_ADC_CHANNEL, &chCfg) != ESP_OK) {
    Serial.println("[RAILS] adc_oneshot_config_channel failed");
    return false;
  }

  // Without eFuse calibration the readings would be guesses, so they are
  // reported as unavailable instead of silently showing 0 V.
  ok = makeCali(VCORE_ADC_CHANNEL, &caliVcore) & makeCali(VTT_ADC_CHANNEL, &caliVtt);
  Serial.printf("[RAILS] %s\n", ok ? "ADC ready (6 dB, eFuse calibrated)" : "ADC calibration unavailable");
  return ok;
}

static bool readMv(adc_channel_t ch, adc_cali_handle_t cali, int* mv) {
  int raw = 0;
  if (adc_oneshot_read(adcUnit, ch, &raw) != ESP_OK) return false;
  return adc_cali_raw_to_voltage(cali, raw, mv) == ESP_OK;
}

void railsTick() {
  if (!ok) return;
  const uint32_t now = millis();

  static uint32_t lastSample = 0;
  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    int vcoreMv = 0, vttMv = 0;
    if (readMv(VCORE_ADC_CHANNEL, caliVcore, &vcoreMv) && readMv(VTT_ADC_CHANNEL, caliVtt, &vttMv)) {
      accVcoreMv += (uint32_t) vcoreMv;
      accVttMv   += (uint32_t) vttMv;
      sampleCount++;
    }
  }

  static uint32_t lastWindow = 0;
  if (now - lastWindow >= WINDOW_MS) {
    lastWindow = now;
    if (sampleCount > 0) {
      latest.vcore = (accVcoreMv / (float) sampleCount) / 1000.0f * RAIL_DIVIDER_RATIO;
      latest.vtt   = (accVttMv   / (float) sampleCount) / 1000.0f * RAIL_DIVIDER_RATIO;
      accVcoreMv = accVttMv = 0;
      sampleCount = 0;
    }
  }
}

RailReading railsLatest() { return latest; }
bool railsOk()            { return ok; }
bool railsCalibrated()    { return caliVcore != nullptr && caliVtt != nullptr; }
