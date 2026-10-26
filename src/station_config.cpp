#include "station_config.h"

#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// Sans secrets.h, la station démarre sans réseau configuré et ouvre
// directement son point d'accès de configuration.
#ifndef STATION_WIFI_SSID
#define STATION_WIFI_SSID ""
#endif
#ifndef STATION_WIFI_PASSWORD
#define STATION_WIFI_PASSWORD ""
#endif
#ifndef STATION_MQTT_HOST
#define STATION_MQTT_HOST ""
#endif
#ifndef STATION_MQTT_USERNAME
#define STATION_MQTT_USERNAME ""
#endif
#ifndef STATION_MQTT_PASSWORD
#define STATION_MQTT_PASSWORD ""
#endif

StationConfig gConfig;

namespace {

constexpr const char* NVS_NAMESPACE = "station";
constexpr const char* NVS_RESET_KEY = "rstcount";

void copyText(char* target, size_t size, const String& value) {
  strncpy(target, value.c_str(), size - 1);
  target[size - 1] = '\0';
}

void applyDefaults(StationConfig& config) {
  memset(&config, 0, sizeof(config));
  copyText(config.wifiSsid, sizeof(config.wifiSsid), STATION_WIFI_SSID);
  copyText(config.wifiPassword, sizeof(config.wifiPassword), STATION_WIFI_PASSWORD);
  copyText(config.hostname, sizeof(config.hostname), "station");
  copyText(config.mqttHost, sizeof(config.mqttHost), STATION_MQTT_HOST);
  config.mqttPort = 1883;
  copyText(config.mqttUser, sizeof(config.mqttUser), STATION_MQTT_USERNAME);
  copyText(config.mqttPassword, sizeof(config.mqttPassword), STATION_MQTT_PASSWORD);
  copyText(config.topicPrefix, sizeof(config.topicPrefix), "station");
  copyText(config.forecastEntity, sizeof(config.forecastEntity), "weather.trelevern");
  copyText(config.labelOutdoor, sizeof(config.labelOutdoor), "EXTÉRIEUR");
  copyText(config.labelGround, sizeof(config.labelGround), "RDC");
  copyText(config.labelFloor, sizeof(config.labelFloor), "ÉTAGE");
  config.staleHours = 3;
  config.ghostCleaningHour = 4;
  config.windArrowStyle = 1;
}

void loadText(Preferences& prefs, const char* key, char* target, size_t size) {
  if (prefs.isKey(key)) {
    copyText(target, size, prefs.getString(key, target));
  }
}

}  // namespace

void loadStationConfig() {
  applyDefaults(gConfig);
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return;  // rien d'enregistré : valeurs par défaut
  }
  loadText(prefs, "wifiSsid", gConfig.wifiSsid, sizeof(gConfig.wifiSsid));
  loadText(prefs, "wifiPass", gConfig.wifiPassword, sizeof(gConfig.wifiPassword));
  loadText(prefs, "hostname", gConfig.hostname, sizeof(gConfig.hostname));
  loadText(prefs, "mqttHost", gConfig.mqttHost, sizeof(gConfig.mqttHost));
  gConfig.mqttPort = prefs.getUShort("mqttPort", gConfig.mqttPort);
  loadText(prefs, "mqttUser", gConfig.mqttUser, sizeof(gConfig.mqttUser));
  loadText(prefs, "mqttPass", gConfig.mqttPassword, sizeof(gConfig.mqttPassword));
  loadText(prefs, "topicPrefix", gConfig.topicPrefix, sizeof(gConfig.topicPrefix));
  loadText(prefs, "forecast", gConfig.forecastEntity, sizeof(gConfig.forecastEntity));
  loadText(prefs, "lblOutdoor", gConfig.labelOutdoor, sizeof(gConfig.labelOutdoor));
  loadText(prefs, "lblGround", gConfig.labelGround, sizeof(gConfig.labelGround));
  loadText(prefs, "lblFloor", gConfig.labelFloor, sizeof(gConfig.labelFloor));
  gConfig.staleHours = prefs.getUChar("staleHours", gConfig.staleHours);
  gConfig.ghostCleaningHour = prefs.getUChar("ghostHour", gConfig.ghostCleaningHour);
  gConfig.windArrowStyle = prefs.getUChar("arrowStyle", gConfig.windArrowStyle);
  loadText(prefs, "adminPass", gConfig.adminPassword, sizeof(gConfig.adminPassword));
  prefs.end();
}

bool saveStationConfig() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  bool ok = true;
  ok &= prefs.putString("wifiSsid", gConfig.wifiSsid) == strlen(gConfig.wifiSsid);
  ok &= prefs.putString("wifiPass", gConfig.wifiPassword) == strlen(gConfig.wifiPassword);
  ok &= prefs.putString("hostname", gConfig.hostname) == strlen(gConfig.hostname);
  ok &= prefs.putString("mqttHost", gConfig.mqttHost) == strlen(gConfig.mqttHost);
  ok &= prefs.putUShort("mqttPort", gConfig.mqttPort) > 0;
  ok &= prefs.putString("mqttUser", gConfig.mqttUser) == strlen(gConfig.mqttUser);
  ok &= prefs.putString("mqttPass", gConfig.mqttPassword) == strlen(gConfig.mqttPassword);
  ok &= prefs.putString("topicPrefix", gConfig.topicPrefix) == strlen(gConfig.topicPrefix);
  ok &= prefs.putString("forecast", gConfig.forecastEntity) == strlen(gConfig.forecastEntity);
  ok &= prefs.putString("lblOutdoor", gConfig.labelOutdoor) == strlen(gConfig.labelOutdoor);
  ok &= prefs.putString("lblGround", gConfig.labelGround) == strlen(gConfig.labelGround);
  ok &= prefs.putString("lblFloor", gConfig.labelFloor) == strlen(gConfig.labelFloor);
  ok &= prefs.putUChar("staleHours", gConfig.staleHours) > 0;
  ok &= prefs.putUChar("ghostHour", gConfig.ghostCleaningHour) > 0;
  ok &= prefs.putUChar("arrowStyle", gConfig.windArrowStyle) > 0;
  ok &= prefs.putString("adminPass", gConfig.adminPassword) == strlen(gConfig.adminPassword);
  prefs.end();
  return ok;
}

void resetStationConfig() {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }
  applyDefaults(gConfig);
}

String accessPointName() {
  const uint64_t mac = ESP.getEfuseMac();
  char name[20];
  snprintf(name, sizeof(name), "Station-%02X%02X",
           static_cast<unsigned>((mac >> 32) & 0xFF), static_cast<unsigned>((mac >> 40) & 0xFF));
  return String(name);
}

String accessPointPassword() {
  // Alphabet sans caractères ambigus (0/O, 1/l/I) pour une saisie facile.
  static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  uint64_t seed = ESP.getEfuseMac() ^ 0x9E3779B97F4A7C15ULL;
  char password[9];
  for (int i = 0; i < 8; ++i) {
    seed ^= seed >> 33;
    seed *= 0xFF51AFD7ED558CCDULL;
    seed ^= seed >> 29;
    password[i] = ALPHABET[seed % (sizeof(ALPHABET) - 1)];
  }
  password[8] = '\0';
  return String(password);
}

bool consumeDoubleReset() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  const bool armed = prefs.getUChar(NVS_RESET_KEY, 0) != 0;
  // Fenêtre armée pendant les premières secondes de fonctionnement : un
  // nouveau reset pendant ce temps demande le mode configuration.
  prefs.putUChar(NVS_RESET_KEY, armed ? 0 : 1);
  prefs.end();
  return armed;
}

void closeDoubleResetWindow() {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUChar(NVS_RESET_KEY, 0);
    prefs.end();
  }
}
