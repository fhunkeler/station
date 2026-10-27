#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>

#include "station_fonts.h"
#include "moon_icons.h"
#include <qrcode.h>
#include "station_config.h"
#include "web_portal.h"

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

// WiFi et MQTT : identifiants et adresses dans gConfig (station_config.h).
constexpr const char* TZ_EUROPE_PARIS = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr const char* NTP_SERVER_1 = "fr.pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "pool.ntp.org";
constexpr const char* NTP_SERVER_3 = "time.google.com";

// Une seule tentative de connexion par intervalle : la boucle ne doit jamais
// rester bloquée plusieurs secondes, sinon l'horloge affichée prend du retard.
constexpr unsigned long NETWORK_RETRY_INTERVAL_MS = 30000UL;
constexpr unsigned long WIFI_FORCE_RECONNECT_AFTER_MS = 120000UL;
constexpr uint16_t MQTT_SOCKET_TIMEOUT_S = 5;
constexpr uint16_t MQTT_KEEPALIVE_S = 60;
// L'âge maximal d'une mesure est réglable (gConfig.staleHours, 3 h par
// défaut : HA publie au moins toutes les heures).
constexpr time_t FORECAST_STALE_AFTER_S = 3 * 3600;
// Rafraîchissement complet périodique contre la rémanence des mises à jour partielles.
constexpr int FULL_REFRESH_EVERY_MINUTES = 60;
// Nettoyage noir/blanc quotidien à gConfig.ghostCleaningHour (4 h par défaut).
// En mode configuration, nouvel essai du WiFi de la maison à cet intervalle,
// seulement si personne n'est connecté au point d'accès.
constexpr unsigned long CONFIG_MODE_WIFI_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long CONFIG_MODE_FIRST_RETRY_MS = 30000UL;
// Fenêtre pendant laquelle un second reset demande le mode configuration.
constexpr unsigned long DOUBLE_RESET_WINDOW_MS = 10000UL;
// Un horodatage antérieur à 2020 signifie que l'horloge n'a jamais été réglée.
constexpr time_t MIN_VALID_EPOCH = 1577836800;
// Sujets MQTT : réglables un par un (gConfig, mode expert de la page web).
const char* const TOPIC_TIDE = gConfig.topicTide;
const char* const TOPIC_OUTDOOR = gConfig.topicOutdoor;
const char* const TOPIC_GROUND = gConfig.topicGround;
const char* const TOPIC_FLOOR = gConfig.topicFloor;
const char* const TOPIC_FORECAST = gConfig.topicForecast;
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

// Zones de l'écran, chacune rafraîchie par une fenêtre partielle : en-tête,
// bande du milieu partagée entre le vent (à gauche) et l'horloge des marées
// (à droite), puis les mesures. La coupure à x = 200 respecte l'alignement
// des fenêtres partielles sur 8 px.
constexpr int16_t SCREEN_MARGIN = 12;
constexpr int16_t HEADER_HEIGHT = 96;
constexpr int16_t MIDDLE_HEIGHT = 112;
constexpr int16_t MIDDLE_SPLIT_X = 200;
const Rect gHeaderArea = {0, 0, 400, HEADER_HEIGHT};
const Rect gWindArea = {0, HEADER_HEIGHT, MIDDLE_SPLIT_X, MIDDLE_HEIGHT};
const Rect gTideArea = {MIDDLE_SPLIT_X, HEADER_HEIGHT, 400 - MIDDLE_SPLIT_X, MIDDLE_HEIGHT};
const Rect gBottomArea = {0, HEADER_HEIGHT + MIDDLE_HEIGHT, 400, 300 - HEADER_HEIGHT - MIDDLE_HEIGHT};

// L'aiguille de l'horloge des marées avance toutes les 10 minutes.
constexpr int TIDE_CHART_REFRESH_MINUTES = 10;

WiFiClient gMqttWiFiClient;
PubSubClient gMqttClient(gMqttWiFiClient);
bool gExteriorNeedsUpdate = false;
bool gGroundNeedsUpdate = false;
bool gUpperNeedsUpdate = false;
bool gTideNeedsUpdate = false;

// Sujets MQTT déjà reçus depuis le démarrage (un bit par sujet), pour
// l'écran de démarrage.
const char* const STATION_TOPICS[] = {TOPIC_TIDE, TOPIC_OUTDOOR, TOPIC_GROUND, TOPIC_FLOOR, TOPIC_FORECAST};
constexpr int STATION_TOPIC_COUNT = sizeof(STATION_TOPICS) / sizeof(STATION_TOPICS[0]);

// Mode configuration : point d'accès ouvert et écran d'instructions affiché
// à la place des mesures.
enum class ConfigReason : uint8_t { NONE, NO_WIFI_SETTINGS, WIFI_UNREACHABLE, REQUESTED };
ConfigReason gConfigReason = ConfigReason::NONE;
// Mise à jour du firmware en cours : l'écran et MQTT restent en pause.
bool gFirmwareUpdating = false;
uint8_t gReceivedTopicsMask = 0;

// Étapes affichées par l'écran de démarrage.
enum BootStepState : uint8_t { STEP_PENDING, STEP_RUNNING, STEP_DONE, STEP_FAILED };
struct BootStep {
  const char* label;
  BootStepState state;
  char detail[24];
};
enum { BOOT_STEP_WIFI, BOOT_STEP_MQTT, BOOT_STEP_TIME, BOOT_STEP_DATA, BOOT_STEP_COUNT };
BootStep gBootSteps[BOOT_STEP_COUNT] = {
  {"WiFi", STEP_PENDING, ""},
  {"Serveur MQTT", STEP_PENDING, ""},
  {"Heure (NTP)", STEP_PENDING, ""},
  {"Données", STEP_PENDING, ""},
};

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

const GFXfont* gCurrentFont = nullptr;

void useFont(const GFXfont* font) {
  gCurrentFont = font;
  display.setFont(font);
}

// Les sources sont en UTF-8, les polices GFX indexent des octets : convertit
// en Latin-1 si la police courante couvre ces caractères, sinon remplace les
// lettres accentuées par leur équivalent sans accent.
void toFontEncoding(const char* utf8, char* out, size_t outSize) {
  static const char ASCII_FOR_LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
  const uint8_t lastGlyph = gCurrentFont != nullptr ? pgm_read_byte(&gCurrentFont->last) : 0x7E;
  size_t n = 0;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8); *p != 0 && n + 1 < outSize; ++p) {
    uint16_t code = *p;
    if ((code & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      code = static_cast<uint16_t>(((code & 0x1F) << 6) | (p[1] & 0x3F));
      ++p;
    } else if (code >= 0x80) {
      code = '?';  // séquence plus longue ou invalide : hors Latin-1
      while ((p[1] & 0xC0) == 0x80) {
        ++p;
      }
    }
    if (code > lastGlyph) {
      code = (code >= 0xC0 && code <= 0xFF) ? ASCII_FOR_LATIN1[code - 0xC0] : '?';
    }
    out[n++] = static_cast<char>(code);
  }
  out[n] = '\0';
}

bool currentFontHasLatin1() {
  return gCurrentFont != nullptr && pgm_read_byte(&gCurrentFont->last) >= 0xFF;
}

// Écrit text avec la police courante, aligné horizontalement sur x.
// Renvoie l'abscisse du bord droit du texte.
int printAligned(const char* utf8, int x, int baseline, TextAlign align) {
  char text[48];
  toFontEncoding(utf8, text, sizeof(text));
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
void printLabel(const char* utf8, int x, int baseline, TextAlign align, int minX, int maxX) {
  char text[48];
  toFontEncoding(utf8, text, sizeof(text));
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
// pas de "°"). "--" si la valeur est inconnue.
void drawTemperature(float value, int x, int baseline, TextAlign align, const GFXfont* font, int degreeRadius) {
  char text[12];
  if (isnan(value)) {
    strcpy(text, "--");
  } else {
    snprintf(text, sizeof(text), "%.1f", value);
  }
  useFont(font);
  // Une police Latin-1 a son propre signe degré, plus net qu'un cercle dessiné.
  const bool showDegree = !isnan(value);
  const bool fontHasDegree = currentFontHasLatin1();
  if (showDegree && fontHasDegree) {
    strcat(text, "\xB0");
  }
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  const int degreeWidth = (showDegree && !fontHasDegree) ? 2 * degreeRadius + 3 : 0;
  const int totalWidth = static_cast<int>(bw) + degreeWidth;
  int left = x;
  if (align == ALIGN_CENTER) {
    left = x - totalWidth / 2;
  } else if (align == ALIGN_RIGHT) {
    left = x - totalWidth;
  }
  display.setCursor(left - bx, baseline);
  display.print(text);
  if (showDegree && !fontHasDegree) {
    const int cx = left + bw + 3 + degreeRadius;
    const int cy = baseline + by + degreeRadius;
    display.drawCircle(cx, cy, degreeRadius, GxEPD_BLACK);
    display.drawCircle(cx, cy, degreeRadius - 1, GxEPD_BLACK);
  }
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

const char* const FRENCH_DAY_NAMES[7] = {
  "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi"
};

const char* const FRENCH_MONTH_NAMES[12] = {
  "janvier", "février", "mars", "avril", "mai", "juin",
  "juillet", "août", "septembre", "octobre", "novembre", "décembre"
};

// Largeur et ascendante d'un texte dans une police.
void measureText(const GFXfont* font, const char* text, int& width, int& ascent) {
  useFont(font);
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  width = bw;
  ascent = -by;
}

void drawHeaderContent() {
  display.setTextColor(GxEPD_BLACK);
  const int width = display.width();
  const int bandHeight = gHeaderArea.h - 4;  // au-dessus du filet

  char timeStr[6] = "--:--";
  char dateStr[24] = "";
  if (gTimeValid) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", gNow.tm_hour, gNow.tm_min);
    snprintf(dateStr, sizeof(dateStr), "%d %s", gNow.tm_mday, FRENCH_MONTH_NAMES[gNow.tm_mon % 12]);
  }
  char moonText[16] = "";
  if (moonIllumination >= 0) {
    snprintf(moonText, sizeof(moonText), "%d%%", moonIllumination);
  }

  {
    // Heure à gauche, centrée verticalement ; jour et date à sa droite ;
    // lune contre le bord droit.
    const GFXfont* clockFont = FONT_CLOCK;
    int clockWidth, clockAscent;
    measureText(clockFont, "00:00", clockWidth, clockAscent);
    const int clockBaseline = (bandHeight + clockAscent) / 2;
    useFont(clockFont);
    const int timeRight = printAligned(timeStr, SCREEN_MARGIN, clockBaseline, ALIGN_LEFT);

    const int moonCenterX = width - SCREEN_MARGIN - MOON_ICON_SIZE / 2;
    if (gTimeValid) {
      const int dateX = timeRight + 14;
      const int middle = bandHeight / 2;
      useFont(FONT_MEDIUM_BOLD);
      printAligned(FRENCH_DAY_NAMES[gNow.tm_wday % 7], dateX, middle - 4, ALIGN_LEFT);
      useFont(FONT_MEDIUM);
      printAligned(dateStr, dateX, middle + 18, ALIGN_LEFT);
    }
    if (moonIllumination >= 0) {
      // Lune au-dessus de son pourcentage, pour laisser la place à la date.
      useFont(FONT_SMALL);
      const int moonCenterY = bandHeight / 2 - 10;
      drawMoonPhase(moonCenterX, moonCenterY, moonPhase);
      printAligned(moonText, moonCenterX, moonCenterY + MOON_ICON_SIZE / 2 + 18, ALIGN_CENTER);
    }
  }

  display.fillRect(SCREEN_MARGIN, gHeaderArea.y + gHeaderArea.h - 3, width - 2 * SCREEN_MARGIN, 2, GxEPD_BLACK);
}

// Remplit le polygone convexe pts (coordonnées en unités de rayon, aiguille
// pointant vers le haut) tourné de angleDeg dans le sens horaire autour de
// (cx, cy).
void fillRotatedTriangle(int cx, int cy, float radius, float angleDeg,
                         float ax, float ay, float bx, float by, float qx, float qy) {
  const float angle = radians(angleDeg);
  const float c = cosf(angle);
  const float s = sinf(angle);
  auto px = [&](float ux, float uy) { return static_cast<int16_t>(cx + roundf((ux * c - uy * s) * radius)); };
  auto py = [&](float ux, float uy) { return static_cast<int16_t>(cy + roundf((ux * s + uy * c) * radius)); };
  display.fillTriangle(px(ax, ay), py(ax, ay), px(bx, by), py(bx, by), px(qx, qy), py(qx, qy), GxEPD_BLACK);
}

// Horloge des marées : pleine mer en haut, basse mer en bas, repérées par
// des graduations plus longues. L'aiguille tourne dans le sens horaire ; à
// droite la mer descend, à gauche elle monte. angleDeg : 0 = pleine mer,
// 180 = basse mer.
void drawTideDial(int cx, int cy, int radius, float angleDeg) {
  display.drawCircle(cx, cy, radius, GxEPD_BLACK);
  display.drawCircle(cx, cy, radius - 1, GxEPD_BLACK);
  // Douze graduations fines, environ une par heure (cycle de 12 h 25) ; pleine
  // mer, basse mer et quarts un peu plus longues.
  for (int i = 0; i < 12; ++i) {
    const float a = radians(i * 30.0f);
    const float sx = sinf(a);
    const float sy = -cosf(a);
    const int inner = (i % 3 == 0) ? radius - 9 : radius - 5;
    display.drawLine(cx + roundf(sx * inner), cy + roundf(sy * inner),
                     cx + roundf(sx * (radius - 2)), cy + roundf(sy * (radius - 2)), GxEPD_BLACK);
  }

  if (!isnan(angleDeg)) {
    // Aiguille pleine longueur avec contrepoids, comme une aiguille d'horloge.
    const float r = radius;
    fillRotatedTriangle(cx, cy, r, angleDeg, 0.0f, -0.86f, 0.2f, -0.5f, -0.2f, -0.5f);    // pointe
    fillRotatedTriangle(cx, cy, r, angleDeg, -0.08f, -0.52f, 0.08f, -0.52f, 0.08f, 0.25f);  // tige
    fillRotatedTriangle(cx, cy, r, angleDeg, -0.08f, -0.52f, 0.08f, 0.25f, -0.08f, 0.25f);
    display.fillCircle(cx, cy, 6, GxEPD_BLACK);
    display.fillCircle(cx, cy, 2, GxEPD_WHITE);
  }
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

// Moitié droite de la bande du milieu : horloge des marées et prochaine étale.
void drawTideContent() {
  display.setTextColor(GxEPD_BLACK);
  const int left = gTideArea.x + 12;
  const int right = gTideArea.x + gTideArea.w - SCREEN_MARGIN;
  const int y0 = gTideArea.y;
  const int titleBaseline = y0 + 20;
  // Séparateur avec la moitié vent.
  display.drawFastVLine(gTideArea.x, y0 + 10, gTideArea.h - 20, GxEPD_BLACK);

  time_t nowTs = 0;
  const int nextIdx = findTideEventIndex(nextTideEpoch);
  const int prevIdx = findTideEventIndex(prevTideEpoch);
  const bool known = currentEpoch(nowTs) && nextTideEpoch != 0 && nextIdx >= 0;

  useFont(FONT_SMALL_BOLD);
  // Le sens (montante, descendante) se lit sur le cadran : le titre reste
  // court pour laisser la place au coefficient.
  printAligned("MARÉE", left, titleBaseline, ALIGN_LEFT);

  constexpr int DIAL_RADIUS = 37;
  const int dialCx = gTideArea.x + 8 + DIAL_RADIUS;
  const int dialCy = y0 + 28 + DIAL_RADIUS;
  if (!known) {
    drawTideDial(dialCx, dialCy, DIAL_RADIUS, NAN);
    useFont(FONT_SMALL);
    printAligned("Données", right, dialCy - 4, ALIGN_RIGHT);
    printAligned("indisponibles", right, dialCy + 14, ALIGN_RIGHT);
    return;
  }

  const TideEvent& next = gTideEvents[nextIdx];
  float angle = NAN;
  if (prevIdx >= 0 && next.ts > gTideEvents[prevIdx].ts) {
    const TideEvent& prev = gTideEvents[prevIdx];
    const float progress =
        constrain(static_cast<float>(nowTs - prev.ts) / static_cast<float>(next.ts - prev.ts), 0.0f, 1.0f);
    const bool fromHigh = strcmp(prev.type, "HIGH") == 0;
    angle = (fromHigh ? 0.0f : 180.0f) + 180.0f * progress;
  }
  drawTideDial(dialCx, dialCy, DIAL_RADIUS, angle);

  // Prochaine étale, à droite du cadran.
  const bool nextHigh = strcmp(next.type, "HIGH") == 0;
  struct tm eventTm;
  char timeStr[6] = "--:--";
  if (localtime_r(&next.ts, &eventTm) != nullptr) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", eventTm.tm_hour, eventTm.tm_min);
  }
  char heightStr[16] = "";
  if (!isnan(next.height)) {
    snprintf(heightStr, sizeof(heightStr), "%.1f m", next.height);
  }
  char coefStr[16];
  if (tideCoefficient >= 0) {
    snprintf(coefStr, sizeof(coefStr), "Coef %d", tideCoefficient);
  } else {
    strcpy(coefStr, "Coef --");
  }

  // Prochaine marée : type, heure en grand, hauteur.
  useFont(FONT_SMALL);
  printAligned(nextHigh ? "Pleine mer" : "Basse mer", right, y0 + 46, ALIGN_RIGHT);
  useFont(FONT_LARGE);
  printAligned(timeStr, right, y0 + 82, ALIGN_RIGHT);
  useFont(FONT_SMALL);
  printAligned(heightStr, right, y0 + 102, ALIGN_RIGHT);
  useFont(FONT_SMALL_BOLD);
  printAligned(coefStr, right, titleBaseline, ALIGN_RIGHT);
}

// Moitié gauche de la bande du milieu : vent actuel.
void drawWindContent() {
  display.setTextColor(GxEPD_BLACK);
  const int y0 = gWindArea.y;
  const int left = SCREEN_MARGIN;

  useFont(FONT_SMALL_BOLD);
  printAligned("VENT", left, y0 + 20, ALIGN_LEFT);

  if (isnan(windSpeedKmh) || isnan(windBearingDegrees)) {
    useFont(FONT_MEDIUM);
    printAligned("--", left, y0 + 66, ALIGN_LEFT);
    return;
  }

  constexpr int ARROW_SIZE = 56;
  const int arrowCenterX = left + ARROW_SIZE / 2;
  const int arrowCenterY = y0 + 30 + ARROW_SIZE / 2;
  drawWindArrow(arrowCenterX, arrowCenterY, windBearingDegrees, ARROW_SIZE);

  const int textX = left + ARROW_SIZE + 12;
  char speedStr[8];
  snprintf(speedStr, sizeof(speedStr), "%.0f", windSpeedKmh);
  useFont(FONT_LARGE);
  const int speedRight = printAligned(speedStr, textX, y0 + 64, ALIGN_LEFT);
  useFont(FONT_SMALL);
  printAligned("km/h", speedRight + 5, y0 + 64, ALIGN_LEFT);
  useFont(FONT_SMALL_BOLD);
  printAligned(compassPoint(windBearingDegrees), speedRight + 5, y0 + 44, ALIGN_LEFT);

  char gustStr[24];
  if (isnan(windGustKmh)) {
    strcpy(gustStr, "Rafales --");
  } else {
    snprintf(gustStr, sizeof(gustStr), "Rafales %.0f", windGustKmh);
  }
  useFont(FONT_SMALL);
  printAligned(gustStr, textX, y0 + 90, ALIGN_LEFT);
}

// Hauteur au-dessus de la ligne de base de sample dans la police font.
int fontAscent(const GFXfont* font, const char* sample) {
  useFont(font);
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(sample, 0, 0, &bx, &by, &bw, &bh);
  return -by;
}

// Une colonne de mesure : titre, grande température, une ou deux lignes de
// détail. Les lignes s'empilent d'après les métriques des polices, pour que
// la mise en page tienne quel que soit le thème typographique.
void drawMeasureColumn(const char* title, int centerX, float temperature, const char* detail, const char* detail2) {
  const bool compact = gBottomArea.h < 100;
  // Ligne de base du titre à 19 px au moins : les capitales accentuées
  // (É) dépassent vers le haut et toucheraient le filet de la bande.
  const int titleBaseline = gBottomArea.y + 19;
  // L'échantillon inclut le signe degré, qui dépasse le haut des chiffres.
  const int temperatureBaseline = titleBaseline + (compact ? 5 : 10) + fontAscent(FONT_LARGE, "0\xB0");
  const int detailBaseline = temperatureBaseline + (compact ? 3 : 7) + fontAscent(FONT_MEDIUM, "C");
  const int detail2Baseline = detailBaseline + 3 + fontAscent(FONT_SMALL, "0");

  useFont(FONT_SMALL_BOLD);
  printAligned(title, centerX, titleBaseline, ALIGN_CENTER);
  drawTemperature(temperature, centerX, temperatureBaseline, ALIGN_CENTER, FONT_LARGE, 4);
  useFont(FONT_MEDIUM);
  printAligned(detail, centerX, detailBaseline, ALIGN_CENTER);
  if (detail2 != nullptr) {
    useFont(FONT_SMALL);
    printAligned(detail2, centerX, detail2Baseline, ALIGN_CENTER);
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

  drawMeasureColumn(gConfig.labelOutdoor, col1, tempExterior, pressureStr, nullptr);
  drawMeasureColumn(gConfig.labelGround, col2, tempRDC, humRdcStr, nullptr);
  drawMeasureColumn(gConfig.labelFloor, col3, tempEtage, humEtageStr, nullptr);

  display.fillRect(SCREEN_MARGIN, gBottomArea.y, width - 2 * SCREEN_MARGIN, 2, GxEPD_BLACK);
  const int separatorTop = gBottomArea.y + 10;
  const int separatorHeight = gBottomArea.h - 18;
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
  refreshArea(gHeaderArea, drawHeaderContent);
}

void updateTideSection() {
  refreshArea(gTideArea, drawTideContent);
}

void updateWindSection() {
  refreshArea(gWindArea, drawWindContent);
}

void updateBottomSection() {
  refreshArea(gBottomArea, drawBottomContent);
}

// Pastille d'état d'une étape : cercle vide (en attente), cercle et point
// (en cours), disque coché (terminée), disque barré (échec).
void drawBootStepMarker(int cx, int cy, BootStepState state) {
  const int r = 8;
  switch (state) {
    case STEP_PENDING:
      display.drawCircle(cx, cy, r, GxEPD_BLACK);
      break;
    case STEP_RUNNING:
      display.drawCircle(cx, cy, r, GxEPD_BLACK);
      display.drawCircle(cx, cy, r - 1, GxEPD_BLACK);
      display.fillCircle(cx, cy, 3, GxEPD_BLACK);
      break;
    case STEP_DONE:
      display.fillCircle(cx, cy, r, GxEPD_BLACK);
      for (int t = 0; t < 2; ++t) {
        display.drawLine(cx - 4, cy + t, cx - 1, cy + 3 + t, GxEPD_WHITE);
        display.drawLine(cx - 1, cy + 3 + t, cx + 4, cy - 3 + t, GxEPD_WHITE);
      }
      break;
    case STEP_FAILED:
      display.fillCircle(cx, cy, r, GxEPD_BLACK);
      for (int t = 0; t < 2; ++t) {
        display.drawLine(cx - 3 + t, cy - 3, cx + 3 + t, cy + 3, GxEPD_WHITE);
        display.drawLine(cx + 3 + t, cy - 3, cx - 3 + t, cy + 3, GxEPD_WHITE);
      }
      break;
  }
}

void drawSplashContent() {
  display.setTextColor(GxEPD_BLACK);
  const int width = display.width();
  const int centerX = width / 2;

  useFont(FONT_LARGE);
  printAligned("Station météo", centerX, 62, ALIGN_CENTER);
  bool anyFailed = false;
  for (const BootStep& step : gBootSteps) {
    anyFailed |= step.state == STEP_FAILED;
  }
  useFont(FONT_MEDIUM);
  printAligned(anyFailed ? "Démarrage incomplet" : "Démarrage en cours", centerX, 94, ALIGN_CENTER);
  display.fillRect(width / 10, 112, (width * 8) / 10, 2, GxEPD_BLACK);

  const int markerX = 70;
  const int labelX = markerX + 18;
  const int detailRight = width - 60;
  for (int i = 0; i < BOOT_STEP_COUNT; ++i) {
    const BootStep& step = gBootSteps[i];
    const int baseline = 150 + i * 32;
    drawBootStepMarker(markerX, baseline - 7, step.state);
    useFont(step.state == STEP_RUNNING ? FONT_MEDIUM_BOLD : FONT_MEDIUM);
    printAligned(step.label, labelX, baseline, ALIGN_LEFT);
    const char* detail = step.detail;
    if (detail[0] == '\0') {
      detail = step.state == STEP_RUNNING ? "en cours..." : (step.state == STEP_FAILED ? "échec" : "");
    }
    useFont(FONT_SMALL);
    printAligned(detail, detailRight, baseline, ALIGN_RIGHT);
  }

  char footer[64];
  snprintf(footer, sizeof(footer), "Réglages : http://%s.local", gConfig.hostname);
  useFont(FONT_SMALL);
  printAligned(footer, centerX, display.height() - 8, ALIGN_CENTER);
}

// Date de compilation en français ("26 octobre 2026"), __DATE__ valant
// "Oct 26 2026".
String firmwareDate() {
  static const char* const BUILD_MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* monthPos = strstr(BUILD_MONTHS, String(__DATE__).substring(0, 3).c_str());
  const int buildMonth = monthPos != nullptr ? static_cast<int>(monthPos - BUILD_MONTHS) / 3 : 0;
  char buildStr[32];
  snprintf(buildStr, sizeof(buildStr), "%d %s %s", atoi(__DATE__ + 4), FRENCH_MONTH_NAMES[buildMonth], __DATE__ + 7);
  return String(buildStr);
}

void updateSplashScreen() {
  refreshArea(Rect{0, 0, static_cast<int16_t>(display.width()), static_cast<int16_t>(display.height())},
              drawSplashContent);
}

// Met à jour gBootSteps d'après l'état du réseau. Renvoie vrai si l'écran de
// démarrage doit être redessiné.
bool refreshBootSteps() {
  BootStep previous[BOOT_STEP_COUNT];
  memcpy(previous, gBootSteps, sizeof(previous));

  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  const bool mqttUp = gMqttClient.connected();
  int received = 0;
  for (int i = 0; i < STATION_TOPIC_COUNT; ++i) {
    received += (gReceivedTopicsMask >> i) & 1U;
  }

  BootStep& wifi = gBootSteps[BOOT_STEP_WIFI];
  wifi.state = wifiUp ? STEP_DONE : STEP_RUNNING;
  if (wifiUp) {
    snprintf(wifi.detail, sizeof(wifi.detail), "%s", WiFi.localIP().toString().c_str());
  }

  BootStep& mqtt = gBootSteps[BOOT_STEP_MQTT];
  mqtt.state = mqttUp ? STEP_DONE : (wifiUp ? STEP_RUNNING : STEP_PENDING);
  snprintf(mqtt.detail, sizeof(mqtt.detail), "%s", mqttUp ? gConfig.mqttHost : "");

  BootStep& clock = gBootSteps[BOOT_STEP_TIME];
  clock.state = gTimeValid ? STEP_DONE : (wifiUp ? STEP_RUNNING : STEP_PENDING);
  if (gTimeValid) {
    snprintf(clock.detail, sizeof(clock.detail), "%02d:%02d", gNow.tm_hour, gNow.tm_min);
  }

  BootStep& data = gBootSteps[BOOT_STEP_DATA];
  data.state = received == STATION_TOPIC_COUNT ? STEP_DONE : (mqttUp ? STEP_RUNNING : STEP_PENDING);
  if (received > 0) {
    snprintf(data.detail, sizeof(data.detail), "%d/%d sources", received, STATION_TOPIC_COUNT);
  }

  return memcmp(previous, gBootSteps, sizeof(previous)) != 0;
}

// Efface la rémanence profonde (restes d'images précédentes visibles en
// transparence) : écran entièrement noir puis entièrement blanc, en mode
// complet. Plus long et plus visible qu'un rafraîchissement normal, il n'est
// fait qu'au démarrage et une fois par nuit.
void clearPanelGhosts() {
  for (uint16_t color : {GxEPD_BLACK, GxEPD_WHITE}) {
    display.setFullWindow();
    display.firstPage();
    do {
      display.fillScreen(color);
    } while (display.nextPage());
  }
}

// Rafraîchissement complet d'un écran entier. Il se fait en deux temps,
// comme à l'origine : passage au blanc en mode complet, puis contenu en mode
// partiel. Dessiné directement par le mode complet rapide du 4,2", le
// contenu laisse un fond grisâtre.
void showFullScreen(void (*draw)()) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());

  display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    draw();
  } while (display.nextPage());
}

void drawAllSections() {
  drawHeaderContent();
  drawTideContent();
  drawWindContent();
  drawBottomContent();
}

// Rafraîchissement complet de l'écran principal : efface aussi la rémanence
// laissée par les mises à jour partielles successives.
void displayWeatherStation() {
  showFullScreen(drawAllSections);

  gExteriorNeedsUpdate = false;
  gTideNeedsUpdate = false;
  gGroundNeedsUpdate = false;
  gUpperNeedsUpdate = false;
}

// QR code "rejoindre ce réseau WiFi", reconnu par l'appareil photo des
// téléphones.
void drawWifiQrCode(int left, int top, int moduleSize, const String& ssid, const String& password) {
  constexpr uint8_t QR_VERSION = 4;  // 33 x 33 modules, jusqu'à 78 caractères
  uint8_t buffer[qrcode_getBufferSize(QR_VERSION)];
  QRCode qr;
  const String payload = String("WIFI:T:WPA;S:") + ssid + ";P:" + password + ";;";
  if (qrcode_initText(&qr, buffer, QR_VERSION, ECC_MEDIUM, payload.c_str()) != 0) {
    return;
  }
  for (uint8_t y = 0; y < qr.size; ++y) {
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        display.fillRect(left + x * moduleSize, top + y * moduleSize, moduleSize, moduleSize, GxEPD_BLACK);
      }
    }
  }
}

void drawConfigScreenContent() {
  display.setTextColor(GxEPD_BLACK);
  const int width = display.width();
  const String ssid = accessPointName();
  const String password = accessPointPassword();

  useFont(FONT_LARGE);
  printAligned("Configuration", width / 2, 44, ALIGN_CENTER);
  char reason[64];
  switch (gConfigReason) {
    case ConfigReason::NO_WIFI_SETTINGS:
      snprintf(reason, sizeof(reason), "Aucun réseau WiFi configuré");
      break;
    case ConfigReason::WIFI_UNREACHABLE:
      snprintf(reason, sizeof(reason), "WiFi « %s » injoignable", gConfig.wifiSsid);
      break;
    default:
      snprintf(reason, sizeof(reason), "Mode configuration demandé");
      break;
  }
  useFont(FONT_MEDIUM);
  printAligned(reason, width / 2, 74, ALIGN_CENTER);
  display.fillRect(SCREEN_MARGIN, 88, width - 2 * SCREEN_MARGIN, 2, GxEPD_BLACK);

  const int x = SCREEN_MARGIN + 4;
  useFont(FONT_SMALL);
  printAligned("1. Rejoindre le réseau WiFi", x, 116, ALIGN_LEFT);
  useFont(FONT_MEDIUM_BOLD);
  printAligned(ssid.c_str(), x + 12, 142, ALIGN_LEFT);
  useFont(FONT_SMALL);
  printAligned("Mot de passe", x + 12, 166, ALIGN_LEFT);
  useFont(FONT_MEDIUM_BOLD);
  printAligned(password.c_str(), x + 12, 192, ALIGN_LEFT);
  useFont(FONT_SMALL);
  printAligned("2. Ouvrir la page", x, 224, ALIGN_LEFT);
  useFont(FONT_MEDIUM_BOLD);
  printAligned("http://192.168.4.1", x + 12, 250, ALIGN_LEFT);

  constexpr int QR_MODULE = 4;
  const int qrSize = 33 * QR_MODULE;
  const int qrLeft = width - SCREEN_MARGIN - qrSize;
  drawWifiQrCode(qrLeft, 104, QR_MODULE, ssid, password);
  useFont(FONT_SMALL);
  printAligned("Scanner pour rejoindre", width - SCREEN_MARGIN, 104 + qrSize + 20, ALIGN_RIGHT);

  printAligned("Les réglages enregistrés redémarrent la station.", width / 2, display.height() - 8, ALIGN_CENTER);
}

// Ouvre le point d'accès de configuration et affiche ses instructions.
void enterConfigMode(ConfigReason reason) {
  gConfigReason = reason;
  if (reason == ConfigReason::WIFI_UNREACHABLE) {
    // Plus de tentatives automatiques en continu : elles balaient les canaux
    // et coupent le téléphone. serviceNetwork() réessaie de loin en loin.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
  }
  webPortalStartAccessPoint();
  STATION_LOG_PRINT("Mode configuration, point d'accès ");
  STATION_LOG_PRINTLN(accessPointName());
  showFullScreen(drawConfigScreenContent);
}

void drawFirmwareUpdateContent() {
  display.setTextColor(GxEPD_BLACK);
  const int centerX = display.width() / 2;
  useFont(FONT_LARGE);
  printAligned("Mise à jour", centerX, 120, ALIGN_CENTER);
  useFont(FONT_MEDIUM);
  printAligned("Installation du nouveau firmware", centerX, 160, ALIGN_CENTER);
  useFont(FONT_SMALL_BOLD);
  printAligned("Ne pas débrancher la station", centerX, 200, ALIGN_CENTER);
  useFont(FONT_SMALL);
  printAligned("Elle redémarrera d'elle-même dans une minute environ.", centerX, 226, ALIGN_CENTER);
}

// Appelée par le portail web au début d'une mise à jour (page web ou
// PlatformIO) : message à l'écran, puis plus rien ne touche à l'écran ni à
// MQTT jusqu'au redémarrage.
void onFirmwareUpdateStart() {
  gFirmwareUpdating = true;
  STATION_LOG_PRINTLN("Mise à jour du firmware");
  gMqttClient.disconnect();
  showFullScreen(drawFirmwareUpdateContent);
}

// Échec de la mise à jour : le firmware actuel continue, on rétablit l'écran.
void onFirmwareUpdateFailed() {
  gFirmwareUpdating = false;
  STATION_LOG_PRINTLN("Échec de la mise à jour du firmware");
  if (gConfigReason != ConfigReason::NONE) {
    showFullScreen(drawConfigScreenContent);
  } else {
    displayWeatherStation();
  }
}

// Le WiFi de la maison est revenu pendant le mode configuration ouvert faute
// de réseau : refermer le point d'accès et reprendre l'affichage normal.
void leaveConfigMode() {
  gConfigReason = ConfigReason::NONE;
  webPortalStopAccessPoint();
  updateDateTimeFromClock();
  updateMoonData();
  updateTideForecastFromStoredEvents();
  updateForecastFromStoredData();
  displayWeatherStation();
}

// Referme la fenêtre de double reset après quelques secondes de marche.
void serviceDoubleResetWindow() {
  static bool closed = false;
  if (!closed && millis() >= DOUBLE_RESET_WINDOW_MS) {
    closed = true;
    closeDoubleResetWindow();
  }
}

// État courant pour la page d'accueil de l'interface web.
void appendStationStatus(String& html) {
  auto row = [&](const char* label, const String& value) {
    html += "<tr><td>";
    html += label;
    html += "</td><td>";
    html += value;
    html += "</td></tr>";
  };
  char buffer[96];

  if (gTimeValid) {
    snprintf(buffer, sizeof(buffer), "%02d:%02d, %s %d %s", gNow.tm_hour, gNow.tm_min,
             FRENCH_DAY_NAMES[gNow.tm_wday % 7], gNow.tm_mday, FRENCH_MONTH_NAMES[gNow.tm_mon % 12]);
    row("Heure", buffer);
  } else {
    row("Heure", "non synchronisée");
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(buffer, sizeof(buffer), " (%d dBm)<br>", WiFi.RSSI());
    row("WiFi", webPortalEscape(gConfig.wifiSsid) + buffer + WiFi.localIP().toString() + " · " +
                    webPortalEscape(gConfig.hostname) + ".local");
  } else {
    row("WiFi", gConfig.wifiSsid[0] != '\0' ? "non connecté" : "non configuré");
  }
  if (webPortalAccessPointActive()) {
    snprintf(buffer, sizeof(buffer), " (%d appareil(s) connecté(s))", webPortalAccessPointClients());
    row("Point d'accès", webPortalEscape(accessPointName().c_str()) + buffer);
  }

  snprintf(buffer, sizeof(buffer), ":%u — %s", gConfig.mqttPort, gMqttClient.connected() ? "connecté" : "déconnecté");
  row("Serveur MQTT", webPortalEscape(gConfig.mqttHost) + buffer);

  String sources;
  for (int i = 0; i < STATION_TOPIC_COUNT; ++i) {
    if (i > 0) {
      sources += ", ";
    }
    sources += webPortalEscape(STATION_TOPICS[i]);
    sources += ((gReceivedTopicsMask >> i) & 1U) ? " ✓" : " ✗";
  }
  row("Sources reçues", sources);

  auto measure = [&](float temperature, int second, const char* unit) {
    String text = isnan(temperature) ? String("--") : String(temperature, 1) + " °C";
    text += " · ";
    text += second >= 0 ? String(second) + unit : String("--");
    return text;
  };
  row(webPortalEscape(gConfig.labelOutdoor).c_str(), measure(tempExterior, pressureAtmospheric, " hPa"));
  row(webPortalEscape(gConfig.labelGround).c_str(), measure(tempRDC, humidityRDC, " %"));
  row(webPortalEscape(gConfig.labelFloor).c_str(), measure(tempEtage, humidityEtage, " %"));

  if (nextTideEpoch != 0) {
    snprintf(buffer, sizeof(buffer), "%s, prochaine étale à %s, coefficient %d",
             isTideRising ? "montante" : "descendante", nextTideTime.c_str(), tideCoefficient);
    row("Marée", buffer);
  } else {
    row("Marée", "données indisponibles");
  }
  if (!isnan(windSpeedKmh) && !isnan(windBearingDegrees)) {
    snprintf(buffer, sizeof(buffer), "%.0f km/h %s, rafales %.0f km/h", windSpeedKmh,
             compassPoint(windBearingDegrees), windGustKmh);
    row("Vent", buffer);
  }

  const unsigned long uptimeMin = millis() / 60000UL;
  snprintf(buffer, sizeof(buffer), "%lu j %lu h %lu min", uptimeMin / 1440UL, (uptimeMin / 60UL) % 24UL, uptimeMin % 60UL);
  row("En marche depuis", buffer);
  snprintf(buffer, sizeof(buffer), "%u Ko libres", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  row("Mémoire", buffer);
  row("Firmware", firmwareDate());
}

// Le panneau reste alimenté entre deux mises à jour : sur ce 4,2", le couper
// (display.powerOff) grise le fond, qui ne s'éclaircit qu'au fil des
// rafraîchissements partiels suivants (constaté sur l'écran réel).
//
// Rafraîchit les zones marquées comme modifiées. C'est le seul endroit qui
// consomme les drapeaux gXxxNeedsUpdate. gExteriorNeedsUpdate couvre aussi
// la prévision : elle alimente la bande vent et la condition météo.
void refreshDirtySections() {
  if (gTideNeedsUpdate) {
    gTideNeedsUpdate = false;
    updateTideSection();
  }
  if (gExteriorNeedsUpdate) {
    updateWindSection();
  }
  if (gExteriorNeedsUpdate || gGroundNeedsUpdate || gUpperNeedsUpdate) {
    gExteriorNeedsUpdate = false;
    gGroundNeedsUpdate = false;
    gUpperNeedsUpdate = false;
    updateBottomSection();
  }}

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
  return (nowTs - measuredAt) > static_cast<time_t>(gConfig.staleHours) * 3600;
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

  if (gTimeValid && gNow.tm_hour == gConfig.ghostCleaningHour && gNow.tm_min == 0) {
    minutesSinceFullRefresh = 0;
    clearPanelGhosts();
    displayWeatherStation();
    return;
  }

  if (++minutesSinceFullRefresh >= FULL_REFRESH_EVERY_MINUTES) {
    minutesSinceFullRefresh = 0;
    displayWeatherStation();
    return;
  }
  updateHeaderSection();
  refreshDirtySections();
}

void setup() {
  STATION_LOG_BEGIN(115200);
  STATION_LOG_PRINTLN("Démarrage de la station météo e-paper");
  STATION_LOG_PRINTF("Processeur à %u MHz\n", static_cast<unsigned>(getCpuFrequencyMhz()));

  // Fuseau posé tout de suite : localtime_r est correct dès la synchro NTP.
  setenv("TZ", TZ_EUROPE_PARIS, 1);
  tzset();

  loadStationConfig();
  const bool configRequested = consumeDoubleReset();

  display.init(DISPLAY_DIAG_BAUD);
  display.setRotation(0); // Mode paysage (400x300)
  display.setTextWrap(false);  // le texte est toujours placé explicitement

  WiFi.mode(WIFI_STA);
  webPortalBegin();
  if (configRequested || gConfig.wifiSsid[0] == '\0') {
    clearPanelGhosts();
    enterConfigMode(configRequested ? ConfigReason::REQUESTED : ConfigReason::NO_WIFI_SETTINGS);
    return;
  }

  // Écran propre, puis écran de démarrage tenu à jour pendant que le réseau
  // fournit l'heure et les messages MQTT retenus : le premier affichage de la
  // station est ainsi complet. Sans réseau, on affiche quand même au bout du
  // délai, en signalant les étapes en échec.
  clearPanelGhosts();
  refreshBootSteps();
  updateSplashScreen();

  // La connexion WiFi prend 8 s environ à 80 MHz, davantage si la box tarde.
  constexpr unsigned long BOOT_NETWORK_WAIT_MS = 30000UL;
  constexpr unsigned long BOOT_RETAINED_WAIT_MS = 3000UL;
  constexpr unsigned long BOOT_SPLASH_FINAL_MS = 1500UL;
  const unsigned long bootStart = millis();
  unsigned long lastProgressAt = 0;
  uint8_t lastTopicsMask = 0;
  bool mqttSeen = false;
  bool bootComplete = false;
  while (millis() - bootStart < BOOT_NETWORK_WAIT_MS) {
    serviceDoubleResetWindow();
    webPortalLoop();
    serviceNetwork();
    if (gMqttClient.connected() && !mqttSeen) {
      mqttSeen = true;
      lastProgressAt = millis();
    }
    if (gReceivedTopicsMask != lastTopicsMask) {
      lastTopicsMask = gReceivedTopicsMask;
      lastProgressAt = millis();
    }
    updateDateTimeFromClock();
    if (refreshBootSteps()) {
      updateSplashScreen();
    }
    const bool allTopics = gBootSteps[BOOT_STEP_DATA].state == STEP_DONE;
    // Un sujet sans message retenu n'arrivera pas : ne plus l'attendre quand
    // rien de nouveau n'est arrivé depuis quelques secondes. Le délai part du
    // dernier message, car chaque mise à jour de l'écran bloque MQTT ~0,6 s.
    if (gTimeValid && mqttSeen && (allTopics || millis() - lastProgressAt >= BOOT_RETAINED_WAIT_MS)) {
      bootComplete = allTopics;
      break;
    }
    delay(50);
  }

  if (!bootComplete) {
    for (BootStep& step : gBootSteps) {
      if (step.state != STEP_DONE) {
        step.state = STEP_FAILED;
      }
    }
  }
  // Laisser lire le bilan avant de passer à l'affichage principal.
  updateSplashScreen();
  delay(BOOT_SPLASH_FINAL_MS);

  if (WiFi.status() != WL_CONNECTED) {
    // Sans WiFi, l'écran principal resterait vide : proposer de corriger les
    // réglages, tout en réessayant le réseau en arrière-plan.
    enterConfigMode(ConfigReason::WIFI_UNREACHABLE);
    return;
  }

  updateDateTimeFromClock();
  updateMoonData();
  updateTideForecastFromStoredEvents();
  updateForecastFromStoredData();
  displayWeatherStation();

  STATION_LOG_PRINTF("Affichage initial terminé (%lu ms après le démarrage)\n", millis());
}

void loop() {
  serviceDoubleResetWindow();
  webPortalLoop();
  if (gFirmwareUpdating) {
    delay(5);
    return;
  }
  serviceNetwork();

  if (gConfigReason != ConfigReason::NONE) {
    if (gConfigReason == ConfigReason::WIFI_UNREACHABLE && WiFi.status() == WL_CONNECTED) {
      leaveConfigMode();
    }
    delay(20);
    return;
  }

  // L'horloge est relue souvent et le rafraîchissement suit le changement de
  // minute : l'affichage reste calé sur la minute réelle même après une
  // opération bloquante.
  if (updateDateTimeFromClock()) {
    onMinuteTick();
  } else {
    refreshDirtySections();
  }

  // Boucle courte : le serveur web doit répondre sans attendre.
  delay(20);
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

  // Aucun réseau configuré, ou mode configuration demandé : seul le point
  // d'accès fonctionne. La radio est unique : en se connectant à la box, le
  // point d'accès suivrait son canal et décrocherait le téléphone.
  if (gConfig.wifiSsid[0] == '\0' || gConfigReason == ConfigReason::REQUESTED) {
    return;
  }

  if (!wifiStarted) {
    wifiStarted = true;
    WiFi.mode(webPortalAccessPointActive() ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname(gConfig.hostname);
    WiFi.setAutoReconnect(true);
    WiFi.begin(gConfig.wifiSsid, gConfig.wifiPassword);
    wifiLostSinceMs = nowMs;
    STATION_LOG_PRINT("Connexion WiFi à ");
    STATION_LOG_PRINTLN(gConfig.wifiSsid);
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Point d'accès ouvert faute de WiFi : les tentatives de connexion font
    // changer la radio de canal et coupent le téléphone connecté. On espace
    // donc les essais, et on n'essaie pas quand quelqu'un est connecté.
    const bool fallbackAccessPoint = gConfigReason == ConfigReason::WIFI_UNREACHABLE;
    // Premier essai rapide : un WiFi simplement lent au démarrage ne doit pas
    // laisser l'écran de configuration affiché pendant 5 minutes.
    static uint8_t fallbackRetries = 0;
    const unsigned long retryAfterMs = !fallbackAccessPoint ? WIFI_FORCE_RECONNECT_AFTER_MS
                                       : (fallbackRetries == 0 ? CONFIG_MODE_FIRST_RETRY_MS : CONFIG_MODE_WIFI_RETRY_MS);
    if (wifiWasConnected) {
      wifiWasConnected = false;
      wifiLostSinceMs = nowMs;
      STATION_LOG_PRINTLN("WiFi perdu");
    } else if (nowMs - wifiLostSinceMs >= retryAfterMs &&
               (!fallbackAccessPoint || webPortalAccessPointClients() == 0)) {
      // Filet de sécurité si la reconnexion automatique ne reprend pas.
      wifiLostSinceMs = nowMs;
      if (fallbackAccessPoint && fallbackRetries < 255) {
        ++fallbackRetries;
      }
      WiFi.disconnect();
      WiFi.begin(gConfig.wifiSsid, gConfig.wifiPassword);
      STATION_LOG_PRINTLN("WiFi: reconnexion forcée");
    }
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    STATION_LOG_PRINT("WiFi connecté, IP: ");
    STATION_LOG_PRINTLN(WiFi.localIP());
    if (!webPortalAccessPointActive()) {
      // Veille radio renforcée : la station n'écoute la box qu'à certaines
      // balises. MQTT et la page web gagnent quelques centaines de ms de
      // latence, sans effet visible. Incompatible avec le point d'accès.
      WiFi.setSleep(WIFI_PS_MAX_MODEM);
    }
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
    gMqttClient.setServer(gConfig.mqttHost, gConfig.mqttPort);
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
  if (gConfig.mqttUser[0] != '\0') {
    connected = gMqttClient.connect(clientId.c_str(), gConfig.mqttUser, gConfig.mqttPassword);
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

  for (int i = 0; i < STATION_TOPIC_COUNT; ++i) {
    if (topicStr == STATION_TOPICS[i]) {
      gReceivedTopicsMask |= static_cast<uint8_t>(1U << i);
    }
  }

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

// Flèche du vent dessinée en vectoriel (nette à toute taille), centrée sur
// (centerX, centerY) et inscrite dans un carré de côté size. Le bearing
// indique d'où vient le vent : la flèche pointe vers où il va (+180°).
// gConfig.windArrowStyle : 1 = flèche à hampe (par défaut), 0 = flèche de
// navigation à encoche.
void drawWindArrow(int centerX, int centerY, float bearingDegrees, int size) {
  if (isnan(bearingDegrees)) {
    return;
  }
  const float angle = radians(bearingDegrees + 180.0f);
  const float cosA = cosf(angle);
  const float sinA = sinf(angle);
  const float half = max(12, size) / 2.0f;
  // Coordonnées en demi-taille, flèche pointant vers le haut (y vers le bas),
  // tournées dans le sens horaire de l'angle.
  auto px = [&](float ux, float uy) {
    return static_cast<int16_t>(centerX + roundf((ux * cosA - uy * sinA) * half));
  };
  auto py = [&](float ux, float uy) {
    return static_cast<int16_t>(centerY + roundf((ux * sinA + uy * cosA) * half));
  };
  auto fillTri = [&](float ax, float ay, float bx, float by, float cx, float cy) {
    display.fillTriangle(px(ax, ay), py(ax, ay), px(bx, by), py(bx, by), px(cx, cy), py(cx, cy), GxEPD_BLACK);
  };

  if (gConfig.windArrowStyle == 1) {
    fillTri(0.0f, -1.0f, 0.62f, -0.15f, -0.62f, -0.15f);  // pointe
    fillTri(-0.17f, -0.2f, 0.17f, -0.2f, 0.17f, 1.0f);    // hampe
    fillTri(-0.17f, -0.2f, 0.17f, 1.0f, -0.17f, 1.0f);
  } else {
    fillTri(0.0f, -1.0f, 0.72f, 0.95f, 0.0f, 0.42f);      // moitié droite
    fillTri(0.0f, -1.0f, 0.0f, 0.42f, -0.72f, 0.95f);     // moitié gauche
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
    filter[gConfig.forecastEntity]["forecast"][0][key] = true;
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

  JsonVariant rootVariant = doc[gConfig.forecastEntity];
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