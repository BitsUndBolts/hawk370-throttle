/*
 * Host-side test for the preset store (hawk_presets.cpp) using the shims in
 * test/shims (String, Preferences/NVS). Run: sh test/run_tests.sh
 */
#include "ArduinoJsonShim.h"
#include "../hawk_presets.h"
#include "../hawk_throttle.h"
#include "Preferences.h"

// The real table lives in hawk_throttle.cpp (hardware code); the test only needs names.
const CpuFamilyInfo CPU_FAMILIES[FAMILY_COUNT] = {
  { "Mendocino", "mendocino", hawk::DEFAULT_TIMING, false },
  { "Coppermine", "coppermine", hawk::DEFAULT_TIMING, false },
  { "Tualatin", "tualatin", hawk::DEFAULT_TIMING, false },
  { "VIA Samuel", "via-samuel", hawk::DEFAULT_TIMING, false },
  { "VIA Ezra", "via-ezra", hawk::DEFAULT_TIMING, false },
  { "VIA Nehemiah", "via-nehemiah", hawk::DEFAULT_TIMING, false },
};

SerialShim Serial; NvsStore nvsStore;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::vector<std::string> names(uint8_t f) {
  JsonDocument d; JsonArray a = d.to<JsonArray>(); presetsList(f, a);
  std::vector<std::string> v;
  for (JsonObject o : a) v.push_back(std::string(o["name"].as<const char*>()) + "=" + std::to_string(o["mhz"].as<int>()));
  return v;
}

int main() {
  // Empty store on first boot, then a few presets in two families.
  presetsBegin();
  CHECK(presetsCount() == 0);
  CHECK(presetsSave(2, "386DX-40", 25, 1400, "") == PRESET_OK);
  CHECK(presetsSave(2, "286-12", 8, 0, "") == PRESET_OK);
  CHECK(presetsSave(0, "Doom", 60, 333, "") == PRESET_OK);
  CHECK(names(2).size() == 2 && names(0).size() == 1);
  CHECK(presetsCount() == 3);

  // Survives a "reboot": reload from the NVS record only.
  presetsBegin();
  auto n = names(2);
  CHECK(n.size() == 2 && n[0] == "286-12=8" && n[1] == "386DX-40=25");

  CHECK(presetsSave(2, "486dx2-66", 90, 0, "") == PRESET_OK);
  CHECK(presetsSave(2, "386dx-40", 30, 0, "") == PRESET_NAME_TAKEN);
  CHECK(presetsSave(0, "386DX-40", 30, 0, "") == PRESET_OK);          // same name, other family
  CHECK(presetsSave(2, "", 30, 0, "") == PRESET_INVALID);
  CHECK(presetsSave(2, "x", 0, 0, "") == PRESET_INVALID);
  CHECK(presetsSave(2, "123456789012345678901234567890123", 5, 0, "") == PRESET_INVALID);   // 33 bytes
  CHECK(presetsSave(2, "bad\nname", 5, 0, "") == PRESET_INVALID);

  // edit + rename
  CHECK(presetsSave(2, "286-16", 11, 1400, "286-12") == PRESET_OK);
  CHECK(presetsSave(2, "386DX-40", 11, 1400, "286-16") == PRESET_NAME_TAKEN);
  CHECK(presetsSave(2, "486DX2-66", 91, 0, "486dx2-66") == PRESET_OK);
  CHECK(presetsSave(2, "nope", 1, 0, "missing") == PRESET_NOT_FOUND);
  n = names(2);
  CHECK(n.size() == 3 && n[0] == "286-16=11" && n[2] == "486DX2-66=91");

  // delete
  CHECK(presetsDelete(2, "286-16") == PRESET_OK);
  CHECK(presetsDelete(2, "286-16") == PRESET_NOT_FOUND);

  // import merge
  JsonDocument imp; JsonArray arr = imp.to<JsonArray>();
  auto add = [&](const char* nm, int m) { JsonObject o = arr.add<JsonObject>(); o["name"] = nm; o["mhz"] = m; o["refBaseMhz"] = 1133; };
  add("386dx-40", 27); add("Wing Commander", 60); add("", 5); add("bad", 0);
  PresetImportStats st;
  CHECK(presetsImport(2, imp.as<JsonArrayConst>(), st) == PRESET_OK);
  CHECK(st.added == 1 && st.updated == 1 && st.skipped == 2);

  // 100 slots shared by all families
  const size_t before = presetsCount();
  JsonDocument big; JsonArray ba = big.to<JsonArray>();
  for (int i = 0; i < 150; i++) { JsonObject o = ba.add<JsonObject>(); o["name"] = (std::string("Preset number ") + std::to_string(i)).c_str(); o["mhz"] = 10 + i; }
  PresetImportStats s2;
  CHECK(presetsImport(1, big.as<JsonArrayConst>(), s2) == PRESET_OK);
  CHECK(s2.added == 100 - before && s2.skipped == 150 - (100 - before));
  CHECK(presetsCount() == 100);
  CHECK(presetsSave(3, "one more", 5, 0, "") == PRESET_FULL);
  CHECK(presetsSave(1, "renamed", 5, 0, "Preset number 5") == PRESET_OK);   // edits still work when full

  // worst case record size: 100 presets with 32-byte names
  presetsBegin();
  printf("record with 100 presets: %zu bytes\n", nvsStore.blobs["presets/v1"].size());

  // a failed NVS write changes nothing
  const auto snapshot = names(1);
  nvsStore.capacity = 10;
  CHECK(presetsDelete(1, "renamed") == PRESET_STORAGE_ERROR);
  CHECK(names(1) == snapshot);
  nvsStore.capacity = 20000;

  // a corrupt record starts empty instead of crashing
  nvsStore.blobs["presets/v1"] = { 'X', 'Y', 1, 5 };
  presetsBegin();
  CHECK(presetsCount() == 0);
  CHECK(presetsRevision() > 0);

  printf(fails ? "%d FAILED\n" : "all preset checks passed\n", fails);
  return fails != 0;
}
