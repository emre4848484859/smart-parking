// bu dosya esp32 tabanlı akıllı otopark düğümünün ana firmware kodunu içerir
// amaç: hcsr04 ultrasonik sensörlerden mesafe ölçümü alıp her bir park
// alanının doluluk durumunu belirlemek ve backend'e bildirim göndermektir.
// açıklamalar kodun hangi bölümde ne yaptığını adım adım anlatır ve
// sahada bakım/inceleme yapan kişilerin anlamasını kolaylaştırır.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <cstring>

/*
 * genel mimari:
 * - esp32, 2 adet 74hc595 kaydırma kayıtçısı kullanarak hangi sensörün
 *   trig pininin aktif olacağını seçer (daha az gpio ile çoklu trig)
 * - her sensör için echo pini ayrı bir gpio'ya bağlanır ve pulseIn()
 *   ile mesafe ölçümü alınır
 * - ölçülen mesafe eşik değeri ile karşılaştırılarak doluluk (
 *   occupied) belirlenir
 * - doluluk değiştiğinde veya belirli aralıklarla backend'e http post
 *   ile bildirim gönderilir
 */

// =======================================================================
//                           yapılandırma
// =======================================================================

// wi-fi erişim bilgileri: sahada kurulum sırasında uygun ağ bilgileri
// ile güncellenmelidir. üretimde provisioning mekanizmaları tercih edin.
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASSWORD = "";

// backend temel url'si: post istekleri bu temel url üzerine /spots/{id}
// yolu ile yapılır. base url içine fazladan path koymayın (ör. "$0")
// çünkü path çiftleşmeleri 400 hatalarına sebep olabilir.
const char *BACKEND_BASE = "https://obscure-halibut-pj59wxv6rggvf67pq-8080.app.github.dev";

// backend tarafından beklenen cihaz API anahtarı. Backend'de SP_DEVICE_TOKENS
// ile aynı değeri paylaşmalıdır.
const char *DEVICE_API_KEY = "demo-device-key";

// HTTPS çağrıları için paylaşılan istemci (sertifika doğrulaması devre dışı)
WiFiClientSecure secureClient;


// =======================================================================
//                         donanım tanımlamaları
// =======================================================================

// kaydırma kayıtçısı (74hc595) pinleri
// data: seri veri, clock: shift clock, latch: çıkışların güncellenmesi
const uint8_t SHIFT_DATA_PIN = 23;   // 74HC595 ser (seri veri girişi)
const uint8_t SHIFT_CLOCK_PIN = 18;  // 74HC595 srclk (shift clock)
const uint8_t SHIFT_LATCH_PIN = 5;   // 74HC595 rclk (latch / çıktı güncelleme)

// sensör yapısı: her park yeri için bir yapı tanımlanır
struct ParkingSensor {
  const char *id;               // backend ile eşleşecek benzersiz spot id'si
  uint8_t shiftIndex;           // kaydırma kayıtçısındaki bit indeksi (0-15)
  uint8_t echoPin;              // echo pini (pulseIn okumaları için)
  bool occupied;                // anlık doluluk durumu
  unsigned long lastPublishMillis; // son başarılı publish zamanı (ms)
  unsigned long lastReadMillis;    // son ölçüm zamanı (ms)
};

// sensör dizisi: burada her öğe fiziksel bağlantıya göre ayarlanmıştır
// not: id alanı backend'deki spot id'leriyle aynı olmalıdır
ParkingSensor sensors[] = {
  {"F1-A1",  1, 34, false, 0, 0},   // sr2:Q1 (ultrasonic1)
  {"F1-A2",  0, 35, false, 0, 0},   // sr2:Q0 (ultrasonic2)
  {"F1-A3",  8, 32, false, 0, 0},   // sr1:Q0 (ultrasonic3)
  {"F1-A4",  6, 33, false, 0, 0},   // sr2:Q6 (ultrasonic4)
  {"F1-A5",  5, 25, false, 0, 0},   // sr2:Q5 (ultrasonic5)
  {"F1-A6",  4, 26, false, 0, 0},   // sr2:Q4 (ultrasonic6)
  {"F1-A7",  9, 27, false, 0, 0},   // sr1:Q1 (ultrasonic7)
  {"F1-A8",  3, 14, false, 0, 0},   // sr2:Q3 (ultrasonic8)
  {"F1-A9",  7, 12, false, 0, 0},   // sr2:Q7 (ultrasonic9)
  {"F1-A10", 2, 13, false, 0, 0}    // sr2:Q2 (ultrasonic10)
};

const size_t SENSOR_COUNT = sizeof(sensors) / sizeof(sensors[0]);

// =======================================================================
//                            parametreler
// =======================================================================

// mesafe eşik değeri (cm). bu değerin altında ölçüm alınırsa park alanı "dolu"
// kabul edilir. sahadaki montaj yüksekliğine ve araç tiplerine göre ayarlayın.
const float OCCUPIED_THRESHOLD_CM = 35.0f;

// her sensör için minimum ölçüm aralığı (ms). daha hızlı tepki için düşürebilirsiniz.
const uint16_t MEASUREMENT_INTERVAL_MS = 200;

// aynı durum için belirli aralıklarla yeniden gönderim yapılır (ms).
// örneğin network geçici olarak düşse bile belli aralıkta tekrar gönderilir.
const uint32_t RESEND_INTERVAL_MS = 12000;

// ardışık sensör tetiklemeleri arasında kısa bekleme (ms), crosstalk azaltır
const uint16_t SENSOR_SETTLE_DELAY_MS = 20;

// hiçbir sensör ölçüm zamanı gelmediyse döngüde yapılacak kısa bekleme (ms)
const uint8_t LOOP_IDLE_DELAY_MS = 1;


// =======================================================================
//                        yardımcı fonksiyonlar
// =======================================================================

// connectwifi(): wi-fi bağlantısını yönetir. bağlı değilse belirtilen
// ssid ile bağlantı kurulmaya çalışılır. basit retry mekanizması mevcuttur.
void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return; // zaten bağlıysa işlem yapma
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


// driveShiftOutputs(pattern): kaydırma kayıtçılarına 16-bit pattern yazar.
// böylece hangi trig hattının aktif olacağı seçilir. örnek: (1 << index)
// ile sadece ilgili sensörün trig'i aktif edilir.
void driveShiftOutputs(uint16_t pattern) {
  digitalWrite(SHIFT_LATCH_PIN, LOW);
  shiftOut(SHIFT_DATA_PIN, SHIFT_CLOCK_PIN, MSBFIRST, (pattern >> 8) & 0xFF); // ikinci 595 (sr1)
  shiftOut(SHIFT_DATA_PIN, SHIFT_CLOCK_PIN, MSBFIRST, pattern & 0xFF);        // birinci 595 (sr2)
  digitalWrite(SHIFT_LATCH_PIN, HIGH);
}


// readDistanceCm(sensor): seçili sensör için trig uygular, echo süresini
// pulseIn ile ölçer ve bunu santimetreye çevirir. timeout veya hata
// durumunda negatif değer döner.
float readDistanceCm(ParkingSensor &sensor) {
  driveShiftOutputs(0); // önce tüm trig pinlerini kapat
  delayMicroseconds(2);

  // sadece ilgili sensörün trig pinini aç
  driveShiftOutputs(1u << sensor.shiftIndex);
  delayMicroseconds(10);
  driveShiftOutputs(0); // trig pinini tekrar kapat

  // echo pininden gelen sinyalin süresini ölç (timeout 25ms ~ 4 metre)
  long duration = pulseIn(sensor.echoPin, HIGH, 25000); 
  if (duration <= 0) {
    return -1.0f; // ölçüm başarısız veya zaman aşımı
  }

  // süreyi santimetreye çevir (ses hızı ~343 m/s)
  return (duration * 0.0343f) / 2.0f;
}


// sendSpotStatus(sensor): sensör durumu json olarak backend'e gönderir.
// bağlantı yoksa gönderim atlanır. http yanıt kodu 200 ise başarılı kabul edilir.
bool sendSpotStatus(ParkingSensor &sensor) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi bağlı değil, gönderim atlandı.");
    return false;
  }

  String url = String(BACKEND_BASE) + "/spots/" + sensor.id;
  String payload = String("{\"occupied\":") + (sensor.occupied ? "true" : "false") + "}";

  HTTPClient http;
  bool isHttps = url.startsWith("https://");
  bool beginOk = false;
  if (isHttps) {
    secureClient.setInsecure(); // her çağrıda güvenli olmayan moda al
    beginOk = http.begin(secureClient, url);
  } else {
    beginOk = http.begin(url);
  }

  if (!beginOk) {
    Serial.println("HTTP istemcisi başlatılamadı.");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  if (DEVICE_API_KEY && strlen(DEVICE_API_KEY) > 0) {
    http.addHeader("X-Device-Key", DEVICE_API_KEY);
  }
  // ngrok geliştirici araçlarında tarayıcı uyarısını atlamak için header
  http.addHeader("ngrok-skip-browser-warning", "true");
  // bağlantıyı kısa tutmak için connection: close
  http.addHeader("Connection", "close");
  http.setTimeout(5000); // 5 saniye zaman aşımı

  Serial.print("HTTP POST -> " + url);
  Serial.print(" | Payload: " + payload);
  
  int httpCode = http.POST(payload);
  
  if (httpCode > 0) {
    Serial.printf(" | yanıt kodu: %d\n", httpCode);
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

// setup(): cihaz başlarken bir kez çalışır. seri port başlatılır, pinler
// konfigüre edilir, wi-fi'ye bağlanılır ve basit bir diagnostik istek atılır.
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nakıllı otopark sensör düğümü başlatılıyor...");

  // pin modlarını ayarla ve kaydırma kayıtçılarını temizle
  pinMode(SHIFT_DATA_PIN, OUTPUT);
  pinMode(SHIFT_CLOCK_PIN, OUTPUT);
  pinMode(SHIFT_LATCH_PIN, OUTPUT);
  driveShiftOutputs(0); // başlangıçta tüm çıkışları kapat

  // echo pinlerini giriş olarak ayarla
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    pinMode(sensors[i].echoPin, INPUT);
  }

  // wi-fi'ye bağlan (ve başarılıysa kısa bir diagnostik test yap)
  connectWifi();
  secureClient.setInsecure(); // geliştirme ortamında CA doğrulamasını devre dışı bırak

  // --- bağlantı diagnostik testi ---
  Serial.println("\n--- genel ağ bağlantı testi başlatılıyor ---");
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http_diag;
    // bilinen ve hafif bir public api'ye istek göndererek internet erişimini test et
    http_diag.begin("http://worldtimeapi.org/api/ip"); 
    int httpCode = http_diag.GET();
    if (httpCode > 0) {
      Serial.printf("[diagnostik] test başarılı! http kodu: %d. internete erişim var.\n", httpCode);
    } else {
      Serial.printf("[diagnostik] test başarısız! hata: %s. internete çıkamıyor olabilir.\n", http_diag.errorToString(httpCode).c_str());
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

// loop(): sensörleri döngü ile okur, doluluk kararını verir, değişiklik
// veya zaman aşımı durumunda backend'e bildirir. ayrıca kısa beklemeler
// ile sensörler arası çakışma engellenir.
void loop() {
  // wi-fi bağlantısını her döngüde kontrol et, kopma varsa yeniden bağlanmayı dene
  connectWifi();

  bool measurementDone = false;

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    ParkingSensor &sensor = sensors[i];

    unsigned long now = millis();
    bool measurementTooEarly = (sensor.lastReadMillis != 0) && ((now - sensor.lastReadMillis) < MEASUREMENT_INTERVAL_MS);
    if (measurementTooEarly) {
      continue; // bu sensörün ölçüm süresi henüz gelmediyse atla
    }

    float distance = readDistanceCm(sensor); // sensörden mesafe ölçümü al
    unsigned long afterRead = millis();
    sensor.lastReadMillis = afterRead; // bir sonraki ölçüm için zaman damgası

    bool detected = (distance > 0 && distance <= OCCUPIED_THRESHOLD_CM); // eşik altı dolu kabul
    bool stateChanged = (sensor.occupied != detected); // önceki durumla karşılaştır
    sensor.occupied = detected;

    // seri çıktı ile durumları gözlemlenebilir yap
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

    bool resendDue = (sensor.lastPublishMillis == 0) || ((afterRead - sensor.lastPublishMillis) >= RESEND_INTERVAL_MS);

    // durum değiştiyse veya belirli bir süre geçtiyse sunucuya veri gönder
    if (stateChanged || resendDue) {
      if (sendSpotStatus(sensor)) {
        // yalnızca başarılı gönderimde zaman damgasını güncelle
        sensor.lastPublishMillis = afterRead;
      }
    }

    delay(SENSOR_SETTLE_DELAY_MS); // ultrasonik sensörler arasında kısa bekleme
    measurementDone = true;
  }

  if (!measurementDone) {
    delay(LOOP_IDLE_DELAY_MS); // yoğun iş yoksa cpu'yu rahatlat
  } else {
    Serial.println("----------------------------------------------");
  }
}