// Minimal host shims for testing hawk_presets.cpp off-target.
#pragma once
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <vector>
class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v) : s(std::to_string(v)) {}
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  bool isEmpty() const { return s.empty(); }
  char operator[](size_t i) const { return s[i]; }
  void trim() { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
                s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1); }
  String operator+(const String& o) const { return String(s + o.s); }
  String operator+(const char* o) const { return String(s + o); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
  bool operator==(const String& o) const { return s == o.s; }
  bool concat(const char* b, size_t n) { s.append(b, n); return true; }
  bool concat(const char* b) { s.append(b); return true; }
  void reserve(size_t n) { s.reserve(n); }
};
struct SerialShim { void printf(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); } void println(const char* x) { puts(x); } };
extern SerialShim Serial;
