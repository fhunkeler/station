#pragma once

#include <Arduino.h>

// Interface web de configuration de la station.
//
// Le serveur répond sur le réseau de la maison (http://<hostname>.local ou
// l'adresse IP) et, quand le point d'accès de configuration est ouvert, sur
// http://192.168.4.1 avec un portail captif qui s'ouvre tout seul sur un
// téléphone. Toute modification enregistrée redémarre la station.

void webPortalBegin();
void webPortalLoop();

void webPortalStartAccessPoint();
void webPortalStopAccessPoint();
bool webPortalAccessPointActive();
// Nombre d'appareils connectés au point d'accès.
int webPortalAccessPointClients();

// Échappe un texte pour l'insérer dans une page HTML.
String webPortalEscape(const char* text);

// Fournie par main.cpp : lignes de tableau HTML (<tr>...</tr>) décrivant
// l'état courant de la station, pour la page d'accueil.
void appendStationStatus(String& html);
