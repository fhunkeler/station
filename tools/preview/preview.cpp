// Prévisualisation de l'écran sur PC : compile src/main.cpp tel quel contre
// des shims Arduino/GxEPD2 (dossier shim/), alimente les vrais parseurs avec
// des payloads au format de Home Assistant et écrit l'image en PGM 400x300.
//
// Usage : preview <sortie.pgm> [empty|stale|splash|splashfail|config]
#include "../../src/main.cpp"

SerialStub Serial;
EspStub ESP;
WiFiStub WiFi;

// Portail web absent de la prévisualisation.
void webPortalBegin() {}
void webPortalLoop() {}
void webPortalStartAccessPoint() {}
void webPortalStopAccessPoint() {}
bool webPortalAccessPointActive() { return false; }
int webPortalAccessPointClients() { return 0; }
String webPortalEscape(const char* text) { return String(text); }

static std::string iso(time_t t) {
  char b[40];
  struct tm u;
  gmtime_r(&t, &u);
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S+00:00", &u);
  return b;
}

static void dump(const char* path) {
  FILE* f = fopen(path, "wb");
  fprintf(f, "P5\n400 300\n255\n");
  for (int i = 0; i < 400 * 300; ++i) fputc(display.fb[i] ? 255 : 0, f);
  fclose(f);
}

static void feedSampleData(time_t now, bool staleFloor) {
  // station/tide : marées autour de maintenant, au format de l'add-on (heure
  // locale, coefficient null sur la première basse mer du jour).
  std::string tide = "{\"days\":[{\"events\":[";
  const time_t period = 6 * 3600 + 12 * 60;
  time_t t = now - 3 * 3600 - 7 * period / 2;
  for (int i = 0; i < 11; ++i, t += period) {
    const bool high = (i % 2) == 1;
    char stamp[40];
    struct tm l;
    localtime_r(&t, &l);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:00+01:00", &l);
    char e[200];
    snprintf(e, sizeof e, "%s{\"tide\":\"%s\",\"time\":\"%s\",\"height_m\":%.2f,\"coefficient\":%s}",
             i ? "," : "", high ? "HIGH" : "LOW", stamp, high ? 9.4 + 0.05 * i : 1.6 - 0.04 * i,
             (i % 4 == 0) ? "null" : "98");
    tide += e;
  }
  tide += "]}]}";
  handleMqttMessage((char*)TOPIC_TIDE, (byte*)tide.c_str(), tide.size());

  // station/forecast : 73 points en UTC (33 x 1 h, 14 x 3 h, 25 x 6 h).
  std::string fc = "{\"weather.trelevern\":{\"forecast\":[";
  t = now - (now % 3600) + 3600;
  for (int i = 0; i < 73; ++i) {
    char e[400];
    snprintf(e, sizeof e,
             "%s{\"apparent_temperature\":22.1,\"condition\":\"%s\",\"datetime\":\"%s\",\"humidity\":85,"
             "\"precipitation\":0.0,\"temperature\":18.0,\"uv_index\":null,\"wind_bearing\":%d,"
             "\"wind_gust_speed\":%.1f,\"wind_speed\":%.1f}",
             i ? "," : "", i < 3 ? "cloudy" : "rainy", iso(t).c_str(), (220 + i * 9) % 360,
             61.2 - i * 1.5, 28.8 - i * 0.9);
    fc += e;
    t += (i < 33 ? 1 : (i < 47 ? 3 : 6)) * 3600;
  }
  fc += "]}}";
  handleMqttMessage((char*)TOPIC_FORECAST, (byte*)fc.c_str(), fc.size());

  auto measure = [&](const char* topic, const char* body, time_t age) {
    char buf[200];
    char stamp[40];
    struct tm l;
    time_t ts = now - age;
    localtime_r(&ts, &l);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S.392181+01:00", &l);
    snprintf(buf, sizeof buf, "{%s, \"updated_at\": \"%s\"}", body, stamp);
    handleMqttMessage((char*)topic, (byte*)buf, strlen(buf));
  };
  measure(TOPIC_OUTDOOR, "\"pressure\": 1004.2, \"temperature\": 16.5", 1800);
  measure(TOPIC_GROUND, "\"humidity\": 68.6, \"temperature\": 20.78", 1800);
  measure(TOPIC_FLOOR, "\"humidity\": 71.3, \"temperature\": 20.44", staleFloor ? 5 * 3600 : 1800);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s sortie.pgm [empty|stale|splash|splashfail|config]\n", argv[0]);
    return 1;
  }
  const std::string mode = argc > 2 ? argv[2] : "";
  setenv("TZ", TZ_EUROPE_PARIS, 1);
  tzset();
  const time_t now = time(nullptr);
  loadStationConfig();
  // Valeurs fictives : les images de docs/ ne doivent rien révéler du
  // secrets.h local.
  strcpy(gConfig.wifiSsid, "MaBox");
  strcpy(gConfig.mqttHost, "192.168.1.10");
  updateDateTimeFromClock();
  display.init(0);
  display.setRotation(0);
  display.setTextWrap(false);
  updateMoonData();

  if (mode == "config") {
    gConfigReason = ConfigReason::WIFI_UNREACHABLE;
    showFullScreen(drawConfigScreenContent);
  } else if (mode == "splash") {
    gBootSteps[BOOT_STEP_WIFI] = {"WiFi", STEP_DONE, "192.168.1.42"};
    gBootSteps[BOOT_STEP_MQTT] = {"Serveur MQTT", STEP_DONE, "192.168.1.10"};
    gBootSteps[BOOT_STEP_TIME] = {"Heure (NTP)", STEP_RUNNING, ""};
    gBootSteps[BOOT_STEP_DATA] = {"Données", STEP_RUNNING, "3/5 sources"};
    updateSplashScreen();
  } else if (mode == "splashfail") {
    gBootSteps[BOOT_STEP_WIFI] = {"WiFi", STEP_DONE, "192.168.1.42"};
    gBootSteps[BOOT_STEP_MQTT] = {"Serveur MQTT", STEP_FAILED, ""};
    gBootSteps[BOOT_STEP_TIME] = {"Heure (NTP)", STEP_DONE, "20:42"};
    gBootSteps[BOOT_STEP_DATA] = {"Données", STEP_FAILED, ""};
    updateSplashScreen();
  } else {
    if (mode != "empty") {
      feedSampleData(now, mode == "stale");
    }
    displayWeatherStation();
  }
  dump(argv[1]);
  return 0;
}
