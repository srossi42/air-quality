/*
 * Sonde extérieure — ESP32 + PMS5003 (particules) + BME280 (T/H/P)
 *
 * Toutes les MEASURE_INTERVAL_MS, la sonde mesure, diffuse en ESP-NOW vers
 * l'écran intérieur et publie sur Sensor.Community (mesures valides seulement).
 *
 * Deux modes pour le PMS5003, selon l'heure :
 *   - mode chantier (semaine, CHANTIER_START → CHANTIER_END) : ventilateur
 *     allumé en continu, moyenne de TOUTES les trames de la période + maximum.
 *     Les pics courts (camion, disqueuse) sont intégrés, pas manqués.
 *   - mode échantillon (le reste du temps) : réveil 30 s avant la mesure,
 *     moyenne de 3 lectures, puis sommeil. Préserve le ventilateur (~8 000 h).
 * Sans heure NTP, la sonde reste en mode échantillon.
 *
 * Bibliothèques : "PMS Library" (fu-hsi), "Adafruit BME280"
 * Wi-Fi : copier secrets.example.h en secrets.h (fichier exclu de Git)
 */
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#include "PMS.h"
#include "secrets.h"

// ==========================================
// RÉGLAGES
// ==========================================
const uint32_t MEASURE_INTERVAL_MS = 5UL * 60 * 1000;  // mesure + ESP-NOW + publication
const uint32_t PMS_WARMUP_MS       = 30UL * 1000;      // stabilisation du flux d'air (datasheet)
const uint8_t  PMS_SAMPLES         = 3;                // lectures en mode échantillon
const uint32_t WIFI_TIMEOUT_MS     = 10UL * 1000;
const uint32_t HTTPS_HANDSHAKE_S   = 15;   // au-delà, l'envoi à Sensor.Community abandonne

// Watchdog : si la boucle ne repasse pas pendant ce délai, la sonde redémarre.
// Un cycle dure au plus ~1 min 30 (35 s de mesure, Wi-Fi, deux envois HTTPS).
const uint32_t WDT_TIMEOUT_S = 180;
// Wi-Fi absent pendant autant de cycles de suite (6 x 5 min) : redémarrage.
// La tendance de pression repart alors de zéro (3 h avant de réapparaître).
const uint8_t  OFFLINE_RESTART_CYCLES = 6;

// Mode chantier : du lundi au vendredi, en minutes depuis minuit.
// Le chantier démarre entre 7 h et 8 h et finit à 18 h ou plus tard.
// Passer CHANTIER_MODE à false une fois le chantier terminé : le ventilateur
// du PMS5003 tourne alors ~11 % du temps au lieu de ~45 %.
const bool     CHANTIER_MODE      = true;
const uint16_t CHANTIER_START_MIN = 7 * 60;   // 07:00
const uint16_t CHANTIER_END_MIN   = 20 * 60;  // 20:00

// Identifiant enregistré sur Sensor.Community : SENSOR_ID, dans secrets.h.
// Sensor.Community n'authentifie que par cet identifiant : il reste hors de Git.
// Le MAC affiché au démarrage permet de vérifier qu'on flashe bien la carte enregistrée.
const char* SC_URL    = "https://api.sensor.community/v1/push-sensor-data/";

// Certificats racines Let's Encrypt, autorité de api.sensor.community :
// ISRG Root X1 (RSA, valable jusqu'en 2035) et ISRG Root X2 (ECDSA, jusqu'en
// 2040). Les deux, pour que la sonde continue de publier si le serveur change
// de chaîne de certificats. Vide ("") : connexion chiffrée mais serveur non
// authentifié (avertissement au log).
const char* SC_ROOT_CA = R"CERT(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)CERT";

const char* TZ_PARIS = "CET-1CEST,M3.5.0,M10.5.0/3";

IPAddress local_IP(192, 168, 1, 150);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(1, 1, 1, 1);

// Écran intérieur : adresse fixe sur le réseau de la box. Chaque mesure lui est
// envoyée par le Wi-Fi (UDP, acquitté et renvoyé par la radio en cas de perte),
// et en ESP-NOW en secours. Doit correspondre à DISPLAY_IP dans ecran-interieur.ino.
IPAddress displayIP(192, 168, 1, 151);
const uint16_t UDP_PORT = 4210;

// ==========================================
// PROTOCOLE ESP-NOW — identique dans ecran-interieur.ino
// ==========================================
const uint32_t PACKET_MAGIC   = 0x41495231;  // "AIR1" : écarte tout autre émetteur ESP-NOW
const uint8_t  PACKET_VERSION = 5;
enum : uint8_t {
  FLAG_BME_OK = 1 << 0,
  FLAG_PMS_OK = 1 << 1,
  FLAG_CHANTIER = 1 << 2,
  FLAG_TREND_OK = 1 << 3,  // 3 h d'historique de pression disponibles
};

struct __attribute__((packed)) AirPacket {
  uint32_t magic;
  uint8_t  version;
  uint8_t  flags;        // capteurs valides + mode chantier
  uint32_t epoch;        // heure de la mesure (UTC), 0 si pas encore synchronisée
  float    temperature;  // °C
  float    humidity;     // %
  float    pressure;     // hPa
  float    pm2_5;        // µg/m³, moyenne de la période
  float    pm10;         // µg/m³, moyenne de la période
  float    pm10Max;      // µg/m³, maximum de la période
  float    pressureTrend3h;  // hPa, variation ramenée à 3 h
  float    pm1_0;        // µg/m³, moyenne de la période
  // Diagnostic (version 5)
  uint32_t uptimeS;      // secondes depuis le démarrage de la sonde
  uint8_t  resetReason;  // esp_reset_reason() du dernier démarrage
  int8_t   rssi;         // force du signal de la box en dBm, 0 si hors ligne
};

// ==========================================
// OBJETS & ÉTAT
// ==========================================
Adafruit_BME280 bme;
bool bmePresent = false;

PMS pms(Serial2);  // RX2 = GPIO 16, TX2 = GPIO 17

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
WiFiUDP udp;

uint32_t lastCycleMs = 0;
bool firstCycle = true;
uint8_t offlineCycles = 0;
esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;

// Historique de pression pour la tendance météo : un point par cycle (5 min),
// 4 h de profondeur. La sonde tourne en continu, l'historique survit donc aux
// redémarrages de l'écran.
const uint8_t PRESSURE_SLOTS = 48;
struct PressurePoint {
  uint32_t atMs;
  float hPa;
};
PressurePoint pressureHistory[PRESSURE_SLOTS];
uint8_t pressureCount = 0;
uint8_t pressureNext = 0;

// Mode chantier : accumulation des trames entre deux publications.
bool pmsContinuous = false;
uint32_t warmupUntilMs = 0;
float accSum1 = 0, accSum25 = 0, accSum10 = 0, accMax10 = 0;
uint32_t accCount = 0;

// ==========================================
// WI-FI & HEURE
// ==========================================
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("Connexion Wi-Fi %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  const bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf(ok ? "\nConnecté, canal %d\n" : "\nÉchec de connexion\n", WiFi.channel());
  return ok;
}

bool timeIsSet() { return time(nullptr) > 1700000000; }  // après novembre 2023

bool chantierHours() {
  if (!CHANTIER_MODE || !timeIsSet()) return false;
  const time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  const bool weekday = local.tm_wday >= 1 && local.tm_wday <= 5;
  const uint16_t minute = local.tm_hour * 60 + local.tm_min;
  return weekday && minute >= CHANTIER_START_MIN && minute < CHANTIER_END_MIN;
}

// ==========================================
// CAPTEURS
// ==========================================
bool readBme(float& t, float& h, float& p) {
  if (!bmePresent) return false;
  bme.takeForcedMeasurement();
  t = bme.readTemperature();
  h = bme.readHumidity();
  p = bme.readPressure() / 100.0F;
  return !isnan(t) && !isnan(h) && !isnan(p) && h >= 0 && h <= 100 && p > 300;
}

void resetAccumulator() {
  accSum1 = accSum25 = accSum10 = accMax10 = 0;
  accCount = 0;
}

void setContinuousMode(bool continuous) {
  pmsContinuous = continuous;
  resetAccumulator();
  if (continuous) {
    pms.wakeUp();
    pms.activeMode();  // une trame par seconde environ, lue au fil de l'eau
    warmupUntilMs = millis() + PMS_WARMUP_MS;
    Serial.println("PMS5003 : mode chantier (continu)");
  } else {
    pms.passiveMode();
    pms.sleep();
    Serial.println("PMS5003 : mode échantillon");
  }
}

/** Mode chantier : à appeler le plus souvent possible depuis loop(). */
void accumulatePms() {
  PMS::DATA data;
  if (!pms.read(data)) return;
  if ((int32_t)(millis() - warmupUntilMs) < 0) return;  // trames de démarrage ignorées
  accSum1 += data.PM_AE_UG_1_0;
  accSum25 += data.PM_AE_UG_2_5;
  accSum10 += data.PM_AE_UG_10_0;
  if (data.PM_AE_UG_10_0 > accMax10) accMax10 = data.PM_AE_UG_10_0;
  accCount++;
}

/**
 * Mode échantillon : réveil, stabilisation, 3 lectures en mode passif, sommeil.
 * On vide le buffer avant chaque demande pour ne jamais lire une trame ancienne.
 */
bool samplePms(float& pm1, float& pm25, float& pm10, float& pm10Max) {
  pms.wakeUp();
  pms.passiveMode();
  delay(PMS_WARMUP_MS);

  float sum1 = 0, sum25 = 0, sum10 = 0, max10 = 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < PMS_SAMPLES; i++) {
    while (Serial2.available()) Serial2.read();
    pms.requestRead();
    PMS::DATA data;
    if (pms.readUntil(data, 2000)) {
      sum1 += data.PM_AE_UG_1_0;
      sum25 += data.PM_AE_UG_2_5;
      sum10 += data.PM_AE_UG_10_0;
      if (data.PM_AE_UG_10_0 > max10) max10 = data.PM_AE_UG_10_0;
      n++;
    }
    delay(1000);
  }
  pms.sleep();

  if (n == 0) return false;
  pm1 = sum1 / n;
  pm25 = sum25 / n;
  pm10 = sum10 / n;
  pm10Max = max10;
  return true;
}

bool readPms(float& pm1, float& pm25, float& pm10, float& pm10Max) {
  if (!pmsContinuous) return samplePms(pm1, pm25, pm10, pm10Max);
  if (accCount == 0) return false;
  pm1 = accSum1 / accCount;
  pm25 = accSum25 / accCount;
  pm10 = accSum10 / accCount;
  pm10Max = accMax10;
  Serial.printf("PMS5003 : moyenne sur %lu trames\n", (unsigned long)accCount);
  resetAccumulator();
  return true;
}

// ==========================================
// SENSOR.COMMUNITY
// ==========================================
String sensorValue(const char* type, float value, int decimals) {
  return String("{\"value_type\":\"") + type + "\",\"value\":\"" + String(value, decimals) + "\"}";
}

bool postToSensorCommunity(const char* pin, const String& values) {
  WiFiClientSecure client;
  client.setHandshakeTimeout(HTTPS_HANDSHAKE_S);  // 120 s par défaut
  if (strlen(SC_ROOT_CA) > 0) {
    client.setCACert(SC_ROOT_CA);
  } else {
    Serial.println("Attention : SC_ROOT_CA vide, serveur non authentifié");
    client.setInsecure();
  }

  HTTPClient http;
  if (!http.begin(client, SC_URL)) return false;
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Pin", pin);
  http.addHeader("X-Sensor", SENSOR_ID);

  const String body =
      String("{\"software_version\":\"sonde-ext-1.1\",\"sensordatavalues\":[") + values + "]}";
  const int code = http.POST(body);
  http.end();

  Serial.printf("Sensor.Community pin %s -> HTTP %d\n", pin, code);
  return code >= 200 && code < 300;
}

// ==========================================
// TENDANCE DE PRESSION
// ==========================================
void recordPressure(float hPa) {
  pressureHistory[pressureNext] = {millis(), hPa};
  pressureNext = (pressureNext + 1) % PRESSURE_SLOTS;
  if (pressureCount < PRESSURE_SLOTS) pressureCount++;
}

/**
 * Variation de pression ramenée à 3 h. Faux tant que l'historique ne contient
 * pas de point vieux de 2 h 30 à 3 h 30 (démarrage, capteur longtemps absent).
 */
bool pressureTrend3h(float& delta) {
  if (pressureCount < 2) return false;
  const PressurePoint& latest =
      pressureHistory[(pressureNext + PRESSURE_SLOTS - 1) % PRESSURE_SLOTS];
  const uint32_t minAge = 150UL * 60 * 1000, maxAge = 210UL * 60 * 1000;
  for (uint8_t i = 0; i < pressureCount; i++) {
    const PressurePoint& p =
        pressureHistory[(pressureNext + PRESSURE_SLOTS - pressureCount + i) % PRESSURE_SLOTS];
    const uint32_t age = latest.atMs - p.atMs;
    if (age >= minAge && age <= maxAge) {
      delta = (latest.hPa - p.hPa) * (180.0F * 60 * 1000 / age);
      return true;
    }
  }
  return false;
}

// ==========================================
// CYCLE DE MESURE
// ==========================================
void runCycle() {
  AirPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = PACKET_VERSION;
  if (pmsContinuous) packet.flags |= FLAG_CHANTIER;

  float t, h, p, pm1, pm25, pm10, pm10Max;
  if (readBme(t, h, p)) {
    packet.flags |= FLAG_BME_OK;
    packet.temperature = t;
    packet.humidity = h;
    packet.pressure = p;
    recordPressure(p);
    float trend;
    if (pressureTrend3h(trend)) {
      packet.flags |= FLAG_TREND_OK;
      packet.pressureTrend3h = trend;
    }
    Serial.printf("BME280  -> %.1f °C | %.0f %% | %.1f hPa\n", t, h, p);
  } else {
    Serial.println("BME280  -> mesure invalide, non publiée");
  }

  if (readPms(pm1, pm25, pm10, pm10Max)) {
    packet.flags |= FLAG_PMS_OK;
    packet.pm1_0 = pm1;
    packet.pm2_5 = pm25;
    packet.pm10 = pm10;
    packet.pm10Max = pm10Max;
    Serial.printf("PMS5003 -> PM1 %.1f | PM2.5 %.1f | PM10 %.1f (max %.0f) µg/m³\n", pm1, pm25,
                  pm10, pm10Max);
  } else {
    Serial.println("PMS5003 -> aucune trame, non publiée");
  }

  const bool online = ensureWiFi();
  packet.epoch = timeIsSet() ? (uint32_t)time(nullptr) : 0;
  packet.uptimeS = millis() / 1000;
  packet.resetReason = (uint8_t)bootReason;
  packet.rssi = online ? (int8_t)WiFi.RSSI() : 0;

  // Broadcast : pas d'accusé de réception possible, seul l'envoi local est vérifiable.
  const esp_err_t sent = esp_now_send(BROADCAST, (const uint8_t*)&packet, sizeof(packet));
  Serial.printf("ESP-NOW -> %s (canal %d, %u octets, v%u)\n",
                sent == ESP_OK ? "émis" : esp_err_to_name(sent), WiFi.channel(),
                (unsigned)sizeof(packet), packet.version);

  if (!online) {
    Serial.println("Hors ligne : envoi Wi-Fi à l'écran et publication Sensor.Community sautés");
    // La reconnexion automatique reste parfois bloquée après une coupure de la
    // box : un redémarrage repart de zéro.
    if (++offlineCycles >= OFFLINE_RESTART_CYCLES) {
      Serial.println("Wi-Fi absent depuis 30 min : redémarrage");
      delay(100);
      ESP.restart();
    }
    return;
  }
  offlineCycles = 0;

  // Même paquet par la box : chemin principal, l'antenne de la box porte bien
  // plus loin que celle d'un ESP32 à travers le vitrage.
  udp.beginPacket(displayIP, UDP_PORT);
  udp.write((const uint8_t*)&packet, sizeof(packet));
  const bool udpSent = udp.endPacket();
  Serial.printf("Wi-Fi   -> %s (%s:%u)\n", udpSent ? "émis" : "échec",
                displayIP.toString().c_str(), UDP_PORT);
  // Sensor.Community reçoit la moyenne ; le maximum reste local (écran).
  if (packet.flags & FLAG_PMS_OK) {
    // P0 = PM1, P1 = PM10, P2 = PM2.5 (nomenclature Sensor.Community)
    postToSensorCommunity("1", sensorValue("P0", pm1, 1) + "," + sensorValue("P1", pm10, 1) + "," +
                                   sensorValue("P2", pm25, 1));
  }
  if (packet.flags & FLAG_BME_OK) {
    postToSensorCommunity("11", sensorValue("temperature", t, 2) + "," +
                                    sensorValue("humidity", h, 2) + "," +
                                    sensorValue("pressure", p * 100.0F, 0));  // en Pa
  }
}


// ==========================================
// SETUP / LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== SONDE EXTÉRIEURE (PMS5003 + BME280) ===");
  bootReason = esp_reset_reason();
  Serial.printf("Cause du démarrage : %d\n", (int)bootReason);

  // Buffer élargi : en mode chantier, les trames continuent d'arriver pendant
  // les quelques secondes d'envoi HTTPS.
  Serial2.setRxBufferSize(1024);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  pms.passiveMode();
  pms.sleep();

  Wire.begin(21, 22);
  bmePresent = bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire);
  if (bmePresent) {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,  // température
                    Adafruit_BME280::SAMPLING_X1,  // pression
                    Adafruit_BME280::SAMPLING_X1,  // humidité
                    Adafruit_BME280::FILTER_OFF);
    Serial.println("BME280 détecté");
  } else {
    Serial.println("Attention : BME280 non détecté");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.config(local_IP, gateway, subnet, primaryDNS);
  Serial.printf("MAC : %s\n", WiFi.macAddress().c_str());
  ensureWiFi();
  configTzTime(TZ_PARIS, "pool.ntp.org", "time.cloudflare.com");

  // ESP-NOW sur le canal courant, donc celui de la box : l'écran le retrouve
  // en cherchant le SSID.
  if (esp_now_init() == ESP_OK) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;  // 0 = canal courant
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    Serial.println("ESP-NOW prêt");
  } else {
    Serial.println("Erreur d'initialisation ESP-NOW");
  }

  // Watchdog sur la tâche principale : une boucle figée redémarre la sonde.
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
  const bool chantier = chantierHours();
  if (chantier != pmsContinuous) setContinuousMode(chantier);
  if (pmsContinuous) accumulatePms();

  if (firstCycle || millis() - lastCycleMs >= MEASURE_INTERVAL_MS) {
    firstCycle = false;
    lastCycleMs = millis();
    runCycle();  // mode échantillon : ~35 s bloquant, la sonde n'a rien d'autre à faire
  }

  // Court en mode chantier pour lire chaque trame, plus long sinon.
  delay(pmsContinuous ? 20 : 1000);
}
