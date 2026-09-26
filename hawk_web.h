/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — web server: dashboard, REST API, SSE stream, OTA and file manager
// =============================================================================

#pragma once

#include <Arduino.h>

void webBegin(bool littlefsMounted);
void webTick();                          // SSE pushes and scheduled reboots, from loop()
void webRequestReboot(uint32_t delayMs);
uint32_t webBootId();                    // random per boot, lets browsers spot a restart
