# Akıllı Otopark (Tek Kat – 10 Alan)

Bu depo, tek katlı (10 park alanı) bir akıllı otopark prototipini içerir:

- ESP32 + 2×74HC595 + 10×HC-SR04 ile sensör ölçümü ve HTTP üzerinden backend’e durum gönderimi (`main.ino`).
- SVG’den alanları otomatik çıkaran Python simülasyon sunucusu (`simulator/mock_server.py`).
- Tek katlı modern web paneli (`web/`), `assets/parking-10.svg` ile görselleştirme.

## Proje Yapısı (Güncel)

- `main.ino` – ESP32 firmware (Wokwi de bunu kullanır)
- `simulator/mock_server.py` – REST API: `/state`, `/spots`, vb.
- `web/index.html`, `web/styles.css`, `web/assets/parking-10.svg`
- `.wokwi/diagram.json` – ESP32 + 2×74HC595 + 10×HC-SR04 devre şeması
- `wokwi.toml` – Wokwi yapılandırması (diagram ve firmware referansı)

Temizlenenler: eski demo dosyaları, çok katlı SVG’ler, donanım kopyaları, geçici log/cache.

## Hızlı Başlangıç

1) Simülasyon sunucusu (8080):

```bash
python3 simulator/mock_server.py
```

2) Web arayüzü (8000):

```bash
python3 -m http.server --directory web 8000
```

3) ESP32/Wokwi firmware:

- `main.ino` içindeki `WIFI_SSID`, `WIFI_PASSWORD` ve `BACKEND_BASE` adresini güncelleyin.
- Wokwi için VS Code’da “Wokwi: Start Simulator” komutunu çalıştırın. `wokwi.toml` `.wokwi/diagram.json` ve `main.ino`’ya işaret eder.

Notlar:
- Codespaces kullanıyorsanız 8080 portunu Public yapın; web paneli otomatik olarak bu adrese yönelir.
- Ngrok kullanmak isterseniz `web/index.html` içindeki NGROK_BASE’i doldurup `BACKEND_BASE` ile aynı adrese ayarlayın.

## API Kısa Özet

- GET `/state` – Genel özet + tek kat snapshot
- GET `/spots` – Tüm spotların durumu
- POST `/spots/{spotId}` – `{ "occupied": true|false }`

## Sorun Giderme

- 400 “Geçersiz istek”: URL’de fazladan path parçası (örn. `$0`) olmadığını doğrulayın.
- Bağlantı hataları: `Connection: close` header’ı firmware’de açık; 8080’in erişilebilir olduğundan emin olun.