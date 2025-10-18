"""
fastapi tabanlı kalıcı backend.

amaç: mevcut web ui ve esp32 firmware sözleşmesini bozmadan /state, /spots ve /floors/*
endpointlerini sunmak; svg dosyasından spot bilgilerini yükleyip
sqlite veya postgresql üzerinde saklar. frontend ile sözleşme
uyumluluğu korunur.

env değişkenleri:
- sp_database_url: veritabanı url'si (varsayılan: sqlite:///./smart_parking.db)
- sp_assets_svg: svg yolu (varsayılan: web/assets/parking-10.svg)
- sp_floor_id: tek kat kimliği (varsayılan: f1)
- sp_floor_label: kat etiketi (varsayılan: zemin kat)

"""

from __future__ import annotations

import json
import math
import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from fastapi import FastAPI, HTTPException, Request
from fastapi.staticfiles import StaticFiles
from fastapi.responses import RedirectResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from sqlalchemy import (
    Boolean,
    Column,
    DateTime,
    Float,
    String,
    create_engine,
    func,
)
from sqlalchemy.engine import Engine
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy.orm import Session, sessionmaker
from xml.etree import ElementTree as ET


# ------------------------- konfigürasyon -------------------------

BASE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_DB_URL = "sqlite:///./smart_parking.db"
DB_URL = os.getenv("SP_DATABASE_URL", DEFAULT_DB_URL)
SVG_PATH = Path(os.getenv("SP_ASSETS_SVG", str(BASE_DIR / "web" / "assets" / "parking-10.svg")))
FLOOR_ID = os.getenv("SP_FLOOR_ID", "F1")
FLOOR_LABEL = os.getenv("SP_FLOOR_LABEL", "Zemin Kat")

SVG_NS = "{http://www.w3.org/2000/svg}"


# ------------------------- veritabanı ----------------------------

Base = declarative_base()


class Floor(Base):
    __tablename__ = "floor"
    id = Column(String, primary_key=True)
    label = Column(String, nullable=False)
    entrance_x = Column(Float, default=0.0)
    entrance_y = Column(Float, default=0.0)
    anchor_spot_id = Column(String, nullable=True)


class Spot(Base):
    __tablename__ = "spot"
    id = Column(String, primary_key=True)
    floor_id = Column(String, nullable=False)
    label = Column(String, nullable=False)
    x = Column(Float, default=0.0)
    y = Column(Float, default=0.0)
    occupied = Column(Boolean, default=False, nullable=False)
    updated_at = Column(DateTime(timezone=True), default=lambda: datetime.now(timezone.utc), nullable=False)


def make_engine(db_url: str) -> Engine:
    connect_args = {"check_same_thread": False} if db_url.startswith("sqlite") else {}
    engine = create_engine(db_url, echo=False, future=True, connect_args=connect_args)
    return engine


engine = make_engine(DB_URL)
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False, future=True)


def init_db() -> None:
    Base.metadata.create_all(engine)


# ------------------------- svg yükleyici -------------------------

def _svg_children(group, tag_name: str):
    # xml.etree ile parse edilen svg ağacında belirli bir tag'i arar
    # örnek: bir <g> grubunun içindeki <rect> veya <text> elemanını almak için
    for child in group:
        if child.tag.endswith(tag_name):
            return child
    return None


def load_spots_from_svg(svg_path: Path) -> Tuple[List[Dict], Dict]:
    if not svg_path.exists():
        raise FileNotFoundError(f"svg bulunamadı: {svg_path}")
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
        spots.append({"id": spot_id, "label": label, "x": x, "y": y})
    # giriş noktası: ilk spotu referans al
    entrance = {"x": 0.0, "y": 0.0, "anchorSpotId": None}
    if spots:
        entrance.update({"x": spots[0]["x"], "y": spots[0]["y"], "anchorSpotId": spots[0]["id"]})
    return spots, entrance


def seed_from_svg(db: Session) -> None:
    # kat kaydı yoksa oluştur
    floor = db.get(Floor, FLOOR_ID)
    if floor is None:
        # svg'den spot listesini ve giriş koordinatlarını oku
        spots, entrance = load_spots_from_svg(SVG_PATH)
        floor = Floor(
            id=FLOOR_ID,
            label=FLOOR_LABEL,
            entrance_x=entrance["x"],
            entrance_y=entrance["y"],
            anchor_spot_id=entrance.get("anchorSpotId"),
        )
        db.add(floor)
        db.flush()
        # spotları ekle (mevcutta yoksa)
        for s in spots:
            if db.get(Spot, s["id"]) is None:
                db.add(
                    Spot(
                        id=s["id"],
                        floor_id=floor.id,
                        label=s["label"],
                        x=s["x"],
                        y=s["y"],
                        occupied=False,
                    )
                )
        db.commit()
    else:
        # kat varsa ve spotlar eksikse svg'den tamamla
        spots, _ = load_spots_from_svg(SVG_PATH)
        added = 0
        for s in spots:
            if db.get(Spot, s["id"]) is None:
                db.add(
                    Spot(
                        id=s["id"],
                        floor_id=floor.id,
                        label=s["label"],
                        x=s["x"],
                        y=s["y"],
                        occupied=False,
                    )
                )
                added += 1
        if added:
            db.commit()


# ------------------------- fastapi app ---------------------------

app = FastAPI(title="Smart Parking Backend", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"]
)


class SpotUpdate(BaseModel):
    occupied: bool


def _state_timestamp(db: Session) -> datetime:
    ts = db.query(func.max(Spot.updated_at)).scalar()
    return ts or datetime.now(timezone.utc)


def compute_floor_snapshot(db: Session, floor_id: str) -> Optional[Dict]:
    # verilen kat id'si için anlık snapshott oluşturur: spot listesi, istatistikler
    # ve girişe en yakın boş park alanını hesaplar. dönen sözleşme frontend'in
    # beklediği json yapısına uygundur.
    floor = db.get(Floor, floor_id)
    if floor is None:
        return None

    spots = db.query(Spot).filter(Spot.floor_id == floor.id).order_by(Spot.id.asc()).all()
    total = len(spots)
    occupied = sum(1 for s in spots if s.occupied)
    free = total - occupied
    rate = (occupied / total * 100.0) if total else 0.0

    def distance_to_entrance(s: Spot) -> float:
        dx = (s.x or 0.0) - (floor.entrance_x or 0.0)
        dy = (s.y or 0.0) - (floor.entrance_y or 0.0)
        return math.hypot(dx, dy)

    nearest = None
    best_d = None
    for s in spots:
        if s.occupied:
            continue
        d = distance_to_entrance(s)
        if best_d is None or d < best_d:
            best_d = d
            nearest = s

    nearest_payload = None
    if nearest is not None and best_d is not None:
        nearest_payload = {"id": nearest.id, "label": nearest.label, "distance": best_d}

    spots_payload = [
        {
            "id": s.id,
            "label": s.label,
            "coords": {"x": s.x, "y": s.y},
            "occupied": bool(s.occupied),
        }
        for s in spots
    ]

    return {
        "id": floor.id,
        "label": floor.label,
        "entrance": {
            "id": f"{floor.id}-entrance",
            "coords": {"x": floor.entrance_x, "y": floor.entrance_y},
            "anchorSpotId": floor.anchor_spot_id,
        },
        "stats": {"total": total, "occupied": occupied, "free": free, "occupancyRate": rate},
        "spots": spots_payload,
        "nearestAvailable": nearest_payload,
        "lastUpdate": _state_timestamp(db).astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }


def build_state_payload(db: Session) -> Dict:
    # tüm katlar için genel durum paketini hazırlar. bu proje tek katlı
    # olduğundan sadece FLOOR_ID kullanılıyor ancak yapıyı genişletmek kolaydır.
    snap = compute_floor_snapshot(db, FLOOR_ID)
    floors = [s for s in [snap] if s]
    overall_total = sum(s["stats"]["total"] for s in floors)
    overall_occ = sum(s["stats"]["occupied"] for s in floors)
    overall_free = sum(s["stats"]["free"] for s in floors)
    rate = (overall_occ / overall_total * 100.0) if overall_total else 0.0
    recommendation = None
    if snap and snap.get("nearestAvailable"):
        na = snap["nearestAvailable"]
        recommendation = {
            "floorId": snap["id"],
            "floorLabel": snap["label"],
            "spotId": na["id"],
            "label": na["label"],
            "distance": na["distance"],
        }
    ts = _state_timestamp(db).astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "timestamp": ts,
        "overall": {
            "total": overall_total,
            "occupied": overall_occ,
            "free": overall_free,
            "occupancyRate": rate,
        },
        "floors": floors,
        "recommendation": recommendation,
    }


@app.on_event("startup")
def on_startup() -> None:
    # uygulama başlarken veritabanını hazırla ve svg'den seed verisi yükle
    init_db()
    with SessionLocal() as db:
        seed_from_svg(db)

# web arayüzünü /ui altında servis et (web/ dizini)
app.mount("/ui", StaticFiles(directory=str(BASE_DIR / "web"), html=True), name="ui")


@app.get("/state")
def get_state():
    # genel durum endpoint'i: ui bu endpointi periyodik olarak çağırır
    with SessionLocal() as db:
        return build_state_payload(db)


@app.get("/spots")
def list_spots():
    # tüm spotların düz liste halinde durumunu döner
    with SessionLocal() as db:
        rows = db.query(Spot).all()
        return [
            {"id": r.id, "floorId": r.floor_id, "occupied": bool(r.occupied)}
            for r in rows
        ]


@app.post("/spots/{spot_id}")
async def update_spot_async(spot_id: str, request: Request):
    try:
        raw = await request.body()
        payload = json.loads(raw.decode("utf-8")) if raw else {}
    except Exception:
        raise HTTPException(status_code=400, detail="Geçersiz JSON")

    occupied = payload.get("occupied")
    if isinstance(occupied, str):
        occupied = occupied.strip().lower() in {"1", "true", "yes", "on"}
    if not isinstance(occupied, bool):
        raise HTTPException(status_code=400, detail="occupied alanı zorunlu")

    # bir spotun doluluk bilgisini günceller. esp32 firmware her durum
    # değişikliğinde veya periyodik resend sırasında bu endpoint'e post atar.
    with SessionLocal() as db:
        spot = db.get(Spot, spot_id)
        if spot is None:
            raise HTTPException(status_code=404, detail="Bilinmeyen park alanı")
        spot.occupied = bool(occupied)
        spot.updated_at = datetime.now(timezone.utc)
        db.add(spot)
        db.commit()
    return {"result": "OK", "spotId": spot_id, "occupied": bool(occupied)}


@app.get("/floors/{floor_id}")
def get_floor(floor_id: str):
    # tek kat bilgisi: kat id'sine göre snapshot döner
    with SessionLocal() as db:
        snap = compute_floor_snapshot(db, floor_id.upper())
        if not snap:
            raise HTTPException(status_code=404, detail="Kat bulunamadı")
        return snap


@app.get("/floors/{floor_id}/spots")
def get_floor_spots(floor_id: str):
    # belirli bir katın spot listesini ve doluluk durumlarını döner
    with SessionLocal() as db:
        rows = db.query(Spot).filter(Spot.floor_id == floor_id.upper()).all()
        return [{"id": r.id, "occupied": bool(r.occupied)} for r in rows]


# kök yolu /ui/ arayüzüne yönlendir
@app.get("/")
def root_redirect():
    return RedirectResponse(url="/ui/")

# sağlık kontrolü
@app.get("/health")
def health() -> Dict[str, str]:
    return {"status": "ok", "db": DB_URL.split(":")[0]}
