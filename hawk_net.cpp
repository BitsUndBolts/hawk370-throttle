/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

#include "hawk_net.h"
#include "hawk_config.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>

static const uint32_t  STA_FALLBACK_AFTER_MS = 20000;   // bring up the AP if not joined by then
static const uint32_t  STA_RETRY_EVERY_MS    = 30000;
static const uint32_t  CRED_TEST_TIMEOUT_MS  = 15000;
static const uint32_t  CRED_TEST_GRACE_MS    = 3000;    // ignore early "no SSID" while scanning
static const byte      DNS_PORT              = 53;
static const IPAddress AP_IP(192, 168, 8, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

static const char* NVS_WIFI     = "wifi-config";
static const char* NVS_THROTTLE = "throttle";           // v0.1 kept the power-save flag here

static DNSServer     dnsServer;
static OperationMode mode          = MODE_UNCONFIGURED;
static bool          apUp          = false;
static String        staSsid, staPass;
static bool          staWasConnected = false;
static uint32_t      staDownSince  = 0;
static uint32_t      staLastRetry  = 0;
static volatile bool psOn          = true;              // true = WIFI_PS_MAX_MODEM

// Credential test (setup wizard). The web task posts, loop() executes.
static portMUX_TYPE           credMux = portMUX_INITIALIZER_UNLOCKED;
static char                   testSsid[33];
static char                   testPass[65];
static bool                   testRequested = false;
static bool                   testRunning   = false;
static uint32_t               testStart     = 0;
static volatile CredentialTest credState    = CRED_IDLE;

// ── Radio helpers ────────────────────────────────────────────────────────────
// The SuperMini's LDO cannot take the surge of a full-power radio start, so
// TX power is clamped BEFORE softAP()/begin() (carried over from AirMeter).

static void startAp(bool keepStation) {
  WiFi.mode(keepStation ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.softAP(HAWK_AP_SSID, HAWK_AP_PASSWORD);
  dnsServer.start(DNS_PORT, "*", AP_IP);
  apUp = true;
  Serial.printf("[NETWORK] Access point '%s' up at %s\n", HAWK_AP_SSID, AP_IP.toString().c_str());
}

static void stopAp() {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  apUp = false;
  Serial.println("[NETWORK] Access point stopped (station connected)");
}

static void applyPowerSaving() {
  if (mode == MODE_STA && !apUp) {
    esp_wifi_set_ps(psOn ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
  }
}

static void startMdns() {
  if (MDNS.begin(HAWK_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[DNS] mDNS active: http://%s.local\n", HAWK_HOSTNAME);
  } else {
    Serial.println("[DNS] mDNS failed to start");
  }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void netBegin() {
  Preferences prefs;
  prefs.begin(NVS_THROTTLE, true);
  psOn = !prefs.getBool("wifiPsNone", false);
  prefs.end();

  prefs.begin(NVS_WIFI, true);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  uint8_t stored = prefs.getUChar("mode", MODE_UNCONFIGURED);
  prefs.end();

  if (stored == MODE_STA && staSsid.isEmpty()) stored = MODE_UNCONFIGURED;
  if (stored > MODE_STA) stored = MODE_UNCONFIGURED;
  mode = (OperationMode) stored;

  WiFi.persistent(false);   // credentials live in our own NVS namespace
  WiFi.setAutoReconnect(true);

  switch (mode) {
    case MODE_STA:
      Serial.printf("[NETWORK] Joining '%s' in the background\n", staSsid.c_str());
      WiFi.mode(WIFI_STA);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
      esp_wifi_set_ps(WIFI_PS_NONE);          // reliable join; preference applied once up
      WiFi.begin(staSsid.c_str(), staPass.c_str());
      staDownSince = staLastRetry = millis();
      break;

    case MODE_AP:
      Serial.println("[NETWORK] Standalone access point mode");
      startAp(false);
      break;

    case MODE_UNCONFIGURED:
    default:
      Serial.println("[NETWORK] Not configured, starting setup access point");
      startAp(false);
      break;
  }

  startMdns();
}

static void tickCredentialTest() {
  bool start = false;
  char ssid[33], pass[65];
  portENTER_CRITICAL(&credMux);
  if (testRequested) {
    testRequested = false;
    start = true;
    memcpy(ssid, testSsid, sizeof(ssid));
    memcpy(pass, testPass, sizeof(pass));
  }
  portEXIT_CRITICAL(&credMux);

  if (start) {
    Serial.printf("[NETWORK] Testing credentials for '%s'\n", ssid);
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.begin(ssid, pass[0] ? pass : nullptr);   // empty password = open network
    testStart   = millis();
    testRunning = true;
    return;
  }

  if (!testRunning) return;

  const wl_status_t st      = WiFi.status();
  const uint32_t    elapsed = millis() - testStart;

  if (st == WL_CONNECTED) {
    testRunning = false;
    portENTER_CRITICAL(&credMux);
    memcpy(ssid, testSsid, sizeof(ssid));
    memcpy(pass, testPass, sizeof(pass));
    portEXIT_CRITICAL(&credMux);

    Preferences prefs;
    prefs.begin(NVS_WIFI, false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putUChar("mode", MODE_STA);
    prefs.end();
    credState = CRED_SUCCESS;                   // the web layer reboots into station mode
    Serial.printf("[NETWORK] Joined '%s' — saved, rebooting into station mode\n", ssid);
    return;
  }

  const bool hardFail = (st == WL_CONNECT_FAILED) ||
                        (st == WL_NO_SSID_AVAIL && elapsed > CRED_TEST_GRACE_MS);
  if (hardFail || elapsed > CRED_TEST_TIMEOUT_MS) {
    testRunning = false;
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    credState = CRED_FAILED;
    Serial.println("[NETWORK] Credential test failed");
  }
}

static void tickStation() {
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    staDownSince = now;
    if (!staWasConnected) {
      staWasConnected = true;
      Serial.printf("[NETWORK] Connected to '%s', IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      if (apUp) stopAp();
      applyPowerSaving();
    }
    return;
  }

  if (staWasConnected) {
    staWasConnected = false;
    Serial.println("[NETWORK] Wi-Fi lost, reconnecting in the background");
  }
  if (!apUp && now - staDownSince > STA_FALLBACK_AFTER_MS) {
    Serial.println("[NETWORK] Network not reachable, opening the access point as well");
    startAp(true);
  }
  if (now - staLastRetry > STA_RETRY_EVERY_MS) {
    staLastRetry = now;
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
}

void netTick() {
  if (apUp) dnsServer.processNextRequest();
  if (mode == MODE_UNCONFIGURED) tickCredentialTest();
  if (mode == MODE_STA) tickStation();
}

// ── Queries ──────────────────────────────────────────────────────────────────

OperationMode netMode()         { return mode; }
bool          netProvisioning() { return mode == MODE_UNCONFIGURED; }
bool          netApActive()     { return apUp; }
bool          netStaConnected() { return mode == MODE_STA && WiFi.status() == WL_CONNECTED; }

String netDisplaySsid() {
  if (netStaConnected()) return WiFi.SSID();
  if (apUp) return HAWK_AP_SSID;
  return staSsid;
}

IPAddress netDisplayIp() {
  if (netStaConnected()) return WiFi.localIP();
  if (apUp) return WiFi.softAPIP();
  return IPAddress();
}

int netRssi() {
  return netStaConnected() ? WiFi.RSSI() : 0;
}

// ── Setup wizard ─────────────────────────────────────────────────────────────

void netStartCredentialTest(const String& ssid, const String& pass) {
  portENTER_CRITICAL(&credMux);
  strlcpy(testSsid, ssid.c_str(), sizeof(testSsid));
  strlcpy(testPass, pass.c_str(), sizeof(testPass));
  testRequested = true;
  credState     = CRED_CHECKING;
  portEXIT_CRITICAL(&credMux);
}

CredentialTest netCredentialTestStatus() {
  return credState;
}

void netChooseStandaloneAp() {
  Preferences prefs;
  prefs.begin(NVS_WIFI, false);
  prefs.putUChar("mode", MODE_AP);
  prefs.end();
  mode = MODE_AP;   // the AP is already up; "/" now serves the dashboard
  Serial.println("[SETUP] Standalone access point mode selected");
}

void netForgetWifi() {
  Preferences prefs;
  prefs.begin(NVS_WIFI, false);
  prefs.clear();
  prefs.end();
  Serial.println("[RESET] Wi-Fi settings erased");
}

// ── Power saving ─────────────────────────────────────────────────────────────

void netSetPowerSaving(bool on) {
  psOn = on;
  Preferences prefs;
  prefs.begin(NVS_THROTTLE, false);
  prefs.putBool("wifiPsNone", !on);
  prefs.end();
  if (netStaConnected()) applyPowerSaving();
  Serial.printf("[Wi-Fi] Power saving %s\n", on ? "ON (lower power, ~220 ms latency)" : "OFF (<15 ms latency)");
}

bool netPowerSaving() {
  return psOn;
}
