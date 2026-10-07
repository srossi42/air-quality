// Copier ce fichier en secrets.h et le remplir.
// secrets.h ne doit jamais être commité : ajouter la ligne "secrets.h" au .gitignore.
#pragma once

const char* WIFI_SSID = "mon-reseau";      // réseau 2,4 GHz : l'ESP32 ne voit pas le 5 GHz
const char* WIFI_PASS = "mon-mot-de-passe";

// Identifiant Sensor.Community : "esp32-" suivi du MAC de la carte, sans ":".
const char* SENSOR_ID = "esp32-xxxxxxxxxxxx";
