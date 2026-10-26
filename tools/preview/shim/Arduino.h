#pragma once
// Shim minimal d'Arduino pour compiler src/main.cpp sur PC (prévisualisation).
#ifndef ARDUINO
#define ARDUINO 100
#endif
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <string>
#include <algorithm>
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#define pgm_read_word(a) (*(const uint16_t*)(a))
#define pgm_read_dword(a) (*(const uint32_t*)(a))
#define pgm_read_pointer(a) (*(void* const*)(a))
#define TWO_PI 6.283185307179586476925286766559
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define radians(d) ((d) * PI / 180.0)
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define HEX 16
#define DEC 10
using std::min;
using std::max;
typedef uint8_t byte;
typedef bool boolean;
class __FlashStringHelper;
inline void delay(unsigned long) {}
inline unsigned long millis() { return 0; }
class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(char c) : s(1, c) {}
  String(int v, int base = 10) { char b[34]; snprintf(b, sizeof b, base == 16 ? "%x" : "%d", v); s = b; }
  String(unsigned v, int base = 10) { char b[34]; snprintf(b, sizeof b, base == 16 ? "%x" : "%u", v); s = b; }
  String(long v) : String((int)v) {}
  String(float v, int dec = 2) { char b[40]; snprintf(b, sizeof b, "%.*f", dec, v); s = b; }
  String(double v, int dec = 2) : String((float)v, dec) {}
  const char* c_str() const { return s.c_str(); }
  unsigned length() const { return s.size(); }
  bool reserve(unsigned n) { s.reserve(n); return true; }
  bool concat(const char* c) { s += c; return true; }
  bool concat(const char* c, unsigned n) { s.append(c, n); return true; }
  bool concat(char c) { s += c; return true; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend String operator+(const String& a, const char* b) { return String(a.s + b); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* o) const { return s == o; }
  bool operator!=(const String& o) const { return s != o.s; }
  bool operator!=(const char* o) const { return s != o; }
  bool equals(const String& o) const { return s == o.s; }
  bool equalsIgnoreCase(const String& o) const { return strcasecmp(s.c_str(), o.s.c_str()) == 0; }
  String substring(unsigned a, unsigned b) const { return String(s.substr(a, b - a)); }
  char operator[](unsigned i) const { return s[i]; }
};
#include "Print.h"
struct SerialStub {
  void begin(int) {}
  template <class... A> void print(A...) {}
  template <class... A> void println(A...) {}
  template <class... A> void printf(A...) {}
};
extern SerialStub Serial;
struct EspStub {
  uint64_t getEfuseMac() { return 0x3C61053C2F0FULL; }
  uint32_t getFreeHeap() { return 180000; }
  void restart() {}
};
extern EspStub ESP;
