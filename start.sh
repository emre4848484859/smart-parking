#!/usr/bin/env bash
set -euo pipefail

# akıllı otopark - başlat/durdur scripti
# kullanım:
#  - ./start.sh start     # başlat (varsayılan, foreground)
#  - ./start.sh stop      # durdur (8080 dinleyen süreçleri)
#  - ./start.sh restart   # durdur + başlat
#  - ./start.sh status    # durum/health kontrolü
#  - ./start.sh serve     # arka planda başlat + hazır olunca ui'yi aç
#  - ./start.sh open      # çalışan sunucu için /ui'yi tarayıcıda aç

# ----------------------------- ortam değişkenleri -----------------------------
# bu değişkenler komut satırından set edilebilir veya script içinde varsayılan
# değerleri alır. örnek: PORT=9000 ./start.sh start
PORT=${PORT:-8080}
HOST=${HOST:-0.0.0.0}
APP=${APP:-backend.app:app}
RELOAD=${RELOAD:-1}          # 1: geliştirme; 0: üretim
FORCE_STOP=${FORCE_STOP:-1}  # 1: port meşgulse otomatik durdurup yeniden başlat
OPEN_URL=${OPEN_URL:-"http://127.0.0.1:$PORT/ui/"}

# ----------------------------- yardımcı fonksiyonlar -----------------------------

# need_deps(): python bağımlılıklarının (fastapi, uvicorn) kurulu olup olmadığını
# kontrol eder. eksikse requirements.txt ile kurulum talimatı verir ve çıkar.
need_deps() {
  if ! python3 -c "import fastapi, uvicorn" >/dev/null 2>&1; then
    echo "Gerekli bağımlılıklar eksik. Önce: pip3 install -r requirements.txt" >&2
    exit 1
  fi
}

# find_pids_by_port(port): belirtilen portu dinleyen süreçlerin pid listesini
# döndürür. lsof veya ss komutu kullanır. bulunmazsa boş string döner.
find_pids_by_port() {
  local port="$1"
  if command -v lsof >/dev/null 2>&1; then
    lsof -t -iTCP:"$port" -sTCP:LISTEN 2>/dev/null || true
  else
    ss -ltnp 2>/dev/null | awk -v p=":$port" '$4 ~ p {print $NF}' | sed -E 's/.*pid=([0-9]+).*/\1/' || true
  fi
}

# stop_server(): belirtilen PORT değişkenindeki portu dinleyen tüm süreçleri
# bulur ve sonlandırır. önce SIGTERM gönderir, bekler; hala çalışıyorsa
# SIGKILL ile zorla sonlandırır.
stop_server() {
  local pids; pids="$(find_pids_by_port "$PORT")"
  if [[ -z "$pids" ]]; then
    echo "[stop] Port $PORT dinleyen süreç yok."
    return 0
  fi
  echo "[stop] Port $PORT dinleyen süreçler sonlandırılıyor: $pids"
  kill $pids 2>/dev/null || true
  for i in $(seq 1 10); do
    sleep 0.3
    pids="$(find_pids_by_port "$PORT")"
    [[ -z "$pids" ]] && break
  done
  if [[ -n "$pids" ]]; then
    echo "[stop] Zorla sonlandırılıyor: $pids"
    kill -9 $pids 2>/dev/null || true
  fi
}

# status_server(): mevcut durumu raporlar. port dinleniyor mu? /health endpointi
# cevap veriyor mu? gibi bilgileri curl ile kontrol eder ve ekrana yazdırır.
status_server() {
  local pids; pids="$(find_pids_by_port "$PORT")"
  if [[ -z "$pids" ]]; then
    echo "[status] Çalışmıyor (port $PORT boş)."
  else
    echo "[status] Port $PORT dinleniyor. PID: $pids"
  fi
  if command -v curl >/dev/null 2>&1; then
    if curl -sS "http://127.0.0.1:$PORT/health" >/dev/null; then
      echo "[status] /health OK"
    else
      echo "[status] /health erişilemedi"
    fi
  fi
}

# start_server(): uvicorn'i önplanda başlatır. önce bağımlılık kontrolü yapar,
# port meşgulse (ve FORCE_STOP=1 ise) durdurur, ardından exec ile çalıştırır.
# bu fonksiyon script'i sonlandırır (exec nedeniyle).
start_server() {
  need_deps
  local pids; pids="$(find_pids_by_port "$PORT")"
  if [[ -n "$pids" ]]; then
    echo "[start] Uyarı: Port $PORT kullanımda (PID: $pids)."
    if [[ "$FORCE_STOP" == "1" ]]; then
      stop_server
    else
      echo "[start] FORCE_STOP=1 ile mevcut süreci otomatik sonlandırabilirsiniz."
      exit 1
    fi
  fi

  local args=("$APP" --host "$HOST" --port "$PORT")
  if [[ "$RELOAD" == "1" ]]; then
    args+=(--reload)
  fi
  echo "[start] Uvicorn başlatılıyor: uvicorn ${args[*]}"
  exec uvicorn "${args[@]}"
}

# start_bg(): uvicorn'i arka planda başlatır (nohup ile). log çıktısı
# /tmp/smart-parking.log'a yönlendirilir. kullanım: ./start.sh serve
start_bg() {
  need_deps
  local pids; pids="$(find_pids_by_port "$PORT")"
  if [[ -n "$pids" ]]; then
    echo "[serve] Uyarı: Port $PORT kullanımda (PID: $pids)."
    if [[ "$FORCE_STOP" == "1" ]]; then
      stop_server
    else
      echo "[serve] FORCE_STOP=1 ile mevcut süreci otomatik sonlandırabilirsiniz."
      exit 1
    fi
  fi
  local args=("$APP" --host "$HOST" --port "$PORT")
  if [[ "$RELOAD" == "1" ]]; then
    args+=(--reload)
  fi
  echo "[serve] Uvicorn arka planda başlatılıyor..."
  nohup uvicorn "${args[@]}" >/tmp/smart-parking.log 2>&1 &
  echo "[serve] Log: /tmp/smart-parking.log"
}

# wait_ready(): sunucunun başlatılmasını bekler. OPEN_URL adresine istek atarak
# hazır olup olmadığını kontrol eder. timeout 10 saniye (40×0.25s). başarılı
# başlangıçta 0, başarısız başlangıçta 1 döner.
wait_ready() {
  for i in $(seq 1 40); do
    if curl -L -sS "$OPEN_URL" >/dev/null; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

# open_ui(): BROWSER veya xdg-open ile ui adresini tarayıcıda açar.
# kullanıcı ortamı destekliyorsa tarayıcı penceresi otomatik açılır.
open_ui() {
  local url="$OPEN_URL"
  echo "[open] UI açılıyor: $url"
  if [[ -n "${BROWSER:-}" ]]; then
    "$BROWSER" "$url" >/dev/null 2>&1 || true
  elif command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$url" >/dev/null 2>&1 || true
  else
    echo "Tarayıcı açılamadı. URL: $url"
  fi
}

cmd="${1:-start}"
case "$cmd" in
  start)   start_server ;;
  stop)    stop_server  ;;
  restart) stop_server; start_server ;;
  status)  status_server ;;
  serve)   start_bg; if wait_ready; then open_ui; else echo "[serve] Sunucu hazır olmadı."; fi ;;
  open)    if wait_ready; then open_ui; else echo "[open] Sunucu hazır değil."; fi ;;
  *) echo "Kullanım: $0 {start|stop|restart|status|serve|open}"; exit 1 ;;
esac
