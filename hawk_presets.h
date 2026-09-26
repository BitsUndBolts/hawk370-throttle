/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 */

// =============================================================================
// HAWK 370 — user speed presets ("386DX-40 = 25 MHz"), per CPU family
//
// A preset stores an effective MHz, not a percentage. Within one family the
// work per clock is the same, so 25 MHz on a Tualatin 1400 (1.8% duty) and
// 25 MHz on a Tualatin 1133 (2.2% duty) give the same speed; the firmware
// just recomputes the duty. refBaseMhz records the CPU it was measured on,
// for reference only.
//
// Storage: all families together in ONE compact binary record in NVS
// (namespace "presets"), not in LittleFS. NVS is its own flash partition, so
// firmware updates, storage-image flashes and USB uploads leave it alone, and
// a Wi-Fi reset only clears the "wifi-config" namespace.
//
// Budget: the NVS partition is 20 KB and also holds the Wi-Fi driver's data
// and PHY calibration. A record is 6 bytes + the name, so 100 presets with
// 32-byte names are at most ~3.8 KB (typically ~2 KB). NVS writes a new copy
// before erasing the old one, so the record has to fit twice: 100 is safe.
//
// Presets live in RAM after boot; every change rewrites the record. A change
// that cannot be written is rolled back. All calls happen on the web task.
// =============================================================================

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

static const size_t   PRESETS_MAX_TOTAL     = 100;   // all families together
static const size_t   PRESET_NAME_MAX_BYTES = 32;    // UTF-8 bytes
static const uint16_t PRESET_MHZ_MAX        = 9999;

enum PresetResult : uint8_t {
  PRESET_OK = 0,
  PRESET_INVALID,
  PRESET_FULL,
  PRESET_NAME_TAKEN,
  PRESET_NOT_FOUND,
  PRESET_STORAGE_ERROR
};

struct PresetImportStats {
  uint16_t added   = 0;
  uint16_t updated = 0;
  uint16_t skipped = 0;   // invalid entries, or no free slots left
};

// Loads the presets from NVS. If an older firmware (0.4) left presets in
// LittleFS under /presets, they are moved into NVS once and the files removed.
bool presetsBegin(bool littlefsOk);

// Fills `out` with {name, mhz, refBaseMhz} objects of one family, A-Z.
bool presetsList(uint8_t family, JsonArray out);
size_t presetsCount();   // all families

// Creates, or with originalName set, edits (and possibly renames) a preset.
PresetResult presetsSave(uint8_t family, const String& name, uint16_t mhz, uint16_t refBaseMhz,
                         const String& originalName);
PresetResult presetsDelete(uint8_t family, const String& name);

// Merge into one family: same name (case-insensitive) is updated, new names added.
PresetResult presetsImport(uint8_t family, JsonArrayConst items, PresetImportStats& stats);

const char* presetResultText(PresetResult r);

// Bumps on every change; presetsLastFamily() says which family changed.
uint32_t presetsRevision();
uint8_t  presetsLastFamily();
