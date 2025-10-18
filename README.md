# Akıllı Otopark Prototipi (Tek Kat - 10 Alan)

bu depo, 10 park alanına sahip tek katlı bir otoparkın doluluk durumunu simüle eden ve bunu modern bir web arayüzünde canlı olarak gösteren tam kapsamlı bir akıllı otopark prototipidir.

proje üç ana bileşenden oluşur:

1. **donanım (esp32 firmware):** `main.ino` dosyası, 10 adet ultrasonik sensörden (hc-sr04) veri okur ve doluluk durumunu http post isteği ile backend'e bildirir.
2. **backend (fastapi sunucusu):** `backend/app.py` dosyası, esp32'den gelen verileri alır, bir veritabanına (sqlite veya postgresql) kaydeder ve web arayüzünün ihtiyaç duyduğu verileri `/state` endpoint'i üzerinden json formatında sunar.
3. **frontend (web arayüzü):** `web/` klasöründeki dosyalar, backend'den aldığı verileri kullanarak otopark planı üzerinde (bir svg dosyası aracılığıyla) hangi park yerinin dolu veya boş olduğunu canlı olarak gösterir ve en yakın boş yeri önerir.



---

## proje mimarisi ve veri akışı

veri akışı şu adımlarla ilerler:

1. **wokwi simülasyonu:** `diagram.json` dosyasına göre çalışan sensörlerin mesafesi değiştirilir.
2. **esp32 (`main.ino`):** `loop()` fonksiyonu periyodik olarak tüm sensörleri okur.
3. **doluluk tespiti:** okunan mesafe, `OCCUPIED_THRESHOLD_CM` (örneğin 30 cm) ile karşılaştırılır. mesafe bu eşiğin altındaysa park yeri "dolu" kabul edilir.
4. **http post:** durum değiştiyse veya `RESEND_INTERVAL_MS` süresi dolduysa, esp32 `backend/app.py` sunucusunun `/spots/{spotId}` endpoint'ine `{"occupied": true}` gibi bir json verisi gönderir.
5. **fastapi backend (`backend/app.py`):** gelen veriyi alır ve veritabanındaki (ör. `smart_parking.db`) ilgili park yerinin durumunu günceller.
6. **web arayüzü (`web/index.html`):** her 5 saniyede bir backend'in `/state` endpoint'ine get isteği atarak tüm otoparkın son durumunu çeker.
7. **görselleştirme:** arayüz, svg haritasındaki park yerlerinin renklerini (boş için yeşil, dolu için kırmızı) ve genel istatistikleri (dolu/boş sayısı) günceller.

---

## nasıl çalışır? (teknik detaylar)

### 1. donanım mimarisi: sensörler nasıl okunuyor?

proje 10 adet hc-sr04 ultrasonik sensör kullanır. bu kadar sensörü tek bir esp32'ye bağlamak için yeterli gpio pini olmadığı için **kaydırma kayıtçısı (shift register)** tekniği uygulanır:

- **trig pinleri:** 10 sensörün `trig` pinleri, esp32'nin yalnızca üç pini (data, clock, latch) tarafından kontrol edilen iki adet 74hc595 kaydırma kayıtçısına bağlanır.
- **sensör seçimi:** `main.ino` içindeki `driveShiftOutputs(1u << sensor.shiftIndex)` fonksiyonu, kaydırma kayıtçılarına 16 bitlik veri göndererek sadece ölçüm yapılacak sensörün `trig` pinini aktif hale getirir.
- **echo pinleri:** her sensörün `echo` pini esp32 üzerinde ayrı bir gpio'ya bağlıdır.
- **ölçüm:** aktif sensörden gelen `echo` sinyali `pulseIn()` ile dinlenir ve süre santimetreye çevrilir. ölçümler arasında `SENSOR_SETTLE_DELAY_MS` ile belirlenen kısa gecikmeler kullanılır.

### 2. backend: en yakın park yeri formülü

web arayüzünde görülen "önerilen park alanı" bilgisi backend tarafından hesaplanır:

- **referans noktası:** sunucu başlarken `web/assets/parking-10.svg` dosyasını okur ve otopark girişini referans alır.
- **hesaplama:** `compute_floor_snapshot` fonksiyonu, o an boş olan park yerlerini listeler.
- **formül:** her boş park yerinin svg koordinatları ile giriş koordinatları arasındaki **öklid mesafesi** hesaplanır:

	`d = sqrt((x_spot - x_giris)^2 + (y_spot - y_giris)^2)`

- **sonuç:** mesafesi en düşük olan park yeri öneri olarak `/state` yanıtına eklenir.

---

## kullanım ve kurulum

projeyi çalıştırmak için backend'i başlatıp web arayüzünü açmanız, ardından wokwi simülasyonunu çalıştırmanız gerekir.

### 1. backend sunucusunun başlatılması

backend python ve fastapi ile çalışır.

**yöntem 1: otomatik başlatma (önerilen)**

```bash
# 1. betiğe çalıştırma izni verin (sadece ilk seferde)
chmod +x start.sh

# 2. sunucuyu arka planda başlatın ve tarayıcıda arayüzü açın
./start.sh serve
```

diğer `start.sh` komutları:

- `./start.sh start`: sunucuyu ön planda (logları terminalde görerek) başlatır.
- `./start.sh stop`: sunucuyu durdurur.
- `./start.sh status`: sunucunun çalışıp çalışmadığını kontrol eder.

**yöntem 2: manuel başlatma**

```bash
# 1. gerekli python kütüphanelerini yükleyin
pip3 install -r requirements.txt

# 2. uvicorn sunucusunu başlatın
uvicorn backend.app:app --host 0.0.0.0 --port 8080 --reload
```

### 2. web arayüzünün görüntülenmesi

backend çalışır durumdayken, arayüz `http://localhost:8080/ui/` adresinden yayınlanır.

> **codespaces / vs code remote notu:** bulut ortamında çalışıyorsanız 8080 portunu "public" yapıp verilen genel url'yi kopyalayın. bu url'yi wokwi ile paylaşmanız gerekir.

### 3. simülasyonun başlatılması (wokwi)

1. `wokwi.com` sitesinde yeni bir esp32 projesi açın.
2. projedeki `diagram.json` içeriğini wokwi'deki `diagram.json` dosyasına aktarın.
3. `main.ino` içeriğini wokwi'deki `sketch.ino` dosyasına aktarın.
4. `BACKEND_BASE` değişkenini kendi genel backend url'inizle güncelleyin.
5. wokwi simülasyonunu başlatın.

---

## beklenen sonuçlar

### wokwi (esp32) çıktısı

- seri monitörde wifi bağlantısının kurulduğunu ve diagnostik testlerin geçtiğini görürsünüz.
- sensör değerlerini 35 cm altına çektiğinizde `{"occupied": true}` içeren post istekleri ve yanıt kodu `200` loglanır.
- eşik üstüne çıktığınızda `{"occupied": false}` gönderilir.

### web arayüzü çıktısı

- post isteği sonrası 5 saniye içinde ilgili park yerinin rengi güncellenir.
- dolu/boş sayaçları ve önerilen park alanı kutucuğu taze veriyi gösterir.
- sağdaki diagnostik panelde son durum loglanır.

---

## api referansı

backend aşağıdaki temel endpoint'leri sağlar:

### `POST /spots/{spot_id}`

- **amaç:** park yerinin durumunu günceller.
- **body:**

	```json
	{ "occupied": true }
	```
- **yanıt (200 ok):**

	```json
	{ "result": "OK", "spotId": "F1-A1", "occupied": true }
	```

### `GET /state`

- **amaç:** otoparkın anlık durumunu, istatistiklerini ve önerileri döndürür.
- **örnek yanıt (özet):**

	```json
	{
		"timestamp": "2025-10-18T12:30:00Z",
		"overall": { "total": 10, "occupied": 1, "free": 9, "occupancyRate": 10.0 },
		"floors": [
			{
				"id": "F1",
				"nearestAvailable": { "id": "F1-A1", "label": "A1", "distance": 0.0 }
			}
		]
	}
	```

### `GET /health`

- **amaç:** sunucunun ve veritabanı bağlantısının durumunu kontrol eder.
- **yanıt (200 ok):**

	```json
	{ "status": "ok", "db": "sqlite" }
	```

---

## veritabanı seçimi (sqlite vs postgresql)

proje `sqlalchemy` sayesinde hem sqlite hem de postgresql ile çalışabilir:

- **varsayılan (sqlite):** `backend/app.py` başlatıldığında `smart_parking.db` dosyası oluşturulur. küçük ölçekli denemeler için idealdir ve `.gitignore` sayesinde depoya eklenmez.
- **üretim (postgresql):** `SP_DATABASE_URL` ortam değişkenini postgres bağlantınızla değiştirmeniz yeterlidir; ek kod değişikliği gerekmez.

---

## katkı ve geliştirme notları

- kodun tamamında türkçe yorumlar bulunur okunabilirlik açısından faydalıdır.
- saha testlerinde sensör eşikleri (`OCCUPIED_THRESHOLD_CM`, `SENSOR_SETTLE_DELAY_MS`) gerçek donanıma göre ayarlanmalıdır.
- sorular, hata kayıtları veya yeni özellik önerileri için issue açabilirsiniz.

