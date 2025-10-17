#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

/*
 * ESP32 tabanlı akıllı otopark sensör düğümü.
 * HCSR04 ultrasonik sensörleri kullanarak her park alanının doluluk durumunu ölçer
 * ve sonuçları HTTP üzerinden backend servisine gönderir.
 */

// Wi-Fi yapılandırması (kendi ağ bilgilerinizi bu alanlara yazın)
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASSWORD = "";

// Backend temel URL'si (ör: http://192.168.1.50:8080)
const char *BACKEND_BASE = "https://kenyetta-microdont-kathey.ngrok-free.dev";

static WiFiClient plainClient;
static WiFiClientSecure secureClient;

// Arduino otomatik prototip üretimini doğru yönlendirmek için ileri bildirimler
struct ParkingSensor;
float readDistanceCm(ParkingSensor &sensor);
bool sendSpotStatus(ParkingSensor &sensor);
// Basit URL ayrıştırıcı ve HTTPClient başlatıcı (SNI ve doğru port ile)
bool beginHttpWithUrl(HTTPClient &http, const String &url) {
  bool isHttps = false;
  uint16_t port = 80;
  int idx = -1;
  if (url.startsWith("https://")) {
    isHttps = true;
    port = 443;
    idx = 8;
  } else if (url.startsWith("http://")) {
    isHttps = false;
    port = 80;
    idx = 7;
  } else {
    return false;
  }

  int slash = url.indexOf('/', idx);
  String host = slash > 0 ? url.substring(idx, slash) : url.substring(idx);
  String path = slash > 0 ? url.substring(slash) : "/";

  if (isHttps) {
    secureClient.stop();
    secureClient.setInsecure();
    return http.begin(secureClient, host.c_str(), port, path.c_str(), true);
  } else {
    return http.begin(plainClient, host.c_str(), port, path.c_str(), false);
  }
}

String extractHost(const String &url) {
  int idx = url.startsWith("https://") ? 8 : (url.startsWith("http://") ? 7 : -1);
  if (idx < 0) return String();
  int slash = url.indexOf('/', idx);
  return slash > 0 ? url.substring(idx, slash) : url.substring(idx);
}


// Kaydırma kayıtçısı (74HC595) pinleri
const uint8_t SHIFT_DATA_PIN = 23;   // 74HC595 SER
const uint8_t SHIFT_CLOCK_PIN = 18;  // 74HC595 SRCLK
const uint8_t SHIFT_LATCH_PIN = 5;   // 74HC595 RCLK (latch)

// HCSR04 sensör eşleşmeleri (ilk 10 park alanı)
struct ParkingSensor {
  const char *id;         // Park alanı kimliği (örn: F1-A1)
  uint8_t shiftIndex;     // 74HC595 çıkış bit indeksi
  uint8_t echoPin;        // Sensörün ECHO pini
  bool occupied;
  unsigned long lastPublishMillis;
};

ParkingSensor sensors[] = {
  {"F1-A1", 1, 34, false, 0},   // sr2:Q1 (ultrasonic1)
  {"F1-A2", 0, 35, false, 0},   // sr2:Q0 (ultrasonic2)
  {"F1-A3", 8, 32, false, 0},   // sr1:Q0 (ultrasonic3)
  {"F1-A4", 6, 33, false, 0},   // sr2:Q6 (ultrasonic4)
  {"F1-A5", 5, 25, false, 0},   // sr2:Q5 (ultrasonic5)
  {"F1-A6", 4, 26, false, 0},   // sr2:Q4 (ultrasonic6)
  {"F1-A7", 9, 27, false, 0},   // sr1:Q1 (ultrasonic7)
  {"F1-A8", 3, 14, false, 0},   // sr2:Q3 (ultrasonic8)
  {"F1-A9", 7, 12, false, 0},   // sr2:Q7 (ultrasonic9)
  {"F1-A10", 2, 13, false, 0},  // sr2:Q2 (ultrasonic10)
};

const size_t SENSOR_COUNT = sizeof(sensors) / sizeof(sensors[0]);

// Mesafe eşik değeri (cm). Eşik altındaki ölçümler park alanının dolu olduğunu varsayar.
const float OCCUPIED_THRESHOLD_CM = 35.0f;

// Okumalar arasındaki bekleme süresi (ms)
const uint16_t MEASUREMENT_INTERVAL_MS = 1500;

// Aynı alan için zorunlu tekrar gönderim aralığı (ms)
const uint32_t RESEND_INTERVAL_MS = 12000;

void driveShiftOutputs(uint16_t pattern) {
  digitalWrite(SHIFT_LATCH_PIN, LOW);
  shiftOut(SHIFT_DATA_PIN, SHIFT_CLOCK_PIN, MSBFIRST, (pattern >> 8) & 0xFF);
  shiftOut(SHIFT_DATA_PIN, SHIFT_CLOCK_PIN, MSBFIRST, pattern & 0xFF);
  digitalWrite(SHIFT_LATCH_PIN, HIGH);
}

float readDistanceCm(ParkingSensor &sensor) {
  driveShiftOutputs(0);
  delayMicroseconds(2);
  driveShiftOutputs(1u << sensor.shiftIndex);
  delayMicroseconds(10);
  driveShiftOutputs(0);

  long duration = pulseIn(sensor.echoPin, HIGH, 25000); // Maksimum ~4m
  if (duration <= 0) {
    return -1.0f;
  }

  float distance = (duration * 0.0343f) / 2.0f; // Ses hızı: 343 m/s
  return distance;
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("WiFi bağlantısı kuruluyor");
  uint8_t retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print('.');
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi bağlantısı başarılı");
    Serial.print("IP adresi: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi bağlantısı başarısız");
  }
}

bool sendSpotStatus(ParkingSensor &sensor) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi bağlı değil, gönderim atlandı");
    return false;
  }

  String url = String(BACKEND_BASE) + "/spots/" + sensor.id;
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);
  bool began = beginHttpWithUrl(http, url);
  if (!began) {
    Serial.println("HTTP isteği başlatılamadı");
    return false;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "true");
  http.addHeader("Connection", "close");

  String payload = String("{\"occupied\":") + (sensor.occupied ? "true" : "false") + "}";
  int httpCode = http.POST(payload);
  if (httpCode <= 0 || httpCode >= 500) {
    // Basit bir tekrar denemesi: kısa bekleyip bir kez daha gönder
    delay(500);
    http.end();
    HTTPClient httpRetry;
    httpRetry.setConnectTimeout(8000);
    httpRetry.setTimeout(8000);
    httpRetry.setReuse(false);
    if (!beginHttpWithUrl(httpRetry, url)) {
      Serial.println("HTTP yeniden başlatılamadı");
      return false;
    }
    httpRetry.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpRetry.addHeader("Content-Type", "application/json");
    httpRetry.addHeader("ngrok-skip-browser-warning", "true");
    httpRetry.addHeader("Connection", "close");
    httpCode = httpRetry.POST(payload);
    httpRetry.end();
  }

  if (httpCode > 0) {
    Serial.print("[HTTP] ");
    Serial.print(sensor.id);
    Serial.print(" -> ");
    Serial.println(httpCode);
    if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_CREATED) {
      Serial.println(http.getString());
    }
  } else {
    Serial.print("[HTTP HATA] ");
    Serial.print(sensor.id);
    Serial.print(" -> ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  return httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  secureClient.setInsecure();

  pinMode(SHIFT_DATA_PIN, OUTPUT);
  pinMode(SHIFT_CLOCK_PIN, OUTPUT);
  pinMode(SHIFT_LATCH_PIN, OUTPUT);
  driveShiftOutputs(0);

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    pinMode(sensors[i].echoPin, INPUT);
  }

  connectWifi();

  // Bağlantı diagnostikleri
  String host = extractHost(BACKEND_BASE);
  if (host.length()) {
    IPAddress ip;
    if (WiFi.hostByName(host.c_str(), ip)) {
      Serial.print("Backend host çözüldü: "); Serial.println(ip);
    } else {
      Serial.println("DNS çözümleme başarısız");
    }

    HTTPClient httpDiag;
    String diagUrl = String(BACKEND_BASE) + "/state";
    httpDiag.setConnectTimeout(6000);
    httpDiag.setTimeout(6000);
    httpDiag.setReuse(false);
    if (beginHttpWithUrl(httpDiag, diagUrl)) {
      int code = httpDiag.GET();
      Serial.print("[HTTP DIAG] /state -> "); Serial.println(code);
      httpDiag.end();
    } else {
      Serial.println("[HTTP DIAG] başlatılamadı");
    }
  }
}

void loop() {
  connectWifi();

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    ParkingSensor &sensor = sensors[i];
    float distance = readDistanceCm(sensor);
    bool detected = (distance > 0 && distance <= OCCUPIED_THRESHOLD_CM);
    bool stateChanged = (sensor.occupied != detected);
    sensor.occupied = detected;

    Serial.print("Spot ");
    Serial.print(sensor.id);
    Serial.print(" mesafe: ");
    Serial.print(distance);
    Serial.print(" cm -> ");
    Serial.println(detected ? "DOLU" : "BOS");

    unsigned long now = millis();
    bool resendDue = (now - sensor.lastPublishMillis) >= RESEND_INTERVAL_MS;
    if (stateChanged || resendDue) {
      if (sendSpotStatus(sensor)) {
        sensor.lastPublishMillis = now;
      }
    }

  delay(250);
  }

  delay(MEASUREMENT_INTERVAL_MS);
}
