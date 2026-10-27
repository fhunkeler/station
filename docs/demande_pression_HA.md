# Demande : pression ramenée au niveau de la mer pour la station e-paper

Home Assistant tourne sur 192.168.0.248 (`ssh root@192.168.0.248`).
**Fais d'abord l'analyse en lecture seule (étape 1), puis présente-moi le diff
complet des changements prévus et attends mon accord avant d'appliquer quoi que
ce soit.** Sauvegarde chaque fichier modifié, recharge la configuration plutôt
que de redémarrer quand c'est possible, et ne recopie aucun secret.

## Contexte

L'automatisation « outdoor to mqtt » publie sur `station/outdoor` :

```json
{"pressure": 1004.2, "temperature": 18.0, "updated_at": "2026-10-26T15:00:00.392181+01:00"}
```

`pressure` vient de `sensor.salon_pressure` : c'est la pression **absolue**, à
l'altitude de la maison. La station l'affiche telle quelle, avec environ 6 hPa
de moins que la pression **ramenée au niveau de la mer** utilisée par les
bulletins météo (1004,2 contre 1010,1 hPa pour `weather.trelevern` lors de
l'analyse du 26/10). Je veux que la station affiche la pression au niveau de la
mer, calculée à partir de la mesure locale.

## 1. Analyse (lecture seule)

1. Vérifie l'unité de `sensor.salon_pressure` (hPa attendu) et l'attribut
   `pressure` de `weather.trelevern` (pression au niveau de la mer de
   Météo-France).
2. Relève l'altitude de la maison : `elevation` dans la configuration générale
   de HA (Paramètres → Système → Général, ou `.storage/core.config`).
3. Contrôle cette altitude avec l'historique des 7 derniers jours : écart moyen
   entre `weather.trelevern.pressure` et `sensor.salon_pressure`. Un écart de
   1 hPa correspond à environ 8,3 m d'altitude. Si `elevation` est absente, vaut
   0, ou s'écarte de plus de 15 m de cette estimation, signale-le et propose la
   valeur à retenir.
4. Montre le YAML actuel de l'automatisation « outdoor to mqtt ».

## 2. Changements proposés (après mon accord)

### Capteur template « Pression niveau de la mer »

Formule (nivellement barométrique) :
`P0 = P × (1 − 0,0065 × h / (T + 0,0065 × h + 273,15)) ^ −5,257`, avec `P` la
mesure du salon en hPa, `h` l'altitude en mètres et `T` la température
extérieure en °C. Modèle à adapter (emplacement selon l'organisation de la
configuration : `configuration.yaml`, `templates.yaml` ou un paquet) :

```yaml
template:
  - sensor:
      - name: "Pression niveau de la mer"
        unique_id: pression_niveau_mer
        unit_of_measurement: "hPa"
        device_class: atmospheric_pressure
        state_class: measurement
        availability: "{{ has_value('sensor.salon_pressure') }}"
        state: >
          {% set p = states('sensor.salon_pressure') | float %}
          {% set h = ALTITUDE_EN_METRES %}
          {% set t = states('sensor.home_outdoor_temperature_2') | float(15) %}
          {{ (p * (1 - 0.0065 * h / (t + 0.0065 * h + 273.15)) ** -5.257) | round(1) }}
```

### Automatisation « outdoor to mqtt »

- Publier ce nouveau capteur dans `pressure` à la place de
  `sensor.salon_pressure`, et déclencher aussi l'automatisation sur son
  changement d'état.
- Garder `updated_at` (la station s'en sert pour détecter une donnée périmée).
- Si ce n'est pas déjà fait (point 1 de `docs/modifications_HA.md`) : remplacer
  `float(default=nan)` par `float(none)` pour publier `null` quand une source est
  indisponible, et passer en `mode: queued` (`max: 3`).

## 3. Vérification

1. Le nouveau capteur a une valeur proche de `weather.trelevern.pressure`, à
   ±1,5 hPa près.
2. Message publié :
   `mosquitto_sub -h 192.168.0.248 -u <user> -P <pass> -t 'station/outdoor' -v -W 70`
   (identifiants dans la configuration Mosquitto ou `secrets.yaml`, à ne pas
   recopier). `pressure` doit être la valeur au niveau de la mer, et
   `updated_at` doit être présent.
3. Le journal de HA ne contient pas d'erreur de template ni de « Already
   running » pour cette automatisation.

## Compte rendu attendu

Altitude retenue et justification, diff de chaque fichier modifié, valeurs
relevées pour la vérification (capteur, Météo-France, payload MQTT). Aucun
secret.
