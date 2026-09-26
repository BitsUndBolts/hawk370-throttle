#pragma once
#include "Arduino.h"
#include <map>
struct FsStore { std::map<std::string, std::string> files; bool failRename = false; };
extern FsStore fsStore;
class File {
 public:
  std::string path; size_t pos = 0; bool writing = false; bool valid = false;
  explicit operator bool() const { return valid; }
  int read() { auto& d = fsStore.files[path]; return pos < d.size() ? (uint8_t) d[pos++] : -1; }
  size_t readBytes(char* b, size_t n) { size_t k = 0; int c; while (k < n && (c = read()) >= 0) b[k++] = (char) c; return k; }
  size_t write(uint8_t c) { fsStore.files[path].push_back((char) c); return 1; }
  size_t write(const uint8_t* b, size_t n) { fsStore.files[path].append((const char*) b, n); return n; }
  void close() {}
};
struct LittleFSShim {
  bool exists(const String& p) {
    if (fsStore.files.count(p.s)) return true;
    for (auto& kv : fsStore.files) if (kv.first.rfind(p.s + "/", 0) == 0) return true;   // a directory
    return false; }
  bool rmdir(const String&) { return true; }
  bool mkdir(const String&) { return true; }
  bool remove(const String& p) { return fsStore.files.erase(p.s) > 0; }
  bool rename(const String& a, const String& b) { if (fsStore.failRename) return false; fsStore.files[b.s] = fsStore.files[a.s]; fsStore.files.erase(a.s); return true; }
  File open(const String& p, const char* mode) { File f; f.path = p.s; f.writing = mode[0] == 'w';
    if (f.writing) { fsStore.files[p.s] = ""; f.valid = true; } else f.valid = fsStore.files.count(p.s) > 0; return f; }
};
extern LittleFSShim LittleFS;
