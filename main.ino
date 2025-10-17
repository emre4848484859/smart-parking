// bu dosya esp32 tabanlı akıllı otopark düğümünün ana firmware kodunu içerir
// amaç: hcsr04 sensörlerle mesafe ölçmek, doluluk durumunu belirlemek ve http üzerinden backend'e göndermek
// not: tüm yorumlar istemin talebi gereği küçük harfle yazılmıştır

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

/*
 * esp32 tabanlı akıllı otopark sensör düğümü.
 * hcsr04 ultrasonik sensörleri kullanarak her park alanının doluluk durumunu ölçer
 * ve sonuçları http üzerinden backend servisine gönderir.
 * * hata ayıklama ve bağlantı diagnostikleri için ek yardımcı çıktılar barındırır.
 */

// =======================================================================
//                           yapılandırma
// =======================================================================

// kendi wi-fi ağ bilgilerinizi bu alanlara yazın
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASSWORD = "";

// backend temel url'si (codespaces 8080 portunun public adresi veya ngrok adresi)
// not: "$0" gibi fazladan path segmentleri kullanmayın. doğrusu aşağıdaki gibidir:
const char *BACKEND_BASE = "https://obscure-halibut-pj59wxv6rggvf67pq-8080.app.github.dev";


// =======================================================================
//                         donanım tanımlamaları
// =======================================================================

// kaydırma kayıtçısı (74hc595) pinleri
const uint8_t SHIFT_DATA_PIN = 23;   // 74HC595 SER (DS)
const uint8_t SHIFT_CLOCK_PIN = 18;  // 74HC595 SRCLK (SH_CP)
const uint8_t SHIFT_LATCH_PIN = 5;   // 74HC595 RCLK (ST_CP)

// hcsr04 sensör eşleşmeleri
struct ParkingSensor {
  const char *id;           // park alanı kimliği (örn: f1-a1)
  uint8_t shiftIndex;       // 74hc595 çıkış bit indeksi (0-15)
  uint8_t echoPin;          // sensörün echo pini
  bool occupied;            // mevcut doluluk durumu
  unsigned long lastPublishMillis; // son başarılı gönderim zamanı (ms)
};

ParkingSensor sensors[] = {
  {"F1-A1",  1, 34, false, 0},   // sr2:Q1 (ultrasonic1)
  {"F1-A2",  0, 35, false, 0},   // sr2:Q0 (ultrasonic2)
  {"F1-A3",  8, 32, false, 0},   // sr1:Q0 (ultrasonic3)
  {"F1-A4",  6, 33, false, 0},   // sr2:Q6 (ultrasonic4)
  {"F1-A5",  5, 25, false, 0},   // sr2:Q5 (ultrasonic5)
  {"F1-A6",  4, 26, false, 0},   // sr2:Q4 (ultrasonic6)
  {"F1-A7",  9, 27, false, 0},   // sr1:Q1 (ultrasonic7)
  {"F1-A8",  3, 14, false, 0},   // sr2:Q3 (ultrasonic8)
  {"F1-A9",  7, 12, false, 0},   // sr2:Q7 (ultrasonic9)
  {"F1-A10", 2, 13, false, 0}    // sr2:Q2 (ultrasonic10)
};

const size_t SENSOR_COUNT = sizeof(sensors) / sizeof(sensors[0]);

// =======================================================================
//                            parametreler
// =======================================================================

// mesafe eşik değeri (cm). bu değerin altındaki ölçümler park alanını dolu sayar.
const float OCCUPIED_THRESHOLD_CM = 35.0f;

// her bir sensör döngüsü arasındaki bekleme süresi (ms)
const uint16_t MEASUREMENT_INTERVAL_MS = 1500;

// bir park yeri durumu değişmese bile ne kadar sürede bir sunucuya tekrar gönderim yapılacağı (ms)
const uint32_t RESEND_INTERVAL_MS = 12000;


// =======================================================================
//                        yardımcı fonksiyonlar
// =======================================================================

// wifi bağlantısını kurar veya mevcut bağlantıyı kontrol eder
void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.print("wifi bağlantısı kuruluyor");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print('.');
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
  Serial.println("\nwifi bağlantısı başarılı!");
  Serial.print("ip adresi: ");
    Serial.println(WiFi.localIP());
  } else {
  Serial.println("\nwifi bağlantısı başarısız. lütfen ağ ayarlarını kontrol edin.");
  }
}

// 74hc595 kaydırma kayıtçılarını kontrol ederek sensörlerin trig pinlerini seçici biçimde sürer
void driveShiftOutputs(uint16_t pattern) {
  digitalWrite(SHIFT_LATCH_PIN, LOW);
  shiftOut(SHIFT_DATA_PIN, SHIFT_CLOCK_PIN, MSBFIRST, (pattern >> 8) & 0xFF); // İkinci 595 (sr1)
  shiftOut(SHIFT_DATA_PIN, SHIFT_CLOCK_PIN, MSBFIRST, pattern & 0xFF);        // Birinci 595 (sr2)
  digitalWrite(SHIFT_LATCH_PIN, HIGH);
}

// belirtilen sensörden ultrasonik mesafe ölçümü yapar
float readDistanceCm(ParkingSensor &sensor) {
  driveShiftOutputs(0); // önce tüm trig pinlerini kapat
  delayMicroseconds(2);

  // sadece ilgili sensörün trig pinini aç
  driveShiftOutputs(1u << sensor.shiftIndex);
  delayMicroseconds(10);
  driveShiftOutputs(0); // trig pinini tekrar kapat

  // echo pininden gelen sinyalin süresini ölç (timeout 25ms, yaklaşık 4 metreye denk gelir)
  long duration = pulseIn(sensor.echoPin, HIGH, 25000); 
  if (duration <= 0) {
    return -1.0f; // zaman aşımı veya hata
  }

  // süreyi santimetreye çevir (ses hızı ~343 m/s)
  return (duration * 0.0343f) / 2.0f;
}

// park yeri durumunu backend sunucusuna http post isteği ile gönderir
bool sendSpotStatus(ParkingSensor &sensor) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi bağlı değil, gönderim atlandı.");
    return false;
  }

  String url = String(BACKEND_BASE) + "/spots/" + sensor.id;
  String payload = String("{\"occupied\":") + (sensor.occupied ? "true" : "false") + "}";

  HTTPClient http;
  http.begin(url); // httpclient'ın kendi url ayrıştırıcısını kullanmak yeterlidir
  http.addHeader("Content-Type", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "true"); // ngrok uyarı sayfasını atlamak için
  http.addHeader("Connection", "close");
  http.setTimeout(5000); // 5 saniye zaman aşımı

  Serial.print("HTTP POST -> " + url);
  Serial.print(" | Payload: " + payload);
  
  // post isteğini gönder ve durum kodunu al
  int httpCode = http.POST(payload);
  
  if (httpCode > 0) {
    Serial.printf(" | Yanıt Kodu: %d\n", httpCode);
    if (httpCode != HTTP_CODE_OK) {
        String responseBody = http.getString();
        Serial.println("sunucu yanıtı: " + responseBody);
    }
  } else {
    Serial.printf(" | hata: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return httpCode == HTTP_CODE_OK;
}


// =======================================================================
//                            ana kurulum
// =======================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nakıllı otopark sensör düğümü başlatılıyor...");

  // pin modlarını ayarla
  pinMode(SHIFT_DATA_PIN, OUTPUT);
  pinMode(SHIFT_CLOCK_PIN, OUTPUT);
  pinMode(SHIFT_LATCH_PIN, OUTPUT);
  driveShiftOutputs(0); // başlangıçta tüm çıkışları kapat

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    pinMode(sensors[i].echoPin, INPUT);
  }

  // wi-fi'ye bağlan
  connectWifi();

  // --- bağlantı diagnostik testi ---
  Serial.println("\n--- genel ağ bağlantı testi başlatılıyor ---");
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http_diag;
    // ngrok'tan bağımsız, bilinen bir public api'ye istek gönderiyoruz
    http_diag.begin("http://worldtimeapi.org/api/ip"); 
    int httpCode = http_diag.GET();
    if (httpCode > 0) {
      Serial.printf("[diagnostik] test başarılı! http kodu: %d. wokwi'nin internete erişimi var.\n", httpCode);
    } else {
      Serial.printf("[diagnostik] test başarısız! hata: %s. wokwi internete çıkamıyor olabilir.\n", http_diag.errorToString(httpCode).c_str());
    }
    http_diag.end();
  } else {
    Serial.println("[diagnostik] wifi bağlı olmadığı için test atlandı.");
  }
  Serial.println("--- genel ağ bağlantı testi tamamlandı ---\n");
}


// =======================================================================
//                             ana döngü
// =======================================================================

void loop() {
  connectWifi(); // her döngü başında bağlantıyı kontrol et, kopmuşsa yeniden bağlanmayı dene

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    ParkingSensor &sensor = sensors[i];
    
  float distance = readDistanceCm(sensor); // sensörden mesafe ölçümü al
  bool detected = (distance > 0 && distance <= OCCUPIED_THRESHOLD_CM); // eşik altı dolu kabul
  bool stateChanged = (sensor.occupied != detected); // önceki durumla karşılaştır
    
    sensor.occupied = detected;

    Serial.print("sensor: ");
    Serial.print(sensor.id);
    Serial.print(" | mesafe: ");
    if (distance > 0) {
      Serial.print(distance);
      Serial.print(" cm");
    } else {
      Serial.print("okuma hatasi");
    }
    Serial.print(" -> durum: ");
    Serial.println(detected ? "dolu" : "bos");

    unsigned long now = millis();
    bool resendDue = (now - sensor.lastPublishMillis) >= RESEND_INTERVAL_MS;

    // durum değiştiyse veya belirli bir süre geçtiyse sunucuya veri gönder
    if (stateChanged || resendDue) {
      if (sendSpotStatus(sensor)) {
        sensor.lastPublishMillis = now; // sadece başarılı gönderimde zamanı güncelle
      }
    }

    delay(100); // sensör okumaları arasında kısa bir bekleme
  }

  Serial.println("----------------------------------------------");
  delay(MEASUREMENT_INTERVAL_MS);
}