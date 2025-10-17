# Akıllı Otopark Simülasyonu

Bu depo, üç katlı bir akıllı otopark sisteminin uçtan uca prototipini içerir. ESP32 + HC-SR04 sensör düğümleri için gömülü kod taslağı, SVG kat planlarından park alanlarını otomatik çıkaran simülasyon sunucusu ve dinamik, çok katlı web paneli birlikte gelir.

## Proje Yapısı

- `hardware/esp32_parking.ino` – ESP32 için Arduino üslubunda sensör okuma ve backend'e veri gönderme kodu
- `simulator/mock_server.py` – SVG planlarını okuyup üç katın tüm park alanlarını çıkaran, REST uç noktaları sağlayan Python tabanlı servis
- `web/index.html` & `web/styles.css` – Çok katlı kat planları, önerilen park yeri ve diagnostik log konsolu ile modern web arayüzü
- `web/assets/kat-plani-*.svg` – Önceki projedeki kat planlarının tekrar kullanılan SVG halleri

## Hızlı Başlangıç (Simülasyon)

1. **Simülasyon sunucusunu başlatın**
	```bash
	python3 simulator/mock_server.py
	```
	- Sunucu varsayılan olarak `http://localhost:8080/state` adresinden çok katlı JSON döndürür. Codespaces gibi ortamlarda 8080 portunu **Public** moda çekmeyi unutmayın.

2. **Web arayüzünü açın**
	```bash
	python3 -m http.server --directory web 8000
	```
	- Tarayıcıda `http://localhost:8000` (veya geliştirici ortamınızın ilettiği URL) adresine giderek paneli izleyin.
	- Panel kat sekmeleriyle SVG planlarını gösterir, boş/dolu durumlarını renklendirir, önerilen park yerini vurgular ve log konsoluyla her istek/yanıtı izlemenizi sağlar.

3. **Durumu manuel değiştirin (isteğe bağlı)**
	```bash
	curl -X POST http://localhost:8080/spots/F1-A1 \
		  -H "Content-Type: application/json" \
		  -d '{"occupied": true}'
	```
	- `occupied` alanını `true` / `false` olarak göndererek ilgili park alanını güncelleyebilirsiniz. Kat bazında güncelleme yapmak isterseniz `POST /floors/F2/spots/F2-B5` gibi yolları da kullanabilirsiniz.

## ESP32 + HCSR04 Donanım Taslağı

- `hardware/esp32_parking.ino` dosyasında:
  - Wi-Fi ayarlarını `WIFI_SSID` ve `WIFI_PASSWORD` alanlarında güncelleyin.
  - REST uç noktası `BACKEND_URL` için simülasyon sunucusunu veya gerçek backend adresinizi kullanın.
  - HCSR04 sensörleri için `trig`/`echo` pinleri `bays` dizisinde tanımlıdır. Giriş-çıkış pinleri ihtiyaçlarınıza göre değiştirilebilir.
  - `OCCUPIED_THRESHOLD_CM` parametresi araç algılama mesafesini (cm) belirler.

Kod, her sensörden periyodik olarak mesafe ölçer, park alanının dolu/boş olduğuna karar verir ve değişiklikleri JSON formatında HTTP POST isteği ile backend'e gönderir.

## Wokwi veya Başka Bir Simülasyon Ortamı

1. [wokwi.com](https://wokwi.com) üzerinde yeni bir `ESP32` projesi açın.
2. Sağlanan `hardware/esp32_parking.ino` kodunu projeye yapıştırın.
3. Her park yeri için `Ultrasonic Distance Sensor (HC-SR04)` bileşenlerini ekleyin ve pinleri kodda belirtilen GPIO'lara bağlayın.
4. Simülasyon sırasında araç giriş-çıkışlarını test etmek için sensörlerin `distance` parametresini değiştirin.

Benzer şekilde, Proteus veya Tinkercad Circuits gibi alternatif platformlarla da sensör okumalarını simüle edebilirsiniz.

## Çok Katlı Web Paneli

- Kat sekmeleri ile `web/assets/kat-plani-*.svg` dosyaları doğrudan yüklenir, boş/dolu durumlar renklerle güncellenir ve kat geçişinde loglara otomatik kayıt düşülür.
- "Önerilen Park Alanı" bileşeni, API'nin global önerisini gösterir ve tek tıklamayla ilgili kata odaklanmanızı sağlar.
- "Kat Özeti" kartları her katın boş/dolu sayısını, doluluk yüzdesini ve aktif katı işaretler. Kartlara tıklayarak tablo oluşturmadan kat değişimi yapabilirsiniz.
- Diagnostik log konsolu her istek/yanıtı, hata ayrıntılarını ve manuel yenilemeleri kronolojik olarak kaydeder; canlı takibi kapatma/açma düğmesi de log üzerinden bildirilir.

## API Özeti

| Yöntem | Yol                         | Açıklama                                                          |
|--------|-----------------------------|--------------------------------------------------------------------|
| GET    | `/state`                    | Tüm katların özetini, spot listesini ve küresel öneriyi döner      |
| GET    | `/spots`                    | Kat bilgisiyle birlikte tüm park alanlarının doluluk durumunu verir |
| GET    | `/floors/{floorId}`         | Belirli katın anlık özetini ve spot listesini döner                 |
| GET    | `/floors/{floorId}/spots`   | Belirli katın spot + doluluk durumlarını listeler                  |
| POST   | `/spots/{spotId}`           | `{ "occupied": true/false }` ile park alanını günceller           |
| POST   | `/floors/{floorId}/spots/{spotId}` | Aynı güncellemeyi kat bazlı yol üzerinden yapma alternatifi |

Gelecekte gerçek sensör verileri geldiğinde, ESP32 kodu bu uç noktaları hedef alarak canlı veriyi aynı panelde gösterecektir.

## Yol Haritası Önerileri

- Her katta gerçek zamanlı yoğunluk tahmini için hareketli ortalama / ML tabanlı tahmin katmanı eklemek
- Simülasyon sunucusunu MQTT köprüsü ile zenginleştirip gerçek ESP32 düğümleriyle çift yönlü haberleşme sağlamak
- Web arayüzüne kullanıcı kimliği ile giriş ve rezerve etme / yönlendirme akışları eklemek