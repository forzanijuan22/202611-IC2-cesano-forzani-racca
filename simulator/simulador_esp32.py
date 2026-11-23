"""
============================================================================
 SIMULADOR DEL ESP32 — Tanque de agua (IoT Campus)
============================================================================
 Reproduce el contrato MQTT de firmware/tanque_esp32/tanque_esp32.ino (v2.1):

   Publica:
     campus/tanque1/telemetria  QoS 0, sin retain, cada 3 s
         {"porcentaje":float,"cota_cm":float,"distancia_cm":float,
          "bomba":bool,"modo":"AUTO"|"MANUAL","sensor_fault":bool,
          "umbral_bajo":float,"umbral_alto":float}
         Mismo FORMATO_TELEMETRIA que el .ino (todos los números con 1 decimal).
         No se publica hasta tener la primera lectura válida del sensor.
     campus/tanque1/estado      "online" retenido al conectar;
                                "offline" retenido por Last Will (QoS 1)
     campus/tanque1/alerta      QoS 0, sin retain, en cada transición de
                                sensor_fault y al reconectar si sigue en falla
         {"tipo":"sensor_fault","estado":true|false}

   Se suscribe:
     campus/tanque1/comando
         {"modo":"AUTO"|"MANUAL","bomba":"ON"|"OFF",
          "umbral_bajo":0..100,"umbral_alto":0..100}

 Mismas constantes, la misma lógica y los mismos mensajes de log que el
 firmware: tanque virtual de 100 cm útiles (120 cm = vacío, 20 cm = lleno),
 mediana de 7 muestras (1 ciclo = 1,05 s), histéresis 50/95 % (bomba ON a
 25 cm, OFF a 70 cm), sensor_fault sin lectura válida o tras 5 ciclos sin
 eco, bomba apagada por seguridad ante falla, umbrales validados como par.

 Uso:
   pip install -r requirements.txt
   python simulador_esp32.py                       # flujo normal
   python simulador_esp32.py --falla-cada 60       # + falla de sensor cada 60 s
   python simulador_esp32.py --sensor-desconectado # sensor sin eco desde el arranque
   python simulador_esp32.py --broker 192.168.0.3  # broker en otra máquina

 Caída del ESP32 (prueba del Last Will): cerrar el simulador con Ctrl+C o
 detener su contenedor. Se corta el socket sin DISCONNECT y el broker
 publica "offline" (al instante si el socket se cierra, o al vencer el
 keepalive: 15 s x 1,5 ≈ 20 s, igual que PubSubClient).

 Lo único que no se simula es el portal Wi-Fi (WiFiManager / botón BOOT).
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
ALERTA_FALLA_ON = '{"tipo":"sensor_fault","estado":true}'
ALERTA_FALLA_OFF = '{"tipo":"sensor_fault","estado":false}'
MQTT_CLIENT = "esp32_tanque1_sim"   # distinto del real para que puedan convivir
KEEPALIVE_S = 15                    # MQTT_KEEPALIVE por defecto de PubSubClient

# Mismo formato que FORMATO_TELEMETRIA en el .ino (snprintf con %.1f)
FORMATO_TELEMETRIA = (
    '{{"porcentaje":{:.1f},"cota_cm":{:.1f},"distancia_cm":{:.1f},'
    '"bomba":{},"modo":"{}","sensor_fault":{},'
    '"umbral_bajo":{:.1f},"umbral_alto":{:.1f}}}'
)

# ─── Parámetros del tanque (idénticos al .ino) ───────────────────────────────
DIST_VACIO = 120.0
DIST_LLENO = 20.0
RANGO_UTIL = DIST_VACIO - DIST_LLENO      # 100 cm → nivel % = 120 − distancia
UMBRAL_BAJO_DEFECTO = 50.0                # 70 cm: apaga la bomba
UMBRAL_ALTO_DEFECTO = 95.0                # 25 cm: enciende la bomba
DIST_MIN_VALIDA_CM = 20.0                 # igual que el .ino: zona ciega
DIST_MAX_VALIDA_CM = 400.0
NUM_MUESTRAS = 7
INTERVALO_MUESTRA_S = 0.150
CICLO_MEDICION_S = NUM_MUESTRAS * INTERVALO_MUESTRA_S   # 1,05 s
MAX_FALLOS_CONSECUTIVOS = 5
INTERVALO_TELEMETRIA_S = 3.0
TAM_BUFFER_COMANDO = 256

# ─── Dinámica simulada del agua ──────────────────────────────────────────────
LLENADO_CM_POR_CICLO = 1.2                 # ingreso constante de agua
VACIADO_CM_POR_CICLO = 3.5                 # lo que saca la bomba encendida
RUIDO_CM = 0.4


def json_bool(v: bool) -> str:
    return "true" if v else "false"


def es_numero(v) -> bool:
    """Equivale a JsonVariant::is<float>() de ArduinoJson: número, no booleano."""
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


class TanqueSimulado:
    def __init__(self, client: mqtt.Client, falla_cada: float, duracion_falla: float,
                 sensor_desconectado: bool):
        self.client = client
        self.falla_cada = falla_cada
        self.duracion_falla = duracion_falla
        self.sensor_desconectado = sensor_desconectado

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
        self.hay_lectura_valida = False
        self.lecturas_fallidas_consecutivas = 0
        self.umbral_bajo = UMBRAL_BAJO_DEFECTO
        self.umbral_alto = UMBRAL_ALTO_DEFECTO

        self.inicio = time.monotonic()

    # ── Medición ──────────────────────────────────────────────────────────
    def sensor_sin_eco(self) -> bool:
        """True si el sensor no devuelve eco (desconectado o ventana de falla)."""
        if self.sensor_desconectado:
            return True
        if self.falla_cada <= 0:
            return False
        t = (time.monotonic() - self.inicio) % self.falla_cada
        return t >= self.falla_cada - self.duracion_falla

    def ciclo_medicion(self):
        """Equivale a un ciclo completo de tareaMedicion() (7 muestras)."""
        if self.sensor_sin_eco():
            for _ in range(NUM_MUESTRAS):
                print("[SENSOR] Error: Timeout (sin eco)")
            self.lecturas_fallidas_consecutivas += 1
        else:
            muestras = []
            for _ in range(NUM_MUESTRAS):
                d = self.distancia_real + random.uniform(-RUIDO_CM, RUIDO_CM)
                if DIST_MIN_VALIDA_CM <= d <= DIST_MAX_VALIDA_CM:
                    muestras.append(d)
                else:
                    print(f"[SENSOR] Error: Fuera de rango ({d:.1f} cm)")
            if not muestras:
                self.lecturas_fallidas_consecutivas += 1
            else:
                muestras.sort()
                self.lecturas_fallidas_consecutivas = 0
                self.distancia_cm = muestras[len(muestras) // 2]   # mediana
                self.hay_lectura_valida = True
        self.actualizar_nivel()
        self.control_automatico()

    def publicar_alerta_sensor(self, en_falla: bool):
        enviada = self.publicar(TOPIC_ALERTA, ALERTA_FALLA_ON if en_falla else ALERTA_FALLA_OFF)
        print("[ALERTA] {} — {}".format(
            "Sensor en falla" if en_falla else "Sensor recuperado",
            "alerta publicada" if enviada else "sin conexión MQTT, no se pudo publicar"))

    def actualizar_nivel(self):
        """Idéntico a actualizarNivel()."""
        self.sensor_fault = (not self.hay_lectura_valida or
                             self.lecturas_fallidas_consecutivas >= MAX_FALLOS_CONSECUTIVOS)
        if self.sensor_fault != self.sensor_fault_anterior:
            self.publicar_alerta_sensor(self.sensor_fault)
            self.sensor_fault_anterior = self.sensor_fault

        if not self.hay_lectura_valida:
            return
        self.cota_cm = min(max(DIST_VACIO - self.distancia_cm, 0.0), RANGO_UTIL)
        self.porcentaje = min(max(self.cota_cm / RANGO_UTIL * 100.0, 0.0), 100.0)

    # ── Física del agua ───────────────────────────────────────────────────
    def actualizar_fisica(self):
        delta = -LLENADO_CM_POR_CICLO          # el agua sube → distancia baja
        if self.bomba_on:
            delta += VACIADO_CM_POR_CICLO      # la bomba vacía → distancia sube
        self.distancia_real = min(max(self.distancia_real + delta, DIST_LLENO), DIST_VACIO)

    # ── Bomba y control (idéntico a controlAutomatico()) ─────────────────
    def activar_bomba(self):
        self.bomba_on = True
        print("[BOMBA] Activada (relé ON - pin HIGH)")

    def desactivar_bomba(self):
        self.bomba_on = False
        print("[BOMBA] Desactivada (relé OFF - pin LOW)")

    def control_automatico(self):
        if not self.modo_auto:
            return
        if self.sensor_fault:
            if self.bomba_on:
                self.desactivar_bomba()
                print("[AUTO] Falla de sensor → bomba APAGADA por seguridad")
            return
        if self.porcentaje >= self.umbral_alto and not self.bomba_on:
            self.activar_bomba()
            print("[AUTO] Nivel alto → bomba ENCENDIDA (vaciando)")
        elif self.porcentaje <= self.umbral_bajo and self.bomba_on:
            self.desactivar_bomba()
            print("[AUTO] Nivel bajo → bomba APAGADA (fin vaciado)")

    # ── Comandos (idéntico a mqttCallback()) ─────────────────────────────
    def procesar_comando(self, payload: bytes):
        if len(payload) >= TAM_BUFFER_COMANDO:
            print(f"[MQTT] Comando demasiado largo ({len(payload)} bytes), ignorado")
            return
        texto = payload.decode("utf-8", errors="replace")
        print(f"[MQTT RX] {TOPIC_COMANDO} → {texto}")
        try:
            doc = json.loads(texto)
        except json.JSONDecodeError as e:
            print(f"[MQTT] Error JSON: {e.msg}")
            return
        if not isinstance(doc, dict):
            print("[MQTT] Comando ignorado: el JSON no es un objeto")
            return

        reconocido = False

        # Modo
        if doc.get("modo") is not None:
            reconocido = True
            modo = doc["modo"]
            if modo == "AUTO":
                self.modo_auto = True
                print("[CMD] Modo → AUTO")
            elif modo == "MANUAL":
                self.modo_auto = False
                print("[CMD] Modo → MANUAL")
            else:
                print('[CMD] Modo inválido (se espera "AUTO" o "MANUAL"), ignorado')

        # Bomba (solo efectiva en MANUAL)
        if doc.get("bomba") is not None:
            reconocido = True
            bomba = doc["bomba"]
            if bomba not in ("ON", "OFF"):
                print('[CMD] Bomba inválida (se espera "ON" u "OFF"), ignorado')
            elif self.modo_auto:
                print("[CMD] Comando de bomba ignorado (modo AUTO)")
            elif bomba == "ON":
                self.activar_bomba()
                print("[CMD] Bomba → ON (manual)")
            else:
                self.desactivar_bomba()
                print("[CMD] Bomba → OFF (manual)")

        # Umbrales: se validan juntos (rango 0..100 y bajo < alto)
        trae_bajo = doc.get("umbral_bajo") is not None
        trae_alto = doc.get("umbral_alto") is not None
        if trae_bajo or trae_alto:
            reconocido = True
            tipos_ok = ((not trae_bajo or es_numero(doc["umbral_bajo"])) and
                        (not trae_alto or es_numero(doc["umbral_alto"])))
            if not tipos_ok:
                print("[CMD] Umbrales rechazados: deben ser numéricos")
            else:
                nuevo_bajo = float(doc["umbral_bajo"]) if trae_bajo else self.umbral_bajo
                nuevo_alto = float(doc["umbral_alto"]) if trae_alto else self.umbral_alto
                en_rango = 0 <= nuevo_bajo <= 100 and 0 <= nuevo_alto <= 100
                if not en_rango or nuevo_bajo >= nuevo_alto:
                    print(f"[CMD] Umbrales rechazados (bajo={nuevo_bajo:.1f}, alto={nuevo_alto:.1f}): "
                          "deben estar entre 0 y 100 y bajo < alto")
                else:
                    self.umbral_bajo = nuevo_bajo
                    self.umbral_alto = nuevo_alto
                    print(f"[CMD] Umbrales → bajo={self.umbral_bajo:.1f}% alto={self.umbral_alto:.1f}%")

        if not reconocido:
            print("[CMD] Comando sin campos reconocidos, ignorado")

    # ── Publicación (idéntica a publicarTelemetria()) ────────────────────
    def publicar(self, topic: str, payload: str, retain: bool = False) -> bool:
        if not self.client.is_connected():
            return False
        return self.client.publish(topic, payload, qos=0, retain=retain).rc == mqtt.MQTT_ERR_SUCCESS

    def publicar_telemetria(self):
        if not self.client.is_connected():
            return
        if not self.hay_lectura_valida:
            print("[MQTT] Telemetría no publicada: todavía no hay ninguna lectura válida del sensor")
            return
        texto = FORMATO_TELEMETRIA.format(
            self.porcentaje, self.cota_cm, self.distancia_cm,
            json_bool(self.bomba_on),
            "AUTO" if self.modo_auto else "MANUAL",
            json_bool(self.sensor_fault),
            self.umbral_bajo, self.umbral_alto,
        )
        if not self.publicar(TOPIC_TELEMETRIA, texto):
            print("[MQTT] Error al publicar telemetría")
            return
        print(f"[MQTT TX] {texto}")


def main():
    ap = argparse.ArgumentParser(description="Simulador del ESP32 del tanque (contrato MQTT del .ino)")
    ap.add_argument("--broker", default=os.getenv("MQTT_BROKER", "localhost"), help="host del broker MQTT")
    ap.add_argument("--port", type=int, default=int(os.getenv("MQTT_PORT", "1883")))
    ap.add_argument("--falla-cada", type=float, default=0,
                    help="segundos entre fallas simuladas del sensor (0 = sin fallas)")
    ap.add_argument("--duracion-falla", type=float, default=15,
                    help="segundos sin eco en cada falla (>= ~6 s para que se declare sensor_fault)")
    ap.add_argument("--sensor-desconectado", action="store_true",
                    help="el sensor nunca devuelve eco: no se publica telemetría, solo alerta y estado")
    args = ap.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=MQTT_CLIENT)
    sim = TanqueSimulado(client, args.falla_cada, args.duracion_falla, args.sensor_desconectado)

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
        if sim.sensor_fault:
            sim.publicar_alerta_sensor(True)   # la alerta pudo perderse sin red

    def on_disconnect(c, userdata, flags, reason_code, properties):
        print(f"[MQTT] Desconectado ({reason_code}) — reintentando con backoff")

    def on_message(c, userdata, msg):
        sim.procesar_comando(msg.payload)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=5, max_delay=60)   # mismo backoff que el .ino
    print(f"[MQTT] Conectando a {args.broker}:{args.port}")
    client.connect_async(args.broker, args.port, keepalive=KEEPALIVE_S)
    client.loop_start()

    print("========================================")
    print("  SIMULADOR ESP32 — TANQUE (contrato v2.1)")
    print(f"  Broker: {args.broker}:{args.port}")
    if args.sensor_desconectado:
        print("  Sensor DESCONECTADO desde el arranque")
    elif args.falla_cada > 0:
        print(f"  Falla de sensor cada {args.falla_cada:.0f} s durante {args.duracion_falla:.0f} s")
    print("  Ctrl+C = simular caída del ESP32 (dispara el Last Will)")
    print("========================================")

    ultimo_telemetria = None
    try:
        while True:
            inicio_ciclo = time.monotonic()
            sim.actualizar_fisica()
            sim.ciclo_medicion()
            ahora = time.monotonic()
            if ultimo_telemetria is None or ahora - ultimo_telemetria >= INTERVALO_TELEMETRIA_S:
                ultimo_telemetria = ahora
                sim.publicar_telemetria()
                print(f"[SENSOR] Dist: {sim.distancia_cm:.1f} cm | Cota: {sim.cota_cm:.1f} cm | "
                      f"Nivel: {sim.porcentaje:.1f}% | Fault: {'SI' if sim.sensor_fault else 'no'}")
            time.sleep(max(0.0, CICLO_MEDICION_S - (time.monotonic() - inicio_ciclo)))
    except KeyboardInterrupt:
        print("\n[SIM] Caída simulada: se corta la conexión sin DISCONNECT → el broker publicará 'offline'")
        os._exit(0)


if __name__ == "__main__":
    main()
