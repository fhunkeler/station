#include "web_portal.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
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

constexpr uint8_t DNS_PORT = 53;
constexpr unsigned long RESTART_DELAY_MS = 1500;

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

// Champ mot de passe : la valeur enregistrée n'est jamais renvoyée au
// navigateur, un champ vide la conserve.
void appendPassword(String& html, const char* label, const char* name, bool isSet) {
  appendInput(html, label, name, "", "password",
              isSet ? "placeholder=\"inchangé\" autocomplete=\"new-password\""
                    : "placeholder=\"aucun\" autocomplete=\"new-password\"");
}

void appendNetworkList(String& html) {
  html += "<datalist id=\"ssids\">";
  const int found = WiFi.scanComplete();
  if (found > 0) {
    for (int i = 0; i < found; ++i) {
      html += "<option value=\"";
      html += htmlEscape(WiFi.SSID(i).c_str());
      html += "\">";
      html += String(WiFi.RSSI(i));
      html += " dBm</option>";
    }
  }
  html += "</datalist>";
  // Relancer une recherche en arrière-plan pour la prochaine visite.
  if (found != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
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
  html += "<p class=\"note\">Un mot de passe WiFi vide est conservé, sauf si le nom du réseau change.</p>";
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
  appendInput(html, "Préfixe des sujets (préfixe/tide, préfixe/outdoor...)", "topicPrefix",
              gConfig.topicPrefix, "text", "maxlength=\"31\"");
  appendInput(html, "Entité météo de la prévision", "forecastEntity", gConfig.forecastEntity, "text",
              "maxlength=\"47\"");
  html += "</section>";

  html += "<section><h2>Affichage</h2><div class=\"row\"><div>";
  appendInput(html, "Colonne 1 (extérieur)", "labelOutdoor", gConfig.labelOutdoor, "text", "maxlength=\"12\"");
  html += "</div><div>";
  appendInput(html, "Colonne 2", "labelGround", gConfig.labelGround, "text", "maxlength=\"12\"");
  html += "</div></div>";
  appendInput(html, "Colonne 3", "labelFloor", gConfig.labelFloor, "text", "maxlength=\"12\"");
  html += "<div class=\"row\"><div>";
  appendInput(html, "Mesure périmée après (heures)", "staleHours", String(gConfig.staleHours).c_str(),
              "number", "min=\"1\" max=\"48\"");
  html += "</div><div>";
  appendInput(html, "Nettoyage de l'écran à (heure)", "ghostHour", String(gConfig.ghostCleaningHour).c_str(),
              "number", "min=\"0\" max=\"23\"");
  html += "</div></div>";
  html += "<label for=\"arrowStyle\">Flèche du vent</label><select id=\"arrowStyle\" name=\"arrowStyle\">";
  html += gConfig.windArrowStyle == 1 ? "<option value=\"1\" selected>" : "<option value=\"1\">";
  html += "Flèche à hampe</option>";
  html += gConfig.windArrowStyle == 0 ? "<option value=\"0\" selected>" : "<option value=\"0\">";
  html += "Flèche de navigation à encoche</option></select>";
  html += "</section>";

  html += "<section><h2>Administration</h2>";
  appendInput(html, "Nouveau mot de passe de cette page (utilisateur : admin)", "adminPassword", "",
              "password", "autocomplete=\"new-password\" placeholder=\"inchangé\"");
  if (gConfig.adminPassword[0] != '\0') {
    html += "<label class=\"check\"><input type=\"checkbox\" name=\"adminClear\" value=\"1\">"
            "Supprimer le mot de passe</label>";
  }
  html += "</section>";

  html += "<div class=\"actions\"><button type=\"submit\">Enregistrer et redémarrer</button></div></form>";

  html += "<section style=\"margin-top:14px\"><h2>Maintenance</h2><div class=\"actions\">"
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
            readText("topicPrefix", updated.topicPrefix, sizeof(updated.topicPrefix), error, "Préfixe") &&
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

// ---------- Portail captif ----------

void handleNotFound() {
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
    if (authorize()) {
      sendPage("", false);
    }
  });
  gServer.on("/save", HTTP_POST, handleSave);
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
  gServer.onNotFound(handleNotFound);
  gServer.begin();
  WiFi.scanNetworks(true);
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
    }
  }

  if (gRestartPending && millis() - gRestartAtMs >= RESTART_DELAY_MS) {
    ESP.restart();
  }
}

void webPortalStartAccessPoint() {
  if (gAccessPointActive) {
    return;
  }
  WiFi.mode(WIFI_AP_STA);
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
