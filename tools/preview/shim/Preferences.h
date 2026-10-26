#pragma once
// NVS simulée : toujours vide, la configuration garde ses valeurs par défaut.
#include <Arduino.h>
class Preferences {
 public:
  bool begin(const char*, bool) { return false; }
  void end() {}
  bool isKey(const char*) { return false; }
  String getString(const char*, const String& def) { return def; }
  uint16_t getUShort(const char*, uint16_t def) { return def; }
  uint8_t getUChar(const char*, uint8_t def) { return def; }
  size_t putString(const char*, const char* v) { return strlen(v); }
  size_t putUShort(const char*, uint16_t) { return 2; }
  size_t putUChar(const char*, uint8_t) { return 1; }
  bool clear() { return true; }
};
