#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "moon_icons.h"
#include "wind_icons.h"
#include "secrets.h"

// Set ENABLE_STATION_LOGS to 0 at build time to strip Serial logs in production.
#ifndef ENABLE_STATION_LOGS
#define ENABLE_STATION_LOGS 0
#endif

#if ENABLE_STATION_LOGS
#define STATION_LOG_BEGIN(...) Serial.begin(__VA_ARGS__)
#define STATION_LOG_PRINT(...) Serial.print(__VA_ARGS__)
#define STATION_LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define STATION_LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define STATION_LOG_BEGIN(...)
#define STATION_LOG_PRINT(...)
#define STATION_LOG_PRINTLN(...)
#define STATION_LOG_PRINTF(...)
#endif

// Configuration écran e-paper 4.2" (400x300)
// Adapter selon votre modèle exact (exemple: GDEW042T2)
// Pour Waveshare 4.2", utiliser GxEPD2_420 avec les bons pins
#define EPD_CS 5
#define EPD_DC 17
#define EPD_RST 16
#define EPD_BUSY 4

// Initialisation de l'écran - À adapter selon votre modèle
// GxEPD2_420 pour 4.2" 400x300
GxEPD2_BW<GxEPD2_420, GxEPD2_420::HEIGHT> display(GxEPD2_420(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ========== ÉTAT AFFICHÉ ==========
// Heure locale courante. gTimeValid reste faux tant que l'horloge n'a jamais
// été synchronisée : on affiche alors "--:--" plutôt qu'une date inventée.
struct tm gNow = {};
bool gTimeValid = false;

// Phase lunaire (0-7: 0=Nouvelle, 2=Premier quartier, 4=Pleine, 6=Dernier quartier)
int moonPhase = 0;
int moonIllumination = -1; // pourcentage de la surface éclairée, -1 = inconnu

// Marée
bool isTideRising = true;  // true = montante, false = descendante
int tideCoefficient = -1;
String nextTideTime = "--:--";
float nextTideHeightMeters = NAN;
time_t nextTideEpoch = 0;
String prevTideTime = "--:--";
time_t prevTideEpoch = 0;

// Extérieur
float tempExterior = NAN;
int pressureAtmospheric = -1;
float windBearingDegrees = NAN;
float windSpeedKmh = NAN;
float windGustKmh = NAN;
char weatherCondition[16] = "";  // valeur HA brute : "cloudy", "rainy"...
time_t forecastLastUpdateEpoch = 0;

// Rez-de-chaussée
float tempRDC = NAN;
int humidityRDC = -1;

// Étage
float tempEtage = NAN;
int humidityEtage = -1;

// Horodatage (epoch) de la dernière mesure de chaque source, afin de
// neutraliser l'affichage si Home Assistant cesse de publier. 0 = inconnu.
time_t gOutdoorMeasuredAt = 0;
time_t gGroundMeasuredAt = 0;
time_t gFloorMeasuredAt = 0;

// WiFi
const char* WIFI_SSID = STATION_WIFI_SSID;
const char* WIFI_PASSWORD = STATION_WIFI_PASSWORD;
constexpr const char* TZ_EUROPE_PARIS = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr const char* NTP_SERVER_1 = "fr.pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "pool.ntp.org";
constexpr const char* NTP_SERVER_3 = "time.google.com";

const char* MQTT_HOST = STATION_MQTT_HOST;
constexpr uint16_t MQTT_PORT = 1883;
const char* MQTT_USERNAME = STATION_MQTT_USERNAME;
const char* MQTT_PASSWORD = STATION_MQTT_PASSWORD;
// Une seule tentative de connexion par intervalle : la boucle ne doit jamais
// rester bloquée plusieurs secondes, sinon l'horloge affichée prend du retard.
constexpr unsigned long NETWORK_RETRY_INTERVAL_MS = 30000UL;
constexpr unsigned long WIFI_FORCE_RECONNECT_AFTER_MS = 120000UL;
constexpr uint16_t MQTT_SOCKET_TIMEOUT_S = 5;
constexpr uint16_t MQTT_KEEPALIVE_S = 60;
// Au-delà, une mesure n'est plus affichée (HA arrêté, capteur muet...).
// HA publie au moins toutes les heures : 3 h = trois publications manquées.
constexpr time_t MEASUREMENT_STALE_AFTER_S = 3 * 3600;
constexpr time_t FORECAST_STALE_AFTER_S = 3 * 3600;
// Rafraîchissement complet périodique contre la rémanence des mises à jour partielles.
constexpr int FULL_REFRESH_EVERY_MINUTES = 60;
// Un horodatage antérieur à 2020 signifie que l'horloge n'a jamais été réglée.
constexpr time_t MIN_VALID_EPOCH = 1577836800;
constexpr const char* TOPIC_TIDE = "station/tide";
constexpr const char* TOPIC_OUTDOOR = "station/outdoor";
constexpr const char* TOPIC_GROUND = "station/ground";
constexpr const char* TOPIC_FLOOR = "station/floor";
constexpr const char* TOPIC_FORECAST = "station/forecast";
constexpr size_t MQTT_BUFFER_SIZE_BYTES = 32768;
constexpr size_t MAX_PIXELS_MOON = MOON_ICON_SIZE * MOON_ICON_SIZE;
constexpr size_t MAX_BITMAP_PIXELS = MAX_PIXELS_MOON;
constexpr size_t MAX_MONO_BITMAP_BYTES = MAX_BITMAP_PIXELS / 8;
constexpr double SYNODIC_MONTH_DAYS = 29.530588853;
constexpr double KNOWN_NEW_MOON_JD = 2451550.1;  // 6 janvier 2000 18:14 UTC
constexpr double UNIX_EPOCH_JD = 2440587.5;

#if ENABLE_STATION_LOGS
constexpr uint32_t DISPLAY_DIAG_BAUD = 115200;
#else
constexpr uint32_t DISPLAY_DIAG_BAUD = 0;  // pas de traces GxEPD2 en production
#endif

struct TideEvent {
  time_t ts;
  char type[8];
  int coeff;
  float height;
};

// 7 jours de marées font environ 27 événements : de quoi absorber un
// payload complet sans troncature.
constexpr int MAX_TIDE_EVENTS = 32;
TideEvent gTideEvents[MAX_TIDE_EVENTS];
int gTideEventCount = 0;

struct ForecastPoint {
  time_t ts;
  float bearing;
  float speed;
  float gust;
  char condition[16];
};

constexpr int MAX_FORECAST_POINTS = 96;
ForecastPoint gForecastPoints[MAX_FORECAST_POINTS];
int gForecastCount = 0;

struct Rect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

// Quatre bandes pleine largeur, chacune rafraîchie par une fenêtre partielle.
// Les ordonnées restent indépendantes de l'alignement sur 8 px, qui ne
// concerne que x et w (toujours 0 et 400 ici).
constexpr int16_t SCREEN_MARGIN = 12;
constexpr Rect HEADER_AREA = {0, 0, 400, 64};
constexpr Rect TIDE_AREA = {0, 64, 400, 68};
constexpr Rect WIND_AREA = {0, 132, 400, 72};
constexpr Rect BOTTOM_AREA = {0, 204, 400, 96};

// Le repère "maintenant" sur l'arc de marée avance toutes les 10 minutes.
constexpr int TIDE_CHART_REFRESH_MINUTES = 10;

WiFiClient gMqttWiFiClient;
PubSubClient gMqttClient(gMqttWiFiClient);
bool gExteriorNeedsUpdate = false;
bool gGroundNeedsUpdate = false;
bool gUpperNeedsUpdate = false;
bool gTideNeedsUpdate = false;

// ========== FONCTIONS D'AFFICHAGE ==========

enum TextAlign { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

void clearArea(const Rect& area) {
  display.fillRect(area.x, area.y, area.w, area.h, GxEPD_WHITE);
}

void updateHeaderSection();
void updateTideSection();
void updateWindSection();
void updateBottomSection();
bool updateTideForecastFromStoredEvents();
void serviceNetwork();
bool connectToMQTT();
void handleMqttMessage(char* topic, byte* payload, unsigned int length);
void updateMoonData();
bool updateDateTimeFromClock();
bool currentEpoch(time_t& out);
bool parseIso8601ToTime(const char* iso, time_t& outEpoch);
bool loadTideDataFromJson(const char* payload, size_t length = 0);
bool loadForecastFromJson(const char* payload, size_t length = 0);
bool updateForecastFromStoredData();
void drawWindArrow(int centerX, int centerY, float bearingDegrees, int size);

bool currentEpoch(time_t& out) {
  const time_t nowTs = time(nullptr);
  if (nowTs < MIN_VALID_EPOCH) {
    return false;
  }
  out = nowTs;
  return true;
}

// Lit l'horloge interne de l'ESP32. Une fois réglée par NTP elle continue de
// tourner sans réseau : une coupure WiFi ne doit donc pas figer l'heure.
// Renvoie vrai si la minute affichée a changé.
bool updateDateTimeFromClock() {
  time_t nowTs = 0;
  if (!currentEpoch(nowTs)) {
    return false;
  }
  struct tm timeinfo;
  if (localtime_r(&nowTs, &timeinfo) == nullptr) {
    return false;
  }

  const bool changed = !gTimeValid ||
                       (gNow.tm_min != timeinfo.tm_min) || (gNow.tm_hour != timeinfo.tm_hour) ||
                       (gNow.tm_mday != timeinfo.tm_mday) || (gNow.tm_mon != timeinfo.tm_mon) ||
                       (gNow.tm_year != timeinfo.tm_year);
  gNow = timeinfo;
  gTimeValid = true;
  return changed;
}

static bool parseDigitsSegment(const char* str, size_t start, size_t length, int& out) {
  int value = 0;
  for (size_t i = 0; i < length; ++i) {
    char c = str[start + i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

static bool convertDateTimeToEpochUtc(int year, int month, int day,
                                      int hour, int minute, int second,
                                      long long& outSeconds) {
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }

  long long y = year;
  long long m = month;
  if (m <= 2) {
    y -= 1;
    m += 12;
  }

  long long era = y / 400;
  long long yoe = y - era * 400;
  long long doy = (153 * (m - 3) + 2) / 5 + day - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = era * 146097 + doe - 719468;  // 719468 = days from 0000-03-01 to 1970-01-01
  outSeconds = days * 86400LL + hour * 3600LL + minute * 60LL + second;
  return true;
}

bool parseIso8601ToTime(const char* iso, time_t& outEpoch) {
  if (iso == nullptr) {
    return false;
  }
  size_t len = strlen(iso);
  if (len < 16) {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parseDigitsSegment(iso, 0, 4, year)) return false;
  if (!parseDigitsSegment(iso, 5, 2, month)) return false;
  if (!parseDigitsSegment(iso, 8, 2, day)) return false;
  if (!parseDigitsSegment(iso, 11, 2, hour)) return false;
  if (!parseDigitsSegment(iso, 14, 2, minute)) return false;
  bool hasSeconds = false;
  if (len >= 19 && iso[16] == ':' && parseDigitsSegment(iso, 17, 2, second)) {
    hasSeconds = true;
  } else {
    second = 0;
  }

  size_t tzSearchStart = hasSeconds ? 19 : 16;
  size_t tzIndex = len;
  for (size_t i = tzSearchStart; i < len; ++i) {
    char c = iso[i];
    if (c == 'Z' || c == 'z' || c == '+' || c == '-') {
      tzIndex = i;
      break;
    }
  }

  bool hasTimezone = tzIndex < len;
  int tzOffsetMinutes = 0;
  if (hasTimezone) {
    char tzChar = iso[tzIndex];
    if (tzChar == 'Z' || tzChar == 'z') {
      tzOffsetMinutes = 0;
    } else {
      int tzHours = 0;
      int tzMinutes = 0;
      size_t hourPos = tzIndex + 1;
      size_t minutePos = hourPos + 2;
      if (hourPos + 1 >= len || !parseDigitsSegment(iso, hourPos, 2, tzHours)) {
        return false;
      }
      if (minutePos < len && iso[minutePos] == ':') {
        ++minutePos;
      }
      if (minutePos + 1 >= len || !parseDigitsSegment(iso, minutePos, 2, tzMinutes)) {
        return false;
      }
      tzOffsetMinutes = tzHours * 60 + tzMinutes;
      if (tzChar == '-') {
        tzOffsetMinutes = -tzOffsetMinutes;
      }
    }
  }

  if (hasTimezone) {
    long long epochSeconds = 0;
    if (!convertDateTimeToEpochUtc(year, month, day, hour, minute, second, epochSeconds)) {
      return false;
    }
    epochSeconds -= static_cast<long long>(tzOffsetMinutes) * 60LL;
    outEpoch = static_cast<time_t>(epochSeconds);
    return true;
  }

  struct tm timeinfo = {};
  timeinfo.tm_year = year - 1900;
  timeinfo.tm_mon = month - 1;
  timeinfo.tm_mday = day;
  timeinfo.tm_hour = hour;
  timeinfo.tm_min = minute;
  timeinfo.tm_sec = second;
  timeinfo.tm_isdst = -1;

  time_t epoch = mktime(&timeinfo);
  if (epoch == static_cast<time_t>(-1)) {
    return false;
  }
  outEpoch = epoch;
  return true;
}

bool loadTideDataFromJson(const char* payload, size_t length) {
  if (payload == nullptr) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = length > 0
                             ? deserializeJson(doc, payload, length)
                             : deserializeJson(doc, payload);
  if (err) {
    STATION_LOG_PRINT("Erreur JSON marées: ");
    STATION_LOG_PRINTLN(err.f_str());
    return false;
  }

  JsonArray days = doc["days"].as<JsonArray>();
  if (days.isNull()) {
    STATION_LOG_PRINTLN("JSON marées invalide: pas de champ days");
    return false;
  }

  gTideEventCount = 0;
  for (JsonObject day : days) {
    JsonArray events = day["events"].as<JsonArray>();
    if (events.isNull()) continue;
    for (JsonObject event : events) {
      if (gTideEventCount >= MAX_TIDE_EVENTS) break;
      const char* timeStr = event["time"];
      time_t eventTs = 0;
      if (!parseIso8601ToTime(timeStr, eventTs)) continue;
      TideEvent& targetEvent = gTideEvents[gTideEventCount];
      targetEvent.ts = eventTs;
      const char* tideType = event["tide"] | "";
      strncpy(targetEvent.type, tideType, sizeof(targetEvent.type) - 1);
      targetEvent.type[sizeof(targetEvent.type) - 1] = '\0';
      targetEvent.coeff = event["coefficient"].isNull() ? -1 : event["coefficient"].as<int>();
      targetEvent.height = event["height_m"].isNull() ? NAN : event["height_m"].as<float>();
      gTideEventCount++;
    }
    if (gTideEventCount >= MAX_TIDE_EVENTS) {
      STATION_LOG_PRINTLN("Marées: limite MAX_TIDE_EVENTS atteinte, jours suivants ignorés");
      break;
    }
  }
  // Tri par date (insertion sort, car peu d'événements)
  for (int i = 1; i < gTideEventCount; ++i) {
    TideEvent key = gTideEvents[i];
    int j = i - 1;
    while (j >= 0 && gTideEvents[j].ts > key.ts) {
      gTideEvents[j + 1] = gTideEvents[j];
      j--;
    }
    gTideEvents[j + 1] = key;
  }

  if (gTideEventCount == 0) {
    STATION_LOG_PRINTLN("Aucun événement de marée trouvé dans le JSON");
    return false;
  }
  updateTideForecastFromStoredEvents();
  return true;
}

bool updateTideForecastFromStoredEvents() {
  if (gTideEventCount == 0) {
    return false;
  }

  // Sans heure fiable on ne sait pas quelle marée est la prochaine : le calcul
  // sera refait au premier tick d'horloge valide.
  time_t nowTs = 0;
  if (!currentEpoch(nowTs)) {
    return false;
  }

  int nextIdx = -1;
  for (int i = 0; i < gTideEventCount; ++i) {
    if (gTideEvents[i].ts > nowTs) {
      nextIdx = i;
      break;
    }
  }
  if (nextIdx == -1) {
    // Toutes les marées connues sont passées : les données sont périmées
    // (add-on arrêté, mauvais topic, message retenu obsolète...). Afficher
    // le dernier événement connu ferait passer une heure du passé pour une
    // prévision, ce qui masque complètement la panne.
    STATION_LOG_PRINTLN("Marées: toutes les données sont périmées, affichage neutralisé");
    bool wasKnown = (nextTideEpoch != 0) || (tideCoefficient >= 0) ||
                    (prevTideEpoch != 0) || (nextTideTime != "--:--");
    nextTideTime = "--:--";
    nextTideHeightMeters = NAN;
    nextTideEpoch = 0;
    tideCoefficient = -1;
    prevTideTime = "--:--";
    prevTideEpoch = 0;
    if (wasKnown) {
      gTideNeedsUpdate = true;
    }
    return wasKnown;
  }
  int prevIdx = (nextIdx > 0) ? nextIdx - 1 : -1;

  bool rising = isTideRising;
  if (nextIdx > 0) {
    const char* prevType = gTideEvents[nextIdx - 1].type;
    const char* nextType = gTideEvents[nextIdx].type;
    if (strcmp(prevType, "LOW") == 0 && strcmp(nextType, "HIGH") == 0) {
      rising = true;
    } else if (strcmp(prevType, "HIGH") == 0 && strcmp(nextType, "LOW") == 0) {
      rising = false;
    }
  } else {
    rising = (strcmp(gTideEvents[nextIdx].type, "HIGH") == 0);
  }

  String newTideTime = "--:--";
  struct tm localTm;
  if (localtime_r(&gTideEvents[nextIdx].ts, &localTm) != nullptr) {
    char buffer[6];
    if (strftime(buffer, sizeof(buffer), "%H:%M", &localTm) > 0) {
      newTideTime = buffer;
    }
  }

  String newPrevTime = "--:--";
  time_t newPrevEpoch = 0;
  if (prevIdx >= 0) {
    struct tm prevTm;
    if (localtime_r(&gTideEvents[prevIdx].ts, &prevTm) != nullptr) {
      char buffer[6];
      if (strftime(buffer, sizeof(buffer), "%H:%M", &prevTm) > 0) {
        newPrevTime = buffer;
      }
    }
    newPrevEpoch = gTideEvents[prevIdx].ts;
  }

  // Le coefficient se rapporte à la pleine mer, et celui des basses mers est
  // incohérent dans les données (null pour la première de la journée) :
  // prendre celui de la prochaine pleine mer.
  int newCoefficient = gTideEvents[nextIdx].coeff;
  for (int i = nextIdx; i < gTideEventCount; ++i) {
    if (strcmp(gTideEvents[i].type, "HIGH") == 0) {
      newCoefficient = gTideEvents[i].coeff;
      break;
    }
  }

  bool changed = (nextTideEpoch != gTideEvents[nextIdx].ts) || (isTideRising != rising) || (tideCoefficient != newCoefficient) || (nextTideTime != newTideTime);
  bool heightWasNaN = isnan(nextTideHeightMeters);
  bool heightIsNaN = isnan(gTideEvents[nextIdx].height);
  if (heightWasNaN != heightIsNaN) {
    changed = true;
  } else if (!heightIsNaN && fabsf(nextTideHeightMeters - gTideEvents[nextIdx].height) > 0.01f) {
    changed = true;
  }
  if (prevTideEpoch != newPrevEpoch || prevTideTime != newPrevTime) {
    changed = true;
  }

  nextTideTime = newTideTime;
  isTideRising = rising;
  tideCoefficient = newCoefficient;
  nextTideHeightMeters = gTideEvents[nextIdx].height;
  nextTideEpoch = gTideEvents[nextIdx].ts;
  prevTideTime = newPrevTime;
  prevTideEpoch = newPrevEpoch;

  if (changed) {
    gTideNeedsUpdate = true;
  }
  return changed;
}

void updateMoonData() {
  // Le jour julien se déduit directement de l'epoch (UTC), comme la référence
  // KNOWN_NEW_MOON_JD : aucun fuseau horaire n'intervient.
  time_t nowTs = 0;
  if (!currentEpoch(nowTs)) {
    return;
  }

  double jd = UNIX_EPOCH_JD + static_cast<double>(nowTs) / 86400.0;
  double daysSinceNew = jd - KNOWN_NEW_MOON_JD;
  double phaseCycle = fmod(daysSinceNew, SYNODIC_MONTH_DAYS);
  if (phaseCycle < 0.0) {
    phaseCycle += SYNODIC_MONTH_DAYS;
  }
  double phase = phaseCycle / SYNODIC_MONTH_DAYS;  // 0.0 = nouvelle lune, 0.5 = pleine lune

  double illuminationRatio = 0.5 * (1.0 - cos(TWO_PI * phase));
  int illuminationPercent = static_cast<int>(illuminationRatio * 100.0 + 0.5);
  illuminationPercent = constrain(illuminationPercent, 0, 100);

  int phaseIndex = static_cast<int>(phase * MOON_PHASE_COUNT + 0.5) % MOON_PHASE_COUNT;

  moonIllumination = illuminationPercent;
  moonPhase = phaseIndex;
}

void drawBitmapBlackOnWhite(int16_t topLeftX, int16_t topLeftY,
                            const unsigned char* bitmap, int16_t width, int16_t height) {
  const size_t byteCount = (static_cast<size_t>(width) * height) / 8;
  if (byteCount > MAX_MONO_BITMAP_BYTES) {
    return;
  }
  static uint8_t workingBuffer[MAX_MONO_BITMAP_BYTES];
  for (size_t i = 0; i < byteCount; ++i) {
    workingBuffer[i] = ~pgm_read_byte(bitmap + i);  // Exported bitmaps use 1=white; invert for GxEPD polarity
  }
  display.drawBitmap(topLeftX, topLeftY, workingBuffer, width, height, GxEPD_BLACK, GxEPD_WHITE);
}

void drawMoonPhase(int centerX, int centerY, int phase) {
  int clampedPhase = constrain(phase, 0, MOON_PHASE_COUNT - 1);
  const unsigned char* bitmap = epd_bitmap_allArray[clampedPhase];
  int topLeftX = centerX - (MOON_ICON_SIZE / 2);
  int topLeftY = centerY - (MOON_ICON_SIZE / 2);
  drawBitmapBlackOnWhite(topLeftX, topLeftY, bitmap, MOON_ICON_SIZE, MOON_ICON_SIZE);
}

// Écrit text avec la police courante, aligné horizontalement sur x.
// Renvoie l'abscisse du bord droit du texte.
int printAligned(const char* text, int x, int baseline, TextAlign align) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  int left = x;
  if (align == ALIGN_CENTER) {
    left = x - static_cast<int>(bw) / 2;
  } else if (align == ALIGN_RIGHT) {
    left = x - static_cast<int>(bw);
  }
  display.setCursor(left - bx, baseline);
  display.print(text);
  return left + bw;
}

// Comme printAligned, mais sur un fond blanc pour rester lisible par-dessus
// la courbe de marée. Le texte est contenu dans [minX, maxX].
void printLabel(const char* text, int x, int baseline, TextAlign align, int minX, int maxX) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  int left = x;
  if (align == ALIGN_CENTER) {
    left = x - static_cast<int>(bw) / 2;
  } else if (align == ALIGN_RIGHT) {
    left = x - static_cast<int>(bw);
  }
  left = constrain(left, minX, maxX - static_cast<int>(bw));
  display.fillRect(left - 2, baseline + by - 2, bw + 4, bh + 4, GxEPD_WHITE);
  display.setCursor(left - bx, baseline);
  display.print(text);
}

// Température en gros avec un signe degré dessiné (les polices GFX n'ont
// pas de "°"). "--.-" si la valeur est inconnue.
void drawTemperature(float value, int x, int baseline, TextAlign align, const GFXfont* font, int degreeRadius) {
  char text[8];
  if (isnan(value)) {
    strcpy(text, "--.-");
  } else {
    snprintf(text, sizeof(text), "%.1f", value);
  }
  display.setFont(font);
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  const int degreeWidth = 2 * degreeRadius + 3;
  const int totalWidth = static_cast<int>(bw) + degreeWidth;
  int left = x;
  if (align == ALIGN_CENTER) {
    left = x - totalWidth / 2;
  } else if (align == ALIGN_RIGHT) {
    left = x - totalWidth;
  }
  display.setCursor(left - bx, baseline);
  display.print(text);
  const int cx = left + bw + 3 + degreeRadius;
  const int cy = baseline + by + degreeRadius;
  display.drawCircle(cx, cy, degreeRadius, GxEPD_BLACK);
  display.drawCircle(cx, cy, degreeRadius - 1, GxEPD_BLACK);
}

const char* compassPoint(float bearingDegrees) {
  static const char* const POINTS[16] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSO", "SO", "OSO", "O", "ONO", "NO", "NNO"
  };
  int index = static_cast<int>(floorf(bearingDegrees / 22.5f + 0.5f)) % 16;
  if (index < 0) {
    index += 16;
  }
  return POINTS[index];
}

// Libellé court des conditions météo de Home Assistant (polices sans accents).
const char* conditionLabel(const char* condition) {
  static const char* const LABELS[][2] = {
    {"clear-night", "Nuit claire"}, {"cloudy", "Couvert"}, {"exceptional", "Exceptionnel"},
    {"fog", "Brouillard"}, {"hail", "Grele"}, {"lightning", "Orage"},
    {"lightning-rainy", "Orage"}, {"partlycloudy", "Eclaircies"}, {"pouring", "Averses"},
    {"rainy", "Pluie"}, {"snowy", "Neige"}, {"snowy-rainy", "Pluie/neige"},
    {"sunny", "Soleil"}, {"windy", "Venteux"}, {"windy-variant", "Venteux"},
  };
  for (const auto& entry : LABELS) {
    if (strcmp(condition, entry[0]) == 0) {
      return entry[1];
    }
  }
  return "--";
}

const char* const FRENCH_DAY_NAMES[7] = {
  "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi"
};

const char* const FRENCH_MONTH_NAMES[12] = {
  "janvier", "fevrier", "mars", "avril", "mai", "juin",
  "juillet", "aout", "septembre", "octobre", "novembre", "decembre"
};

void drawHeaderContent() {
  display.setTextColor(GxEPD_BLACK);

  char timeStr[6] = "--:--";
  if (gTimeValid) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", gNow.tm_hour, gNow.tm_min);
  }
  display.setFont(&FreeSansBold24pt7b);
  const int timeRight = printAligned(timeStr, SCREEN_MARGIN, 47, ALIGN_LEFT);

  if (gTimeValid) {
    char dateStr[24];
    snprintf(dateStr, sizeof(dateStr), "%d %s", gNow.tm_mday, FRENCH_MONTH_NAMES[gNow.tm_mon % 12]);
    const int dateX = timeRight + 14;
    display.setFont(&FreeSansBold12pt7b);
    printAligned(FRENCH_DAY_NAMES[gNow.tm_wday % 7], dateX, 26, ALIGN_LEFT);
    display.setFont(&FreeSans12pt7b);
    printAligned(dateStr, dateX, 48, ALIGN_LEFT);
  }

  const int moonCenterX = display.width() - SCREEN_MARGIN - MOON_ICON_SIZE / 2;
  const int moonCenterY = 30;
  if (moonIllumination >= 0) {
    drawMoonPhase(moonCenterX, moonCenterY, moonPhase);
    char moonText[16];
    snprintf(moonText, sizeof(moonText), "%d%%", moonIllumination);
    display.setFont(&FreeSans9pt7b);
    printAligned(moonText, moonCenterX - MOON_ICON_SIZE / 2 - 6, moonCenterY + 6, ALIGN_RIGHT);
  }

  display.fillRect(SCREEN_MARGIN, HEADER_AREA.y + HEADER_AREA.h - 3, display.width() - 2 * SCREEN_MARGIN, 2, GxEPD_BLACK);
}

// Indice de l'étale à l'instant ts dans gTideEvents, -1 si absente.
int findTideEventIndex(time_t ts) {
  for (int i = 0; i < gTideEventCount; ++i) {
    if (gTideEvents[i].ts == ts) {
      return i;
    }
  }
  return -1;
}

// Heure et hauteur d'une étale sur deux lignes, alignées sur x.
void drawTideExtremum(const TideEvent& event, int x, TextAlign align) {
  struct tm eventTm;
  char timeStr[6] = "--:--";
  if (localtime_r(&event.ts, &eventTm) != nullptr) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", eventTm.tm_hour, eventTm.tm_min);
  }
  char detail[20];
  const char* kind = strcmp(event.type, "HIGH") == 0 ? "PM" : "BM";
  if (isnan(event.height)) {
    snprintf(detail, sizeof(detail), "%s", kind);
  } else {
    snprintf(detail, sizeof(detail), "%s %.1f m", kind, event.height);
  }
  display.setFont(&FreeSansBold12pt7b);
  printAligned(timeStr, x, TIDE_AREA.y + 42, align);
  display.setFont(&FreeSans9pt7b);
  printAligned(detail, x, TIDE_AREA.y + 61, align);
}

// Marée réduite à la demi-oscillation en cours : étale précédente à gauche,
// suivante à droite, et entre les deux l'arc correspondant avec la position
// actuelle.
void drawTideContent() {
  const int left = SCREEN_MARGIN;
  const int right = display.width() - SCREEN_MARGIN;
  const int titleBaseline = TIDE_AREA.y + 16;

  display.setTextColor(GxEPD_BLACK);
  time_t nowTs = 0;
  const int nextIdx = findTideEventIndex(nextTideEpoch);
  const int prevIdx = findTideEventIndex(prevTideEpoch);
  if (!currentEpoch(nowTs) || nextTideEpoch == 0 || nextIdx < 0) {
    display.setFont(&FreeSansBold9pt7b);
    printAligned("MAREE", left, titleBaseline, ALIGN_LEFT);
    display.setFont(&FreeSans9pt7b);
    printAligned("Donnees indisponibles", display.width() / 2, TIDE_AREA.y + 46, ALIGN_CENTER);
    return;
  }

  display.setFont(&FreeSansBold9pt7b);
  printAligned(isTideRising ? "MAREE MONTANTE" : "MAREE DESCENDANTE", left, titleBaseline, ALIGN_LEFT);
  char coefStr[16];
  if (tideCoefficient >= 0) {
    snprintf(coefStr, sizeof(coefStr), "Coef %d", tideCoefficient);
  } else {
    strcpy(coefStr, "Coef --");
  }
  printAligned(coefStr, right, titleBaseline, ALIGN_RIGHT);

  const TideEvent& next = gTideEvents[nextIdx];
  drawTideExtremum(next, right, ALIGN_RIGHT);
  if (prevIdx < 0) {
    return;
  }
  const TideEvent& prev = gTideEvents[prevIdx];
  drawTideExtremum(prev, left, ALIGN_LEFT);

  // Arc entre les deux étales (règle du cosinus), trame sous la courbe.
  const int arcLeft = left + 86;
  const int arcRight = right - 86;
  const int arcTop = TIDE_AREA.y + 26;
  const int arcBottom = TIDE_AREA.y + 60;
  const bool rising = !isnan(prev.height) && !isnan(next.height) ? next.height > prev.height : isTideRising;
  auto arcY = [&](float progress) {
    const float level = (1.0f - cosf(PI * progress)) / 2.0f;  // 0 au départ, 1 à l'arrivée
    const float fromLow = rising ? level : 1.0f - level;
    return arcBottom - static_cast<int>(roundf(fromLow * (arcBottom - arcTop)));
  };
  int prevY = -1;
  for (int x = arcLeft; x <= arcRight; ++x) {
    const int y = arcY(static_cast<float>(x - arcLeft) / (arcRight - arcLeft));
    for (int yy = y + 3; yy <= arcBottom + 2; ++yy) {
      if ((x % 2 == 0) && (yy % 2 == 0)) {
        display.drawPixel(x, yy, GxEPD_BLACK);
      }
    }
    if (prevY >= 0) {
      display.drawLine(x - 1, prevY, x, y, GxEPD_BLACK);
      display.drawLine(x - 1, prevY + 1, x, y + 1, GxEPD_BLACK);
    }
    prevY = y;
  }
  display.drawFastHLine(arcLeft, arcBottom + 3, arcRight - arcLeft + 1, GxEPD_BLACK);

  if (next.ts > prev.ts) {
    const float progress = constrain(static_cast<float>(nowTs - prev.ts) / static_cast<float>(next.ts - prev.ts), 0.0f, 1.0f);
    const int nowX = arcLeft + static_cast<int>(roundf(progress * (arcRight - arcLeft)));
    const int nowY = arcY(progress);
    display.fillCircle(nowX, nowY, 6, GxEPD_WHITE);
    display.fillCircle(nowX, nowY, 4, GxEPD_BLACK);
  }
}

// Premier point de prévision à partir de l'instant t (tolérance 30 min).
const ForecastPoint* forecastAt(time_t t) {
  for (int i = 0; i < gForecastCount; ++i) {
    if (gForecastPoints[i].ts >= t - 1800) {
      return &gForecastPoints[i];
    }
  }
  return nullptr;
}

// Bande vent : situation actuelle à gauche, prévision à +3/+6/+9/+12 h à droite.
void drawWindContent() {
  display.setTextColor(GxEPD_BLACK);
  const int width = display.width();
  const int y0 = WIND_AREA.y;
  display.fillRect(SCREEN_MARGIN, y0, width - 2 * SCREEN_MARGIN, 2, GxEPD_BLACK);

  const int splitX = 200;
  const int arrowSize = 46;
  const int arrowCenterX = SCREEN_MARGIN + arrowSize / 2 + 2;
  const int arrowCenterY = y0 + 38;
  const int textX = SCREEN_MARGIN + arrowSize + 12;

  if (isnan(windSpeedKmh) || isnan(windBearingDegrees)) {
    display.setFont(&FreeSansBold9pt7b);
    printAligned("VENT", SCREEN_MARGIN, y0 + 24, ALIGN_LEFT);
    display.setFont(&FreeSans12pt7b);
    printAligned("--", SCREEN_MARGIN, y0 + 52, ALIGN_LEFT);
  } else {
    drawWindArrow(arrowCenterX, arrowCenterY, windBearingDegrees, arrowSize);
    char speedStr[8];
    snprintf(speedStr, sizeof(speedStr), "%.0f", windSpeedKmh);
    display.setFont(&FreeSansBold24pt7b);
    const int speedRight = printAligned(speedStr, textX, y0 + 44, ALIGN_LEFT);
    display.setFont(&FreeSans9pt7b);
    printAligned("km/h", speedRight + 5, y0 + 44, ALIGN_LEFT);
    display.setFont(&FreeSansBold9pt7b);
    printAligned(compassPoint(windBearingDegrees), speedRight + 5, y0 + 24, ALIGN_LEFT);

    char gustStr[24];
    if (isnan(windGustKmh)) {
      strcpy(gustStr, "Rafales --");
    } else {
      snprintf(gustStr, sizeof(gustStr), "Rafales %.0f", windGustKmh);
    }
    display.setFont(&FreeSans9pt7b);
    printAligned(gustStr, textX, y0 + 65, ALIGN_LEFT);
  }

  display.drawFastVLine(splitX, y0 + 10, WIND_AREA.h - 18, GxEPD_BLACK);

  // Tendance : flèche et vitesse moyenne toutes les 3 heures.
  time_t nowTs = 0;
  const bool timeKnown = currentEpoch(nowTs);
  constexpr int SLOT_COUNT = 4;
  const int slotsLeft = splitX + 4;
  const int slotWidth = (width - SCREEN_MARGIN - slotsLeft) / SLOT_COUNT;
  for (int slot = 0; slot < SLOT_COUNT; ++slot) {
    const int cx = slotsLeft + slot * slotWidth + slotWidth / 2;
    const ForecastPoint* point = timeKnown ? forecastAt(nowTs + (slot + 1) * 3 * 3600) : nullptr;
    char hourStr[6] = "--";
    char slotSpeed[8] = "--";
    if (point != nullptr) {
      struct tm pointTm;
      if (localtime_r(&point->ts, &pointTm) != nullptr) {
        snprintf(hourStr, sizeof(hourStr), "%dh", pointTm.tm_hour);
      }
      if (!isnan(point->speed)) {
        snprintf(slotSpeed, sizeof(slotSpeed), "%.0f", point->speed);
      }
      if (!isnan(point->bearing)) {
        drawWindArrow(cx, y0 + 39, point->bearing, 26);
      }
    }
    display.setFont(&FreeSans9pt7b);
    printAligned(hourStr, cx, y0 + 21, ALIGN_CENTER);
    display.setFont(&FreeSansBold9pt7b);
    printAligned(slotSpeed, cx, y0 + 66, ALIGN_CENTER);
  }
}

// Une colonne de mesure : titre, grande température, une ou deux lignes de détail.
void drawMeasureColumn(const char* title, int centerX, float temperature, const char* detail, const char* detail2) {
  display.setFont(&FreeSansBold9pt7b);
  printAligned(title, centerX, BOTTOM_AREA.y + 19, ALIGN_CENTER);
  drawTemperature(temperature, centerX, BOTTOM_AREA.y + 56, ALIGN_CENTER, &FreeSansBold24pt7b, 4);
  display.setFont(&FreeSans12pt7b);
  printAligned(detail, centerX, BOTTOM_AREA.y + 78, ALIGN_CENTER);
  if (detail2 != nullptr) {
    display.setFont(&FreeSans9pt7b);
    printAligned(detail2, centerX, BOTTOM_AREA.y + 94, ALIGN_CENTER);
  }
}

void drawBottomContent() {
  display.setTextColor(GxEPD_BLACK);
  const int width = display.width();
  const int columnWidth = (width - 2 * SCREEN_MARGIN) / 3;
  const int col1 = SCREEN_MARGIN + columnWidth / 2;
  const int col2 = col1 + columnWidth;
  const int col3 = col2 + columnWidth;

  char humRdcStr[16];
  char humEtageStr[16];
  char pressureStr[16];
  if (humidityRDC >= 0) {
    snprintf(humRdcStr, sizeof(humRdcStr), "%d %%", humidityRDC);
  } else {
    strcpy(humRdcStr, "-- %");
  }
  if (humidityEtage >= 0) {
    snprintf(humEtageStr, sizeof(humEtageStr), "%d %%", humidityEtage);
  } else {
    strcpy(humEtageStr, "-- %");
  }
  if (pressureAtmospheric >= 0) {
    snprintf(pressureStr, sizeof(pressureStr), "%d hPa", pressureAtmospheric);
  } else {
    strcpy(pressureStr, "-- hPa");
  }

  drawMeasureColumn("EXTERIEUR", col1, tempExterior, conditionLabel(weatherCondition), pressureStr);
  drawMeasureColumn("RDC", col2, tempRDC, humRdcStr, nullptr);
  drawMeasureColumn("ETAGE", col3, tempEtage, humEtageStr, nullptr);

  display.fillRect(SCREEN_MARGIN, BOTTOM_AREA.y, width - 2 * SCREEN_MARGIN, 2, GxEPD_BLACK);
  const int separatorTop = BOTTOM_AREA.y + 10;
  const int separatorHeight = BOTTOM_AREA.h - 18;
  display.drawFastVLine(SCREEN_MARGIN + columnWidth, separatorTop, separatorHeight, GxEPD_BLACK);
  display.drawFastVLine(SCREEN_MARGIN + 2 * columnWidth, separatorTop, separatorHeight, GxEPD_BLACK);
}

void refreshArea(const Rect& area, void (*draw)()) {
  display.setPartialWindow(area.x, area.y, area.w, area.h);
  display.firstPage();
  do {
    clearArea(area);
    draw();
  } while (display.nextPage());
}

void updateHeaderSection() {
  refreshArea(HEADER_AREA, drawHeaderContent);
}

void updateTideSection() {
  refreshArea(TIDE_AREA, drawTideContent);
}

void updateWindSection() {
  refreshArea(WIND_AREA, drawWindContent);
}

void updateBottomSection() {
  refreshArea(BOTTOM_AREA, drawBottomContent);
}

// Rafraîchissement complet en une seule passe : efface la rémanence laissée
// par les mises à jour partielles successives.
void displayWeatherStation() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeaderContent();
    drawTideContent();
    drawWindContent();
    drawBottomContent();
  } while (display.nextPage());
  display.powerOff();

  gExteriorNeedsUpdate = false;
  gTideNeedsUpdate = false;
  gGroundNeedsUpdate = false;
  gUpperNeedsUpdate = false;
}

// Rafraîchit les zones marquées comme modifiées. C'est le seul endroit qui
// consomme les drapeaux gXxxNeedsUpdate. gExteriorNeedsUpdate couvre aussi
// la prévision : elle alimente la bande vent et la condition météo.
void refreshDirtySections() {
  bool refreshed = false;
  if (gTideNeedsUpdate) {
    gTideNeedsUpdate = false;
    updateTideSection();
    refreshed = true;
  }
  if (gExteriorNeedsUpdate) {
    updateWindSection();
    refreshed = true;
  }
  if (gExteriorNeedsUpdate || gGroundNeedsUpdate || gUpperNeedsUpdate) {
    gExteriorNeedsUpdate = false;
    gGroundNeedsUpdate = false;
    gUpperNeedsUpdate = false;
    updateBottomSection();
    refreshed = true;
  }
  if (refreshed) {
    display.powerOff();
  }
}

// Horodatage d'une mesure : updated_at fourni par HA, sinon l'heure de
// réception. Un message retenu est redistribué à chaque reconnexion MQTT, et
// HA ne publie rien quand un capteur est indisponible : seul updated_at
// permet de savoir qu'une valeur est ancienne.
time_t measurementTimestamp(const JsonDocument& doc) {
  time_t ts = 0;
  if (parseIso8601ToTime(doc["updated_at"] | "", ts)) {
    return ts;
  }
  currentEpoch(ts);  // reste à 0 si l'heure est encore inconnue
  return ts;
}

bool isMeasurementStale(time_t& measuredAt) {
  time_t nowTs = 0;
  if (!currentEpoch(nowTs)) {
    return false;
  }
  if (measuredAt == 0) {
    // Reçue avant la synchro de l'heure : dater à partir de maintenant.
    measuredAt = nowTs;
    return false;
  }
  return (nowTs - measuredAt) > MEASUREMENT_STALE_AFTER_S;
}

// Efface les mesures que Home Assistant n'a plus publiées depuis trop
// longtemps, pour ne pas afficher indéfiniment une valeur figée.
void expireStaleMeasurements() {
  if ((!isnan(tempExterior) || pressureAtmospheric >= 0) && isMeasurementStale(gOutdoorMeasuredAt)) {
    tempExterior = NAN;
    pressureAtmospheric = -1;
    gExteriorNeedsUpdate = true;
  }
  if ((!isnan(tempRDC) || humidityRDC >= 0) && isMeasurementStale(gGroundMeasuredAt)) {
    tempRDC = NAN;
    humidityRDC = -1;
    gGroundNeedsUpdate = true;
  }
  if ((!isnan(tempEtage) || humidityEtage >= 0) && isMeasurementStale(gFloorMeasuredAt)) {
    tempEtage = NAN;
    humidityEtage = -1;
    gUpperNeedsUpdate = true;
  }
}

// Travail fait à chaque changement de minute.
void onMinuteTick() {
  static int minutesSinceFullRefresh = 0;
  updateMoonData();
  updateTideForecastFromStoredEvents();
  updateForecastFromStoredData();
  expireStaleMeasurements();

  if (gTimeValid && gNow.tm_min % TIDE_CHART_REFRESH_MINUTES == 0) {
    gTideNeedsUpdate = true;  // fait glisser la courbe et le repère "maintenant"
  }

  if (++minutesSinceFullRefresh >= FULL_REFRESH_EVERY_MINUTES) {
    minutesSinceFullRefresh = 0;
    displayWeatherStation();
    return;
  }
  updateHeaderSection();
  refreshDirtySections();
  display.powerOff();
}

void setup() {
  STATION_LOG_BEGIN(115200);
  STATION_LOG_PRINTLN("Démarrage de la station météo e-paper");

  // Fuseau posé tout de suite : localtime_r est correct dès la synchro NTP.
  setenv("TZ", TZ_EUROPE_PARIS, 1);
  tzset();

  display.init(DISPLAY_DIAG_BAUD);
  display.setRotation(0); // Mode paysage (400x300)

  // Laisser au réseau le temps de fournir l'heure et les messages MQTT
  // retenus, pour que le premier affichage soit complet. Sans réseau, on
  // affiche quand même au bout du délai.
  constexpr unsigned long BOOT_NETWORK_WAIT_MS = 20000UL;
  constexpr unsigned long BOOT_RETAINED_WAIT_MS = 2000UL;
  const unsigned long bootStart = millis();
  unsigned long mqttConnectedAt = 0;
  bool mqttSeen = false;
  while (millis() - bootStart < BOOT_NETWORK_WAIT_MS) {
    serviceNetwork();
    if (gMqttClient.connected() && !mqttSeen) {
      mqttSeen = true;
      mqttConnectedAt = millis();
    }
    updateDateTimeFromClock();
    if (gTimeValid && mqttSeen && millis() - mqttConnectedAt >= BOOT_RETAINED_WAIT_MS) {
      break;
    }
    delay(50);
  }

  updateDateTimeFromClock();
  updateMoonData();
  updateTideForecastFromStoredEvents();
  updateForecastFromStoredData();
  displayWeatherStation();

  STATION_LOG_PRINTLN("Affichage initial terminé");
}

void loop() {
  serviceNetwork();

  // L'horloge est relue souvent et le rafraîchissement suit le changement de
  // minute : l'affichage reste calé sur la minute réelle même après une
  // opération bloquante.
  if (updateDateTimeFromClock()) {
    onMinuteTick();
  } else {
    refreshDirtySections();
  }

  delay(200);
}

// Maintenance réseau non bloquante : au plus une tentative de connexion MQTT
// par intervalle, la reconnexion WiFi étant confiée à la pile ESP32.
void serviceNetwork() {
  static bool wifiStarted = false;
  static bool timeConfigured = false;
  static bool mqttAttempted = false;
  static unsigned long lastMqttAttemptMs = 0;
  static unsigned long wifiLostSinceMs = 0;
  static bool wifiWasConnected = false;
  const unsigned long nowMs = millis();

  if (!wifiStarted) {
    wifiStarted = true;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiLostSinceMs = nowMs;
    STATION_LOG_PRINT("Connexion WiFi à ");
    STATION_LOG_PRINTLN(WIFI_SSID);
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      wifiWasConnected = false;
      wifiLostSinceMs = nowMs;
      STATION_LOG_PRINTLN("WiFi perdu");
    } else if (nowMs - wifiLostSinceMs >= WIFI_FORCE_RECONNECT_AFTER_MS) {
      // Filet de sécurité si la reconnexion automatique ne reprend pas.
      wifiLostSinceMs = nowMs;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      STATION_LOG_PRINTLN("WiFi: reconnexion forcée");
    }
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    STATION_LOG_PRINT("WiFi connecté, IP: ");
    STATION_LOG_PRINTLN(WiFi.localIP());
  }

  if (!timeConfigured) {
    // Synchronisation SNTP en tâche de fond, renouvelée automatiquement.
    configTzTime(TZ_EUROPE_PARIS, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    timeConfigured = true;
  }

  if (gMqttClient.connected()) {
    gMqttClient.loop();
    return;
  }
  if (!mqttAttempted || nowMs - lastMqttAttemptMs >= NETWORK_RETRY_INTERVAL_MS) {
    mqttAttempted = true;
    lastMqttAttemptMs = nowMs;
    connectToMQTT();
  }
}

bool connectToMQTT() {
  static bool configured = false;
  if (!configured) {
    configured = true;
    gMqttClient.setServer(MQTT_HOST, MQTT_PORT);
    gMqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
    // 15 s par défaut : le broker coupait la station plusieurs fois par mois.
    gMqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
    if (!gMqttClient.setBufferSize(MQTT_BUFFER_SIZE_BYTES)) {
      // Sans ce contrôle, un échec d'allocation se traduisait par des messages
      // tronqués silencieusement, donc par un JSON invalide.
      STATION_LOG_PRINTLN("Echec d'allocation du buffer MQTT");
    }
    gMqttClient.setCallback(handleMqttMessage);
  }

  String clientId = "epd-station-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);
  bool connected = false;
  if (MQTT_USERNAME != nullptr && MQTT_USERNAME[0] != '\0') {
    connected = gMqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD);
  } else {
    connected = gMqttClient.connect(clientId.c_str());
  }
  if (!connected) {
    STATION_LOG_PRINTF("Connexion MQTT impossible (état %d)\n", gMqttClient.state());
    return false;
  }

  STATION_LOG_PRINTLN("MQTT connecté");
  auto subscribeAndLog = [&](const char* topic) {
    bool ok = gMqttClient.subscribe(topic);
    STATION_LOG_PRINT("Souscription ");
    STATION_LOG_PRINT(topic);
    STATION_LOG_PRINTLN(ok ? " OK" : " ECHEC");
  };
  subscribeAndLog(TOPIC_TIDE);
  subscribeAndLog(TOPIC_OUTDOOR);
  subscribeAndLog(TOPIC_GROUND);
  subscribeAndLog(TOPIC_FLOOR);
  subscribeAndLog(TOPIC_FORECAST);
  return true;
}

float readNumber(JsonVariantConst value) {
  return value.is<float>() ? value.as<float>() : NAN;
}

// Affecte une température ; renvoie vrai si la valeur affichée change.
bool assignTemperature(float& target, float value) {
  if (isnan(value) && isnan(target)) {
    return false;
  }
  if (!isnan(value) && !isnan(target) && fabsf(value - target) <= 0.01f) {
    return false;
  }
  target = value;
  return true;
}

// Affecte une valeur entière bornée (-1 = inconnue) ; vrai si elle change.
bool assignRounded(int& target, float value, int minValue, int maxValue) {
  const int rounded = isnan(value) ? -1 : constrain(static_cast<int>(roundf(value)), minValue, maxValue);
  if (rounded == target) {
    return false;
  }
  target = rounded;
  return true;
}

void handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
#if ENABLE_STATION_LOGS
  // Copie réservée au débogage : sans ce garde, le payload entier était
  // recopié dans le tas à chaque message alors que les macros de log ne
  // compilent rien.
  String payloadStr;
  payloadStr.reserve(length + 1);
  for (unsigned int i = 0; i < length; ++i) {
    payloadStr += static_cast<char>(payload[i]);
  }
  STATION_LOG_PRINT("[MQTT] Reçu -> ");
  STATION_LOG_PRINT(topicStr);
  STATION_LOG_PRINT(" : ");
  STATION_LOG_PRINTLN(payloadStr);
#endif

  if (topicStr == TOPIC_TIDE) {
    loadTideDataFromJson(reinterpret_cast<const char*>(payload), length);
    return;
  }

  if (topicStr == TOPIC_FORECAST) {
    loadForecastFromJson(reinterpret_cast<const char*>(payload), length);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    STATION_LOG_PRINT("Erreur JSON MQTT: ");
    STATION_LOG_PRINTLN(err.f_str());
    return;
  }

  // Chaque message est un instantané complet : un champ absent, null ou non
  // numérique signifie que la valeur est inconnue.
  const time_t measuredAt = measurementTimestamp(doc);
  time_t staleCheck = measuredAt;
  if (isMeasurementStale(staleCheck)) {
    STATION_LOG_PRINTLN("Mesure MQTT trop ancienne, ignorée");
    return;
  }
  const float temperature = readNumber(doc["temperature"]);

  if (topicStr == TOPIC_OUTDOOR) {
    gOutdoorMeasuredAt = measuredAt;
    bool updated = assignTemperature(tempExterior, temperature);
    updated |= assignRounded(pressureAtmospheric, readNumber(doc["pressure"]), 0, 2000);
    if (updated) {
      gExteriorNeedsUpdate = true;
    }
    return;
  }

  if (topicStr == TOPIC_GROUND) {
    gGroundMeasuredAt = measuredAt;
    bool updated = assignTemperature(tempRDC, temperature);
    updated |= assignRounded(humidityRDC, readNumber(doc["humidity"]), 0, 100);
    if (updated) {
      gGroundNeedsUpdate = true;
    }
    return;
  }

  if (topicStr == TOPIC_FLOOR) {
    gFloorMeasuredAt = measuredAt;
    bool updated = assignTemperature(tempEtage, temperature);
    updated |= assignRounded(humidityEtage, readNumber(doc["humidity"]), 0, 100);
    if (updated) {
      gUpperNeedsUpdate = true;
    }
    return;
  }
}

void drawWindArrow(int centerX, int centerY, float bearingDegrees, int size) {
  int clampedSize = max(16, size);

  if (isnan(bearingDegrees)) {
    return;
  }

  // Le bearing indique d'où vient le vent. La flèche doit pointer vers où il va (+180°).
  // L'icône pointe vers le nord (0°), on la tourne dans le sens horaire.
  // Échantillonnage inverse : chaque pixel de destination va chercher son
  // pixel source, ce qui évite les trous d'une rotation pixel par pixel.
  float angleRad = radians(bearingDegrees + 180.0f);
  float cosA = cosf(angleRad);
  float sinA = sinf(angleRad);
  float invScale = static_cast<float>(WIND_ICON_SIZE) / clampedSize;
  float halfSrc = WIND_ICON_SIZE / 2.0f;
  int radius = (clampedSize * 3) / 4 + 1;  // couvre la diagonale de l'icône tournée

  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      float relX = static_cast<float>(dx) * invScale;
      float relY = static_cast<float>(dy) * invScale;
      // Rotation inverse (angle opposé) vers le repère de l'icône.
      int srcX = static_cast<int>(floorf(relX * cosA + relY * sinA + halfSrc));
      int srcY = static_cast<int>(floorf(-relX * sinA + relY * cosA + halfSrc));
      if (srcX < 0 || srcY < 0 || srcX >= WIND_ICON_SIZE || srcY >= WIND_ICON_SIZE) {
        continue;
      }
      size_t bitIndex = static_cast<size_t>(srcY) * WIND_ICON_SIZE + srcX;
      uint8_t byteValue = pgm_read_byte(epd_bitmap_wind_north + bitIndex / 8);
      bool pixelBlack = ((byteValue >> (7 - (bitIndex % 8))) & 0x01) == 0;
      if (!pixelBlack) {
        continue;
      }
      int dstX = centerX + dx;
      int dstY = centerY + dy;
      if (dstX < 0 || dstY < 0 || dstX >= display.width() || dstY >= display.height()) {
        continue;
      }
      display.drawPixel(dstX, dstY, GxEPD_BLACK);
    }
  }
}

bool loadForecastFromJson(const char* payload, size_t length) {
  if (payload == nullptr) {
    return false;
  }

  // Le message fait ~17 Ko et 73 points de 10 champs : ne garder que les
  // champs utilisés divise par plusieurs la mémoire du document JSON.
  JsonDocument filter;
  for (const char* key : {"datetime", "time", "wind_bearing", "wind_speed", "wind_gust_speed", "condition"}) {
    filter["weather.trelevern"]["forecast"][0][key] = true;
    filter["forecast"][0][key] = true;
  }

  JsonDocument doc;
  const auto filterOption = DeserializationOption::Filter(filter);
  DeserializationError err = length > 0
                             ? deserializeJson(doc, payload, length, filterOption)
                             : deserializeJson(doc, payload, filterOption);
  if (err) {
    STATION_LOG_PRINT("Erreur JSON prévision: ");
    STATION_LOG_PRINTLN(err.f_str());
    return false;
  }

  JsonVariant rootVariant = doc["weather.trelevern"];
  JsonObject root = rootVariant.isNull() ? doc.as<JsonObject>() : rootVariant.as<JsonObject>();
  JsonArray forecast = root["forecast"].as<JsonArray>();
  if (forecast.isNull() || forecast.size() == 0) {
    STATION_LOG_PRINTLN("Forecast JSON: tableau forecast vide");
    return false;
  }

  gForecastCount = 0;
  for (JsonObject entry : forecast) {
    if (gForecastCount >= MAX_FORECAST_POINTS) {
      break;
    }
    String dateBuffer;
    JsonVariantConst datetimeVar = entry["datetime"];
    if (!datetimeVar.isNull()) {
      dateBuffer = datetimeVar.as<String>();
    } else {
      JsonVariantConst timeVar = entry["time"];
      if (!timeVar.isNull()) {
        dateBuffer = timeVar.as<String>();
      }
    }

    if (!dateBuffer.length()) {
      STATION_LOG_PRINTLN("Forecast entry sans datetime/time");
      continue;
    }

    time_t ts = 0;
    if (!parseIso8601ToTime(dateBuffer.c_str(), ts)) {
      STATION_LOG_PRINT("Forecast entry datetime invalide: ");
      STATION_LOG_PRINTLN(dateBuffer);
      continue;
    }
    float bearing = entry["wind_bearing"].isNull() ? NAN : entry["wind_bearing"].as<float>();
    if (!isnan(bearing)) {
      bearing = fmodf(bearing, 360.0f);
      if (bearing < 0) bearing += 360.0f;
    }
    float speed = entry["wind_speed"].isNull() ? NAN : entry["wind_speed"].as<float>();
    float gust = entry["wind_gust_speed"].isNull() ? NAN : entry["wind_gust_speed"].as<float>();

    ForecastPoint& point = gForecastPoints[gForecastCount];
    point.ts = ts;
    point.bearing = bearing;
    point.speed = speed;
    point.gust = gust;
    strncpy(point.condition, entry["condition"] | "", sizeof(point.condition) - 1);
    point.condition[sizeof(point.condition) - 1] = '\0';
    ++gForecastCount;
  }

  if (gForecastCount == 0) {
    STATION_LOG_PRINTLN("Forecast JSON: aucune entrée valide");
    return false;
  }

  return updateForecastFromStoredData();
}

bool updateForecastFromStoredData() {
  if (gForecastCount == 0) {
    return false;
  }

  time_t nowTs = 0;
  if (!currentEpoch(nowTs)) {
    return false;
  }

  struct tm nowTm;
  char nowBuf[20] = "--";
  if (localtime_r(&nowTs, &nowTm)) {
    strftime(nowBuf, sizeof(nowBuf), "%Y-%m-%d %H:%M", &nowTm);
  }
  STATION_LOG_PRINTF("[Forecast] nowTs=%s count=%d\n", nowBuf, gForecastCount);

  int targetIdx = -1;
  // Chercher le point le plus récent dont le timestamp est <= maintenant
  for (int i = gForecastCount - 1; i >= 0; --i) {
    if (gForecastPoints[i].ts <= nowTs) {
      targetIdx = i;
      break;
    }
  }
  // Si tous les points sont dans le futur, prendre le premier
  if (targetIdx == -1) {
    targetIdx = 0;
  }

  const ForecastPoint& point = gForecastPoints[targetIdx];
  float newBearing = point.bearing;
  float newSpeed = point.speed;
  float newGust = point.gust;
  const char* newCondition = point.condition;
  time_t newEpoch = point.ts;
  // Prévision qui n'a plus été renouvelée : ne pas figer un vent ancien.
  if (nowTs - point.ts > FORECAST_STALE_AFTER_S) {
    newBearing = NAN;
    newSpeed = NAN;
    newGust = NAN;
    newCondition = "";
  }

  struct tm pointTm;
  char timeBuf[20] = "--";
  if (localtime_r(&newEpoch, &pointTm)) {
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &pointTm);
  }
  STATION_LOG_PRINTF("[Forecast] idx=%d ts=%s bearing=%.1f speed=%.1f\n",
                     targetIdx, timeBuf,
                     isnan(newBearing) ? -1.0f : newBearing,
                     isnan(newSpeed) ? -1.0f : newSpeed);

  bool changed = (forecastLastUpdateEpoch != newEpoch);
  if (!changed) {
    if ((isnan(windBearingDegrees) != isnan(newBearing)) || (!isnan(newBearing) && fabsf(windBearingDegrees - newBearing) > 0.1f)) {
      changed = true;
    }
    if ((isnan(windSpeedKmh) != isnan(newSpeed)) || (!isnan(newSpeed) && fabsf(windSpeedKmh - newSpeed) > 0.1f)) {
      changed = true;
    }
    if ((isnan(windGustKmh) != isnan(newGust)) || (!isnan(newGust) && fabsf(windGustKmh - newGust) > 0.1f)) {
      changed = true;
    }
    if (strcmp(weatherCondition, newCondition) != 0) {
      changed = true;
    }
  }

  if (changed) {
    windBearingDegrees = newBearing;
    windSpeedKmh = newSpeed;
    windGustKmh = newGust;
    strncpy(weatherCondition, newCondition, sizeof(weatherCondition) - 1);
    weatherCondition[sizeof(weatherCondition) - 1] = '\0';
    forecastLastUpdateEpoch = newEpoch;
    gExteriorNeedsUpdate = true;
  }

  return changed;
}