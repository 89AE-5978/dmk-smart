#pragma once
// Mock minimo do Arduino para testar DmkLink no PC.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#define PROGMEM
#define FPSTR(x) (x)
typedef const char *PGM_P;
uint32_t millis();
void delay(uint32_t);
inline void yield() {}
class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &x) : s(x) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(unsigned long v, int base) { char b[32]; snprintf(b, 32, base == 16 ? "%lx" : "%lu", v); s = b; }
  const char *c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  bool isEmpty() const { return s.empty(); }
  char operator[](size_t i) const { return s[i]; }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *o) { s += o; return *this; }
  String &operator+=(char c) { s += c; return *this; }
  bool operator==(const String &o) const { return s == o.s; }
  bool operator==(const char *o) const { return s == o; }
  bool operator!=(const String &o) const { return s != o.s; }
  String substring(int a, int b = -1) const { if (b < 0 || b > (int)s.size()) b = s.size(); if (a > b) return String(); return String(s.substr(a, b - a)); }
  int indexOf(char c, int from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const char *c, int from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  void replace(const char *a, const char *b) { size_t p = 0, la = strlen(a), lb = strlen(b); while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, la, b); p += lb; } }
  void trim() { while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back(); size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++; s = s.substr(i); }
  long toInt() const { return atol(s.c_str()); }
};
inline String operator+(const String &a, const String &b) { return String(a.s + b.s); }
inline String operator+(const String &a, const char *b) { return String(a.s + b); }
inline String operator+(const char *a, const String &b) { return String(std::string(a) + b.s); }
class IPAddress {
 public:
  uint8_t b[4] = {0, 0, 0, 0};
  IPAddress() {}
  IPAddress(uint8_t a, uint8_t c, uint8_t d, uint8_t e) { b[0] = a; b[1] = c; b[2] = d; b[3] = e; }
  uint8_t operator[](int i) const { return b[i]; }
  bool operator==(const IPAddress &o) const { return memcmp(b, o.b, 4) == 0; }
  bool operator!=(const IPAddress &o) const { return !(*this == o); }
};
struct SerialMock { template <typename... A> void printf(const char *f, A... a) { std::printf("    [%d] ", curNode()); std::printf(f, a...); } void println(const char *t) { std::printf("    [%d] %s\n", curNode(), t); } static int curNode(); };
extern SerialMock Serial;
