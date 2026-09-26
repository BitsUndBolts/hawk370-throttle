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
// Storage: one small JSON file per family in LittleFS (/presets/<slug>.json),
// written atomically. NVS would only hold a couple of hundred presets in total.
// Flashing a storage image replaces LittleFS; the Files page backs presets up
// and restores them around such a flash.
//
// All calls happen on the AsyncTCP task (web handlers); a mutex guards anyway.
// =============================================================================

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

static const size_t PRESETS_MAX_PER_FAMILY = 128;
static const size_t PRESET_NAME_MAX_BYTES  = 40;    // UTF-8 bytes
static const uint16_t PRESET_MHZ_MAX       = 9999;

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
  uint16_t skipped = 0;   // invalid entries, or the family is full
};

bool presetsBegin(bool littlefsOk);

// Fills `out` with {name, mhz, refBaseMhz} objects, sorted by name (A-Z).
bool presetsList(uint8_t family, JsonArray out);

// Creates, or with originalName set, edits (and possibly renames) a preset.
PresetResult presetsSave(uint8_t family, const String& name, uint16_t mhz, uint16_t refBaseMhz,
                         const String& originalName);
PresetResult presetsDelete(uint8_t family, const String& name);

// Merge: same name (case-insensitive) is updated, new names are added.
PresetResult presetsImport(uint8_t family, JsonArrayConst items, PresetImportStats& stats);

const char* presetResultText(PresetResult r);

// Bumps on every change; presetsLastFamily() says which family changed.
uint32_t presetsRevision();
uint8_t  presetsLastFamily();
