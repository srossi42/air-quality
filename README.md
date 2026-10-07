# Station air

Une station de qualité de l'air faite maison, en deux boîtiers ESP32 :

- une **sonde extérieure** mesure les particules, la température, l'humidité et
  la pression, et publie ses mesures sur [Sensor.Community](https://sensor.community) ;
- un **écran intérieur** mesure le CO2 et les COV de la pièce, reçoit les
  mesures de la sonde et affiche une seule consigne : **aérer ou attendre**.

Point de départ : un grand chantier en face de chez moi. Aérer quand le CO2
monte, oui, mais pas quand la poussière du chantier est au plus haut.

## Ce que fait la station

| Consigne | Condition |
|---|---|
| AIR OK | CO2 et COV normaux |
| AERER | CO2 ≥ 1 000 ppm ou indice COV ≥ 250, et air extérieur correct |
| ATTENDRE | besoin d'aérer, mais PM2.5 ≥ 25 ou PM10 ≥ 50 µg/m³ dehors |
| AERER 5 MIN | CO2 ≥ 1 400 ppm : on aère brièvement même si l'air extérieur est chargé |

Chaque seuil a une hystérésis (par exemple retour à la normale sous 900 ppm) :
la consigne n'oscille pas autour d'une valeur.

L'écran affiche aussi le CO2, les COV, les températures et humidités intérieure
et extérieure, les PM1, PM2.5 et PM10, et une tendance météo calculée sur
l'évolution de la pression pendant 3 heures.

## Matériel

| Boîtier | Composants |
|---|---|
| Sonde extérieure | ESP32, Plantower PMS5003, Bosch BME280, boîtier IP65 à l'ombre |
| Écran intérieur | ESP32, Sensirion SCD40 (CO2), Sensirion SGP40 (COV), e-paper Waveshare 2,9" (296 × 128) |

Câblage :

| Composant | Broches ESP32 |
|---|---|
| PMS5003 (UART) | TX du capteur sur GPIO 16, RX du capteur sur GPIO 17, 5 V |
| BME280, SCD40, SGP40 (I2C) | SDA GPIO 21, SCL GPIO 22, 3,3 V |
| E-paper (SPI) | CS 5, DC 17, RST 16, BUSY 4, SCK 18, MOSI 23 |

## Fonctionnement

```
Sonde extérieure ──ESP-NOW (direct)──────────┐
        │        ──UDP (par la box)──────────┤
        │                                    ▼
        └──HTTPS──> Sensor.Community    Écran intérieur ──HTTPS──> Google Sheets
```

**Sonde.** Une mesure toutes les 5 minutes. Le PMS5003 a deux modes :

- *mode chantier* (jours de semaine, 7 h à 20 h) : ventilateur allumé en
  continu, moyenne de toutes les trames de la période et maximum. Les pics
  courts (camion, disqueuse) sont mesurés, pas manqués ;
- *mode échantillon* (le reste du temps) : réveil 30 s avant la mesure,
  3 lectures, puis sommeil, pour préserver le ventilateur.

Chaque mesure part par deux chemins vers l'écran : ESP-NOW en direct, et UDP
par la box. Si l'un des deux passe, l'écran est à jour. Les paquets portent un
marqueur et un numéro de version : l'écran rejette tout autre émetteur.

**Écran.** L'e-paper ne consomme rien entre deux affichages. Il n'est
redessiné que si l'information change vraiment : consigne modifiée (au plus
toutes les 2 minutes), autre valeur (au plus toutes les 10 minutes), et au moins
une fois par heure. Rien la nuit, de 23 h à 6 h 30.

**Historique.** Toutes les 5 minutes, l'écran envoie une ligne à un script
Google Apps Script, qui l'ajoute à un tableau Google Sheets. Un onglet
« Tableau de bord » affiche 7 graphes, avec période, pas d'agrégation et
échelles réglables. Voir [`google-sheets/LISEZMOI.md`](google-sheets/LISEZMOI.md).

## Installation

1. Arduino IDE, carte « ESP32 Dev Module » (core ESP32 3.x).
2. Bibliothèques :
   - sonde : *PMS Library* (fu-hsi), *Adafruit BME280* ;
   - écran : *GxEPD2*, *Adafruit GFX*, *Sensirion I2C SCD4x* (v1.x),
     *Adafruit SGP40 Sensor*.
3. Dans chaque dossier, copier `secrets.example.h` en `secrets.h` et le remplir.
   `secrets.h` est exclu de Git.
4. Écran : régler `EPD_MODEL` selon le modèle de dalle (6 pour la Waveshare 2,9" V3).
5. Téléverser la sonde, puis l'écran. Le moniteur série (115 200 bauds) affiche
   chaque envoi et chaque réception.

## Vie privée

- La sonde publie uniquement des mesures extérieures, avec une position
  arrondie. Le CO2 et les COV intérieurs ne quittent pas la maison, sauf vers
  mon propre tableau Google Sheets.
- Les identifiants Wi-Fi et le jeton du script sont dans `secrets.h`, jamais
  dans le code.
- L'écriture dans Google Sheets est protégée par un jeton secret partagé entre
  l'écran et le script.

## Contribution

Le dossier [`contrib-aereni`](contrib-aereni) contient une contribution au
réseau citoyen de mesure de la qualité de l'air [Aereni](https://github.com/ateliersolidairesaintouen/aereni) :
accepter les capteurs Plantower (PMS5003) en plus du SDS011.

## Limites connues

- Le BME280 dans un boîtier peut lire 1 à 3 °C trop haut. Une correction sera
  ajoutée après quelques jours de comparaison avec la météo.
- La tendance météo vient d'un seul baromètre : elle est indicative, sur un
  horizon de 6 à 12 heures.
- Le PMS5003 n'est pas un appareil de référence : les valeurs sont utiles pour
  comparer des périodes, pas pour une mesure réglementaire.
