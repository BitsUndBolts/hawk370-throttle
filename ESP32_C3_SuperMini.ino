/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 Wireless ThrottleBlaster — ESP32-C3 SuperMini Firmware 0.4
//
// Wireless Socket 370 CPU throttle controller (Mendocino / Coppermine /
// Tualatin / VIA C3) for the HAWK 370 Slotket. Web server, Wi-Fi provisioning,
// mDNS and OTA are adapted from the author's AirMeter project.
//
// Architecture:
//   Browser  <-- SSE (telemetry, throttle) / REST -->  ESP32-C3
//   ESP32-C3 --> RMT loop / GPIO --> 2N7002 --> STPCLK# (Socket 370)
//   ESP32-C3 <-- ADC1 (GPIO0 = VCORE, GPIO1 = VTT, 10k/10k dividers)
//
// Modules:
//   hawk_config.h     pins, polarity, network identity
//   hawk_pattern.*    STPCLK# timing maths (pure, host-tested: test/)
//   hawk_throttle.*   RMT output, glitch-free switching, Pause, CPU families
//   hawk_presets.*    user speed presets per CPU family (LittleFS, host-tested)
//   hawk_rails.*      VCORE / VTT monitoring
//   hawk_net.*        Wi-Fi modes, setup wizard backend, background reconnect
//   hawk_web.*        REST API, SSE, OTA (firmware + LittleFS image), files
//
// Threading: web handlers run on the AsyncTCP task and only post requests.
// Every pin, RMT, radio-mode and NVS change happens here in loop().
//
// Board: "ESP32C3 Dev Module", USB CDC On Boot: Enabled. The partition table
// comes from partitions.csv in this folder (same layout as the core default).
// The CPU runs at 160 MHz whatever the IDE menu says (see setup()).
// =============================================================================

#include <LittleFS.h>
#include "esp_bt.h"

#include "hawk_config.h"
#include "hawk_throttle.h"
#include "hawk_rails.h"
#include "hawk_net.h"
#include "hawk_web.h"

void setup() {
  // Before anything else: STPCLK# released. (The 10k gate pull-down on the PCB
  // already holds the FET off through reset; this makes the level explicit.)
  throttleEarlyPinInit();

  // 160 MHz regardless of the IDE's CPU-frequency menu. The STPCLK# timing does
  // not depend on it (the RMT counts the fixed 80 MHz APB clock), but the web
  // server and the edge-synchronised pattern switch get twice the headroom for
  // a few mA more than 80 MHz.
  setCpuFrequencyMhz(160);

  Serial.begin(115200);
  Serial.printf("\n--- HAWK 370 ThrottleBlaster %s booting ---\n", HAWK_FIRMWARE_VERSION);

  // Bluetooth is unused: hand its RAM back to the heap.
  esp_bt_mem_release(ESP_BT_MODE_BLE);

  throttleBegin();   // always starts at full speed
  railsBegin();

  const bool littlefsOk = LittleFS.begin(true);
  Serial.println(littlefsOk ? "[STORAGE] LittleFS mounted." : "[CRITICAL] LittleFS mount failed!");

  netBegin();        // never blocks: station mode joins in the background
  webBegin(littlefsOk);
}

void loop() {
  throttleTick();
  netTick();
  railsTick();
  webTick();
  delay(1);
}
