/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — Wi-Fi: provisioning, standalone AP, station mode with fallback
//
// Three persisted modes:
//   UNCONFIGURED  first boot or after a Wi-Fi reset: AP + setup wizard
//   AP            standalone access point (bench use), dashboard on 192.168.8.1
//   STA           joins the saved network. Never blocks boot: if the network
//                 is not reachable within 20 s the setup AP comes up as well,
//                 serving the dashboard, while the station keeps retrying in
//                 the background. The AP goes away once the station connects.
// =============================================================================

#pragma once

#include <Arduino.h>
#include <IPAddress.h>

enum OperationMode : uint8_t {
  MODE_UNCONFIGURED = 0,
  MODE_AP           = 1,
  MODE_STA          = 2
};

enum CredentialTest : uint8_t {
  CRED_IDLE     = 0,
  CRED_CHECKING = 1,
  CRED_SUCCESS  = 2,
  CRED_FAILED   = 3
};

void netBegin();
void netTick();

OperationMode netMode();
bool      netProvisioning();      // true -> "/" serves the setup wizard
bool      netApActive();          // our access point is up
bool      netStaConnected();
String    netDisplaySsid();       // network we are on, or our AP name
IPAddress netDisplayIp();         // station IP if connected, else AP IP
int       netRssi();              // 0 when not connected as a station

// Setup wizard (only valid while provisioning)
void           netStartCredentialTest(const String& ssid, const String& pass);
CredentialTest netCredentialTestStatus();   // on success the credentials are saved
void           netChooseStandaloneAp();

// Wi-Fi reset: forget the network and the chosen mode (reboot afterwards).
void netForgetWifi();

// Modem power saving (station mode). Persisted.
void netSetPowerSaving(bool on);
bool netPowerSaving();
