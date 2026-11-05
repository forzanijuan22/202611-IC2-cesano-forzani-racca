"""
============================================================================
 SIMULADOR DEL ESP32 — Tanque de agua (IoT Campus)
============================================================================
 Reproduce el contrato MQTT de firmware/tanque_esp32/tanque_esp32.ino:

   Publica:
     campus/tanque1/telemetria  QoS 0, sin retain, cada 3 s
         {"porcentaje":float,"cota_cm":float,"distancia_cm":float,
          "bomba":bool,"modo":"AUTO"|"MANUAL","sensor_fault":bool,
          "umbral_bajo":float,"umbral_alto":float}
     campus/tanque1/estado      "online" retenido al conectar;
                                "offline" retenido por Last Will (QoS 1)
     campus/tanque1/alerta      QoS 0, sin retain, solo en transiciones
         {"tipo":"sensor_fault","estado":true|false}

   Se suscribe:
     campus/tanque1/comando
         {"modo":"AUTO"|"MANUAL","bomba":"ON"|"OFF",
          "umbral_bajo":0..100,"umbral_alto":0..100}

 Mismas constantes y la misma lógica que el firmware: rango útil de 175 cm,
 histéresis 50/85 %, falla de sensor tras 5 ciclos sin eco, bomba apagada
 por seguridad ante falla, validación cruzada de umbrales.

 Uso:
   pip install -r requirements.txt
   python simulador_esp32.py                       # flujo normal
   python simulador_esp32.py --falla-cada 60       # + falla de sensor cada 60 s
   python simulador_esp32.py --broker 192.168.0.3  # broker en otra máquina

 Caída del ESP32 (prueba del Last Will): cerrar el simulador con Ctrl+C.
 Se corta el socket sin DISCONNECT, así que el broker publica "offline"
 cuando vence el keepalive (15 s x 1,5 ≈ 20 s, igual que PubSubClient).
============================================================================
"""

import argparse
import json
import math
import os
import random
import time

import paho.mqtt.client as mqtt

# ─── Contrato MQTT (idéntico al .ino) ────────────────────────────────────────
TOPIC_TELEMETRIA = "campus/tanque1/telemetria"
TOPIC_COMANDO = "campus/tanque1/comando"
TOPIC_ESTADO = "campus/tanque1/estado"
TOPIC_ALERTA = "campus/tanque1/alerta"
MQTT_CLIENT = "esp32_tanque1_sim"   # distinto del real para que puedan convivir
KEEPALIVE_S = 15                    # MQTT_KEEPALIVE por defecto de PubSubClient

# ─── Parámetros del tanque (idénticos al .ino) ───────────────────────────────
DIST_VACIO = 200.0
DIST_LLENO = 25.0
RANGO_UTIL = DIST_VACIO - DIST_LLENO      # 175 cm
MAX_FALLOS_CONSECUTIVOS = 5
INTERVALO_TELEMETRIA_S = 3.0
CICLO_MEDICION_S = 1.05                    # 7 muestras x 150 ms en el firmware

# ─── Dinámica simulada del agua ──────────────────────────────────────────────
LLENADO_CM_POR_CICLO = 1.2                 # ingreso constante de agua
VACIADO_CM_POR_CICLO = 3.5                 # lo que saca la bomba encendida
RUIDO_CM = 0.4


def compact(obj) -> str:
    """JSON compacto, igual que serializeJson() de ArduinoJson."""
    return json.dumps(obj, separators=(",", ":"))


def num(x: float):
    """ArduinoJson serializa 50.0 como 50: se replica para que el JSON sea idéntico."""
    return int(x) if float(x).is_integer() else x


def es_numero(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


class TanqueSimulado:
    def __init__(self, client: mqtt.Client, falla_cada: float, duracion_falla: float):
        self.client = client
        self.falla_cada = falla_cada
        self.duracion_falla = duracion_falla

        # Estado físico simulado: distancia real sensor → agua
        self.distancia_real = DIST_VACIO - 0.40 * RANGO_UTIL

        # Variables de estado (mismos nombres que el firmware)
        self.distancia_cm = 0.0
        self.cota_cm = 0.0
        self.porcentaje = 0.0
        self.bomba_on = False
        self.modo_auto = True
        self.sensor_fault = False
        self.sensor_fault_anterior = False
        self.lecturas_fallidas_consecutivas = 0
        self.umbral_bajo = 50.0
        self.umbral_alto = 85.0

        self.inicio = time.monotonic()

    # ── Medición ──────────────────────────────────────────────────────────
    def sensor_sin_eco(self) -> bool:
        """True mientras dura la ventana de falla simulada."""
        if self.falla_cada <= 0:
            return False
        t = (time.monotonic() - self.inicio) % self.falla_cada
        return t >= self.falla_cada - self.duracion_falla

    def medir_distancia_filtrada(self) -> float:
        if self.sensor_sin_eco():
            # Como en el firmware: sin ninguna muestra válida se mantiene la
            # última lectura válida y se cuenta el fallo.
            self.lecturas_fallidas_consecutivas += 1
            return self.distancia_cm
        self.lecturas_fallidas_consecutivas = 0
        return self.distancia_real + random.uniform(-RUIDO_CM, RUIDO_CM)

    def calcular_nivel(self):
        self.distancia_cm = self.medir_distancia_filtrada()
        self.sensor_fault = self.lecturas_fallidas_consecutivas >= MAX_FALLOS_CONSECUTIVOS

        if self.sensor_fault and not self.sensor_fault_anterior:
            self.publicar(TOPIC_ALERTA, compact({"tipo": "sensor_fault", "estado": True}))
            print("[ALERTA] Sensor en falla — alerta publicada")
        elif not self.sensor_fault and self.sensor_fault_anterior:
            self.publicar(TOPIC_ALERTA, compact({"tipo": "sensor_fault", "estado": False}))
            print("[ALERTA] Sensor recuperado — alerta publicada")
        self.sensor_fault_anterior = self.sensor_fault

        self.cota_cm = min(max(DIST_VACIO - self.distancia_cm, 0.0), RANGO_UTIL)
        self.porcentaje = min(max(self.cota_cm / RANGO_UTIL * 100.0, 0.0), 100.0)

    # ── Física del agua ───────────────────────────────────────────────────
    def actualizar_fisica(self):
        delta = -LLENADO_CM_POR_CICLO          # el agua sube → distancia baja
        if self.bomba_on:
            delta += VACIADO_CM_POR_CICLO      # la bomba vacía → distancia sube
        self.distancia_real = min(max(self.distancia_real + delta, DIST_LLENO), DIST_VACIO)

    # ── Control (idéntico a controlAutomatico()) ─────────────────────────
    def control_automatico(self):
        if not self.modo_auto:
            return
        if self.sensor_fault:
            if self.bomba_on:
                self.bomba_on = False
                print("[AUTO] Falla de sensor → bomba APAGADA por seguridad")
            return
        if self.porcentaje >= self.umbral_alto and not self.bomba_on:
            self.bomba_on = True
            print("[AUTO] Nivel alto → bomba ENCENDIDA (vaciando)")
        elif self.porcentaje <= self.umbral_bajo and self.bomba_on:
            self.bomba_on = False
            print("[AUTO] Nivel bajo → bomba APAGADA (fin vaciado)")

    # ── Comandos (idéntico a mqttCallback()) ─────────────────────────────
    def procesar_comando(self, payload: bytes):
        if len(payload) >= 256:
            print("[MQTT] Payload demasiado largo, ignorado")
            return
        texto = payload.decode("utf-8", errors="replace")
        print(f"[MQTT RX] {TOPIC_COMANDO} → {texto}")
        try:
            doc = json.loads(texto)
        except json.JSONDecodeError as e:
            print(f"[MQTT] Error JSON: {e}")
            return
        if not isinstance(doc, dict):
            print("[MQTT] Error JSON: no es un objeto")
            return

        modo = doc.get("modo")
        if isinstance(modo, str):
            if modo == "AUTO":
                self.modo_auto = True
                print("[CMD] Modo → AUTO")
            elif modo == "MANUAL":
                self.modo_auto = False
                print("[CMD] Modo → MANUAL")

        bomba = doc.get("bomba")
        if isinstance(bomba, str):
            if not self.modo_auto:
                if bomba == "ON":
                    self.bomba_on = True
                    print("[CMD] Bomba → ON (manual)")
                elif bomba == "OFF":
                    self.bomba_on = False
                    print("[CMD] Bomba → OFF (manual)")
            else:
                print("[CMD] Comando de bomba ignorado (modo AUTO)")

        nuevo = doc.get("umbral_bajo")
        if es_numero(nuevo):
            if 0 <= nuevo <= 100 and nuevo < self.umbral_alto:
                self.umbral_bajo = float(nuevo)
                print(f"[CMD] Umbral bajo → {self.umbral_bajo:.1f}%")
            else:
                print("[CMD] Umbral bajo rechazado (fuera de rango o >= umbral alto)")

        nuevo = doc.get("umbral_alto")
        if es_numero(nuevo):
            if 0 <= nuevo <= 100 and nuevo > self.umbral_bajo:
                self.umbral_alto = float(nuevo)
                print(f"[CMD] Umbral alto → {self.umbral_alto:.1f}%")
            else:
                print("[CMD] Umbral alto rechazado (fuera de rango o <= umbral bajo)")

    # ── Publicación (idéntica a publicarTelemetria()) ────────────────────
    def publicar(self, topic: str, payload: str, retain: bool = False):
        if self.client.is_connected():
            self.client.publish(topic, payload, qos=0, retain=retain)

    def publicar_telemetria(self):
        doc = {
            "porcentaje": num(round(self.porcentaje, 1)),
            "cota_cm": num(round(self.cota_cm, 1)),
            "distancia_cm": num(round(self.distancia_cm, 1)),
            "bomba": self.bomba_on,
            "modo": "AUTO" if self.modo_auto else "MANUAL",
            "sensor_fault": self.sensor_fault,
            "umbral_bajo": num(self.umbral_bajo),
            "umbral_alto": num(self.umbral_alto),
        }
        texto = compact(doc)
        self.publicar(TOPIC_TELEMETRIA, texto)
        print(f"[MQTT TX] {texto}")


def main():
    ap = argparse.ArgumentParser(description="Simulador del ESP32 del tanque (contrato MQTT del .ino)")
    ap.add_argument("--broker", default=os.getenv("MQTT_BROKER", "localhost"), help="host del broker MQTT")
    ap.add_argument("--port", type=int, default=int(os.getenv("MQTT_PORT", "1883")))
    ap.add_argument("--falla-cada", type=float, default=0,
                    help="segundos entre fallas simuladas del sensor (0 = sin fallas)")
    ap.add_argument("--duracion-falla", type=float, default=15,
                    help="segundos sin eco en cada falla (>= ~6 s para que se declare sensor_fault)")
    args = ap.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=MQTT_CLIENT)
    sim = TanqueSimulado(client, args.falla_cada, args.duracion_falla)

    # Last Will: igual que mqtt.connect(..., TOPIC_ESTADO, 1, true, "offline")
    client.will_set(TOPIC_ESTADO, "offline", qos=1, retain=True)

    def on_connect(c, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            print(f"[MQTT] Fallo, rc={reason_code}")
            return
        print("[MQTT] Conectado ✓")
        c.publish(TOPIC_ESTADO, "online", qos=0, retain=True)
        c.subscribe(TOPIC_COMANDO)
        print(f"[MQTT] Suscrito a {TOPIC_COMANDO}")

    def on_disconnect(c, userdata, flags, reason_code, properties):
        print(f"[MQTT] Desconectado ({reason_code}) — reintentando con backoff")

    def on_message(c, userdata, msg):
        sim.procesar_comando(msg.payload)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=5, max_delay=60)   # mismo backoff que el .ino
    client.connect_async(args.broker, args.port, keepalive=KEEPALIVE_S)
    client.loop_start()

    print("========================================")
    print("  SIMULADOR ESP32 — TANQUE (contrato v2.0)")
    print(f"  Broker: {args.broker}:{args.port}")
    if args.falla_cada > 0:
        print(f"  Falla de sensor cada {args.falla_cada:.0f} s durante {args.duracion_falla:.0f} s")
    print("  Ctrl+C = simular caída del ESP32 (dispara el Last Will)")
    print("========================================")

    ultimo_telemetria = 0.0
    try:
        while True:
            sim.actualizar_fisica()
            sim.calcular_nivel()
            sim.control_automatico()
            ahora = time.monotonic()
            if ahora - ultimo_telemetria >= INTERVALO_TELEMETRIA_S:
                ultimo_telemetria = ahora
                sim.publicar_telemetria()
            time.sleep(CICLO_MEDICION_S)
    except KeyboardInterrupt:
        print("\n[SIM] Caída simulada: se corta la conexión sin DISCONNECT → el broker publicará 'offline'")
        os._exit(0)


if __name__ == "__main__":
    main()
