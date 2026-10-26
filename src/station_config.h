#pragma once

#include <Arduino.h>

// Réglages de la station, modifiables depuis l'interface web et conservés en
// mémoire non volatile (NVS). Les valeurs par défaut viennent de secrets.h
// quand il existe, ce qui garde le comportement d'avant au premier démarrage.
struct StationConfig {
  // Réseau
  char wifiSsid[33];
  char wifiPassword[65];
  char hostname[32];  // nom mDNS : http://<hostname>.local
  // MQTT
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[33];
  char mqttPassword[65];
  // Sujets MQTT lus par la station, un par source (mode expert).
  char topicTide[64];
  char topicOutdoor[64];
  char topicGround[64];
  char topicFloor[64];
  char topicForecast[64];
  char forecastEntity[48];  // clé racine du message de prévision
  // Affichage (libellés en UTF-8)
  char labelOutdoor[24];
  char labelGround[24];
  char labelFloor[24];
  uint8_t staleHours;         // âge maximal d'une mesure avant "--"
  uint8_t ghostCleaningHour;  // heure du nettoyage noir/blanc quotidien
  uint8_t windArrowStyle;     // 1 = flèche à hampe, 0 = flèche à encoche
  // Administration : mot de passe de l'interface web (vide = sans).
  char adminPassword[33];
};

extern StationConfig gConfig;

void loadStationConfig();
bool saveStationConfig();
// Efface les réglages enregistrés : retour aux valeurs par défaut.
void resetStationConfig();

// Point d'accès de configuration : nom et mot de passe dérivés de l'adresse
// MAC, donc stables d'un démarrage à l'autre.
String accessPointName();
String accessPointPassword();

// Vrai si la carte a été réinitialisée deux fois de suite à quelques secondes
// d'intervalle : demande explicite du mode configuration. À appeler une fois
// au démarrage ; closeDoubleResetWindow() referme la fenêtre ensuite.
bool consumeDoubleReset();
void closeDoubleResetWindow();
