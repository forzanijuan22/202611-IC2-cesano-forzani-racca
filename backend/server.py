"""
============================================================================
 IOT CAMPUS — Servicio de Ingesta MQTT + API REST
 Backend unificado: FastAPI + Paho-MQTT + SQLite
============================================================================
 Funciones:
   1. Crear la base SQLite y la tabla si no existen (con migración simple).
   2. Suscribirse a <base>/telemetria, <base>/estado y <base>/alerta.
   3. Validar cada telemetría y guardarla en SQLite (descarta payloads
      inválidos en lugar de guardar ceros).
   4. Exponer:
        GET /api/historial  → últimos N registros
        GET /api/ultimo     → última lectura
        GET /api/estado     → online/offline del ESP32 + falla de sensor
        GET /api/health     → salud del servicio
   5. Servir el dashboard (../frontend) en la raíz "/".

 Ejecutar:
   pip install -r requirements.txt
   python server.py
============================================================================
"""

import os
import json
import math
import threading
import logging
from datetime import datetime, timezone
from contextlib import asynccontextmanager

import sqlite3
import uvicorn
import paho.mqtt.client as paho_mqtt
from dotenv import load_dotenv
from fastapi import FastAPI, Query, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles

# ─── Logging ─────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("iot-backend")

# ─── Configuración (.env opcional) ───────────────────────────────────────────
load_dotenv()

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC_BASE = os.getenv("MQTT_TOPIC_BASE", "campus/tanque1").rstrip("/")
TOPIC_TELEMETRIA = f"{MQTT_TOPIC_BASE}/telemetria"
TOPIC_ESTADO = f"{MQTT_TOPIC_BASE}/estado"
TOPIC_ALERTA = f"{MQTT_TOPIC_BASE}/alerta"

API_HOST = os.getenv("API_HOST", "0.0.0.0")
API_PORT = int(os.getenv("API_PORT", "8000"))
DB_PATH = os.getenv("DB_PATH") or os.path.join(BASE_DIR, "iot_campus.db")

# ─── Estado en memoria del dispositivo ───────────────────────────────────────
# Permite distinguir "ESP32 sin red" (conexion=offline, por LWT) de
# "sensor dañado" (conexion=online pero sensor_fault=true).
estado_dispositivo = {
    "conexion": "desconocido",      # online | offline | desconocido
    "sensor_fault": None,           # True | False | None (sin datos aún)
    "ultima_telemetria": None,      # ISO-8601 UTC
    "ultima_alerta": None,          # último payload de <base>/alerta
    "mqtt_backend_conectado": False,
}
estado_lock = threading.Lock()


def ahora_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


# ─── SQLite ──────────────────────────────────────────────────────────────────
db_conn = None
db_lock = threading.Lock()   # una sola conexión compartida entre hilos


def init_database():
    """Crea la base de datos y la tabla si no existen."""
    global db_conn
    os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
    db_conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    db_conn.row_factory = sqlite3.Row
    with db_lock:
        cur = db_conn.cursor()
        cur.execute("""
            CREATE TABLE IF NOT EXISTS historial_tanque (
                id              INTEGER       PRIMARY KEY AUTOINCREMENT,
                fecha_hora      DATETIME      DEFAULT CURRENT_TIMESTAMP,
                porcentaje      REAL          NOT NULL,
                cota_cm         REAL          NOT NULL,
                distancia_cm    REAL          NOT NULL,
                estado_bomba    BOOLEAN       NOT NULL DEFAULT 0,
                modo_operacion  VARCHAR(10)   NOT NULL DEFAULT 'AUTO',
                sensor_fault    BOOLEAN       NOT NULL DEFAULT 0
            )
        """)
        # Migración: bases creadas con la versión anterior no tienen sensor_fault
        columnas = {row["name"] for row in cur.execute("PRAGMA table_info(historial_tanque)")}
        if "sensor_fault" not in columnas:
            cur.execute("ALTER TABLE historial_tanque ADD COLUMN sensor_fault BOOLEAN NOT NULL DEFAULT 0")
            log.info("Migración: columna sensor_fault agregada")
        cur.execute("CREATE INDEX IF NOT EXISTS idx_fecha ON historial_tanque(fecha_hora DESC)")
        db_conn.commit()
    log.info("Base de datos SQLite inicializada en '%s' ✓", DB_PATH)


def _es_numero(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def validar_telemetria(data) -> str | None:
    """Devuelve None si el payload cumple el contrato, o el motivo del rechazo."""
    if not isinstance(data, dict):
        return "el payload no es un objeto JSON"
    rangos = {"porcentaje": (0, 100), "cota_cm": (0, 500), "distancia_cm": (0, 500)}
    for campo, (lo, hi) in rangos.items():
        v = data.get(campo)
        if not _es_numero(v):
            return f"'{campo}' falta o no es numérico"
        if not lo <= v <= hi:
            return f"'{campo}'={v} fuera de rango [{lo}, {hi}]"
    if not isinstance(data.get("bomba"), bool):
        return "'bomba' falta o no es booleano"
    if data.get("modo") not in ("AUTO", "MANUAL"):
        return "'modo' debe ser 'AUTO' o 'MANUAL'"
    if "sensor_fault" in data and not isinstance(data["sensor_fault"], bool):
        return "'sensor_fault' no es booleano"
    return None


def insertar_telemetria(data: dict):
    """Inserta un registro de telemetría ya validado."""
    try:
        with db_lock:
            db_conn.execute("""
                INSERT INTO historial_tanque
                (porcentaje, cota_cm, distancia_cm, estado_bomba, modo_operacion, sensor_fault)
                VALUES (?, ?, ?, ?, ?, ?)
            """, (
                float(data["porcentaje"]),
                float(data["cota_cm"]),
                float(data["distancia_cm"]),
                bool(data["bomba"]),
                data["modo"],
                bool(data.get("sensor_fault", False)),
            ))
            db_conn.commit()
    except Exception as e:
        log.error("Error al insertar en SQLite: %s", e)


def consultar(sql: str, params: tuple = ()) -> list[dict]:
    with db_lock:
        rows = db_conn.execute(sql, params).fetchall()
    resultado = []
    for row in rows:
        r = dict(row)
        r["estado_bomba"] = bool(r.get("estado_bomba"))
        r["sensor_fault"] = bool(r.get("sensor_fault"))
        resultado.append(r)
    return resultado


# ─── MQTT ────────────────────────────────────────────────────────────────────

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        log.warning("MQTT rechazó la conexión: %s", reason_code)
        return
    log.info("MQTT conectado a %s:%d", MQTT_BROKER, MQTT_PORT)
    with estado_lock:
        estado_dispositivo["mqtt_backend_conectado"] = True
    # Re-suscribir en cada (re)conexión
    client.subscribe([(TOPIC_TELEMETRIA, 0), (TOPIC_ESTADO, 1), (TOPIC_ALERTA, 0)])
    log.info("Suscrito a: %s, %s, %s", TOPIC_TELEMETRIA, TOPIC_ESTADO, TOPIC_ALERTA)


def on_disconnect(client, userdata, flags, reason_code, properties):
    with estado_lock:
        estado_dispositivo["mqtt_backend_conectado"] = False
    log.warning("MQTT desconectado (%s) — paho reintenta automáticamente", reason_code)


def on_message(client, userdata, msg):
    try:
        texto = msg.payload.decode("utf-8", errors="replace").strip()

        if msg.topic == TOPIC_ESTADO:
            if texto in ("online", "offline"):
                with estado_lock:
                    estado_dispositivo["conexion"] = texto
                nivel = logging.INFO if texto == "online" else logging.WARNING
                log.log(nivel, "ESP32 %s", texto.upper())
            else:
                log.warning("Estado desconocido en %s: %r", msg.topic, texto)
            return

        data = json.loads(texto)

        if msg.topic == TOPIC_ALERTA:
            if isinstance(data, dict) and data.get("tipo") == "sensor_fault" and isinstance(data.get("estado"), bool):
                with estado_lock:
                    estado_dispositivo["sensor_fault"] = data["estado"]
                    estado_dispositivo["ultima_alerta"] = {**data, "recibida": ahora_iso()}
                log.warning("ALERTA sensor_fault=%s", data["estado"])
            else:
                log.warning("Alerta con formato inválido: %r", texto)
            return

        # Telemetría
        motivo = validar_telemetria(data)
        if motivo:
            log.warning("Telemetría descartada (%s): %s", motivo, texto)
            return
        log.info("← [%s] %s", msg.topic, texto)
        with estado_lock:
            estado_dispositivo["ultima_telemetria"] = ahora_iso()
            estado_dispositivo["sensor_fault"] = bool(data.get("sensor_fault", False))
            # Si llega telemetría, el equipo está online aunque no hayamos visto el retain
            estado_dispositivo["conexion"] = "online"
        insertar_telemetria(data)

    except json.JSONDecodeError as e:
        log.warning("JSON inválido en %s: %s", msg.topic, e)
    except Exception as e:
        log.error("Error procesando mensaje MQTT: %s", e)


def iniciar_mqtt():
    """Arranca el cliente MQTT en un hilo daemon con reconexión automática."""
    client = paho_mqtt.Client(
        paho_mqtt.CallbackAPIVersion.VERSION2,
        client_id="backend_ingesta",
    )
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)   # backoff exponencial
    client.connect_async(MQTT_BROKER, MQTT_PORT, keepalive=60)

    def runner():
        # retry_first_connection: si el broker todavía no levantó, sigue intentando
        client.loop_forever(retry_first_connection=True)

    threading.Thread(target=runner, daemon=True, name="mqtt").start()
    log.info("Hilo MQTT iniciado (broker %s:%d)", MQTT_BROKER, MQTT_PORT)


# ─── FastAPI ─────────────────────────────────────────────────────────────────

@asynccontextmanager
async def lifespan(app):
    init_database()
    iniciar_mqtt()
    log.info("Servidor listo en http://localhost:%d", API_PORT)
    yield


app = FastAPI(
    title="IoT Campus — API de Telemetría",
    version="1.1.0",
    description="Backend para sistema de control de nivel de tanques.",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

COLUMNAS = "id, fecha_hora, porcentaje, cota_cm, distancia_cm, estado_bomba, modo_operacion, sensor_fault"


@app.get("/api/historial")
def obtener_historial(limit: int = Query(default=100, ge=1, le=1000)):
    """Devuelve los últimos `limit` registros de telemetría (más nuevo primero)."""
    try:
        rows = consultar(
            f"SELECT {COLUMNAS} FROM historial_tanque ORDER BY id DESC LIMIT ?", (limit,)
        )
        return {"total": len(rows), "registros": rows}
    except Exception as e:
        log.error("Error al consultar historial: %s", e)
        raise HTTPException(status_code=500, detail="Database error")


@app.get("/api/ultimo")
def obtener_ultimo():
    """Devuelve la última lectura registrada."""
    try:
        rows = consultar(f"SELECT {COLUMNAS} FROM historial_tanque ORDER BY id DESC LIMIT 1")
        return {"registro": rows[0] if rows else None}
    except Exception as e:
        log.error("Error al consultar último: %s", e)
        raise HTTPException(status_code=500, detail="Database error")


@app.get("/api/estado")
def obtener_estado():
    """Estado del dispositivo: online/offline (LWT) y falla de sensor."""
    with estado_lock:
        return dict(estado_dispositivo)


@app.get("/api/health")
def health_check():
    """Endpoint de salud para monitoreo."""
    with estado_lock:
        mqtt_ok = estado_dispositivo["mqtt_backend_conectado"]
    return {"status": "ok", "mqtt": mqtt_ok, "timestamp": ahora_iso()}


# ─── Frontend estático (al final, para no tapar /api/*) ─────────────────────
frontend_dir = os.path.abspath(os.path.join(BASE_DIR, "..", "frontend"))
if os.path.isdir(frontend_dir):
    app.mount("/", StaticFiles(directory=frontend_dir, html=True), name="frontend")
    log.info("Dashboard servido desde: %s", frontend_dir)


if __name__ == "__main__":
    uvicorn.run(app, host=API_HOST, port=API_PORT, log_level="info")
