/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

#include "hawk_presets.h"
#include "hawk_throttle.h"

#include <Preferences.h>
#include <LittleFS.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <strings.h>

// ── Record format (little endian) ────────────────────────────────────────────
//   'H' 'P' version(1) count(1)
//   count x { family(1) mhz(2) refBaseMhz(2) nameLen(1) name(nameLen) }
// Families are stored by index: new families must only ever be appended to
// CPU_FAMILIES, never inserted or reordered.
static const char*   NVS_NAMESPACE = "presets";
static const char*   NVS_KEY       = "v1";
static const uint8_t FORMAT_VER    = 1;

struct Preset {
  uint8_t  family;
  String   name;
  uint16_t mhz;
  uint16_t refBaseMhz;
};

static std::vector<Preset>   presets;     // all families, in RAM
static bool                  loaded = false;
static std::mutex            presetMutex;
static std::atomic<uint32_t> revision{0};
static std::atomic<uint8_t>  lastFamily{0};

// ── Helpers ──────────────────────────────────────────────────────────────────

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

static bool validMhz(int mhz) { return mhz >= 1 && mhz <= PRESET_MHZ_MAX; }
static bool validRef(int ref) { return ref >= 0 && ref <= PRESET_MHZ_MAX; }

static int findIndex(const std::vector<Preset>& v, uint8_t family, const String& name) {
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i].family == family && strcasecmp(v[i].name.c_str(), name.c_str()) == 0) return (int) i;
  }
  return -1;
}

static bool nameLess(const Preset& a, const Preset& b) {
  const int c = strcasecmp(a.name.c_str(), b.name.c_str());
  return c != 0 ? c < 0 : strcmp(a.name.c_str(), b.name.c_str()) < 0;
}

// ── NVS record ───────────────────────────────────────────────────────────────

static std::vector<uint8_t> encode(const std::vector<Preset>& v) {
  std::vector<uint8_t> b;
  b.reserve(4 + v.size() * (6 + 16));
  b.push_back('H'); b.push_back('P'); b.push_back(FORMAT_VER); b.push_back((uint8_t) v.size());
  for (const Preset& p : v) {
    b.push_back(p.family);
    b.push_back(p.mhz & 0xFF);        b.push_back(p.mhz >> 8);
    b.push_back(p.refBaseMhz & 0xFF); b.push_back(p.refBaseMhz >> 8);
    b.push_back((uint8_t) p.name.length());
    for (size_t i = 0; i < p.name.length(); i++) b.push_back((uint8_t) p.name[i]);
  }
  return b;
}

static bool decode(const uint8_t* b, size_t len, std::vector<Preset>& out) {
  out.clear();
  if (len < 4 || b[0] != 'H' || b[1] != 'P' || b[2] != FORMAT_VER) return false;
  const uint8_t count = b[3];
  size_t pos = 4;
  for (uint8_t i = 0; i < count; i++) {
    if (pos + 6 > len) return false;
    Preset p;
    p.family     = b[pos];
    p.mhz        = (uint16_t) (b[pos + 1] | (b[pos + 2] << 8));
    p.refBaseMhz = (uint16_t) (b[pos + 3] | (b[pos + 4] << 8));
    const uint8_t n = b[pos + 5];
    pos += 6;
    if (pos + n > len) return false;
    String name;
    for (uint8_t k = 0; k < n; k++) name.concat((char) b[pos + k]);
    pos += n;
    p.name = cleanName(name);
    if (p.family < FAMILY_COUNT && !p.name.isEmpty() && validMhz(p.mhz) && out.size() < PRESETS_MAX_TOTAL) {
      out.push_back(p);
    }
  }
  return true;
}

static bool writeRecord(const std::vector<Preset>& v) {
  const std::vector<uint8_t> b = encode(v);
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  const size_t written = prefs.putBytes(NVS_KEY, b.data(), b.size());
  prefs.end();
  return written == b.size();
}

// Replaces the presets with `next` only if it could be stored.
static PresetResult commit(std::vector<Preset>& next, uint8_t family) {
  if (!writeRecord(next)) return PRESET_STORAGE_ERROR;
  presets.swap(next);
  lastFamily = family;
  revision++;
  return PRESET_OK;
}

// ── One-time move of v0.4 presets out of LittleFS ────────────────────────────

static void migrateFromLittleFs() {
  const char* dir = "/presets";
  if (!LittleFS.exists(dir)) return;

  std::vector<Preset> next = presets;
  size_t moved = 0;
  for (uint8_t f = 0; f < FAMILY_COUNT; f++) {
    const String path = String(dir) + "/" + CPU_FAMILIES[f].slug + ".json";
    if (!LittleFS.exists(path)) continue;
    File file = LittleFS.open(path, "r");
    if (!file) continue;
    JsonDocument doc;
    const bool ok = !deserializeJson(doc, file);
    file.close();
    if (!ok) continue;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
      const String name = cleanName(o["n"] | "");
      const int mhz = o["m"] | 0;
      const int ref = o["r"] | 0;
      if (name.isEmpty() || !validMhz(mhz) || !validRef(ref)) continue;
      if (findIndex(next, f, name) >= 0 || next.size() >= PRESETS_MAX_TOTAL) continue;
      next.push_back({ f, name, (uint16_t) mhz, (uint16_t) ref });
      moved++;
    }
  }

  if (moved && commit(next, 0) != PRESET_OK) {
    Serial.println("[PRESETS] Could not move LittleFS presets into NVS; leaving the files in place");
    return;
  }
  for (uint8_t f = 0; f < FAMILY_COUNT; f++) {
    const String path = String(dir) + "/" + CPU_FAMILIES[f].slug + ".json";
    if (LittleFS.exists(path)) LittleFS.remove(path);
    if (LittleFS.exists(path + ".tmp")) LittleFS.remove(path + ".tmp");
  }
  LittleFS.rmdir(dir);
  Serial.printf("[PRESETS] Moved %u preset(s) from LittleFS into NVS\n", (unsigned) moved);
}

// ── Public API ───────────────────────────────────────────────────────────────

bool presetsBegin(bool littlefsOk) {
  std::lock_guard<std::mutex> lock(presetMutex);
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  const size_t len = prefs.isKey(NVS_KEY) ? prefs.getBytesLength(NVS_KEY) : 0;
  if (len > 0) {
    std::vector<uint8_t> buf(len);
    prefs.getBytes(NVS_KEY, buf.data(), len);
    if (!decode(buf.data(), len, presets)) {
      Serial.println("[PRESETS] Stored record unreadable, starting empty");
      presets.clear();
    }
  }
  prefs.end();
  loaded = true;

  if (littlefsOk) migrateFromLittleFs();
  Serial.printf("[PRESETS] %u of %u preset slots used\n", (unsigned) presets.size(), (unsigned) PRESETS_MAX_TOTAL);
  return true;
}

bool presetsList(uint8_t family, JsonArray out) {
  if (!loaded || family >= FAMILY_COUNT) return false;
  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> v;
  for (const Preset& p : presets) if (p.family == family) v.push_back(p);
  std::sort(v.begin(), v.end(), nameLess);
  for (const Preset& p : v) {
    JsonObject o = out.add<JsonObject>();
    o["name"]       = p.name;
    o["mhz"]        = p.mhz;
    o["refBaseMhz"] = p.refBaseMhz;
  }
  return true;
}

size_t presetsCount() {
  std::lock_guard<std::mutex> lock(presetMutex);
  return presets.size();
}

PresetResult presetsSave(uint8_t family, const String& rawName, uint16_t mhz, uint16_t refBaseMhz,
                         const String& originalName) {
  if (!loaded) return PRESET_STORAGE_ERROR;
  const String name = cleanName(rawName);
  if (family >= FAMILY_COUNT || name.isEmpty() || !validMhz(mhz) || !validRef(refBaseMhz)) return PRESET_INVALID;

  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> next = presets;
  const int existing = findIndex(next, family, name);
  if (originalName.length()) {                       // edit
    const int original = findIndex(next, family, originalName);
    if (original < 0) return PRESET_NOT_FOUND;
    if (existing >= 0 && existing != original) return PRESET_NAME_TAKEN;
    next[original] = { family, name, mhz, refBaseMhz };
  } else {                                           // create
    if (existing >= 0) return PRESET_NAME_TAKEN;
    if (next.size() >= PRESETS_MAX_TOTAL) return PRESET_FULL;
    next.push_back({ family, name, mhz, refBaseMhz });
  }
  return commit(next, family);
}

PresetResult presetsDelete(uint8_t family, const String& name) {
  if (!loaded) return PRESET_STORAGE_ERROR;
  if (family >= FAMILY_COUNT) return PRESET_INVALID;

  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> next = presets;
  const int i = findIndex(next, family, name);
  if (i < 0) return PRESET_NOT_FOUND;
  next.erase(next.begin() + i);
  return commit(next, family);
}

PresetResult presetsImport(uint8_t family, JsonArrayConst items, PresetImportStats& stats) {
  if (!loaded) return PRESET_STORAGE_ERROR;
  if (family >= FAMILY_COUNT) return PRESET_INVALID;

  std::lock_guard<std::mutex> lock(presetMutex);
  std::vector<Preset> next = presets;
  PresetImportStats s;
  for (JsonObjectConst o : items) {
    const String name = cleanName(o["name"] | "");
    const int mhz = o["mhz"] | 0;
    const int ref = o["refBaseMhz"] | 0;
    if (name.isEmpty() || !validMhz(mhz) || !validRef(ref)) { s.skipped++; continue; }
    const int i = findIndex(next, family, name);
    if (i >= 0) {
      next[i] = { family, name, (uint16_t) mhz, (uint16_t) ref };
      s.updated++;
    } else if (next.size() < PRESETS_MAX_TOTAL) {
      next.push_back({ family, name, (uint16_t) mhz, (uint16_t) ref });
      s.added++;
    } else {
      s.skipped++;
    }
  }
  if (s.added || s.updated) {
    const PresetResult r = commit(next, family);
    if (r != PRESET_OK) return r;
  }
  stats.added += s.added;
  stats.updated += s.updated;
  stats.skipped += s.skipped;
  return PRESET_OK;
}

const char* presetResultText(PresetResult r) {
  switch (r) {
    case PRESET_OK:            return "OK";
    case PRESET_INVALID:       return "Name must be 1-32 characters and MHz 1-9999";
    case PRESET_FULL:          return "All 100 preset slots are used - delete or export some first";
    case PRESET_NAME_TAKEN:    return "A preset with this name already exists";
    case PRESET_NOT_FOUND:     return "Preset not found";
    case PRESET_STORAGE_ERROR: return "Could not save to the settings memory";
  }
  return "Unknown error";
}

uint32_t presetsRevision()   { return revision.load(); }
uint8_t  presetsLastFamily() { return lastFamily.load(); }
