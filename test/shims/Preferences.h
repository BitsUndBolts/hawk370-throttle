// In-memory stand-in for the ESP32 Preferences (NVS) API, host tests only.
#pragma once
#include "Arduino.h"
#include <map>
#include <vector>
struct NvsStore {
  std::map<std::string, std::vector<uint8_t>> blobs;
  size_t capacity = 20000;   // bytes a blob may use; lower it to simulate a full NVS
  int writes = 0;
};
extern NvsStore nvsStore;
class Preferences {
  std::string ns;
 public:
  bool begin(const char* name, bool = false) { ns = name; return true; }
  void end() {}
  bool isKey(const char* k) { return nvsStore.blobs.count(ns + "/" + k) > 0; }
  size_t getBytesLength(const char* k) { return isKey(k) ? nvsStore.blobs[ns + "/" + k].size() : 0; }
  size_t getBytes(const char* k, void* buf, size_t max) {
    auto& b = nvsStore.blobs[ns + "/" + k]; size_t n = b.size() < max ? b.size() : max;
    memcpy(buf, b.data(), n); return n; }
  size_t putBytes(const char* k, const void* v, size_t len) {
    if (len > nvsStore.capacity) return 0;
    nvsStore.blobs[ns + "/" + k].assign((const uint8_t*) v, (const uint8_t*) v + len);
    nvsStore.writes++; return len; }
};
