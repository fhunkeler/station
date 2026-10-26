#include "web_portal.h"

#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "station_config.h"

namespace {

WebServer gServer(80);
DNSServer gDns;
bool gAccessPointActive = false;
bool gMdnsStarted = false;
unsigned long gRestartAtMs = 0;
bool gRestartPending = false;
bool gUploadRejected = false;  // envoi de firmware refusé (accès ou fichier)
bool gArduinoOtaStarted = false;

constexpr uint8_t DNS_PORT = 53;

#if ENABLE_STATION_LOGS
void logRequest(const char* what) {
  Serial.printf("[HTTP] %s %s %s (hôte %s, client %s)\n", what,
                gServer.method() == HTTP_POST ? "POST" : "GET", gServer.uri().c_str(),
                gServer.hostHeader().c_str(), gServer.client().remoteIP().toString().c_str());
}
#else
void logRequest(const char*) {}
#endif
constexpr unsigned long RESTART_DELAY_MS = 1500;
constexpr uint16_t ARDUINO_OTA_PORT = 3232;

bool firmwareUpdateAllowed();

// ---------- Utilitaires HTML ----------

String htmlEscape(const char* text) {
  String out;
  for (const char* p = text; *p != '\0'; ++p) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *p;
    }
  }
  return out;
}

const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Station météo</title>
<style>
:root{--ink:#1d1d1f;--muted:#6b6b70;--line:#d9d9de;--bg:#f6f6f3;--card:#fff;--accent:#1f5f99;--warn:#a33}
*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;color:var(--ink);background:var(--bg)}
main{max-width:640px;margin:0 auto;padding:16px}
h1{font-size:1.5rem;margin:.2em 0 .6em}h2{font-size:1.05rem;margin:0 0 .6em}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin:0 0 14px}
label{display:block;font-size:.85rem;color:var(--muted);margin:.7em 0 .2em}
input,select{width:100%;padding:9px 10px;font:inherit;border:1px solid var(--line);border-radius:7px;background:#fff}
.row{display:grid;grid-template-columns:1fr 1fr;gap:0 12px}
.check{display:flex;gap:8px;align-items:center;color:var(--ink);font-size:.95rem}.check input{width:auto}
table{width:100%;border-collapse:collapse;font-size:.92rem}td{padding:5px 0;border-bottom:1px solid var(--line);vertical-align:top}
td:first-child{color:var(--muted);width:42%}
button{font:inherit;padding:11px 16px;border-radius:8px;border:1px solid var(--accent);background:var(--accent);color:#fff;cursor:pointer}
button.secondary{background:#fff;color:var(--accent)}button.danger{background:#fff;color:var(--warn);border-color:var(--warn)}
.actions{display:flex;flex-wrap:wrap;gap:10px}.note{font-size:.85rem;color:var(--muted)}
.msg{padding:10px 12px;border-radius:8px;margin:0 0 14px;background:#e8f1fa;border:1px solid #b9d3ea}
.msg.error{background:#fbeaea;border-color:#e6b8b8}
</style></head><body><main>
)HTML";

const char PAGE_TAIL[] PROGMEM = "</main></body></html>";

void appendInput(String& html, const char* label, const char* name, const char* value,
                 const char* type = "text", const char* extra = "") {
  html += "<label for=\"";
  html += name;
  html += "\">";
  html += label;
  html += "</label><input id=\"";
  html += name;
  html += "\" name=\"";
  html += name;
  html += "\" type=\"";
  html += type;
  html += "\" value=\"";
  html += htmlEscape(value);
  html += "\" ";
  html += extra;
  html += ">";
}

// Sujet MQTT : non vide, sans espace ni joker d'abonnement.
const char* const TOPIC_FIELD_ATTRIBUTES = "required maxlength=\"63\" pattern=\"[^#+ ]+\"";

// Champ mot de passe : la valeur enregistrée n'est jamais renvoyée au
// navigateur, un champ vide la conserve.
void appendPassword(String& html, const char* label, const char* name, bool isSet) {
  appendInput(html, label, name, "", "password",
              isSet ? "placeholder=\"inchangé\" autocomplete=\"new-password\""
                    : "placeholder=\"aucun\" autocomplete=\"new-password\"");
}

// Réseaux trouvés par la dernière recherche. La recherche n'est jamais lancée
// pendant une réponse : elle fait changer la radio de canal pendant 2 à 3 s
// et coupe les téléphones connectés au point d'accès.
void appendNetworkList(String& html) {
  html += "<datalist id=\"ssids\">";
  const int found = WiFi.scanComplete();
  for (int i = 0; i < found; ++i) {
    html += "<option value=\"";
    html += htmlEscape(WiFi.SSID(i).c_str());
    html += "\">";
    html += String(WiFi.RSSI(i));
    html += " dBm</option>";
  }
  html += "</datalist>";
}

// Recherche synchrone des réseaux WiFi (quelques secondes).
void scanNetworks() {
  WiFi.scanDelete();
  WiFi.scanNetworks(false);
}

void sendPage(const String& message, bool isError, int code = 200) {
  String html;
  html.reserve(9000);
  html += FPSTR(PAGE_HEAD);
  html += "<h1>Station météo</h1>";
  if (message.length() > 0) {
    html += isError ? "<div class=\"msg error\">" : "<div class=\"msg\">";
    html += message;
    html += "</div>";
  }
  if (gConfig.adminPassword[0] == '\0') {
    html += "<div class=\"msg\">Aucun mot de passe d'administration : toute personne "
            "connectée au réseau peut modifier ces réglages.</div>";
  }

  html += "<section><h2>État</h2><table>";
  appendStationStatus(html);
  html += "</table></section>";

  html += "<form method=\"post\" action=\"/save\">";

  html += "<section><h2>Réseau WiFi</h2>";
  appendInput(html, "Nom du réseau (SSID)", "wifiSsid", gConfig.wifiSsid, "text",
              "list=\"ssids\" maxlength=\"32\" autocomplete=\"off\"");
  appendNetworkList(html);
  appendPassword(html, "Mot de passe WiFi", "wifiPassword", gConfig.wifiPassword[0] != '\0');
  appendInput(html, "Nom de la station sur le réseau (.local)", "hostname", gConfig.hostname, "text",
              "maxlength=\"31\" pattern=\"[a-zA-Z0-9-]+\"");
  html += "<p class=\"note\">Un mot de passe WiFi vide est conservé, sauf si le nom du réseau change. "
          "<a href=\"/scan\">Rechercher les réseaux</a> (quelques secondes).</p>";
  html += "</section>";

  html += "<section><h2>Serveur MQTT</h2><div class=\"row\"><div>";
  appendInput(html, "Adresse", "mqttHost", gConfig.mqttHost, "text", "maxlength=\"63\"");
  html += "</div><div>";
  appendInput(html, "Port", "mqttPort", String(gConfig.mqttPort).c_str(), "number", "min=\"1\" max=\"65535\"");
  html += "</div></div><div class=\"row\"><div>";
  appendInput(html, "Utilisateur", "mqttUser", gConfig.mqttUser, "text", "maxlength=\"32\" autocomplete=\"off\"");
  html += "</div><div>";
  appendPassword(html, "Mot de passe", "mqttPassword", gConfig.mqttPassword[0] != '\0');
  html += "</div></div>";
  html += "</section>";

  html += "<section><h2>Affichage</h2><div class=\"row\"><div>";
  appendInput(html, "Colonne 1 (extérieur)", "labelOutdoor", gConfig.labelOutdoor, "text", "maxlength=\"12\"");
  html += "</div><div>";
  appendInput(html, "Colonne 2", "labelGround", gConfig.labelGround, "text", "maxlength=\"12\"");
  html += "</div></div>";
  appendInput(html, "Colonne 3", "labelFloor", gConfig.labelFloor, "text", "maxlength=\"12\"");
  appendInput(html, "Nettoyage de l'écran à (heure)", "ghostHour", String(gConfig.ghostCleaningHour).c_str(),
              "number", "min=\"0\" max=\"23\"");
  html += "<label for=\"arrowStyle\">Flèche du vent</label><select id=\"arrowStyle\" name=\"arrowStyle\">";
  html += gConfig.windArrowStyle == 1 ? "<option value=\"1\" selected>" : "<option value=\"1\">";
  html += "Flèche à hampe</option>";
  html += gConfig.windArrowStyle == 0 ? "<option value=\"0\" selected>" : "<option value=\"0\">";
  html += "Flèche de navigation à encoche</option></select>";
  html += "</section>";

  // Mode expert : replié par défaut, pour les réglages qui dépendent de la
  // configuration de Home Assistant.
  html += "<section><details><summary><h2 style=\"display:inline\">Mode expert</h2></summary>"
          "<p class=\"note\">Sujets MQTT lus par la station. Ils doivent correspondre exactement à ceux "
          "publiés par Home Assistant (sans # ni +).</p>";
  appendInput(html, "Marées", "topicTide", gConfig.topicTide, "text", TOPIC_FIELD_ATTRIBUTES);
  appendInput(html, "Extérieur (température, pression)", "topicOutdoor", gConfig.topicOutdoor, "text",
              TOPIC_FIELD_ATTRIBUTES);
  appendInput(html, "Colonne 2 (température, humidité)", "topicGround", gConfig.topicGround, "text",
              TOPIC_FIELD_ATTRIBUTES);
  appendInput(html, "Colonne 3 (température, humidité)", "topicFloor", gConfig.topicFloor, "text",
              TOPIC_FIELD_ATTRIBUTES);
  appendInput(html, "Prévision météo (vent)", "topicForecast", gConfig.topicForecast, "text",
              TOPIC_FIELD_ATTRIBUTES);
  appendInput(html, "Entité météo dans le message de prévision", "forecastEntity", gConfig.forecastEntity,
              "text", "maxlength=\"47\"");
  appendInput(html, "Mesure périmée après (heures, d'après updated_at)", "staleHours",
              String(gConfig.staleHours).c_str(), "number", "min=\"1\" max=\"48\"");
  html += "</details></section>";

  html += "<section><h2>Administration</h2>";
  appendInput(html, "Nouveau mot de passe de cette page (utilisateur : admin)", "adminPassword", "",
              "password", "autocomplete=\"new-password\" placeholder=\"inchangé\"");
  if (gConfig.adminPassword[0] != '\0') {
    html += "<label class=\"check\"><input type=\"checkbox\" name=\"adminClear\" value=\"1\">"
            "Supprimer le mot de passe</label>";
  }
  html += "</section>";

  html += "<div class=\"actions\"><button type=\"submit\">Enregistrer et redémarrer</button></div></form>";

  html += "<section style=\"margin-top:14px\"><h2>Mise à jour du firmware</h2>";
  if (firmwareUpdateAllowed()) {
    html += "<form method=\"post\" action=\"/update\" enctype=\"multipart/form-data\" "
            "onsubmit=\"this.querySelector('button').disabled=true;"
            "this.querySelector('button').textContent='Envoi en cours...'\">"
            "<input type=\"file\" name=\"firmware\" accept=\".bin\" required>"
            "<div class=\"actions\" style=\"margin-top:10px\"><button>Installer</button></div></form>"
            "<p class=\"note\">Fichier <code>.pio/build/lolin_d32/firmware.bin</code> produit par "
            "<code>pio run</code>. Ne pas couper l'alimentation pendant l'installation (une minute environ). "
            "Depuis PlatformIO : <code>pio run -e lolin_d32_ota -t upload</code>.</p>";
  } else {
    html += "<p class=\"note\">Définissez d'abord un mot de passe d'administration : il protège "
            "l'installation d'un nouveau firmware depuis le réseau.</p>";
  }
  html += "</section>";

  html += "<section><h2>Maintenance</h2><div class=\"actions\">"
          "<form method=\"post\" action=\"/restart\"><button class=\"secondary\">Redémarrer</button></form>"
          "<form method=\"post\" action=\"/reset\" onsubmit=\"return confirm('Revenir aux réglages "
          "d\\'usine ? La station redémarrera.')\"><button class=\"danger\">Réglages d'usine</button></form>"
          "</div><p class=\"note\">Mode configuration à tout moment : appuyer deux fois sur reset à "
          "quelques secondes d'intervalle. La station ouvre alors son propre réseau WiFi.</p></section>";

  html += FPSTR(PAGE_TAIL);
  gServer.send(code, "text/html; charset=utf-8", html);
}

void sendRestartPage(const char* message) {
  String html;
  html += FPSTR(PAGE_HEAD);
  html += "<h1>Station météo</h1><div class=\"msg\">";
  html += message;
  html += "</div><p class=\"note\">La page se recharge dans 20 secondes. Si le réseau a changé, "
          "reconnectez-vous au même réseau que la station.</p>"
          "<script>setTimeout(function(){location.href='/'},20000)</script>";
  html += FPSTR(PAGE_TAIL);
  gServer.send(200, "text/html; charset=utf-8", html);
  gRestartPending = true;
  gRestartAtMs = millis();
}

// ---------- Accès ----------

bool requestFromAccessPoint() {
  return gAccessPointActive && gServer.client().localIP() == WiFi.softAPIP();
}

// Le point d'accès suppose un accès physique (son mot de passe s'affiche sur
// l'écran) : pas de mot de passe d'administration demandé par ce chemin.
// C'est aussi la voie de secours si le mot de passe est oublié.
// Même règle qu'authorize(), sans répondre au navigateur : utilisable
// pendant la réception d'un fichier.
bool isAuthorized() {
  return gConfig.adminPassword[0] == '\0' || requestFromAccessPoint() ||
         gServer.authenticate("admin", gConfig.adminPassword);
}

// Installer un firmware depuis le réseau exige un mot de passe
// d'administration ; le point d'accès (accès physique) suffit aussi.
bool firmwareUpdateAllowed() {
  return gConfig.adminPassword[0] != '\0' || requestFromAccessPoint();
}

bool authorize() {
  if (gConfig.adminPassword[0] == '\0' || requestFromAccessPoint()) {
    return true;
  }
  if (gServer.authenticate("admin", gConfig.adminPassword)) {
    return true;
  }
  gServer.requestAuthentication(BASIC_AUTH, "Station météo");
  return false;
}

// ---------- Enregistrement ----------

bool readText(const char* name, char* target, size_t size, String& error, const char* label) {
  const String value = gServer.arg(name);
  if (value.length() >= size) {
    error = String(label) + " : trop long.";
    return false;
  }
  strncpy(target, value.c_str(), size - 1);
  target[size - 1] = '\0';
  return true;
}

// Un sujet lu par la station est un nom exact : les jokers MQTT (# et +)
// feraient correspondre plusieurs sources à un même champ.
bool readTopic(const char* name, char* target, size_t size, String& error, const char* label) {
  if (!readText(name, target, size, error, label)) {
    return false;
  }
  if (target[0] == '\0' || strpbrk(target, "#+ ") != nullptr) {
    error = String(label) + " : sujet vide ou contenant #, + ou un espace.";
    return false;
  }
  return true;
}

bool readNumber(const char* name, long minValue, long maxValue, long& out, String& error, const char* label) {
  const String value = gServer.arg(name);
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (value.length() == 0 || end == nullptr || *end != '\0' || parsed < minValue || parsed > maxValue) {
    error = String(label) + " : valeur entre " + String(minValue) + " et " + String(maxValue) + " attendue.";
    return false;
  }
  out = parsed;
  return true;
}

bool validHostname(const char* name) {
  if (name[0] == '\0') {
    return false;
  }
  for (const char* p = name; *p != '\0'; ++p) {
    if (!isalnum(static_cast<unsigned char>(*p)) && *p != '-') {
      return false;
    }
  }
  return true;
}

void handleSave() {
  if (!authorize()) {
    return;
  }
  StationConfig updated = gConfig;
  String error;
  long port = 0;
  long staleHours = 0;
  long ghostHour = 0;
  long arrowStyle = 0;
  bool ok = readText("wifiSsid", updated.wifiSsid, sizeof(updated.wifiSsid), error, "Nom du réseau") &&
            readText("hostname", updated.hostname, sizeof(updated.hostname), error, "Nom de la station") &&
            readText("mqttHost", updated.mqttHost, sizeof(updated.mqttHost), error, "Adresse MQTT") &&
            readNumber("mqttPort", 1, 65535, port, error, "Port MQTT") &&
            readText("mqttUser", updated.mqttUser, sizeof(updated.mqttUser), error, "Utilisateur MQTT") &&
            readTopic("topicTide", updated.topicTide, sizeof(updated.topicTide), error, "Sujet des marées") &&
            readTopic("topicOutdoor", updated.topicOutdoor, sizeof(updated.topicOutdoor), error, "Sujet extérieur") &&
            readTopic("topicGround", updated.topicGround, sizeof(updated.topicGround), error, "Sujet colonne 2") &&
            readTopic("topicFloor", updated.topicFloor, sizeof(updated.topicFloor), error, "Sujet colonne 3") &&
            readTopic("topicForecast", updated.topicForecast, sizeof(updated.topicForecast), error, "Sujet prévision") &&
            readText("forecastEntity", updated.forecastEntity, sizeof(updated.forecastEntity), error, "Entité météo") &&
            readText("labelOutdoor", updated.labelOutdoor, sizeof(updated.labelOutdoor), error, "Colonne 1") &&
            readText("labelGround", updated.labelGround, sizeof(updated.labelGround), error, "Colonne 2") &&
            readText("labelFloor", updated.labelFloor, sizeof(updated.labelFloor), error, "Colonne 3") &&
            readNumber("staleHours", 1, 48, staleHours, error, "Mesure périmée") &&
            readNumber("ghostHour", 0, 23, ghostHour, error, "Heure de nettoyage") &&
            readNumber("arrowStyle", 0, 1, arrowStyle, error, "Flèche du vent");
  if (ok && !validHostname(updated.hostname)) {
    error = "Nom de la station : lettres, chiffres et tirets uniquement.";
    ok = false;
  }
  if (!ok) {
    sendPage(htmlEscape(error.c_str()), true, 400);
    return;
  }
  updated.mqttPort = static_cast<uint16_t>(port);
  updated.staleHours = static_cast<uint8_t>(staleHours);
  updated.ghostCleaningHour = static_cast<uint8_t>(ghostHour);
  updated.windArrowStyle = static_cast<uint8_t>(arrowStyle);

  // Mots de passe : champ vide = inchangé, sauf si l'identifiant associé a
  // changé (réseau ouvert, broker sans mot de passe...).
  const String wifiPassword = gServer.arg("wifiPassword");
  if (wifiPassword.length() > 0 || strcmp(updated.wifiSsid, gConfig.wifiSsid) != 0) {
    if (!readText("wifiPassword", updated.wifiPassword, sizeof(updated.wifiPassword), error, "Mot de passe WiFi")) {
      sendPage(htmlEscape(error.c_str()), true, 400);
      return;
    }
  }
  const String mqttPassword = gServer.arg("mqttPassword");
  if (mqttPassword.length() > 0 || strcmp(updated.mqttUser, gConfig.mqttUser) != 0) {
    if (!readText("mqttPassword", updated.mqttPassword, sizeof(updated.mqttPassword), error, "Mot de passe MQTT")) {
      sendPage(htmlEscape(error.c_str()), true, 400);
      return;
    }
  }
  if (gServer.hasArg("adminClear")) {
    updated.adminPassword[0] = '\0';
  } else if (gServer.arg("adminPassword").length() > 0) {
    if (!readText("adminPassword", updated.adminPassword, sizeof(updated.adminPassword), error,
                  "Mot de passe d'administration")) {
      sendPage(htmlEscape(error.c_str()), true, 400);
      return;
    }
  }

  gConfig = updated;
  if (!saveStationConfig()) {
    sendPage("L'enregistrement a échoué. Réessayez.", true, 500);
    return;
  }
  sendRestartPage("Réglages enregistrés. La station redémarre.");
}

// ---------- Mise à jour du firmware ----------

// Réception du fichier, morceau par morceau.
void handleFirmwareUpload() {
  HTTPUpload& upload = gServer.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START:
      gUploadRejected = !firmwareUpdateAllowed() || !isAuthorized();
      if (gUploadRejected) {
        return;
      }
      logRequest("mise à jour");
      onFirmwareUpdateStart();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        gUploadRejected = true;
      }
      break;
    case UPLOAD_FILE_WRITE:
      // Update vérifie l'en-tête du premier bloc : un fichier qui n'est pas
      // un firmware ESP32 est refusé avant toute écriture utile.
      if (!gUploadRejected && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        gUploadRejected = true;
      }
      break;
    case UPLOAD_FILE_END:
      if (!gUploadRejected && !Update.end(true)) {
        gUploadRejected = true;
      }
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      gUploadRejected = true;
      break;
  }
}

// Réponse une fois le fichier entièrement reçu.
void handleFirmwareUploaded() {
  if (!firmwareUpdateAllowed()) {
    sendPage("Mise à jour refusée : définissez d'abord un mot de passe d'administration.", true, 403);
    return;
  }
  if (!authorize()) {
    return;
  }
  if (gUploadRejected || Update.hasError() || !Update.isFinished()) {
    String message = "La mise à jour a échoué";
    if (Update.hasError()) {
      message += " : ";
      message += htmlEscape(Update.errorString());
    }
    message += ". Le firmware actuel est conservé.";
    Update.abort();
    onFirmwareUpdateFailed();
    sendPage(message, true, 400);
    return;
  }
  sendRestartPage("Nouveau firmware installé. La station redémarre.");
}

// Mise à jour depuis PlatformIO (protocole espota), avec le même mot de
// passe que la page. Lancée seulement si ce mot de passe existe.
void startArduinoOta() {
  if (gArduinoOtaStarted || gConfig.adminPassword[0] == '\0' || WiFi.status() != WL_CONNECTED) {
    return;
  }
  ArduinoOTA.setPort(ARDUINO_OTA_PORT);
  ArduinoOTA.setHostname(gConfig.hostname);
  ArduinoOTA.setPassword(gConfig.adminPassword);
  ArduinoOTA.setMdnsEnabled(false);  // mDNS déjà géré ici
  ArduinoOTA.onStart([]() { onFirmwareUpdateStart(); });
  ArduinoOTA.onError([](ota_error_t) { onFirmwareUpdateFailed(); });
  ArduinoOTA.begin();
  if (gMdnsStarted) {
    MDNS.enableArduino(ARDUINO_OTA_PORT, true);
  }
  gArduinoOtaStarted = true;
}

// ---------- Portail captif ----------

void handleNotFound() {
  logRequest("inconnue");
  // Sur le point d'accès, toute adresse inconnue renvoie vers la page de
  // réglages : c'est ce qui fait apparaître le portail sur un téléphone.
  if (gAccessPointActive) {
    const String host = gServer.hostHeader();
    const String apAddress = WiFi.softAPIP().toString();
    if (host != apAddress) {
      gServer.sendHeader("Location", String("http://") + apAddress + "/", true);
      gServer.send(302, "text/plain", "");
      return;
    }
  }
  gServer.send(404, "text/plain; charset=utf-8", "Page introuvable");
}

}  // namespace

void webPortalBegin() {
  gServer.on("/", HTTP_GET, []() {
    logRequest("page");
    if (authorize()) {
      sendPage("", false);
    }
  });
  gServer.on("/save", HTTP_POST, []() {
    logRequest("enregistrement");
    handleSave();
  });
  gServer.on("/scan", HTTP_GET, []() {
    logRequest("recherche");
    if (authorize()) {
      scanNetworks();
      gServer.sendHeader("Location", "/", true);
      gServer.send(302, "text/plain", "");
    }
  });
  gServer.on("/restart", HTTP_POST, []() {
    if (authorize()) {
      sendRestartPage("La station redémarre.");
    }
  });
  gServer.on("/reset", HTTP_POST, []() {
    if (authorize()) {
      resetStationConfig();
      sendRestartPage("Réglages d'usine rétablis. La station redémarre.");
    }
  });
  gServer.on("/update", HTTP_POST, handleFirmwareUploaded, handleFirmwareUpload);
  gServer.onNotFound(handleNotFound);
  gServer.begin();
}

void webPortalLoop() {
  if (gAccessPointActive) {
    gDns.processNextRequest();
  }
  gServer.handleClient();

  if (!gMdnsStarted && WiFi.status() == WL_CONNECTED) {
    gMdnsStarted = MDNS.begin(gConfig.hostname);
    if (gMdnsStarted) {
      MDNS.addService("http", "tcp", 80);
      if (gArduinoOtaStarted) {
        MDNS.enableArduino(ARDUINO_OTA_PORT, true);
      }
    }
  }

  startArduinoOta();
  if (gArduinoOtaStarted) {
    ArduinoOTA.handle();  // redémarre seul à la fin d'une mise à jour réussie
  }

  if (gRestartPending && millis() - gRestartAtMs >= RESTART_DELAY_MS) {
    ESP.restart();
  }
}

void webPortalStartAccessPoint() {
  if (gAccessPointActive) {
    return;
  }
  // Liste des réseaux préparée avant l'ouverture du point d'accès, quand
  // aucun téléphone ne peut encore en souffrir.
  scanNetworks();
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(WIFI_PS_NONE);  // un point d'accès en veille décroche les téléphones
  WiFi.softAP(accessPointName().c_str(), accessPointPassword().c_str());
  gDns.setErrorReplyCode(DNSReplyCode::NoError);
  gDns.start(DNS_PORT, "*", WiFi.softAPIP());
  gAccessPointActive = true;
}

void webPortalStopAccessPoint() {
  if (!gAccessPointActive) {
    return;
  }
  gDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gAccessPointActive = false;
}

bool webPortalAccessPointActive() {
  return gAccessPointActive;
}

String webPortalEscape(const char* text) {
  return htmlEscape(text);
}

int webPortalAccessPointClients() {
  return gAccessPointActive ? WiFi.softAPgetStationNum() : 0;
}
