#!/usr/bin/env python3
"""tek katlı akıllı otopark simülasyon servisi.

esp32 tabanlı sensörlerden gelecek veriyi taklit ederek rest api üretir.
svg kat planlarından park alanlarını otomatik çıkarır, kat başına en yakın boş yeri hesaplar ve ön uç arayüzünün ihtiyaç duyduğu toplu özet verilerini sunar.
not: yorumlar talep gereği küçük harfle yazılmıştır.
"""

from __future__ import annotations

import json
import os
import random
import re
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Dict, List, Optional
from urllib.parse import urlparse
from xml.etree import ElementTree as ET

HOST = "0.0.0.0"  # tüm arayüzlerden dinle
PORT = 8080        # varsayılan http portu (sim için)

BASE_DIR = Path(__file__).resolve().parent
ASSETS_DIR = BASE_DIR.parent / "web" / "assets"  # svg kaynakları

SVG_NS = "{http://www.w3.org/2000/svg}"

FLOOR_SPECS = [
  {
    "id": "F1",
    "label": "Zemin Kat",
    "svg": ASSETS_DIR / "parking-10.svg",
    "entrance": {"id": "F1-entrance", "coords": {"x": 20.0, "y": 55.0}},
  },
]

SPOT_ID_NUMBER_RE = re.compile(r"(\d+)$")

STATE_LOCK = threading.Lock()
FLOORS: Dict[str, Dict] = {}
FLOOR_SPOT_IDS: Dict[str, List[str]] = {}
SPOT_METADATA: Dict[str, Dict] = {}
SPOT_STATUS: Dict[str, bool] = {}
LAST_UPDATE_TS = time.time()


def _env_flag(name: str, default: bool) -> bool:
  raw = os.environ.get(name)
  if raw is None:
    return default
  raw = raw.strip().lower()
  if raw in {"1", "true", "yes", "on"}:
    return True
  if raw in {"0", "false", "no", "off"}:
    return False
  return default


ENABLE_RANDOM_MUTATIONS = _env_flag("SP_RANDOM_MUTATIONS", True)  # rastgele durum değişimleri
RANDOM_INITIAL_OCCUPANCY = _env_flag("SP_RANDOM_INITIAL", ENABLE_RANDOM_MUTATIONS)  # başlangıçta rastgele doluluk


def _svg_children(group, tag_name: str):
  """svg grup elemanı içinde basit alt öğe bulucu (etikete göre)."""
  for child in group:
    if child.tag.endswith(tag_name):
      return child
  return None


def load_floor_layouts() -> None:
  """svg'den spotları okuyup dahili yapıları hazırlar."""
  FLOORS.clear()
  FLOOR_SPOT_IDS.clear()
  SPOT_METADATA.clear()

  for spec in FLOOR_SPECS:
    svg_path: Path = spec["svg"]
    if not svg_path.exists():
      raise FileNotFoundError(f"Kat planı bulunamadı: {svg_path}")

    tree = ET.parse(svg_path)
    root = tree.getroot()

    spots: List[Dict] = []
    for group in root.findall(f".//{SVG_NS}g"):
      class_attr = group.attrib.get("class", "")
      if "parking-spot-svg" not in class_attr:
        continue

      spot_id = group.attrib.get("id")
      if not spot_id:
        continue

      rect = _svg_children(group, "rect")
      if rect is None:
        continue

      try:
        x = float(rect.attrib.get("x", "0"))
        y = float(rect.attrib.get("y", "0"))
      except ValueError:
        x, y = 0.0, 0.0

      label = None
      text_node = _svg_children(group, "text")
      if text_node is not None:
        label = "".join(text_node.itertext()).strip()
      if not label:
        label = spot_id.split("-")[-1]

      spot = {
        "id": spot_id,
        "label": label,
        "coords": {"x": x, "y": y},
      }
      spots.append(spot)

      SPOT_METADATA[spot_id] = {
        "id": spot_id,
        "label": label,
        "coords": {"x": x, "y": y},
        "floorId": spec["id"],
      }

    def _spot_sort_key(spot_id: str):
      # f1-a10 gibi id'lerde doğal sıralama için son sayısal parçayı kullan
      match = SPOT_ID_NUMBER_RE.search(spot_id)
      if match:
        prefix = spot_id[: match.start()]
        number = int(match.group(1))
        return (prefix, number)
      return (spot_id, 0)

    spots.sort(key=lambda s: _spot_sort_key(s["id"]))
    entrance_spec = dict(spec["entrance"])
    if spots:
      anchor = spots[0]
      entrance_spec["coords"] = {
        "x": anchor["coords"]["x"],
        "y": anchor["coords"]["y"],
      }
      entrance_spec["anchorSpotId"] = anchor["id"]

    FLOORS[spec["id"]] = {
      "id": spec["id"],
      "label": spec["label"],
      "entrance": entrance_spec,
      "spots": spots,
    }
    FLOOR_SPOT_IDS[spec["id"]] = [spot["id"] for spot in spots]
def init_state(seed: Optional[int] = None, random_initial: bool = True) -> None:
  """başlangıç durumunu oluşturur; isteğe bağlı rastgele doluluk uygular."""
  global LAST_UPDATE_TS
  if seed is not None:
    random.seed(seed)
  load_floor_layouts()
  with STATE_LOCK:
    SPOT_STATUS.clear()
    for spot_id in SPOT_METADATA:
      if random_initial:
        SPOT_STATUS[spot_id] = random.random() < 0.45
      else:
        SPOT_STATUS[spot_id] = False
    LAST_UPDATE_TS = time.time()


def set_spot_state(spot_id: str, occupied: bool) -> bool:
  """tek bir park alanının durumunu günceller."""
  global LAST_UPDATE_TS
  with STATE_LOCK:
    if spot_id not in SPOT_STATUS:
      return False
    SPOT_STATUS[spot_id] = occupied
    LAST_UPDATE_TS = time.time()
    return True


def compute_floor_stats(floor_id: str) -> Dict[str, float]:
  """kat bazında toplam/boş/dolu ve doluluk yüzdesini hesaplar."""
  spot_ids = FLOOR_SPOT_IDS.get(floor_id, [])
  total = len(spot_ids)
  occupied = sum(1 for sid in spot_ids if SPOT_STATUS.get(sid, False))
  free = total - occupied
  occupancy_rate = (occupied / total * 100.0) if total else 0.0
  return {
    "total": total,
    "occupied": occupied,
    "free": free,
    "occupancyRate": occupancy_rate,
  }


def compute_floor_nearest(floor_id: str) -> Optional[Dict]:
  """giriş noktasına en yakın boş park alanını döndürür."""
  floor = FLOORS[floor_id]
  entrance = floor["entrance"]
  ex = entrance["coords"]["x"]
  ey = entrance["coords"]["y"]

  best_spot = None
  best_distance = None
  for spot_id in FLOOR_SPOT_IDS.get(floor_id, []):
    if SPOT_STATUS.get(spot_id, False):
      continue
    coords = SPOT_METADATA[spot_id]["coords"]
    dx = coords["x"] - ex
    dy = coords["y"] - ey
    distance = (dx ** 2 + dy ** 2) ** 0.5
    if best_distance is None or distance < best_distance:
      best_distance = distance
      best_spot = spot_id

  if best_spot is None:
    return None

  meta = SPOT_METADATA[best_spot]
  return {
    "id": meta["id"],
    "label": meta["label"],
    "distance": best_distance,
  }


def build_floor_snapshot(floor_id: str) -> Optional[Dict]:
  """tek katlık anlık görüntüyü (spot listesi + istatistik) derler."""
  if floor_id not in FLOORS:
    return None
  stats = compute_floor_stats(floor_id)
  nearest = compute_floor_nearest(floor_id)
  floor = FLOORS[floor_id]

  spots_payload = []
  for spot_id in FLOOR_SPOT_IDS.get(floor_id, []):
    meta = SPOT_METADATA[spot_id]
    spots_payload.append(
      {
        "id": meta["id"],
        "label": meta["label"],
        "coords": meta["coords"],
        "occupied": SPOT_STATUS.get(spot_id, False),
      }
    )

  snapshot = {
    "id": floor["id"],
    "label": floor["label"],
    "entrance": floor["entrance"],
    "stats": stats,
    "spots": spots_payload,
    "nearestAvailable": nearest,
    "lastUpdate": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(LAST_UPDATE_TS)),
  }
  return snapshot


def make_state_payload() -> Dict:
  """ui'nın beklediği genel state json'unu hazırlar."""
  with STATE_LOCK:
    floor_snapshots = [build_floor_snapshot(fid) for fid in FLOORS]
    floor_snapshots = [snap for snap in floor_snapshots if snap]

    overall_total = sum(snap["stats"]["total"] for snap in floor_snapshots)
    overall_occupied = sum(snap["stats"]["occupied"] for snap in floor_snapshots)
    overall_free = sum(snap["stats"]["free"] for snap in floor_snapshots)
    occupancy_rate = (overall_occupied / overall_total * 100.0) if overall_total else 0.0

    recommendation = None
    for spec in FLOOR_SPECS:
      snap = next((s for s in floor_snapshots if s["id"] == spec["id"]), None)
      if not snap:
        continue
      nearest = snap["nearestAvailable"]
      if not nearest:
        continue
      recommendation = {
        "floorId": snap["id"],
        "floorLabel": snap["label"],
        "spotId": nearest["id"],
        "label": nearest["label"],
        "distance": nearest["distance"],
      }
      break

    payload = {
      "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(LAST_UPDATE_TS)),
      "overall": {
        "total": overall_total,
        "occupied": overall_occupied,
        "free": overall_free,
        "occupancyRate": occupancy_rate,
      },
      "floors": floor_snapshots,
      "recommendation": recommendation,
    }
    return payload


class ParkingRequestHandler(BaseHTTPRequestHandler):
  """basit rest http işleyicisi (threading http server ile)."""
  server_version = "SmartParkingSim/0.2"

  def log_message(self, fmt: str, *args) -> None:  # noqa: D401
    log_line = f"[{time.strftime('%H:%M:%S')}] {self.address_string()} {fmt % args}\n"
    with Path("simulator.log").open("a", encoding="utf-8") as log_file:
      log_file.write(log_line)

  def _set_headers(self, status=HTTPStatus.OK, content_type="application/json") -> None:
    """json/cors başlıklarını yazar."""
    self.send_response(status)
    self.send_header("Content-Type", content_type)
    self.send_header("Access-Control-Allow-Origin", "*")
    self.send_header(
      "Access-Control-Allow-Headers",
      "Content-Type, ngrok-skip-browser-warning"
    )
    self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
    self.end_headers()

  def respond_json(self, payload, status=HTTPStatus.OK) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    self._set_headers(status=status)
    self.wfile.write(body)

  def do_OPTIONS(self):  # noqa: N802
    self._set_headers(status=HTTPStatus.NO_CONTENT)

  def do_GET(self):  # noqa: N802
    """/state, /spots ve kat bazlı sorguları işler."""
    parsed = urlparse(self.path)
    path = parsed.path.rstrip("/") or "/"

    if path == "/state":
      self.respond_json(make_state_payload())
      return

    if path == "/spots":
      with STATE_LOCK:
        payload = [
          {
            "id": sid,
            "floorId": SPOT_METADATA[sid]["floorId"],
            "occupied": occ,
          }
          for sid, occ in SPOT_STATUS.items()
        ]
      self.respond_json(payload)
      return

    if path.startswith("/floors/"):
      parts = path.split("/")[2:]
      if not parts:
        self.respond_json({"error": "Kat belirtilmedi"}, status=HTTPStatus.BAD_REQUEST)
        return

      floor_id = parts[0].upper()
      if floor_id not in FLOORS:
        self.respond_json({"error": "Kat bulunamadı"}, status=HTTPStatus.NOT_FOUND)
        return

      if len(parts) == 1:
        with STATE_LOCK:
          snapshot = build_floor_snapshot(floor_id)
        self.respond_json(snapshot)
        return

      if len(parts) == 2 and parts[1] == "spots":
        with STATE_LOCK:
          payload = [
            {
              "id": sid,
              "occupied": SPOT_STATUS.get(sid, False),
            }
            for sid in FLOOR_SPOT_IDS.get(floor_id, [])
          ]
        self.respond_json(payload)
        return

    self.respond_json({"error": "Kaynak bulunamadı"}, status=HTTPStatus.NOT_FOUND)

  def do_POST(self):  # noqa: N802
    """/spots/{id} ve /floors/{fid}/spots/{id} güncellemelerini işler."""
    parsed = urlparse(self.path)
    path = parsed.path.rstrip("/")

    target_spot: Optional[str] = None
    if path.startswith("/spots/"):
      target_spot = path.split("/")[-1]
    elif path.startswith("/floors/"):
      parts = path.split("/")[2:]
      if len(parts) == 3 and parts[1] == "spots":
        target_spot = parts[2]

    if not target_spot:
      self.respond_json({"error": "Geçersiz istek"}, status=HTTPStatus.BAD_REQUEST)
      return

    length = int(self.headers.get("Content-Length", 0))
    raw = self.rfile.read(length) if length else b""
    try:
      payload = json.loads(raw.decode("utf-8")) if raw else {}
    except json.JSONDecodeError:
      self.respond_json({"error": "Geçersiz JSON"}, status=HTTPStatus.BAD_REQUEST)
      return

    occupied = payload.get("occupied")
    if isinstance(occupied, str):
      occupied = occupied.lower() in {"1", "true", "yes", "on"}

    if not isinstance(occupied, bool):
      self.respond_json({"error": "occupied alanı zorunlu"}, status=HTTPStatus.BAD_REQUEST)
      return

    if not set_spot_state(target_spot, occupied):
      self.respond_json({"error": "Bilinmeyen park alanı"}, status=HTTPStatus.NOT_FOUND)
      return

    self.respond_json({"result": "OK", "spotId": target_spot, "occupied": occupied})


def random_mutation_worker():
  """rastgele zamanlarda bir grup park alanının durumunu değiştirir."""
  global LAST_UPDATE_TS
  while True:
    time.sleep(random.uniform(3, 6))
    with STATE_LOCK:
      if not SPOT_STATUS:
        continue
      population = list(SPOT_STATUS.keys())
      change_count = max(1, int(len(population) * random.uniform(0.03, 0.12)))
      change_count = min(change_count, len(population))
      for spot_id in random.sample(population, change_count):
        target_ratio = random.uniform(0.35, 0.7)
        SPOT_STATUS[spot_id] = random.random() < target_ratio
      LAST_UPDATE_TS = time.time()


def run_server():
  """çok iş parçacıklı http sunucusunu başlatır."""
  ThreadingHTTPServer.allow_reuse_address = True
  server = ThreadingHTTPServer((HOST, PORT), ParkingRequestHandler)
  print(f"Simülasyon sunucusu http://{HOST}:{PORT} adresinde çalışıyor")
  try:
    server.serve_forever()
  except KeyboardInterrupt:
    print("\nSunucu kapatılıyor...")
    server.server_close()


if __name__ == "__main__":
  init_state(random_initial=RANDOM_INITIAL_OCCUPANCY)
  if ENABLE_RANDOM_MUTATIONS:
    threading.Thread(target=random_mutation_worker, daemon=True).start()
  run_server()
