/*
 * Station intérieure — ESP32 + SCD40 (CO2) + SGP40 (COV) + e-paper 2,9" (296 x 128)
 *
 * Combine l'air intérieur (mesuré ici) et l'air extérieur (reçu de la sonde en
 * ESP-NOW toutes les 5 min) pour répondre à une seule question : faut-il ouvrir ?
 *
 *   AIR OK       rien à faire
 *   AERER        CO2 ou COV élevés, air extérieur correct
 *   ATTENDRE     CO2 ou COV élevés, mais particules élevées dehors (chantier)
 *   AERER 5 MIN  CO2 très élevé : on aère brièvement même si l'air extérieur est chargé
 *
 * Tendance météo : d'après l'évolution de la pression extérieure sur 3 h,
 * calculée par la sonde (baromètre à un seul point : indicatif, horizon 6 à 12 h).
 *
 * Affichage : rien la nuit (23 h – 6 h 30) ; le jour, redessin quand l'information
 * change vraiment (au plus toutes les 10 min, toutes les 2 min si le verdict
 * change), et au moins une fois par heure.
 *
 * Historique : toutes les 5 min, l'écran envoie une ligne (intérieur +
 * extérieur) à un tableau Google Sheets, par un script Apps Script (voir le
 * dossier google-sheets). Il se connecte pour cela à la box : ESP-NOW reste
 * disponible, puisque la sonde émet sur le canal de cette même box.
 *
 * Bibliothèques : GxEPD2 (+ Adafruit GFX), "Sensirion I2C SCD4x" (v1.x),
 *                 "Adafruit SGP40 Sensor" (inclut l'algorithme d'indice COV Sensirion)
 * Wi-Fi et Sheets : copier secrets.example.h en secrets.h (fichier exclu de Git)
 */
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <time.h>
#include <sys/time.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <Adafruit_SGP40.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include "secrets.h"

// ==========================================
// SEUILS DE DÉCISION (avec hystérésis : on n'oscille pas autour d'un seuil)
// ==========================================
const uint16_t CO2_VENTILATE_PPM = 1000;  // au-delà : aérer
const uint16_t CO2_RELAX_PPM     = 900;   // retour à la normale en dessous
const uint16_t CO2_URGENT_PPM    = 1400;  // au-delà : aérer même si l'air extérieur est chargé
const int32_t  VOC_VENTILATE     = 250;   // indice COV (100 = moyenne des 24 dernières heures)
const int32_t  VOC_RELAX         = 200;

// Air extérieur chargé : seuils « mauvais » de l'indice ATMO (µg/m³).
const float PM25_BAD = 25, PM25_RELAX = 20;
const float PM10_BAD = 50, PM10_RELAX = 40;

// ==========================================
// RÈGLES D'AFFICHAGE
// ==========================================
const uint16_t NIGHT_START_MIN = 23 * 60;      // 23:00 : plus de rafraîchissement
const uint16_t WAKE_MIN        = 6 * 60 + 30;  // 06:30 : écran à jour avant le lever

const uint32_t MIN_REDRAW_MS     = 10UL * 60 * 1000;  // plafond général
const uint32_t VERDICT_REDRAW_MS = 2UL * 60 * 1000;   // plafond si le verdict change
const uint32_t FORCED_REDRAW_MS  = 60UL * 60 * 1000;  // au moins un dessin par heure le jour

// Écart à partir duquel une valeur justifie un nouveau dessin.
const float    DELTA_TEMP_C  = 0.5;
const float    DELTA_HUM_PCT = 5.0;
const uint16_t DELTA_CO2_PPM = 100;
const int32_t  DELTA_VOC     = 50;
const float    DELTA_PM25    = 5.0;
const float    DELTA_PM10    = 10.0;

// Le SCD40 chauffe un peu, l'ESP32 et l'écran aussi : décalage retiré de sa
// température. 4 °C est la valeur d'usine ; l'ajuster d'après un thermomètre fiable.
const float SCD40_TEMP_OFFSET_C = 4.0;

// Tendance barométrique sur 3 h (hPa), seuils usuels des bulletins maritimes.
const float PRESSURE_STEADY_3H = 0.5;  // en dessous : stable
const float PRESSURE_FALL_3H   = 1.6;  // baisse franche : pluie probable
const float PRESSURE_FAST_3H   = 3.6;  // baisse rapide : dégradation, vent
const float HUMID_RAIN_PCT     = 85;   // humidité extérieure qui renforce la pluie

const uint32_t STALE_AFTER_S   = 20 * 60;           // sonde muette : 4 paquets manqués
const uint32_t RESCAN_EVERY_MS = 15UL * 60 * 1000;  // recherche du canal si muette

const char* TZ_PARIS = "CET-1CEST,M3.5.0,M10.5.0/3";

// ==========================================
// HISTORIQUE GOOGLE SHEETS
// ==========================================
const uint32_t LOG_INTERVAL_MS = 5UL * 60 * 1000;  // une ligne toutes les 5 min
const uint32_t WIFI_TIMEOUT_MS = 10UL * 1000;
const uint32_t HTTPS_HANDSHAKE_S = 15;              // 120 s par défaut
const uint32_t WIFI_RETRY_MS   = 5UL * 60 * 1000;  // box décrochée : nouvelle connexion
// Lignes d'historique perdues de suite (6 x 5 min) avant un redémarrage.
const uint8_t  LOG_FAILS_BEFORE_RESTART = 6;
// Watchdog : boucle figée plus longtemps (Wi-Fi + envoi + dessin < 40 s) : redémarrage.
const uint32_t WDT_TIMEOUT_S = 90;

// Adresse fixe de l'écran : la sonde lui envoie ses mesures ici, par la box.
// Doit correspondre à displayIP dans sonde-exterieure.ino, et être hors de la
// plage distribuée automatiquement par la box (ou réservée dans la box).
IPAddress DISPLAY_IP(192, 168, 1, 151);
IPAddress GATEWAY(192, 168, 1, 1);
IPAddress SUBNET(255, 255, 255, 0);
IPAddress DNS_SERVER(1, 1, 1, 1);
const uint16_t UDP_PORT = 4210;

// Certificats racines de Google (script.google.com) : Google Trust Services R1
// (RSA) et R4 (ECDSA), plus GlobalSign Root CA, qui signe encore une partie de
// leurs chaînes.
const char* GOOGLE_ROOT_CA = R"CERT(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDdTCCAl2gAwIBAgILBAAAAAABFUtaw5QwDQYJKoZIhvcNAQEFBQAwVzELMAkG
A1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24gbnYtc2ExEDAOBgNVBAsTB1Jv
b3QgQ0ExGzAZBgNVBAMTEkdsb2JhbFNpZ24gUm9vdCBDQTAeFw05ODA5MDExMjAw
MDBaFw0yODAxMjgxMjAwMDBaMFcxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9i
YWxTaWduIG52LXNhMRAwDgYDVQQLEwdSb290IENBMRswGQYDVQQDExJHbG9iYWxT
aWduIFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDaDuaZ
jc6j40+Kfvvxi4Mla+pIH/EqsLmVEQS98GPR4mdmzxzdzxtIK+6NiY6arymAZavp
xy0Sy6scTHAHoT0KMM0VjU/43dSMUBUc71DuxC73/OlS8pF94G3VNTCOXkNz8kHp
1Wrjsok6Vjk4bwY8iGlbKk3Fp1S4bInMm/k8yuX9ifUSPJJ4ltbcdG6TRGHRjcdG
snUOhugZitVtbNV4FpWi6cgKOOvyJBNPc1STE4U6G7weNLWLBYy5d4ux2x8gkasJ
U26Qzns3dLlwR5EiUWMWea6xrkEmCMgZK9FGqkjWZCrXgzT/LCrBbBlDSgeF59N8
9iFo7+ryUp9/k5DPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8E
BTADAQH/MB0GA1UdDgQWBBRge2YaRQ2XyolQL30EzTSo//z9SzANBgkqhkiG9w0B
AQUFAAOCAQEA1nPnfE920I2/7LqivjTFKDK1fPxsnCwrvQmeU79rXqoRSLblCKOz
yj1hTdNGCbM+w6DjY1Ub8rrvrTnhQ7k4o+YviiY776BQVvnGCv04zcQLcFGUl5gE
38NflNUVyRRBnMRddWQVDf9VMOyGj/8N7yy5Y0b2qvzfvGn9LhJIZJrglfCm7ymP
AbEVtQwdpf5pLGkkeB6zpxxxYu7KyJesF12KwvhHhm4qxFYxldBniYUr+WymXUad
DKqC5JlR3XC321Y9YeRq4VzW9v493kHMB65jUr9TU/Qr6cf9tveCX4XSQRjbgbME
HMUfpIBvFSDJ3gyICh3WZlXi/EjJKSZp4A==
-----END CERTIFICATE-----
)CERT";

// ==========================================
// PROTOCOLE ESP-NOW — identique dans sonde-exterieure.ino
// ==========================================
const uint32_t PACKET_MAGIC   = 0x41495231;  // "AIR1"
const uint8_t  PACKET_VERSION = 5;
enum : uint8_t {
  FLAG_BME_OK = 1 << 0,
  FLAG_PMS_OK = 1 << 1,
  FLAG_CHANTIER = 1 << 2,
  FLAG_TREND_OK = 1 << 3,  // la sonde a 3 h d'historique de pression
};

struct __attribute__((packed)) AirPacket {
  uint32_t magic;
  uint8_t  version;
  uint8_t  flags;
  uint32_t epoch;
  float    temperature;
  float    humidity;
  float    pressure;
  float    pm2_5;
  float    pm10;
  float    pm10Max;
  float    pressureTrend3h;  // hPa sur 3 h, calculée par la sonde
  float    pm1_0;            // µg/m³
  // Diagnostic (version 5)
  uint32_t uptimeS;          // secondes depuis le démarrage de la sonde
  uint8_t  resetReason;      // esp_reset_reason() de la sonde
  int8_t   rssi;             // signal de la box vu par la sonde, dBm (0 = hors ligne)
};

// ==========================================
// MATÉRIEL
// ==========================================
// Écran SPI : BUSY=4, RST=16, DC=17, CS=5, CLK=18, DIN=23. Capteurs I2C : SDA=21, SCL=22.
//
// Pilote de l'écran : le contrôleur change selon la version du module
// Waveshare 2,9". Un mauvais choix donne « Busy Timeout! » au moniteur série
// et un écran qui ne bouge pas. Changer le numéro, recompiler, essayer.
//   1 : noir/blanc V2 (SSD1680)                    GxEPD2_290_T94_V2
//   2 : noir/blanc V1 (IL3820)                     GxEPD2_290
//   3 : noir/blanc, contrôleur UC8151D             GxEPD2_290_T5D
//   4 : noir/blanc/rouge, module (B) V3 (UC8151D)  GxEPD2_290_Z13c
//   5 : noir/blanc/rouge, module (B) V4 (SSD1680)  GxEPD2_290_C90c
//   6 : DEPG0290BS (pilote du premier essai)       GxEPD2_290_BS
//   7 : noir/blanc, dalle GDEY029T94 (SSD1680)     GxEPD2_290_GDEY029T94
//   8 : noir/blanc, dalle GDEM029T94 (1re série)   GxEPD2_290_T94
// Ton premier code affichait avec le 6 : c'est le bon pilote pour ton module V3.
#define EPD_MODEL 6

// Hibernation : consommation nulle entre deux dessins, mais le réveil passe
// OBLIGATOIREMENT par la broche RST. Si RST est mal reliée, l'écran reste
// endormi (« Busy Timeout! »). false = simple mise hors tension, sans risque.
const bool EPD_HIBERNATE = false;

#define EPD_PINS /*CS=*/5, /*DC=*/17, /*RST=*/16, /*BUSY=*/4
#if EPD_MODEL == 1
GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> display(GxEPD2_290_T94_V2(EPD_PINS));
#elif EPD_MODEL == 2
GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT> display(GxEPD2_290(EPD_PINS));
#elif EPD_MODEL == 3
GxEPD2_BW<GxEPD2_290_T5D, GxEPD2_290_T5D::HEIGHT> display(GxEPD2_290_T5D(EPD_PINS));
#elif EPD_MODEL == 4
GxEPD2_3C<GxEPD2_290_Z13c, GxEPD2_290_Z13c::HEIGHT> display(GxEPD2_290_Z13c(EPD_PINS));
#elif EPD_MODEL == 5
GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> display(GxEPD2_290_C90c(EPD_PINS));
#elif EPD_MODEL == 7
GxEPD2_BW<GxEPD2_290_GDEY029T94, GxEPD2_290_GDEY029T94::HEIGHT> display(GxEPD2_290_GDEY029T94(EPD_PINS));
#elif EPD_MODEL == 8
GxEPD2_BW<GxEPD2_290_T94, GxEPD2_290_T94::HEIGHT> display(GxEPD2_290_T94(EPD_PINS));
#else
GxEPD2_BW<GxEPD2_290_BS, GxEPD2_290_BS::HEIGHT> display(GxEPD2_290_BS(EPD_PINS));
#endif
SensirionI2cScd4x scd4x;
Adafruit_SGP40 sgp;
bool scdPresent = false;
bool sgpPresent = false;

// ==========================================
// ÉTAT
// ==========================================
// Le callback ESP-NOW tourne dans la tâche Wi-Fi : échange protégé par un mutex.
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool rxPending = false;
// Diagnostic radio : tout ce qui arrive, et pourquoi c'est rejeté.
volatile uint32_t rxFrames = 0, rxBadSize = 0, rxBadMagic = 0;
// Paquets valides reçus par chaque chemin : au total, et depuis la dernière
// ligne d'historique (pour mesurer les pertes sans moniteur série).
volatile uint32_t rxEspNowOk = 0, rxUdpOk = 0;
volatile uint32_t logEspNowOk = 0, logUdpOk = 0;
uint32_t rxUdpFrames = 0;
WiFiUDP udp;
volatile int rxLastLen = 0;
volatile uint8_t rxLastVersion = 0;
uint32_t lastRadioReportMs = 0;
uint32_t lastLogMs = 0;
bool firstLogDone = false;
uint8_t logFailures = 0;
uint32_t lastWifiTryMs = 0;
esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;
// Compteur de démarrages : survit aux redémarrages logiciels, pas aux coupures de courant.
RTC_NOINIT_ATTR uint32_t bootCount;
AirPacket rxBuffer;

AirPacket outdoor = {};  // dernière mesure extérieure reçue
bool haveOutdoor = false;
uint32_t lastRxMs = 0;

struct Indoor {
  bool     co2Ok;
  uint16_t co2;          // ppm
  float    temperature;  // °C
  float    humidity;     // %
  int32_t  voc;          // indice 1–500, 0 = pas encore disponible (apprentissage)
};
Indoor indoor = {};

enum Verdict : uint8_t { V_UNKNOWN, V_OK, V_VENTILATE, V_VENTILATE_BRIEFLY, V_WAIT };
Verdict verdict = V_UNKNOWN;
bool needVentilation = false;  // états mémorisés pour l'hystérésis
bool outdoorBad = false;

// Ce qui est à l'écran, pour décider d'un nouveau dessin.
AirPacket shownOutdoor = {};
Indoor shownIndoor = {};
Verdict shownVerdict = V_UNKNOWN;
bool shownStale = false;
const char* shownForecast = "";
bool drawnOnce = false;

uint32_t lastDrawMs = 0;
uint32_t lastScanMs = 0;
uint32_t lastSgpMs = 0;

// ==========================================
// RÉCEPTION ESP-NOW
// ==========================================
/**
 * Contrôle commun aux deux chemins (ESP-NOW et Wi-Fi). Le même paquet arrive
 * souvent deux fois : le doublon remplace simplement le premier, sans effet.
 */
bool acceptPacket(const uint8_t* data, int len) {
  rxLastLen = len;
  if (len != (int)sizeof(AirPacket)) {  // taille inattendue : pas un paquet de la sonde
    rxBadSize++;
    return false;
  }
  AirPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != PACKET_MAGIC || packet.version != PACKET_VERSION) {
    rxBadMagic++;
    rxLastVersion = packet.version;
    return false;
  }

  portENTER_CRITICAL(&rxMux);
  rxBuffer = packet;
  rxPending = true;
  portEXIT_CRITICAL(&rxMux);
  return true;
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  rxFrames++;
  if (acceptPacket(data, len)) {
    rxEspNowOk++;
    logEspNowOk++;
  }
}

/** Chemin Wi-Fi : paquets UDP envoyés par la sonde via la box. */
void pollUdp() {
  const int size = udp.parsePacket();
  if (size <= 0) return;
  uint8_t buffer[64];
  const int len = udp.read(buffer, sizeof(buffer));
  rxUdpFrames++;
  if (acceptPacket(buffer, len)) {
    rxUdpOk++;
    logUdpOk++;
  }
}

/** La sonde émet sur le canal de la box : on le retrouve en cherchant le SSID. */
bool tuneToRouterChannel() {
  lastScanMs = millis();
  const int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  int channel = 0;
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      channel = WiFi.channel(i);
      break;
    }
  }
  WiFi.scanDelete();

  if (channel == 0) {
    Serial.printf("SSID %s introuvable, canal inchangé\n", WIFI_SSID);
    return false;
  }
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  Serial.printf("Écoute ESP-NOW sur le canal %d\n", channel);
  return true;
}

/**
 * Connexion à la box. Une fois connecté, l'écran est sur le canal de la box,
 * donc sur celui de la sonde : plus besoin de le chercher par scan.
 */
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("Connexion Wi-Fi %s", WIFI_SSID);
  WiFi.config(DISPLAY_IP, GATEWAY, SUBNET, DNS_SERVER);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  const bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf(ok ? "\nConnecté, canal %d\n" : "\nÉchec de connexion Wi-Fi\n", WiFi.channel());
  return ok;
}

// ==========================================
// CAPTEURS INTÉRIEURS
// ==========================================
void setupIndoorSensors() {
  Wire.begin(21, 22);

  scd4x.begin(Wire, SCD40_I2C_ADDR_62);
  scd4x.wakeUp();
  scd4x.stopPeriodicMeasurement();  // obligatoire avant tout réglage
  scd4x.reinit();
  scd4x.setTemperatureOffset(SCD40_TEMP_OFFSET_C);  // non persisté : pas d'usure de l'EEPROM
  scdPresent = scd4x.startPeriodicMeasurement() == 0;  // une mesure toutes les 5 s
  Serial.println(scdPresent ? "SCD40 démarré" : "Attention : SCD40 non détecté");

  sgpPresent = sgp.begin(&Wire);
  Serial.println(sgpPresent ? "SGP40 détecté" : "Attention : SGP40 non détecté");
}

/** SCD40 : lit la mesure dès qu'elle est prête (toutes les 5 s). */
void readScd40() {
  if (!scdPresent) return;
  bool ready = false;
  if (scd4x.getDataReadyStatus(ready) != 0 || !ready) return;

  uint16_t co2 = 0;
  float temperature = 0, humidity = 0;
  if (scd4x.readMeasurement(co2, temperature, humidity) != 0 || co2 == 0) return;
  indoor.co2Ok = true;
  indoor.co2 = co2;
  indoor.temperature = temperature;
  indoor.humidity = humidity;
}

/**
 * SGP40 : l'algorithme d'indice COV exige un appel par seconde, compensé en
 * température et humidité. L'indice est RELATIF : 100 = la moyenne de ta maison
 * sur 24 h. Il vaut 0 pendant la première minute, et s'affine sur 12 h.
 */
void readSgp40() {
  if (!sgpPresent || millis() - lastSgpMs < 1000) return;
  lastSgpMs = millis();
  const float t = indoor.co2Ok ? indoor.temperature : 25;
  const float h = indoor.co2Ok ? indoor.humidity : 50;
  indoor.voc = sgp.measureVocIndex(t, h);
}

// ==========================================
// DÉCISION
// ==========================================
bool isStale() { return !haveOutdoor || millis() - lastRxMs > STALE_AFTER_S * 1000UL; }

bool outdoorPmKnown() { return !isStale() && (outdoor.flags & FLAG_PMS_OK); }

void updateVerdict() {
  if (!indoor.co2Ok) {
    verdict = V_UNKNOWN;
    return;
  }

  const bool vocKnown = indoor.voc > 0;
  if (needVentilation) {
    needVentilation = indoor.co2 >= CO2_RELAX_PPM || (vocKnown && indoor.voc >= VOC_RELAX);
  } else {
    needVentilation = indoor.co2 >= CO2_VENTILATE_PPM || (vocKnown && indoor.voc >= VOC_VENTILATE);
  }

  if (!outdoorPmKnown()) {
    outdoorBad = false;  // inconnu : on ne bloque pas l'aération (l'écran affiche "ext ?")
  } else if (outdoorBad) {
    outdoorBad = outdoor.pm2_5 >= PM25_RELAX || outdoor.pm10 >= PM10_RELAX;
  } else {
    outdoorBad = outdoor.pm2_5 >= PM25_BAD || outdoor.pm10 >= PM10_BAD;
  }

  if (!needVentilation) {
    verdict = V_OK;
  } else if (!outdoorBad) {
    verdict = V_VENTILATE;
  } else {
    // Air chargé des deux côtés : un CO2 très élevé l'emporte, le temps d'un
    // courant d'air bref ; sinon, on attend que les particules retombent.
    verdict = indoor.co2 >= CO2_URGENT_PPM ? V_VENTILATE_BRIEFLY : V_WAIT;
  }
}

// ==========================================
// TENDANCE MÉTÉO (pression extérieure)
// ==========================================
// Calculée par la sonde, qui tourne en continu : la tendance survit aux
// redémarrages de l'écran. Elle n'apparaît que 3 h après un redémarrage de la sonde.

/** Libellé court pour le bandeau. Indicatif : une tendance, pas un bulletin. */
const char* forecastLabel() {
  if (isStale() || !(outdoor.flags & FLAG_BME_OK)) return "";
  if (!(outdoor.flags & FLAG_TREND_OK)) return "tendance...";
  const float delta = outdoor.pressureTrend3h;
  if (delta <= -PRESSURE_FAST_3H) return "degradation";
  if (delta <= -PRESSURE_FALL_3H) return "pluie probable";
  if (delta <= -PRESSURE_STEADY_3H) {
    return outdoor.humidity >= HUMID_RAIN_PCT ? "pluie possible" : "se couvre";
  }
  if (delta >= PRESSURE_FALL_3H) return "amelioration";
  return "stable";
}

const char* verdictLabel(Verdict v) {
  switch (v) {
    case V_OK:                return "AIR OK";
    case V_VENTILATE:         return "AERER";
    case V_VENTILATE_BRIEFLY: return "AERER 5 MIN";
    case V_WAIT:              return "ATTENDRE";
    default:                  return "...";
  }
}

// ==========================================
// HISTORIQUE
// ==========================================
const char* resetReasonLabel(uint8_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "alimentation";
    case ESP_RST_EXT:       return "bouton";
    case ESP_RST_SW:        return "logiciel";
    case ESP_RST_PANIC:     return "plantage";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "sous-tension";
    case ESP_RST_DEEPSLEEP: return "veille";
    default:                return "inconnu";
  }
}

/** Ajoute `"clé":valeur` au JSON, ou `"clé":null` si la mesure manque. */
void jsonNumber(String& json, const char* key, bool valid, float value, int decimals) {
  json += ",\"";
  json += key;
  json += "\":";
  json += valid ? String(value, decimals) : String("null");
}

/**
 * Envoie l'état courant au script Apps Script, qui ajoute une ligne au tableau.
 * Apps Script répond 302 (redirection vers sa réponse) une fois la ligne écrite :
 * 302 vaut donc succès, inutile de suivre la redirection.
 */
bool logToSheets() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Historique : Wi-Fi déconnecté, ligne sautée");
    return false;
  }
  const bool fresh = !isStale();
  const bool bmeOk = fresh && (outdoor.flags & FLAG_BME_OK);
  const bool pmsOk = fresh && (outdoor.flags & FLAG_PMS_OK);
  const bool trendOk = fresh && (outdoor.flags & FLAG_TREND_OK);

  String json = String("{\"token\":\"") + SHEETS_TOKEN + "\"";
  jsonNumber(json, "co2", indoor.co2Ok, indoor.co2, 0);
  jsonNumber(json, "cov", indoor.voc > 0, indoor.voc, 0);
  jsonNumber(json, "t_int", indoor.co2Ok, indoor.temperature, 1);
  jsonNumber(json, "h_int", indoor.co2Ok, indoor.humidity, 0);
  jsonNumber(json, "t_ext", bmeOk, outdoor.temperature, 1);
  jsonNumber(json, "h_ext", bmeOk, outdoor.humidity, 0);
  jsonNumber(json, "pression", bmeOk, outdoor.pressure, 1);
  jsonNumber(json, "pm25", pmsOk, outdoor.pm2_5, 1);
  jsonNumber(json, "pm10", pmsOk, outdoor.pm10, 1);
  jsonNumber(json, "pm10_max", pmsOk, outdoor.pm10Max, 0);
  jsonNumber(json, "pm1", pmsOk, outdoor.pm1_0, 1);
  jsonNumber(json, "tendance", trendOk, outdoor.pressureTrend3h, 1);
  json += String(",\"verdict\":\"") + verdictLabel(verdict) + "\"";
  json += String(",\"chantier\":") + ((fresh && (outdoor.flags & FLAG_CHANTIER)) ? "true" : "false");
  json += String(",\"sonde\":") + (fresh ? "true" : "false");
  // Réception depuis la dernière ligne : ~1 attendu par chemin toutes les 5 min.
  json += String(",\"rx_espnow\":") + logEspNowOk + ",\"rx_wifi\":" + logUdpOk;
  logEspNowOk = 0;
  logUdpOk = 0;
  // Diagnostic : redémarrages et qualité du lien Wi-Fi, de l'écran et de la sonde.
  json += String(",\"ecran_boot\":") + bootCount;
  json += String(",\"ecran_reset\":\"") + resetReasonLabel(bootReason) + "\"";
  json += String(",\"ecran_uptime_min\":") + (millis() / 60000);
  json += String(",\"ecran_rssi\":") + WiFi.RSSI();
  json += String(",\"sonde_reset\":") +
          (fresh ? String("\"") + resetReasonLabel(outdoor.resetReason) + "\"" : String("null"));
  jsonNumber(json, "sonde_uptime_min", fresh, outdoor.uptimeS / 60, 0);
  jsonNumber(json, "sonde_rssi", fresh && outdoor.rssi != 0, outdoor.rssi, 0);
  json += "}";

  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOT_CA);
  client.setHandshakeTimeout(HTTPS_HANDSHAKE_S);
  HTTPClient http;
  if (!http.begin(client, SHEETS_URL)) {
    Serial.println("Historique : URL invalide (SHEETS_URL)");
    return false;
  }
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(json);
  http.end();
  const bool ok = code == 200 || code == 302;
  Serial.printf("Historique -> HTTP %d%s\n", code, ok ? " (ligne ajoutée)" : "");
  return ok;
}

// ==========================================
// RÈGLES D'AFFICHAGE
// ==========================================
bool timeIsSet() { return time(nullptr) > 1700000000; }

/** Heure inconnue : on considère qu'on est de jour, pour ne jamais rester figé. */
bool daytime() {
  if (!timeIsSet()) return true;
  const time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  const uint16_t minute = local.tm_hour * 60 + local.tm_min;
  return minute >= WAKE_MIN && minute < NIGHT_START_MIN;
}

bool movedBy(float a, float b, float delta) { return fabsf(a - b) >= delta; }

bool significantChange() {
  if (isStale() != shownStale) return true;
  if (strcmp(forecastLabel(), shownForecast) != 0) return true;
  if (indoor.co2Ok != shownIndoor.co2Ok) return true;
  if (indoor.co2Ok &&
      (movedBy(indoor.co2, shownIndoor.co2, DELTA_CO2_PPM) ||
       movedBy(indoor.temperature, shownIndoor.temperature, DELTA_TEMP_C) ||
       movedBy(indoor.humidity, shownIndoor.humidity, DELTA_HUM_PCT))) {
    return true;
  }
  if ((indoor.voc > 0) != (shownIndoor.voc > 0) ||
      movedBy(indoor.voc, shownIndoor.voc, DELTA_VOC)) {
    return true;
  }
  if (outdoor.flags != shownOutdoor.flags) return true;
  if ((outdoor.flags & FLAG_BME_OK) &&
      (movedBy(outdoor.temperature, shownOutdoor.temperature, DELTA_TEMP_C) ||
       movedBy(outdoor.humidity, shownOutdoor.humidity, DELTA_HUM_PCT))) {
    return true;
  }
  if ((outdoor.flags & FLAG_PMS_OK) &&
      (movedBy(outdoor.pm2_5, shownOutdoor.pm2_5, DELTA_PM25) ||
       movedBy(outdoor.pm10, shownOutdoor.pm10, DELTA_PM10))) {
    return true;
  }
  return false;
}

// ==========================================
// AFFICHAGE
// ==========================================
void formatTime(uint32_t epoch, char* out, size_t size) {
  const time_t t = epoch;
  tm local;
  localtime_r(&t, &local);
  snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
}

/** Texte dans une police donnée, à la suite du curseur. */
void printIn(const GFXfont* font, const char* text) {
  display.setFont(font);
  display.print(text);
}

/** Texte aligné à droite, se terminant en xEnd. */
void printRight(int16_t xEnd, int16_t y, const GFXfont* font, const char* text) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setFont(font);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(xEnd - (int16_t)w - x1, y);
  display.print(text);
}

/** Une ligne du tableau : libellé à gauche, valeur en gras alignée à droite. */
void printRow(int16_t x, int16_t xEnd, int16_t y, const char* label, const char* value) {
  display.setCursor(x, y);
  printIn(&FreeSans9pt7b, label);
  printRight(xEnd, y, &FreeSansBold9pt7b, value);
}

/*
 * Mise en page 296 x 128 (disposition A3.4) : un bandeau, puis deux tableaux.
 *
 *   ██ AERER ███████████████ pluie probable ██   <- verdict + tendance météo
 *   Interieur               │ Exterieur    14:30
 *   ────────────────────    │ ────────────────────
 *   CO2 ppm           1150  │ Temp.        14.2 C
 *   COV                180  │ Humidite       78 %
 *   Temp.           21.3 C  │ PM1               8
 *   Humidite          45 %  │ PM2.5            12
 *                           │ PM10             20
 *
 * Valeurs alignées à droite de leur colonne : un nombre qui grandit s'étend
 * vers la gauche et ne déborde jamais de l'écran.
 */
void drawScreen() {
  const bool stale = isStale();
  const bool bmeOk = !stale && (outdoor.flags & FLAG_BME_OK);
  const bool pmsOk = !stale && (outdoor.flags & FLAG_PMS_OK);
  const char* forecast = forecastLabel();

  char measuredAt[8] = "";
  if (outdoor.epoch) formatTime(outdoor.epoch, measuredAt, sizeof(measuredAt));

  char buf[16];
  const int16_t L = 6, L_END = 141, R = 155, R_END = 290, SPLIT = 148;
  const int16_t HEADER_Y = 36;
  const int16_t ROWS_Y[] = {56, 73, 90, 107, 124};

  display.setFullWindow();  // rafraîchissement complet : aucune rémanence
  display.setTextWrap(false);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Bandeau : verdict à gauche, tendance météo à droite
    display.fillRect(0, 0, display.width(), 20, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(L, 15);
    printIn(&FreeSansBold9pt7b, verdictLabel(verdict));
    if (forecast[0]) printRight(R_END, 15, &FreeSans9pt7b, forecast);

    display.setTextColor(GxEPD_BLACK);
    display.drawFastVLine(SPLIT, 24, display.height() - 24, GxEPD_BLACK);

    // En-têtes soulignés
    display.setCursor(L, HEADER_Y);
    printIn(&FreeSansBold9pt7b, "Interieur");
    display.setCursor(R, HEADER_Y);
    if (!haveOutdoor) {
      printIn(&FreeSansBold9pt7b, "Sonde");
      printRight(R_END, HEADER_Y, &FreeSans9pt7b, "absente");
    } else if (stale) {
      printIn(&FreeSansBold9pt7b, "Sonde");
      printRight(R_END, HEADER_Y, &FreeSans9pt7b, "muette");
    } else {
      printIn(&FreeSansBold9pt7b, "Exterieur");
      printRight(R_END, HEADER_Y, &FreeSans9pt7b, measuredAt);
    }
    display.drawFastHLine(L, HEADER_Y + 4, L_END - L, GxEPD_BLACK);
    display.drawFastHLine(R, HEADER_Y + 4, R_END - R, GxEPD_BLACK);

    // Intérieur
    snprintf(buf, sizeof(buf), indoor.co2Ok ? "%u" : "--", indoor.co2);
    printRow(L, L_END, ROWS_Y[0], "CO2 ppm", buf);
    if (indoor.voc > 0) {
      snprintf(buf, sizeof(buf), "%ld", (long)indoor.voc);
    } else {
      snprintf(buf, sizeof(buf), sgpPresent ? "..." : "--");  // "..." : apprentissage
    }
    printRow(L, L_END, ROWS_Y[1], "COV", buf);
    snprintf(buf, sizeof(buf), indoor.co2Ok ? "%.1f C" : "--", indoor.temperature);
    printRow(L, L_END, ROWS_Y[2], "Temp.", buf);
    snprintf(buf, sizeof(buf), indoor.co2Ok ? "%.0f %%" : "--", indoor.humidity);
    printRow(L, L_END, ROWS_Y[3], "Humidite", buf);

    // Extérieur
    snprintf(buf, sizeof(buf), bmeOk ? "%.1f C" : "--", outdoor.temperature);
    printRow(R, R_END, ROWS_Y[0], "Temp.", buf);
    snprintf(buf, sizeof(buf), bmeOk ? "%.0f %%" : "--", outdoor.humidity);
    printRow(R, R_END, ROWS_Y[1], "Humidite", buf);
    // Des plus fines aux plus grosses. Le PM10 max reste dans l'historique.
    snprintf(buf, sizeof(buf), pmsOk ? "%.0f" : "--", outdoor.pm1_0);
    printRow(R, R_END, ROWS_Y[2], "PM1", buf);
    snprintf(buf, sizeof(buf), pmsOk ? "%.0f" : "--", outdoor.pm2_5);
    printRow(R, R_END, ROWS_Y[3], "PM2.5", buf);
    snprintf(buf, sizeof(buf), pmsOk ? "%.0f" : "--", outdoor.pm10);
    printRow(R, R_END, ROWS_Y[4], "PM10", buf);
  } while (display.nextPage());

  EPD_HIBERNATE ? display.hibernate() : display.powerOff();  // image conservée
  shownOutdoor = outdoor;
  shownIndoor = indoor;
  shownVerdict = verdict;
  shownStale = stale;
  shownForecast = forecast;
  drawnOnce = true;
  lastDrawMs = millis();
  Serial.printf("Écran : %s | %s | CO2 %u ppm | COV %ld\n", verdictLabel(verdict), forecast,
                indoor.co2, (long)indoor.voc);
}

void drawWaitingScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(8, 70);
    display.print("Demarrage...");
  } while (display.nextPage());
  EPD_HIBERNATE ? display.hibernate() : display.powerOff();
}


// ==========================================
// SETUP / LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== STATION INTÉRIEURE (SCD40 + SGP40 + E-PAPER) ===");
  bootReason = esp_reset_reason();
  // Après une coupure de courant, la mémoire RTC contient n'importe quoi.
  if (bootReason == ESP_RST_POWERON || bootReason == ESP_RST_BROWNOUT) bootCount = 0;
  bootCount++;
  Serial.printf("Démarrage n° %lu, cause : %s\n", (unsigned long)bootCount, resetReasonLabel(bootReason));

  setenv("TZ", TZ_PARIS, 1);
  tzset();

  display.init(115200);
  display.setRotation(1);  // paysage : 296 x 128
  drawWaitingScreen();

  setupIndoorSensors();

  WiFi.mode(WIFI_STA);
  // Sans économie d'énergie : en veille entre deux balises de la box, la radio
  // manquerait des paquets ESP-NOW de la sonde.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (!connectWiFi()) tuneToRouterChannel();  // sans box : on écoute quand même la sonde
  udp.begin(UDP_PORT);  // reste ouvert à travers les reconnexions

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDataRecv);
    Serial.println("ESP-NOW prêt");
  } else {
    Serial.println("Erreur d'initialisation ESP-NOW");
  }

  // Watchdog sur la tâche principale : une boucle figée redémarre l'écran.
#if ESP_IDF_VERSION_MAJOR >= 5
  const esp_task_wdt_config_t wdt = {WDT_TIMEOUT_S * 1000, 0, true};
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  pollUdp();

  if (rxPending) {
    portENTER_CRITICAL(&rxMux);
    outdoor = rxBuffer;
    rxPending = false;
    portEXIT_CRITICAL(&rxMux);

    haveOutdoor = true;
    Serial.printf("Paquet sonde reçu (flags %u)\n", outdoor.flags);
    lastRxMs = millis();
    // L'heure de la sonde fait foi : l'écran n'a pas besoin de se connecter.
    if (outdoor.epoch) {
      timeval tv = {(time_t)outdoor.epoch, 0};
      settimeofday(&tv, nullptr);
    }
  }

  readScd40();
  readSgp40();
  updateVerdict();

  // Sonde muette : la box a peut-être changé de canal.
  // Hors connexion seulement : connecté, l'écran suit déjà le canal de la box.
  if (isStale() && WiFi.status() != WL_CONNECTED && millis() - lastScanMs > RESCAN_EVERY_MS) {
    tuneToRouterChannel();
  }

  // La reconnexion automatique reste parfois bloquée après une coupure de la
  // box : on relance la connexion nous-mêmes.
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTryMs >= WIFI_RETRY_MS) {
    lastWifiTryMs = millis();
    WiFi.disconnect();
    connectWiFi();
  }

  // Historique : première ligne dès le premier affichage, puis toutes les 5 min.
  // 30 min sans pouvoir écrire une ligne : on redémarre plutôt que de rester muet.
  if (drawnOnce && (!firstLogDone || millis() - lastLogMs >= LOG_INTERVAL_MS)) {
    firstLogDone = true;
    lastLogMs = millis();
    if (logToSheets()) {
      logFailures = 0;
    } else if (++logFailures >= LOG_FAILS_BEFORE_RESTART) {
      Serial.println("Historique impossible depuis 30 min : redémarrage");
      delay(100);
      ESP.restart();
    }
  }

  // Premier dessin dès que le CO2 ou la sonde répondent, même la nuit.
  // Au plus tard 60 s après le démarrage, même sans aucune donnée : l'écran
  // montre alors "--" au lieu de rester bloqué sur "Demarrage...".
  if (!drawnOnce) {
    if (indoor.co2Ok || haveOutdoor || millis() > 60000) drawScreen();
  } else if (daytime()) {
    const uint32_t sinceDraw = millis() - lastDrawMs;
    // Au réveil, le dernier dessin date de la veille : le dessin forcé part à 6 h 30.
    // Une donnée qui apparaît ou disparaît (sonde reçue ou muette, COV prêt)
    // s'affiche aussi vite qu'un changement de verdict.
    const bool availabilityChanged =
        isStale() != shownStale || (indoor.voc > 0) != (shownIndoor.voc > 0);
    if (sinceDraw >= FORCED_REDRAW_MS ||
        ((verdict != shownVerdict || availabilityChanged) && sinceDraw >= VERDICT_REDRAW_MS) ||
        (sinceDraw >= MIN_REDRAW_MS && significantChange())) {
      drawScreen();
    }
  }

  // Bilan radio toutes les minutes, pour diagnostiquer une sonde « absente ».
  if (millis() - lastRadioReportMs >= 60000) {
    lastRadioReportMs = millis();
    uint8_t channel = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&channel, &second);
    Serial.printf("Radio : canal %u | ESP-NOW %lu trames, %lu valides | Wi-Fi %lu trames, %lu valides "
                  "| %lu mauvaise taille (dernière %d, attendu %u) | %lu autre version (dernière v%u)\n",
                  channel, (unsigned long)rxFrames, (unsigned long)rxEspNowOk,
                  (unsigned long)rxUdpFrames, (unsigned long)rxUdpOk, (unsigned long)rxBadSize,
                  rxLastLen, (unsigned)sizeof(AirPacket), (unsigned long)rxBadMagic, rxLastVersion);
  }

  delay(50);  // boucle rapide : le SGP40 doit être lu chaque seconde
}
