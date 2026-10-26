# Modifications à appliquer sur Home Assistant pour la station e-paper

Ce document fait suite au rapport d'analyse du 26/10/2026. **Ces modifications changent la configuration de HA** :
présente chaque changement (diff) au propriétaire et attends son accord avant de l'appliquer.
Fais une sauvegarde de chaque fichier modifié, recharge la configuration plutôt que de redémarrer quand c'est possible,
et ne recopie aucun secret.

## Ce que le firmware attend désormais

Le firmware de la station a été adapté au rapport :
- l'âge d'une mesure est lu dans `updated_at` (ISO 8601 avec fuseau). **Ce champ doit rester présent** dans
  `station/outdoor`, `station/ground` et `station/floor`. Au-delà de 3 h, la mesure est ignorée et l'écran affiche `--` ;
- une valeur `null` (ou non numérique) s'affiche `--` ;
- le coefficient affiché est celui de la prochaine pleine mer, et la marée suivante est calculée à partir de `days[]`
  (`summary` n'est pas utilisé) ;
- `station/forecast` est utilisé pour le vent, les rafales (`wind_gust_speed`) et la condition météo du moment (`condition`).

## 1. Publier `null` au lieu d'échouer quand un capteur est indisponible

Automatisations « outdoor to mqtt », « ground to mqtt » et « floor to mqtt » (`automations.yaml`).

Remplacer chaque `| float(default=nan)` par `| float(none)`. Un capteur `unavailable` ou `unknown` donne alors `null`,
`tojson` fonctionne, et `updated_at` est publié normalement.

Vérification dans Outils de développement → Modèle :

```jinja
{{ {'temperature': states('sensor.inexistant') | float(none), 'updated_at': now().isoformat()} | tojson }}
```

Résultat attendu : `{"temperature": null, "updated_at": "…"}`.

## 2. Ne plus perdre de déclenchements (« Already running »)

Pour les trois mêmes automatisations, passer de `mode: single` à :

```yaml
mode: queued
max: 3
```

Ainsi, le changement simultané de la température et de l'humidité publie bien la dernière valeur des deux.

## 3. Pression ramenée au niveau de la mer

`station/outdoor.pressure` vient de `sensor.salon_pressure` : c'est la pression à l'altitude de la maison,
environ 6 hPa sous la pression au niveau de la mer de Météo-France (1004,2 contre 1010,1).

Option recommandée : créer un capteur template « Pression niveau de la mer » à partir de `sensor.salon_pressure`
et de l'altitude de la maison (`elevation` dans la configuration générale de HA) :

```
P0 = P × (1 − 0,0065 × h / (T + 0,0065 × h + 273,15)) ^ −5,257
```

avec `h` l'altitude en mètres et `T` la température extérieure en °C. Le plus simple est d'utiliser
`sensor.home_outdoor_temperature_2` pour `T`, ou 15 °C si elle est indisponible.
Publier ce capteur dans `pressure`, arrondi à 0,1 hPa et avec `float(none)`.

Alternative plus simple : `state_attr('weather.trelevern', 'pressure')`. C'est la valeur modélisée par Météo-France,
moins réactive qu'une mesure locale.

## 4. Marées : plus de marge et une deuxième tentative

L'add-on Tide Scraper n'est lancé qu'à 00:00 et publie 3 jours, J-1 compris. Si le lancement de minuit échoue,
la courbe de marée de la station se vide dès le lendemain.
- Dans les options de l'add-on : passer `days` de `3` à `4`.
- Dans l'automatisation « Scrape tides » : ajouter un second déclencheur à 06:00 et un déclencheur au démarrage de HA
  (`platform: homeassistant`, `event: start`).
- Facultatif : passer la `qos` de l'add-on de `0` à `1`, comme les autres topics.

Vérifier ensuite que `station/tide` reste sous 4 Ko et compte au plus 32 événements.

## 5. Facultatif : tendance de pression sur 3 h

Ajouter un helper « Dérivée » (Paramètres → Appareils et services → Entrées → Dérivée) sur le capteur de pression
retenu au point 3 : fenêtre de 3 h, unité de temps en heures, résultat multiplié par 3 pour obtenir des hPa/3 h.
Ne pas encore l'envoyer à la station : le firmware ne l'affiche pas encore. Indiquer seulement l'`entity_id` créé.

## Contrôle final

```
mosquitto_sub -h 192.168.0.248 -u <user> -P <pass> -t 'station/#' -v -W 20
```

Vérifier les points suivants :
- les payloads `outdoor`, `ground` et `floor` contiennent bien `updated_at` et des nombres (ou `null`) ;
- la pression est cohérente avec `weather.trelevern` (à ±1 hPa) ;
- le log ne contient plus de « Already running » pour ces automatisations.

Rends un court compte rendu : changements appliqués, diff de chaque fichier, résultat des vérifications.
