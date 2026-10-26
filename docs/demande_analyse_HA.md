# Demande d'analyse Home Assistant pour la station e-paper

Mission en **LECTURE SEULE** sur Home Assistant (192.168.0.248, accessible avec `ssh root@192.168.0.248`).
Ne modifie rien, ne redémarre rien, et ne recopie aucun secret (mots de passe, jetons) dans ta réponse.

## Contexte

Une station e-paper ESP32 (écran 400×300) s'abonne en MQTT au broker 192.168.0.248:1883,
avec l'utilisateur `station`, aux topics suivants :

| Topic              | Ce que le firmware lit                                                   |
|--------------------|--------------------------------------------------------------------------|
| `station/tide`     | `days[].events[]` avec `time` (ISO 8601), `tide` (`HIGH`/`LOW`), `coefficient`, `height_m` |
| `station/outdoor`  | `temperature`, `pressure` (nombres)                                      |
| `station/ground`   | `temperature`, `humidity` (rez-de-chaussée)                              |
| `station/floor`    | `temperature`, `humidity` (étage)                                        |
| `station/forecast` | `weather.trelevern.forecast[]` (ou `forecast[]` à la racine), avec `datetime` ou `time`, `wind_bearing`, `wind_speed` |

Limites du firmware : 32 Ko au maximum par message MQTT, 32 événements de marée, 96 points de prévision.
Au démarrage, la station a besoin des messages **retenus** (retain) pour s'afficher tout de suite.

## Questions

1. **Qui publie chaque topic `station/*` ?** (automatisation, script, add-on tide_scraper, Node-RED, AppDaemon…)
   Pour chaque topic, indiquer : le fichier ou l'emplacement, le déclencheur et la cadence, `retain` (oui/non),
   la QoS, le template du payload et les entités sources.

2. **Payload réel de chaque topic**, par exemple :
   `mosquitto_sub -h 192.168.0.248 -u <user> -P <pass> -t 'station/#' -v -W 20`
   Les identifiants se trouvent dans la config de l'add-on Mosquitto ou dans `secrets.yaml`, mais ne les recopie pas.
   Tronquer au-delà de 1500 caractères. Indiquer aussi la taille en octets de `station/tide` et de `station/forecast`.

3. **`station/tide`** :
   - version de l'add-on tide_scraper et topic configuré ;
   - nombre de jours publiés et nombre d'événements ;
   - les événements `LOW` ont-ils un `coefficient` (ou `null`) ?
   - format de `time` : avec fuseau horaire (`+01:00`) ou non ?
   - le topic est-il celui de l'add-on lui-même, ou une automatisation le republie-t-elle ?

4. **`station/forecast`** :
   - pas horaire ou journalier, nombre de points, horizon couvert ;
   - la clé racine `weather.trelevern` existe-t-elle ?
   - champs présents (`wind_speed`, `wind_bearing`, `wind_gust_speed`, `condition`, `temperature`, `precipitation`, `precipitation_probability`…) ;
   - unités : `wind_speed` est-il bien en km/h ?

5. **Capteur indisponible** : que publient `outdoor`, `ground` et `floor` quand une entité source est
   `unavailable` ou `unknown` ? La chaîne `"unavailable"`, `null`, `0`, ou rien du tout ?

6. **Entités utiles qui ne sont pas envoyées aujourd'hui** (donner l'`entity_id` et la valeur actuelle) :
   - tendance de pression (sur 3 h) ;
   - rafales de vent ;
   - humidité extérieure ;
   - condition météo actuelle (pluie, nuageux…) ;
   - lever et coucher du soleil ;
   - températures min/max du jour ;
   - pluie prévue dans les prochaines heures ;
   - autres données intéressantes pour un affichage de bord de mer (température de l'eau, houle…).

7. **Santé** : y a-t-il des erreurs dans les logs HA ou Mosquitto qui concernent `station/*` ou le client
   `epd-station-*` (connexions refusées, déconnexions répétées) ?

## Livrable attendu

Un rapport Markdown concis :
- un tableau récapitulatif (topic → source, cadence, retain, QoS) ;
- un payload d'exemple par topic ;
- les réponses aux questions 3 à 7 ;
- la liste des entités supplémentaires disponibles.

Aucun secret dans le rapport.
