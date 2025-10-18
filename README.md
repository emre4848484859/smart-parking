# Akıllı Otopark (Tek Kat – 10 Alan)

Bu depo, tek katlı (10 park alanı) bir akıllı otopark prototipini içerir:

- ESP32 + 2×74HC595 + 10×HC-SR04 ile sensör ölçümü ve HTTP üzerinden backend’e durum gönderimi (`main.ino`).
- Kalıcı FastAPI backend (SQLite veya PostgreSQL) (`backend/app.py`).
- Tek katlı modern web paneli (`web/`), `assets/parking-10.svg` ile görselleştirme.

## Proje Yapısı (Güncel)

- `main.ino` – ESP32 firmware (Wokwi de bunu kullanır); kapsamlı küçük harf türkçe yorumlarla açıklanmıştır
- `backend/app.py` – Kalıcı REST API (FastAPI + SQLAlchemy): `/state`, `/spots`, `/floors/*`; küçük harf türkçe açıklayıcı yorumlarla zenginleştirilmiştir
- `web/index.html` – Arayüz kodu; javascript fonksiyonlarına küçük harf türkçe açıklamalar eklenmiştir
- `web/styles.css` – Stil dosyası
- `web/assets/parking-10.svg` – Tek katlı park planı
- `.wokwi/diagram.json` – ESP32 + 2×74HC595 + 10×HC-SR04 devre şeması
- `wokwi.toml` – Wokwi yapılandırması (diagram ve firmware referansı)
- `start.sh` – Otomatik başlatma/durdurma betiği (start/stop/restart/status/serve/open)
- `requirements.txt` – Python bağımlılıkları

Temizlenenler: eski demo dosyaları, çok katlı SVG'ler, donanım kopyaları, geçici log/cache.

## Hızlı Başlangıç

### Yöntem 1: Otomatik Başlatma (Önerilen)

Projenin kök dizininde `start.sh` betiği bulunur. Tek komutla backend'i başlatıp UI'yi tarayıcıda açar:

```bash
chmod +x start.sh          # ilk seferde izin ver
./start.sh serve           # arka planda başlat + ui aç
```

Diğer kullanım seçenekleri:
```bash
./start.sh start           # önplanda başlat (log terminalden izlenir)
./start.sh stop            # durduracağında çalıştır
./start.sh restart         # yeniden başlat
./start.sh status          # durum kontrolü ve /health testi
./start.sh open            # çalışan sunucunun ui'sini tarayıcıda aç
```

### Yöntem 2: Manuel Başlatma

1) Bağımlılıkları yükle ve backend'i başlat:

```bash
pip3 install -r requirements.txt
uvicorn backend.app:app --host 0.0.0.0 --port 8080 --reload
```

Alternatif: VS Code Görevi — "Terminal > Run Task" menüsünden "Run Smart Parking backend (FastAPI)" görevini çalıştırın.

2) Web arayüzü:

- FastAPI tarafından `/ui` altında servis ediliyor. Tarayıcıdan `http://localhost:8080/` veya `http://localhost:8080/ui/` adresine gidin.

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

Bu sözleşme `backend/app.py` tarafından uygulanır.

## Sorun Giderme

- 400 “Geçersiz istek”: URL’de fazladan path parçası (örn. `$0`) olmadığını doğrulayın.
- Bağlantı hataları: `Connection: close` header’ı firmware’de açık; 8080’in erişilebilir olduğundan emin olun.

## Tasarım Notları: SQLite mi PostgreSQL mi?

- Başlangıç ve tek cihaz/az eşzamanlı yük: SQLite yeterli, kurulum kolay, dosya bazlı depolama.
- Çoklu cihaz, eşzamanlı yazma, raporlama/analitik ve güvenilirlik: PostgreSQL önerilir.
- Kod, SQLAlchemy ile soyutlandığı için `SP_DATABASE_URL` değiştirerek iki veritabanı arasında geçiş yapılabilir.

Öneri:

1. Geliştirme ve küçük saha denemeleri için SQLite ile başlayın.
2. Üretime geçişte PostgreSQL’e taşıyın. Geçiş için şema aynı kalır; ileride Alembic ile migration eklenebilir.

## Otomatik Çalıştırma

- VS Code içinde `.vscode/tasks.json` ile backend ve web sunucu görevleri ekli. Görevleri arka planda çalıştırabilirsiniz.
- Codespaces’te 8080 portunu Public yaparsanız UI otomatik bağlanır.