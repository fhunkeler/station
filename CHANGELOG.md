# Changelog

Toutes les évolutions notables du firmware de la station e-paper.
Format inspiré de [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/).

## [2.0.0] - 2026-10-26

### Ajouté
- **Nouvelle mise en page** en quatre bandes : heure, date et lune ; marée
  réduite à l'oscillation en cours (étales précédente et suivante, arc avec
  la position actuelle, sens et coefficient) ; vent (flèche, vitesse,
  direction, rafales, prévision à +3/+6/+9/+12 h) ; trois colonnes de mesures.
- **Police Barlow Condensed** (licence OFL) convertie en Latin-1 : accents et
  signe degré. FreeSans reste disponible (`STATION_FONT_THEME=0`).
- **Flèche du vent vectorielle**, nette à toutes les tailles, en deux styles
  (à hampe ou de navigation à encoche).
- **Écran de démarrage** suivant le WiFi, le serveur MQTT, l'heure NTP et
  les sources de données reçues, avec bilan en cas d'échec.
- **Interface web de configuration** (`http://station.local`) : état de la
  station, WiFi, MQTT, libellés des colonnes, heure du nettoyage de l'écran,
  style de flèche, mot de passe d'administration, redémarrage et réglages
  d'usine. Réglages conservés en mémoire non volatile ; `secrets.h` devient
  facultatif et ne fournit plus que des valeurs par défaut.
- **Mode expert** : sujet MQTT réglable pour chaque source, entité de
  prévision et délai de péremption des mesures.
- **Point d'accès WiFi de configuration** (`Station-XXXX`) avec portail
  captif et QR code à l'écran, ouvert si aucun WiFi n'est configuré, si le
  WiFi est injoignable au démarrage, ou sur double reset.
- **Mise à jour sans câble (OTA)** par la page web ou depuis PlatformIO
  (`lolin_d32_ota`), protégée par le mot de passe d'administration.
- **Prévisualisation sur PC** (`tools/preview/render.sh`) : le vrai
  `main.cpp` est compilé avec des shims et les images de `docs/` sont
  régénérées sans flasher la station.
- Documentation des échanges avec Home Assistant (`docs/`).

### Modifié
- L'âge des mesures est lu dans `updated_at` : une mesure de plus de 3 h
  (réglable) est ignorée ou effacée, même si le message retenu est
  redistribué à chaque reconnexion MQTT.
- Un champ `null` ou non numérique s'affiche `--`.
- Le coefficient affiché est celui de la prochaine pleine mer (celui des
  basses mers est incohérent dans les données).
- La prévision n'extrait que les champs utiles (filtre JSON) et conserve les
  rafales.
- Keepalive MQTT porté à 60 s.
- Processeur à 80 MHz et veille WiFi renforcée sur la box : consommation
  réduite sur secteur.
- Attente du WiFi au démarrage portée à 30 s ; si le WiFi n'est pas joint,
  nouvel essai après 30 s puis toutes les 5 minutes.

### Corrigé
- L'heure se figeait pendant une coupure WiFi ; elle est désormais lue sur
  l'horloge interne et l'écran suit le changement réel de minute.
- Les reconnexions WiFi et MQTT bloquaient la boucle jusqu'à 10 s.
- Une date fictive s'affichait tant que l'heure n'était pas synchronisée.
- Les valeurs restaient figées indéfiniment si Home Assistant cessait de
  publier.
- Le sens de la marée dépendait de la présence d'un coefficient.
- La flèche du vent présentait des trous.
- Fond d'écran grisâtre : rafraîchissement complet en deux temps, cycle noir
  puis blanc au démarrage et chaque nuit contre la rémanence, panneau
  laissé alimenté entre deux mises à jour.

## [1.0.0] - 2026-08-18

Version d'origine : heure, date, phase de lune, températures et humidités
du rez-de-chaussée et de l'étage, marée (prochaine, précédente,
coefficient), température extérieure, pression et vent, reçus de Home
Assistant par MQTT.
