// Copier ce fichier en secrets.h et le remplir.
// secrets.h ne doit jamais être commité : ajouter la ligne "secrets.h" au .gitignore.
#pragma once

// Box : l'écran s'y connecte pour envoyer l'historique. Réseau 2,4 GHz.
const char* WIFI_SSID = "mon-reseau";
const char* WIFI_PASS = "mon-mot-de-passe";

// Historique Google Sheets (voir google-sheets/LISEZMOI.md) :
// l'URL du déploiement « Application Web », qui finit par /exec,
// et le même jeton que la constante TOKEN du script.
const char* SHEETS_URL   = "https://script.google.com/macros/s/.../exec";
const char* SHEETS_TOKEN = "une-phrase-longue-et-secrete";
