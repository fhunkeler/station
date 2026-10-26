#pragma once
#include <Arduino.h>
#include <WiFi.h>
class PubSubClient {
 public:
  PubSubClient(WiFiClient&) {}
  bool connected() { return false; }
  bool loop() { return true; }
  void setServer(const char*, uint16_t) {}
  bool setBufferSize(size_t) { return true; }
  template <class F> void setCallback(F) {}
  bool connect(const char*) { return false; }
  bool connect(const char*, const char*, const char*) { return false; }
  bool subscribe(const char*) { return true; }
  void setSocketTimeout(uint16_t) {}
  void setKeepAlive(uint16_t) {}
  int state() { return -1; }
};
