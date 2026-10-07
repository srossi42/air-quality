# Historique et tableau de bord dans Google Sheets

L'écran intérieur envoie une ligne toutes les 5 minutes (intérieur et
extérieur) à un script Google Apps Script, qui l'ajoute à l'onglet « Mesures ».
L'onglet « Tableau de bord » affiche 7 graphes, réglables : période, pas
d'agrégation, échelle de chaque graphe.

## 1. Créer le tableau et le script

1. Crée un tableau Google Sheets vide, par exemple « Station air ».
2. Fichier > Paramètres : fuseau horaire **(GMT+01:00) Paris**.
3. Extensions > Apps Script. Remplace le contenu de `Code.gs` par celui de ce
   dossier.
4. Dans l'éditeur, roue dentée **Paramètres du projet** : fuseau horaire
   **Europe/Paris** aussi. Sans ça, les regroupements par heure ou par jour
   sont décalés de 1 à 2 heures.
5. Toujours dans **Paramètres du projet**, section **Propriétés du script** :
   ajoute la propriété `TOKEN`, avec pour valeur une phrase longue et secrète.
   Le jeton n'est jamais écrit dans `Code.gs`, qui est publié sur Git.

## 2. Tester le script seul

Choisis la fonction `testerAjout` puis Exécuter. Google demande l'autorisation
d'accéder au tableau : accepte. Un onglet « Mesures » apparaît avec une ligne
« TEST ». Supprime cette ligne.

## 3. Publier en application web

1. Déployer > Nouveau déploiement > type **Application Web**.
2. Exécuter en tant que : **moi**. Qui a accès : **tout le monde**.
   L'écran n'a pas de compte Google : c'est le jeton qui protège l'écriture.
3. Copie l'URL, qui finit par `/exec`.

Une modification ultérieure du script demande un nouveau déploiement :
Déployer > Gérer les déploiements > modifier > Nouvelle version. L'URL reste
la même.

## 4. Configurer l'écran

Dans `ecran-interieur/secrets.h` (modèle : `secrets.example.h`) :

```cpp
const char* WIFI_SSID    = "ton-reseau";
const char* WIFI_PASS    = "ton-mot-de-passe";
const char* SHEETS_URL   = "https://script.google.com/macros/s/.../exec";
const char* SHEETS_TOKEN = "la-meme-phrase-que-TOKEN";
```

Téléverse. Une ligne arrive toutes les 5 minutes dans l'onglet Mesures.

## 5. Installer le tableau de bord

Recharge le tableau : un menu **Station air** apparaît. Choisis
**Installer le tableau de bord** et accepte l'autorisation (elle sert à
l'actualisation automatique). L'onglet « Tableau de bord » se crée :

| Cellule | Réglage |
|---|---|
| B3 Période | 24 heures, 3 jours, 7 jours, 30 jours, Tout, Personnalisée |
| B4 / B5 Du / Au | dates de début et de fin, si Période = Personnalisée |
| B6 Pas | Auto, Brut (5 min), 15 min, 1 heure, 1 jour |
| B10 à C16 | Y min et Y max de chaque graphe ; vide = automatique |

Chaque modification d'un réglage recalcule les graphes en quelques secondes.
Les graphes s'actualisent aussi seuls toutes les 30 minutes, et à la demande
par le menu Station air > Actualiser.

Le regroupement fait la **moyenne** des mesures, sauf pour le **PM10 max**
(on garde le maximum) et les paquets reçus (on fait la somme). En Auto, le pas
est choisi pour ne pas dépasser 1 500 points par courbe.

Les graphes :

1. CO2 intérieur, avec le seuil d'aération à 1 000 ppm
2. Particules extérieures (PM1, PM2.5, PM10, PM10 max), avec la limite européenne
   PM10 de 50 µg/m³ en moyenne sur une journée
3. Températures intérieure et extérieure
4. Humidités intérieure et extérieure
5. Pression extérieure
6. Indice COV intérieur
7. Réception de la sonde : paquets reçus par ESP-NOW et par le Wi-Fi. Au pas
   de 5 minutes, chaque courbe devrait valoir 1. Un creux sur ESP-NOW seul,
   c'est la vitre ; un creux sur les deux, c'est la sonde.

La limite PM10 européenne porte sur une moyenne journalière : compare-la avec
le pas « 1 jour », pas avec les pics sur 5 minutes.

## En cas de problème

| Symptôme | Cause |
|---|---|
| aucune ligne dans Mesures | URL fausse, déploiement pas sur « tout le monde », ou jeton différent |
| menu Station air absent | recharger le tableau après avoir enregistré le script |
| heures décalées | fuseau horaire du tableau ou du projet Apps Script pas sur Paris |
| graphes lents | période longue au pas Brut : passer en Auto |
