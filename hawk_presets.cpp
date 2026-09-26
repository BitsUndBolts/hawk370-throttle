/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

#include "hawk_presets.h"
#include "hawk_throttle.h"

#include <LittleFS.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <strings.h>

static const char* PRESET_DIR = "/presets";

struct Preset {
  String   name;
  uint16_t mhz;
  uint16_t refBaseMhz;
};

static bool                  storageOk = false;
static std::mutex            presetMutex;
static std::atomic<uint32_t> revision{0};
static std::atomic<uint8_t>  lastFamily{0};

static String pathFor(uint8_t family) {
  return String(PRESET_DIR) + "/" + CPU_FAMILIES[family].slug + ".json";
}

// Trims, then checks length and control characters. Empty result = invalid.
static String cleanName(const String& raw) {
  String n = raw;
  n.trim();
  if (n.isEmpty() || n.length() > PRESET_NAME_MAX_BYTES) return String();
  for (size_t i = 0; i < n.length(); i++) {
    if ((uint8_t) n[i] < 0x20 || n[i] == 0x7F) return String();
  }
  return n;
}

static bool sameName(const String& a, const String& b) {
  return strcasecmp(a.c_str(), b.c_str()) == 0;
}

static void sortPresets(std::vector<Preset>& v) {
  std::sort(v.begin(), v.end(), [](const Preset& a, const Preset& b) {
    const int c = strcasecmp(a.name.c_str(), b.name.c_str());
    return c != 0 ? c < 0 : strcmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

static int findIndex(const std::vector<Preset>& v, const String& name) {
  for (size_t i = 0; i < v.size(); i++) {
    if (sameName(v[i].name, name)) return (int) i;
  }
  return -1;
}

static bool load(uint8_t family, std::vector<Preset>& out) {
  out.clear();
  const String path = pathFor(family);
  if (!LittleFS.exists(path)) return true;   // no presets yet

  File f = LittleFS.open(path, "r");
  if (!f) return false;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[PRESETS] %s unreadable (%s), treating as empty\n", path.c_str(), err.c_str());
    return true;
  }
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    const String name = cleanName(o["n"] | "");
    const int mhz = o["m"] | 0;
    if (name.isEmpty() || mhz < 1 || mhz > PRESET_MHZ_MAX || out.size() >= PRESETS_MAX_PER_FAMILY) continue;
    out.push_back({ name, (uint16_t) mhz, (uint16_t) (o["r"] | 0) });
  }
  sortPresets(out);
  return true;
}

static bool store(uint8_t family, std::vector<Preset>& v) {
  sortPresets(v);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const Preset& p : v) {
    JsonObject o = arr.add<JsonObject>();
    o["n"] = p.name;
    o["m"] = p.mhz;
    if (p.refBaseMhz) o["r"] = p.refBaseMhz;
  }

  const String path = pathFor(family);
  const String tmp  = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t written = serializeJson(doc, f);
  f.close();
  if (written == 0) { LittleFS.remove(tmp); return false; }
  if (LittleFS.exists(path) && !LittleFS.remove(path)) return false;
  if (!LittleFS.rename(tmp, path)) return false;

  lastFamily = family;
  revision++;
  return true;
}

// ── Public API ───────────────────────────────────────────────────────────────

bool presetsBegin(bool littlefsOk) {
  storageOk = littlefsOk;
  if (!storageOk) return false;
  if (!LittleFS.exists(PRESET_DIR)) LittleFS.mkdir(PRESET_DIR);
  for (uint8_t f = 0; f < FAMILY_COUNT; f++) {       // drop half-written files
    const String tmp = pathFor(f) + ".tmp";
    if (LittleFS.exists(tmp)) LittleFS.remove(tmp);
  }
  return true;
}

bool presetsList(uint8_t family, JsonArray out) {
  if (!storageOk || family >= FAMILY_COUNT) return false;
  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> v;
  if (!load(family, v)) return false;
  for (const Preset& p : v) {
    JsonObject o = out.add<JsonObject>();
    o["name"]       = p.name;
    o["mhz"]        = p.mhz;
    o["refBaseMhz"] = p.refBaseMhz;
  }
  return true;
}

PresetResult presetsSave(uint8_t family, const String& rawName, uint16_t mhz, uint16_t refBaseMhz,
                         const String& originalName) {
  if (!storageOk) return PRESET_STORAGE_ERROR;
  const String name = cleanName(rawName);
  if (family >= FAMILY_COUNT || name.isEmpty() || mhz < 1 || mhz > PRESET_MHZ_MAX ||
      refBaseMhz > PRESET_MHZ_MAX) {
    return PRESET_INVALID;
  }

  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> v;
  if (!load(family, v)) return PRESET_STORAGE_ERROR;

  const int existing = findIndex(v, name);
  if (originalName.length()) {                       // edit
    const int original = findIndex(v, originalName);
    if (original < 0) return PRESET_NOT_FOUND;
    if (existing >= 0 && existing != original) return PRESET_NAME_TAKEN;
    v[original] = { name, mhz, refBaseMhz };
  } else {                                           // create
    if (existing >= 0) return PRESET_NAME_TAKEN;
    if (v.size() >= PRESETS_MAX_PER_FAMILY) return PRESET_FULL;
    v.push_back({ name, mhz, refBaseMhz });
  }
  return store(family, v) ? PRESET_OK : PRESET_STORAGE_ERROR;
}

PresetResult presetsDelete(uint8_t family, const String& name) {
  if (!storageOk) return PRESET_STORAGE_ERROR;
  if (family >= FAMILY_COUNT) return PRESET_INVALID;

  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> v;
  if (!load(family, v)) return PRESET_STORAGE_ERROR;
  const int i = findIndex(v, name);
  if (i < 0) return PRESET_NOT_FOUND;
  v.erase(v.begin() + i);
  return store(family, v) ? PRESET_OK : PRESET_STORAGE_ERROR;
}

PresetResult presetsImport(uint8_t family, JsonArrayConst items, PresetImportStats& stats) {
  if (!storageOk) return PRESET_STORAGE_ERROR;
  if (family >= FAMILY_COUNT) return PRESET_INVALID;

  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> v;
  if (!load(family, v)) return PRESET_STORAGE_ERROR;

  for (JsonObjectConst o : items) {
    const String name = cleanName(o["name"] | "");
    const int mhz = o["mhz"] | 0;
    const int ref = o["refBaseMhz"] | 0;
    if (name.isEmpty() || mhz < 1 || mhz > PRESET_MHZ_MAX || ref < 0 || ref > PRESET_MHZ_MAX) {
      stats.skipped++;
      continue;
    }
    const int i = findIndex(v, name);
    if (i >= 0) {
      v[i] = { name, (uint16_t) mhz, (uint16_t) ref };
      stats.updated++;
    } else if (v.size() < PRESETS_MAX_PER_FAMILY) {
      v.push_back({ name, (uint16_t) mhz, (uint16_t) ref });
      stats.added++;
    } else {
      stats.skipped++;
    }
  }
  if (stats.added == 0 && stats.updated == 0) return PRESET_OK;
  return store(family, v) ? PRESET_OK : PRESET_STORAGE_ERROR;
}

const char* presetResultText(PresetResult r) {
  switch (r) {
    case PRESET_OK:            return "OK";
    case PRESET_INVALID:       return "Name must be 1-40 characters and MHz 1-9999";
    case PRESET_FULL:          return "This family already has 128 presets";
    case PRESET_NAME_TAKEN:    return "A preset with this name already exists";
    case PRESET_NOT_FOUND:     return "Preset not found";
    case PRESET_STORAGE_ERROR: return "Could not read or write preset storage";
  }
  return "Unknown error";
}

uint32_t presetsRevision()   { return revision.load(); }
uint8_t  presetsLastFamily() { return lastFamily.load(); }
