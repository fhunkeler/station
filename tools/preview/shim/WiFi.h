#pragma once
#include <Arduino.h>
#define WL_CONNECTED 3
#define WIFI_STA 1
struct WiFiStub {
  struct Ip { String toString() const { return String("192.168.0.161"); } };
  int status() { return 0; }
  void mode(int) {}
  void setAutoReconnect(bool) {}
  void disconnect() {}
  void begin(const char*, const char*) {}
  Ip localIP() { return Ip(); }
};
extern WiFiStub WiFi;
class WiFiClient {};
inline void configTzTime(const char*, const char*, const char*, const char*) {}
inline bool getLocalTime(struct tm* t, int) { time_t n = time(nullptr); localtime_r(&n, t); return true; }
