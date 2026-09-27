/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

#include "hawk_web.h"
#include "hawk_config.h"
#include "hawk_net.h"
#include "hawk_rails.h"
#include "hawk_throttle.h"
#include "hawk_presets.h"

#include <WiFi.h>
#include <Update.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <math.h>

static AsyncWebServer   server(80);
static AsyncEventSource events("/events");

static bool     littlefsOk    = false;
static volatile bool     rebootPending = false;
static volatile uint32_t rebootAt      = 0;

static uint32_t bootId = 0;

static const uint32_t TELEMETRY_PUSH_MS = 500;
static const uint32_t SSE_PING_MS       = 25000;

uint32_t webBootId() { return bootId; }

void webRequestReboot(uint32_t delayMs) {
  rebootAt      = millis() + delayMs;
  rebootPending = true;
}

// ── Response helpers ─────────────────────────────────────────────────────────

static void sendJson(AsyncWebServerRequest* request, int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  request->send(code, "application/json", out);
}

static void sendStatus(AsyncWebServerRequest* request, int code, const char* status,
                       const char* message = nullptr) {
  JsonDocument doc;
  doc["status"] = status;
  if (message) doc["message"] = message;
  sendJson(request, code, doc);
}

static void sendFile(AsyncWebServerRequest* request, const String& path, const String& type) {
  const String gz = path + ".gz";
  AsyncWebServerResponse* r;
  if (LittleFS.exists(gz)) {
    r = request->beginResponse(LittleFS, gz, type);
    r->addHeader("Content-Encoding", "gzip");
  } else {
    r = request->beginResponse(LittleFS, path, type);
  }
  r->addHeader("Cache-Control", "no-cache");   // always revalidate after a web-file update
  request->send(r);
}

static String contentTypeFor(const String& path) {
  if (path.endsWith(".html"))  return "text/html";
  if (path.endsWith(".css"))   return "text/css";
  if (path.endsWith(".js"))    return "application/javascript";
  if (path.endsWith(".json"))  return "application/json";
  if (path.endsWith(".svg"))   return "image/svg+xml";
  if (path.endsWith(".png"))   return "image/png";
  if (path.endsWith(".webp"))  return "image/webp";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))   return "image/x-icon";
  if (path.endsWith(".woff2")) return "font/woff2";
  return "text/plain";
}

// A LittleFS path we are willing to write, rename or delete.
static bool validPath(const String& p) {
  return p.length() > 1 && p.length() < 64 && p.startsWith("/") && p.indexOf("..") < 0 &&
         !p.endsWith("/") && !p.endsWith(".tmp");
}

// Hex-encoded XOR, matching obfuscate() in setup.html. Obfuscation only.
static bool deobfuscate(const String& hex, const char* key, String& out) {
  out = "";
  if (hex.length() % 2 != 0) return false;
  const size_t keyLen = strlen(key);
  out.reserve(hex.length() / 2);
  for (size_t i = 0; i < hex.length(); i += 2) {
    char* end = nullptr;
    const String pair = hex.substring(i, i + 2);
    const long b = strtol(pair.c_str(), &end, 16);
    if (end == nullptr || *end != '\0') return false;
    out += (char) ((uint8_t) b ^ (uint8_t) key[(i / 2) % keyLen]);
  }
  return true;
}

// ── Payload builders (shared by REST and SSE) ────────────────────────────────

static void fillThrottle(JsonObject o, const ThrottleState& s) {
  const CpuFamilyInfo& fam = CPU_FAMILIES[s.family];
  o["family"]           = s.family;
  o["familyName"]       = fam.name;
  o["verified"]         = fam.benchVerified;
  o["baseMhz"]          = s.baseMhz;
  o["speedPercent"]     = s.requestedPercent;
  o["requestedPercent"] = s.requestedPercent;
  o["deliveredPercent"] = s.deliveredPercent;
  o["deliveredMhz"]     = s.deliveredPercent * s.baseMhz / 100.0f;
  o["paused"]           = s.paused;
  o["rev"]              = s.rev;
  o["bootId"]           = bootId;
  o["minPercent"]       = hawk::minModulatedPercent(fam.timing);
  o["maxPercent"]       = hawk::maxModulatedPercent(fam.timing);
}

static String throttleJson() {
  JsonDocument doc;
  fillThrottle(doc.to<JsonObject>(), throttleGetState());
  String out;
  serializeJson(doc, out);
  return out;
}

static String telemetryJson() {
  const RailReading r = railsLatest();
  JsonDocument doc;
  if (isnan(r.vtt))   doc["vtt"]   = nullptr; else doc["vtt"]   = r.vtt;
  if (isnan(r.vcore)) doc["vcore"] = nullptr; else doc["vcore"] = r.vcore;
  doc["temp"]   = temperatureRead();
  doc["rssi"]   = netRssi();
  doc["mem"]    = (unsigned long) ESP.getFreeHeap();
  doc["wifiPs"] = netPowerSaving();
  doc["ts"]     = millis();
  String out;
  serializeJson(doc, out);
  return out;
}

// ── Uploads ──────────────────────────────────────────────────────────────────

struct UploadContext {
  bool   started = false;
  bool   error   = false;
  String message;
  File   file;
  String finalPath;
  String tmpPath;
  bool   filesystemImage = false;
  bool   keepRunning     = false;   // storage image with ?reboot=0: more uploads follow
};

static UploadContext otaCtx;
static UploadContext fileCtx;

static void uploadFail(UploadContext& ctx, const String& msg) {
  ctx.error   = true;
  ctx.message = msg;
  Serial.println("[OTA] " + msg);
}

// Firmware (.bin) or, with ?target=fs, a LittleFS image built by mklittlefs.
// A storage image with ?reboot=0 is mounted right away instead of restarting,
// so the Files page can flash the web pages and then the firmware in one run
// (one restart at the end).
static void handleFirmwareUpload(AsyncWebServerRequest* request, const String& filename,
                                 size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    otaCtx = UploadContext{};
    otaCtx.started = true;
    otaCtx.filesystemImage = request->hasParam("target") && request->getParam("target")->value() == "fs";
    otaCtx.keepRunning     = otaCtx.filesystemImage &&
                             request->hasParam("reboot") && request->getParam("reboot")->value() == "0";
    Serial.printf("[OTA] %s upload started: %s\n", otaCtx.filesystemImage ? "Filesystem" : "Firmware",
                  filename.c_str());
    if (otaCtx.filesystemImage) {
      LittleFS.end();                 // never write under a mounted filesystem
      littlefsOk = false;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, otaCtx.filesystemImage ? U_SPIFFS : U_FLASH)) {
      uploadFail(otaCtx, String("Update.begin() failed: ") + Update.errorString());
      return;
    }
  }
  if (otaCtx.error) return;

  if (len && Update.write(data, len) != len) {
    uploadFail(otaCtx, String("Write error: ") + Update.errorString());
    Update.abort();
    return;
  }

  if (final) {
    if (!Update.end(true)) {
      uploadFail(otaCtx, String("Verify error: ") + Update.errorString());
    } else {
      Serial.printf("[OTA] %u bytes written and verified\n", (unsigned) (index + len));
    }
  }
}

static void finishFirmwareUpload(AsyncWebServerRequest* request) {
  if (!otaCtx.started) {
    sendStatus(request, 400, "error", "No file received");
    return;
  }
  if (otaCtx.error || Update.hasError()) {
    const String msg = otaCtx.message.length() ? otaCtx.message : String(Update.errorString());
    sendStatus(request, 500, "error", msg.c_str());
    if (otaCtx.filesystemImage) webRequestReboot(1500);   // storage was unmounted
    return;
  }
  if (otaCtx.keepRunning) {
    // More uploads follow (usually the firmware): mount the new web pages
    // and stay up; the firmware flash restarts the device at the end.
    littlefsOk = LittleFS.begin(false);
    if (!littlefsOk) {
      sendStatus(request, 500, "error", "Storage image written, but it cannot be mounted");
      webRequestReboot(1500);
      return;
    }
    Serial.println("[OTA] Storage image mounted, waiting for the next upload");
    sendStatus(request, 200, "success");
    return;
  }
  sendStatus(request, 200, "success");
  webRequestReboot(1000);   // let the response reach the browser first
}

// Regular file into LittleFS, written to <name>.tmp and renamed when complete.
static void handleFileUpload(AsyncWebServerRequest* request, const String& filename,
                             size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    fileCtx = UploadContext{};
    fileCtx.started = true;

    String target = "/" + filename;
    if (request->hasParam("path", true)) {
      target = request->getParam("path", true)->value();
      if (!target.startsWith("/")) target = "/" + target;
    }
    if (!littlefsOk) { uploadFail(fileCtx, "Storage unavailable"); return; }
    if (!validPath(target)) { uploadFail(fileCtx, "Invalid file name"); return; }

    size_t replaced = 0;
    if (LittleFS.exists(target)) {
      File old = LittleFS.open(target, "r");
      replaced = old ? old.size() : 0;
      old.close();
    }
    const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes() + replaced;
    if (request->contentLength() > freeBytes) {
      uploadFail(fileCtx, "Insufficient storage space");
      return;
    }

    fileCtx.finalPath = target;
    fileCtx.tmpPath   = target + ".tmp";
    fileCtx.file      = LittleFS.open(fileCtx.tmpPath, "w");
    if (!fileCtx.file) { uploadFail(fileCtx, "Cannot open " + fileCtx.tmpPath); return; }
    Serial.printf("[FS] Upload started: %s\n", target.c_str());
  }
  if (fileCtx.error) return;

  if (len && fileCtx.file.write(data, len) != len) {
    fileCtx.file.close();
    uploadFail(fileCtx, "Write error (storage full?)");
    return;
  }

  if (final) {
    fileCtx.file.close();
    if (LittleFS.exists(fileCtx.finalPath) && !LittleFS.remove(fileCtx.finalPath)) {
      uploadFail(fileCtx, "Cannot replace " + fileCtx.finalPath);
      return;
    }
    if (!LittleFS.rename(fileCtx.tmpPath, fileCtx.finalPath)) {
      uploadFail(fileCtx, "Cannot rename upload into place");
      return;
    }
    Serial.printf("[FS] Written: %s (%u bytes)\n", fileCtx.finalPath.c_str(), (unsigned) (index + len));
  }
}

static void finishFileUpload(AsyncWebServerRequest* request) {
  if (!fileCtx.started) {
    sendStatus(request, 400, "error", "No file received");
    return;
  }
  if (fileCtx.error) {
    if (fileCtx.file) fileCtx.file.close();
    if (fileCtx.tmpPath.length() && LittleFS.exists(fileCtx.tmpPath)) LittleFS.remove(fileCtx.tmpPath);
    sendStatus(request, 500, "error", fileCtx.message.c_str());
    return;
  }
  sendStatus(request, 200, "success");
}

// Left over when a browser dropped an upload half-way.
static void removeStaleTempFiles() {
  File root = LittleFS.open("/");
  if (!root) return;
  String stale[8];
  int n = 0;
  for (File f = root.openNextFile(); f && n < 8; f = root.openNextFile()) {
    const String name = String("/") + f.name();
    if (name.endsWith(".tmp")) stale[n++] = name;
  }
  root.close();
  for (int i = 0; i < n; i++) LittleFS.remove(stale[i]);
}

// ── Route helpers ────────────────────────────────────────────────────────────

static void onJsonPost(const char* uri, ArJsonRequestHandlerFunction fn, int maxBody = 1024) {
  AsyncCallbackJsonWebHandler* h = new AsyncCallbackJsonWebHandler(uri, fn);
  h->setMethod(HTTP_POST);
  h->setMaxContentLength(maxBody);
  server.addHandler(h);
}

static bool requireProvisioning(AsyncWebServerRequest* request, bool wanted) {
  if (netProvisioning() == wanted) return true;
  sendStatus(request, 403, "error", wanted ? "Only available during setup" : "Not available during setup");
  return false;
}

// ── Routes ───────────────────────────────────────────────────────────────────

static void setupRoutes() {
  // Root: setup wizard until a Wi-Fi mode is chosen, dashboard afterwards.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    const char* page = netProvisioning() ? "/setup.html" : "/index.html";
    if (littlefsOk && (LittleFS.exists(page) || LittleFS.exists(String(page) + ".gz"))) {
      sendFile(request, page, "text/html");
    } else {
      request->send(404, "text/plain", String("Error 404: '") + page + "' missing from storage.");
    }
  });

  // ── Setup wizard ─────────────────────────────────────────────────────
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireProvisioning(request, true)) return;
    if (!request->hasParam("ssid", true) || !request->hasParam("pass", true)) {
      sendStatus(request, 400, "error", "Missing ssid or pass");
      return;
    }
    const String ssid = request->getParam("ssid", true)->value();
    String pass;
    if (ssid.isEmpty() || ssid.length() > 32 ||
        !deobfuscate(request->getParam("pass", true)->value(), HAWK_OBFUSCATION_KEY, pass) ||
        pass.length() > 64) {
      sendStatus(request, 400, "error", "Malformed credentials");
      return;
    }
    netStartCredentialTest(ssid, pass);   // empty password = open network
    sendStatus(request, 200, "checking");
  });

  server.on("/api/setup/ap-mode", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireProvisioning(request, true)) return;
    netChooseStandaloneAp();
    sendStatus(request, 200, "success");
  });

  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!requireProvisioning(request, true)) return;
    static uint32_t lastTrigger = 0;
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      sendStatus(request, 202, "scanning");
      return;
    }
    if (n >= 0) {
      JsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();
      for (int i = 0; i < n; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        JsonObject e = arr.add<JsonObject>();
        e["ssid"] = ssid;
        e["rssi"] = WiFi.RSSI(i);
        e["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      }
      WiFi.scanDelete();
      sendJson(request, 200, doc);
      return;
    }
    if (millis() - lastTrigger > 3000) {
      lastTrigger = millis();
      WiFi.scanDelete();
      WiFi.scanNetworks(true, false);
    }
    sendStatus(request, 202, "scanning");
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!requireProvisioning(request, true)) return;
    switch (netCredentialTestStatus()) {
      case CRED_SUCCESS:
        sendStatus(request, 200, "success");
        if (!rebootPending) webRequestReboot(1500);
        break;
      case CRED_FAILED:
        sendStatus(request, 200, "fail");
        break;
      default:
        sendStatus(request, 200, "still_checking");
        break;
    }
  });

  // ── Liveness probe ───────────────────────────────────────────────────
  // Cross-origin on purpose: after a Wi-Fi reset the dashboard (still loaded
  // from the old LAN address) polls 192.168.8.1 / hawk370.local to find the
  // device again, and after a restart bootId tells it the reboot happened.
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["device"]       = "hawk370";
    doc["firmware"]     = HAWK_FIRMWARE_VERSION;
    doc["bootId"]       = bootId;
    doc["uptimeMs"]     = millis();
    doc["provisioning"] = netProvisioning();
    String out;
    serializeJson(doc, out);
    AsyncWebServerResponse* r = request->beginResponse(200, "application/json", out);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Cache-Control", "no-store");
    request->send(r);
  });

  // ── System ───────────────────────────────────────────────────────────
  server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireProvisioning(request, false)) return;
    Serial.println("[RESET] Wi-Fi reset requested via Web UI");
    netForgetWifi();
    sendStatus(request, 200, "success");
    webRequestReboot(1000);
  });

  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireProvisioning(request, false)) return;
    sendStatus(request, 200, "success");
    webRequestReboot(1000);
  });

  server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["firmware"]  = HAWK_FIRMWARE_VERSION;
    doc["bootId"]    = bootId;
    doc["ipAddress"] = netDisplayIp().toString();
    doc["ssid"]      = netDisplaySsid();
    doc["rssi"]      = netRssi();
    // Power saving only applies to a station link; the UI hides the toggle otherwise.
    doc["apMode"]    = netApActive();
    doc["staConnected"] = netStaConnected();

    JsonObject esp = doc["esp"].to<JsonObject>();
    esp["memory"] = ESP.getFreeHeap();
    esp["temp"]   = temperatureRead();
    esp["wifiPs"] = netPowerSaving();
    esp["model"]  = ESP.getChipModel();
    esp["cpuMhz"] = getCpuFrequencyMhz();
    esp["cores"]  = ESP.getChipCores();
    esp["rev"]    = ESP.getChipRevision();
    esp["ver"]    = ESP.getCoreVersion();

    JsonObject hw = doc["hardware"].to<JsonObject>();
    hw["throttle"]        = throttleHardwareOk();
    hw["rails"]           = railsOk();
    hw["railsCalibrated"] = railsCalibrated();
    hw["storage"]         = littlefsOk;

    const size_t total = littlefsOk ? LittleFS.totalBytes() : 0;
    const size_t used  = littlefsOk ? LittleFS.usedBytes() : 0;
    doc["littlefs_total"] = total;
    doc["littlefs_free"]  = total - used;
    sendJson(request, 200, doc);
  });

  onJsonPost("/api/system/wifi-ps", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json["ps"].is<bool>()) {
      sendStatus(request, 400, "error", "Missing boolean 'ps'");
      return;
    }
    netSetPowerSaving(json["ps"].as<bool>());
    sendStatus(request, 200, "success");
  });

  // ── Throttle ─────────────────────────────────────────────────────────
  server.on("/api/cpu-families", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < FAMILY_COUNT; i++) {
      JsonObject e = arr.add<JsonObject>();
      e["id"]       = i;
      e["name"]     = CPU_FAMILIES[i].name;
      e["slug"]     = CPU_FAMILIES[i].slug;
      e["verified"] = CPU_FAMILIES[i].benchVerified;
      e["wMinUs"]   = CPU_FAMILIES[i].timing.wMinUs;
      e["periodUs"] = CPU_FAMILIES[i].timing.periodUs;
    }
    sendJson(request, 200, doc);
  });

  server.on("/api/throttle", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", throttleJson());
  });

  // body: { "family": int, "baseMhz": int, "speedPercent"?: number 0-100 }
  onJsonPost("/api/throttle/config", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json["family"].is<int>() || !json["baseMhz"].is<int>()) {
      sendStatus(request, 400, "error", "Expected integer family and baseMhz");
      return;
    }
    const int family  = json["family"].as<int>();
    const int baseMhz = json["baseMhz"].as<int>();
    if (family < 0 || family >= FAMILY_COUNT || baseMhz <= 0 || baseMhz > 9999) {
      sendStatus(request, 400, "error", "family or baseMhz out of range");
      return;
    }
    // Optional speedPercent (0-100): switch family, base clock and speed in one step.
    float speedPercent = -1.0f;
    if (!json["speedPercent"].isNull()) {
      if (!json["speedPercent"].is<float>()) {
        sendStatus(request, 400, "error", "speedPercent must be a number 0-100");
        return;
      }
      speedPercent = json["speedPercent"].as<float>();
      if (!(speedPercent >= 0.0f && speedPercent <= 100.0f)) {
        sendStatus(request, 400, "error", "speedPercent must be 0-100");
        return;
      }
    }
    const ThrottleState s = throttleRequestConfig((uint8_t) family, (uint32_t) baseMhz, speedPercent);
    JsonDocument doc;
    doc["status"] = "success";
    fillThrottle(doc.as<JsonObject>(), s);
    sendJson(request, 200, doc);
  });

  // body: { "speedPercent": number 0-100 }  (0 = Pause, 100 = full speed)
  onJsonPost("/api/throttle/speed", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json["speedPercent"].is<float>()) {
      sendStatus(request, 400, "error", "speedPercent must be a number 0-100");
      return;
    }
    const float requested = json["speedPercent"].as<float>();
    if (!(requested >= 0.0f && requested <= 100.0f)) {
      sendStatus(request, 400, "error", "speedPercent must be 0-100");
      return;
    }
    const ThrottleState s = throttleRequestSpeed(requested);
    JsonDocument doc;
    doc["status"] = "success";
    fillThrottle(doc.as<JsonObject>(), s);
    sendJson(request, 200, doc);
  });

  // ── Presets (per CPU family, stored as effective MHz) ────────────────
  server.on("/api/presets", HTTP_GET, [](AsyncWebServerRequest* request) {
    const int family = request->hasParam("family") ? request->getParam("family")->value().toInt() : -1;
    if (family < 0 || family >= FAMILY_COUNT) {
      sendStatus(request, 400, "error", "family out of range");
      return;
    }
    JsonDocument doc;
    doc["family"]     = family;
    doc["familyName"] = CPU_FAMILIES[family].name;
    doc["familySlug"] = CPU_FAMILIES[family].slug;
    doc["max"]        = PRESETS_MAX_TOTAL;   // shared by all families
    doc["total"]      = presetsCount();
    if (!presetsList((uint8_t) family, doc["presets"].to<JsonArray>())) {
      sendStatus(request, 503, "error", presetResultText(PRESET_STORAGE_ERROR));
      return;
    }
    sendJson(request, 200, doc);
  });

  // body: { family, name, mhz, refBaseMhz, originalName? }  (originalName = edit)
  onJsonPost("/api/presets/save", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json["family"].is<int>() || !json["mhz"].is<int>() || !json["name"].is<const char*>()) {
      sendStatus(request, 400, "error", "Expected family, name and mhz");
      return;
    }
    const int family = json["family"].as<int>();
    const int mhz    = json["mhz"].as<int>();
    const int ref    = json["refBaseMhz"] | 0;
    if (family < 0 || family >= FAMILY_COUNT || mhz < 1 || mhz > PRESET_MHZ_MAX || ref < 0 || ref > PRESET_MHZ_MAX) {
      sendStatus(request, 400, "error", presetResultText(PRESET_INVALID));
      return;
    }
    const PresetResult r = presetsSave((uint8_t) family, json["name"].as<String>(), (uint16_t) mhz,
                                       (uint16_t) ref, json["originalName"] | "");
    const int code = r == PRESET_OK ? 200 : r == PRESET_NAME_TAKEN ? 409 : r == PRESET_NOT_FOUND ? 404 :
                     r == PRESET_FULL ? 507 : r == PRESET_INVALID ? 400 : 500;
    sendStatus(request, code, r == PRESET_OK ? "success" : "error", r == PRESET_OK ? nullptr : presetResultText(r));
  });

  // body: { family, name }
  onJsonPost("/api/presets/delete", [](AsyncWebServerRequest* request, JsonVariant& json) {
    const int family = json["family"] | -1;
    if (family < 0 || family >= FAMILY_COUNT || !json["name"].is<const char*>()) {
      sendStatus(request, 400, "error", "Expected family and name");
      return;
    }
    const PresetResult r = presetsDelete((uint8_t) family, json["name"].as<String>());
    sendStatus(request, r == PRESET_OK ? 200 : r == PRESET_NOT_FOUND ? 404 : 500,
               r == PRESET_OK ? "success" : "error", r == PRESET_OK ? nullptr : presetResultText(r));
  });

  // body: { family, presets: [{name, mhz, refBaseMhz}] } — merge by name.
  // The browser splits an export file by family and sends one call each.
  onJsonPost("/api/presets/import", [](AsyncWebServerRequest* request, JsonVariant& json) {
    const int family = json["family"] | -1;
    if (family < 0 || family >= FAMILY_COUNT || !json["presets"].is<JsonArrayConst>()) {
      sendStatus(request, 400, "error", "Expected family and a presets array");
      return;
    }
    PresetImportStats stats;
    const PresetResult r = presetsImport((uint8_t) family, json["presets"].as<JsonArrayConst>(), stats);
    if (r != PRESET_OK) {
      sendStatus(request, 500, "error", presetResultText(r));
      return;
    }
    JsonDocument doc;
    doc["status"]  = "success";
    doc["added"]   = stats.added;
    doc["updated"] = stats.updated;
    doc["skipped"] = stats.skipped;
    sendJson(request, 200, doc);
  }, 32768);

  server.on("/api/telemetry", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", telemetryJson());
  });

  // ── Files ────────────────────────────────────────────────────────────
  server.on("/api/files/all", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    if (littlefsOk) {
      File root = LittleFS.open("/");
      for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (f.isDirectory()) continue;
        JsonObject e = arr.add<JsonObject>();
        e["name"] = String(f.name());
        e["size"] = f.size();
      }
    }
    sendJson(request, 200, doc);
  });

  onJsonPost("/api/files/delete", [](AsyncWebServerRequest* request, JsonVariant& json) {
    const String path = json["path"] | "";
    if (!littlefsOk)          { sendStatus(request, 503, "error", "Storage unavailable"); return; }
    if (!validPath(path))     { sendStatus(request, 400, "error", "Invalid path"); return; }
    if (!LittleFS.exists(path)) { sendStatus(request, 404, "error", "File not found"); return; }
    if (!LittleFS.remove(path)) { sendStatus(request, 500, "error", "Delete failed"); return; }
    Serial.printf("[FS] Deleted: %s\n", path.c_str());
    sendStatus(request, 200, "success");
  });

  onJsonPost("/api/files/rename", [](AsyncWebServerRequest* request, JsonVariant& json) {
    const String from = json["from"] | "";
    const String to   = json["to"] | "";
    if (!littlefsOk)                    { sendStatus(request, 503, "error", "Storage unavailable"); return; }
    if (!validPath(from) || !validPath(to)) { sendStatus(request, 400, "error", "Invalid path"); return; }
    if (from == to)                     { sendStatus(request, 200, "success"); return; }
    if (!LittleFS.exists(from))         { sendStatus(request, 404, "error", "File not found"); return; }
    if (LittleFS.exists(to))            { sendStatus(request, 409, "error", "Target name already exists"); return; }
    if (!LittleFS.rename(from, to))     { sendStatus(request, 500, "error", "Rename failed"); return; }
    Serial.printf("[FS] Renamed: %s -> %s\n", from.c_str(), to.c_str());
    sendStatus(request, 200, "success");
  });

  server.on("/ota/file", HTTP_POST, finishFileUpload, handleFileUpload);
  server.on("/ota/firmware", HTTP_POST, finishFirmwareUpload, handleFirmwareUpload);

  // ── Static assets ────────────────────────────────────────────────────
  if (littlefsOk) {
    server.serveStatic("/", LittleFS, "/").setCacheControl("no-cache");
  }

  // ── SSE ──────────────────────────────────────────────────────────────
  events.onConnect([](AsyncEventSourceClient* client) {
    client->send("Connected to HAWK 370 Throttle Stream", nullptr, millis(), 10000);
    client->send(throttleJson().c_str(), "throttle", millis());
  });
  server.addHandler(&events);

  // ── Fallback: extension-less pages ("/files" -> "/files.html") ────────
  server.onNotFound([](AsyncWebServerRequest* request) {
    String path = request->url();
    if (path.startsWith("/api/") || path.startsWith("/ota/") || path == "/save" || path == "/events" ||
        !littlefsOk) {
      request->send(404, "text/plain", "404: Not Found");
      return;
    }
    if (path.indexOf('.') < 0) path += ".html";
    if (LittleFS.exists(path) || LittleFS.exists(path + ".gz")) {
      sendFile(request, path, contentTypeFor(path));
      return;
    }
    request->send(404, "text/plain", "Error 404: '" + path + "' not found on this device.");
  });
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void webBegin(bool littlefsMounted) {
  littlefsOk = littlefsMounted;
  bootId     = esp_random() | 1;   // never 0
  if (littlefsOk) removeStaleTempFiles();
  presetsBegin();
  setupRoutes();
  server.begin();
  Serial.println("[SYSTEM] Web server started.");
}

void webTick() {
  const uint32_t now = millis();

  if (rebootPending && (int32_t) (now - rebootAt) >= 0) {
    Serial.println("[SYSTEM] Rebooting...");
    delay(50);
    ESP.restart();
  }

  if (events.count() == 0) return;

  static uint32_t lastThrottleVersion = 0;
  const uint32_t v = throttleStateVersion();
  if (v != lastThrottleVersion) {
    lastThrottleVersion = v;
    events.send(throttleJson().c_str(), "throttle", now);
  }

  // Another browser changed presets: tell everyone which family to reload.
  static uint32_t lastPresetsRev = 0;
  const uint32_t pr = presetsRevision();
  if (pr != lastPresetsRev) {
    lastPresetsRev = pr;
    char msg[32];
    snprintf(msg, sizeof(msg), "{\"family\":%u}", (unsigned) presetsLastFamily());
    events.send(msg, "presets", now);
  }

  static uint32_t lastTelemetry = 0;
  if (now - lastTelemetry >= TELEMETRY_PUSH_MS) {
    lastTelemetry = now;
    events.send(telemetryJson().c_str(), "telemetry", now);
  }

  static uint32_t lastPing = 0;
  if (now - lastPing >= SSE_PING_MS) {
    lastPing = now;
    events.send("", "ping", now);
  }
}
