/*
 * Host-side test for the preset store (hawk_presets.cpp) using the shims in
 * test/shims (String, LittleFS, family table). Run: sh test/run_tests.sh
 */
#include "ArduinoJsonShim.h"
#include "../hawk_presets.h"
#include "../hawk_throttle.h"

// The real table lives in hawk_throttle.cpp (hardware code); the test only needs names.
const CpuFamilyInfo CPU_FAMILIES[FAMILY_COUNT] = {
  { "Mendocino", "mendocino", hawk::DEFAULT_TIMING, false },
  { "Coppermine", "coppermine", hawk::DEFAULT_TIMING, false },
  { "Tualatin", "tualatin", hawk::DEFAULT_TIMING, false },
  { "VIA Samuel", "via-samuel", hawk::DEFAULT_TIMING, false },
  { "VIA Ezra", "via-ezra", hawk::DEFAULT_TIMING, false },
  { "VIA Nehemiah", "via-nehemiah", hawk::DEFAULT_TIMING, false },
};
#include "LittleFS.h"
#include <cassert>
SerialShim Serial; FsStore fsStore; LittleFSShim LittleFS;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static std::vector<std::string> names(uint8_t f) { JsonDocument d; JsonArray a = d.to<JsonArray>(); presetsList(f, a);
  std::vector<std::string> v; for (JsonObject o : a) v.push_back(std::string(o["name"].as<const char*>()) + "=" + std::to_string(o["mhz"].as<int>())); return v; }
int main() {
  presetsBegin(true);
  CHECK(names(2).empty());
  CHECK(presetsSave(2, "386DX-40", 25, 1400, "") == PRESET_OK);
  CHECK(presetsSave(2, "  286-12  ", 8, 1400, "") == PRESET_OK);
  CHECK(presetsSave(2, "486dx2-66", 90, 0, "") == PRESET_OK);
  CHECK(presetsSave(2, "386dx-40", 30, 0, "") == PRESET_NAME_TAKEN);
  CHECK(presetsSave(2, "", 30, 0, "") == PRESET_INVALID);
  CHECK(presetsSave(2, "x", 0, 0, "") == PRESET_INVALID);
  CHECK(presetsSave(2, "12345678901234567890123456789012345678901", 5, 0, "") == PRESET_INVALID);
  CHECK(presetsSave(2, "bad\nname", 5, 0, "") == PRESET_INVALID);
  auto n = names(2);
  CHECK(n.size() == 3 && n[0] == "286-12=8" && n[1] == "386DX-40=25" && n[2] == "486dx2-66=90");
  // edit + rename, rename onto another name, case-only rename
  CHECK(presetsSave(2, "286-16", 11, 1400, "286-12") == PRESET_OK);
  CHECK(presetsSave(2, "386DX-40", 11, 1400, "286-16") == PRESET_NAME_TAKEN);
  CHECK(presetsSave(2, "486DX2-66", 91, 0, "486dx2-66") == PRESET_OK);
  CHECK(presetsSave(2, "nope", 1, 0, "missing") == PRESET_NOT_FOUND);
  n = names(2);
  CHECK(n.size() == 3 && n[0] == "286-16=11" && n[2] == "486DX2-66=91");
  // families are separate
  CHECK(names(0).empty());
  // delete
  CHECK(presetsDelete(2, "286-16") == PRESET_OK);
  CHECK(presetsDelete(2, "286-16") == PRESET_NOT_FOUND);
  CHECK(names(2).size() == 2);
  // import merge
  JsonDocument imp; JsonArray arr = imp.to<JsonArray>();
  auto add = [&](const char* nm, int m) { JsonObject o = arr.add<JsonObject>(); o["name"] = nm; o["mhz"] = m; o["refBaseMhz"] = 1133; };
  add("386dx-40", 27); add("Doom", 60); add("", 5); add("bad", 0);
  PresetImportStats st;
  CHECK(presetsImport(2, imp.as<JsonArrayConst>(), st) == PRESET_OK);
  CHECK(st.added == 1 && st.updated == 1 && st.skipped == 2);
  n = names(2);
  CHECK(n.size() == 3 && n[0] == "386dx-40=27" && n[1] == "486DX2-66=91" && n[2] == "Doom=60");
  // capacity: fill to 128
  JsonDocument big; JsonArray ba = big.to<JsonArray>();
  for (int i = 0; i < 200; i++) { JsonObject o = ba.add<JsonObject>(); o["name"] = String("P") + String(i); o["mhz"] = 10 + i; }
  PresetImportStats s2;
  CHECK(presetsImport(1, big.as<JsonArrayConst>(), s2) == PRESET_OK);
  CHECK(s2.added == 128 && s2.skipped == 72);
  CHECK(names(1).size() == 128);
  CHECK(presetsSave(1, "one more", 5, 0, "") == PRESET_FULL);
  CHECK(presetsSave(1, "P5-renamed", 5, 0, "P5") == PRESET_OK);   // edits still allowed when full
  printf("file size for 128 presets: %zu bytes\n", fsStore.files["/presets/coppermine.json"].size());
  // corrupt file is treated as empty, not a crash
  fsStore.files["/presets/via-ezra.json"] = "{not json";
  CHECK(names(4).empty());
  // storage failure is reported
  fsStore.failRename = true;
  CHECK(presetsSave(3, "x", 5, 0, "") == PRESET_STORAGE_ERROR);
  fsStore.failRename = false;
  CHECK(presetsRevision() > 0);
  printf(fails ? "%d FAILED\n" : "all preset checks passed\n", fails);
  return fails != 0;
}
