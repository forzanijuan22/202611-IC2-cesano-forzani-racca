# 💧 IoT Campus — Telemetría y Control de Nivel de Tanque

Sistema de monitoreo y control del nivel de agua de un tanque con **ESP32**, **MQTT (Mosquitto)**, **backend FastAPI + SQLite** y **dashboard web** en tiempo real.

---

## 1. Identificación

| | |
|---|---|
| **Integrantes** | Juan Ignacio Forzani · Agustín Cesano · Alejo Racca |
| **Universidad** | Universidad Nacional de Rafaela (UNRaf) |
| **Materia** | IC2 |
| **Comisión** | _(completar)_ |
| **Trabajo** | TP Final — _(completar número/título oficial)_ |

---

## 2. Descripción del proyecto

El ESP32 mide la distancia a la superficie del agua con un sensor ultrasónico, calcula la **cota** (altura de agua en cm) y el **porcentaje de llenado**, y publica la telemetría por MQTT cada 3 s. Un automatismo con **histéresis** enciende la bomba de vaciado al superar el umbral alto (85 % por defecto) y la apaga al bajar del umbral bajo (50 %). Los umbrales, el modo (AUTO/MANUAL) y la bomba se pueden comandar por MQTT desde el dashboard.

El backend guarda cada lectura en SQLite y expone una API REST con el histórico. El dashboard muestra el nivel en vivo (MQTT por WebSocket), el gráfico histórico (API REST), las alertas de falla de sensor y el estado online/offline del equipo.

### Componentes físicos

| Componente | Modelo | Función |
|---|---|---|
| Microcontrolador | ESP32 WROOM-32D (placa *ESP32 Dev Module*) | Medición, control y comunicación Wi-Fi/MQTT |
| Sensor de nivel | Ultrasónico impermeable **AJ-SR04M** | Distancia sensor → agua (rango usado 25–200 cm) |
| Actuador | Módulo relé 5 V (1 canal), activo en HIGH | Maneja la bomba de vaciado (**lógica lista, módulo aún no conectado**, ver §9) |
| Adaptación de nivel | Divisor resistivo 1 kΩ / 2 kΩ | Baja el ECHO de 5 V a ~3,3 V para el GPIO14 |

---

## 3. Arquitectura del sistema

```
┌────────────────────┐  campus/tanque1/telemetria  ┌──────────────────┐
│ ESP32 (firmware)   │ ──────────────────────────► │                  │
│  o simulador       │  campus/tanque1/estado      │   Broker MQTT    │
│  AJ-SR04M + relé   │ ──────────────────────────► │   Mosquitto      │
│                    │  campus/tanque1/alerta      │  :1883 TCP       │
│                    │ ──────────────────────────► │  :9001 WebSocket │
│                    │ ◄────────────────────────── │                  │
└────────────────────┘  campus/tanque1/comando     └──┬───────────┬───┘
                                                      │ TCP       │ WebSocket
                                      ┌───────────────▼──┐     ┌──▼────────────────┐
                                      │ Backend FastAPI  │     │ Dashboard web     │
                                      │ - suscriptor MQTT│REST │ - nivel en vivo   │
                                      │ - validación     │────►│ - histórico       │
                                      │ - API REST :8000 │     │ - alertas/estado  │
                                      └────────┬─────────┘     │ - comandos MQTT   │
                                               │ SQL           └───────────────────┘
                                      ┌────────▼─────────┐
                                      │ SQLite           │
                                      │ iot_campus.db    │
                                      └──────────────────┘
```

| Capa | Qué corre | Dónde |
|---|---|---|
| **ESP32** | `firmware/tanque_esp32/tanque_esp32.ino`: medición con mediana de 7 muestras, histéresis, reconexión Wi-Fi/MQTT no bloqueante, LWT, detección de falla de sensor | Placa física (o `simulator/simulador_esp32.py`) |
| **Broker MQTT** | Eclipse Mosquitto 2.0.18 (`mosquitto.conf`) | Contenedor `iot-mosquitto` |
| **Backend** | `backend/server.py`: suscriptor MQTT, validación de payloads, API REST, sirve el dashboard | Contenedor `iot-backend` (puerto 8000) |
| **Base de datos** | SQLite, tabla `historial_tanque` (`backend/schema.sql`) | Volumen Docker `backend_data` |
| **Dashboard** | `frontend/index.html` (Chart.js + Paho MQTT JS) | Servido por el backend en `http://localhost:8000` |

---

## 4. Stack tecnológico y dependencias

| Capa | Tecnología | Versión |
|---|---|---|
| Firmware | Arduino core ESP32 vía PlatformIO `espressif32` | 6.9.0 |
| | PubSubClient (Nick O'Leary) | 2.8 |
| | ArduinoJson (Benoît Blanchon) | 7.2.1 |
| | WiFiManager (tzapu) | 2.0.17 |
| | Preferences | incluida en el core |
| Broker | eclipse-mosquitto (Docker) | 2.0.18 |
| Backend | Python (imagen Docker `python:3.12.7-slim`; local ≥ 3.10) | 3.12.7 |
| | fastapi | 0.115.6 |
| | uvicorn[standard] | 0.34.0 |
| | paho-mqtt | 2.1.0 |
| | python-dotenv | 1.1.0 |
| | SQLite | incluida en Python |
| Simulador | Python + paho-mqtt | 2.1.0 |
| Dashboard | Chart.js (CDN) | 4.4.1 |
| | chartjs-adapter-date-fns (CDN) | 3.0.0 |
| | Eclipse Paho MQTT JavaScript (servido localmente, `frontend/paho-mqtt.min.js`) | 1.1.0 |
| Orquestación | Docker Compose | v2 |

---

## 5. Estructura del repositorio

```
.
├── backend/
│   ├── Dockerfile            ← imagen del backend (incluye el dashboard)
│   ├── server.py             ← FastAPI + suscriptor MQTT + SQLite
│   ├── schema.sql            ← esquema de referencia de la tabla
│   ├── requirements.txt      ← dependencias Python con versión exacta
│   └── .env.example          ← plantilla de configuración (sin secretos)
├── docs/                     ← informe técnico en PDF
├── firmware/
│   ├── platformio.ini        ← entorno PlatformIO con librerías fijadas
│   └── tanque_esp32/
│       └── tanque_esp32.ino  ← firmware del ESP32
├── frontend/
│   ├── index.html            ← dashboard
│   ├── paho-mqtt.min.js      ← cliente MQTT por WebSocket (local)
│   └── favicon.svg
├── simulator/
│   ├── simulador_esp32.py    ← simulador del ESP32 (mismo contrato que el .ino)
│   ├── requirements.txt
│   └── Dockerfile
├── docker-compose.yml        ← broker + backend (+ simulador con --profile sim)
├── mosquitto.conf            ← listeners 1883 (TCP) y 9001 (WebSocket)
├── .gitignore
└── README.md
```

---

## 6. Contrato de tópicos MQTT

Prefijo común: `campus/tanque1`. Todos los JSON se envían compactos (sin espacios), codificación UTF-8.

| Tópico | Publicador | Suscriptor(es) | QoS | Retain | Payload |
|---|---|---|:---:|:---:|---|
| `campus/tanque1/telemetria` | ESP32 / simulador, cada 3 s | Backend, dashboard | 0 | No | JSON de telemetría (abajo) |
| `campus/tanque1/estado` | ESP32 / simulador al conectar; **broker** (Last Will) si el equipo se cae | Backend, dashboard | `online`: 0 · LWT `offline`: 1 | **Sí** | Texto plano `online` \| `offline` |
| `campus/tanque1/alerta` | ESP32 / simulador, solo cuando cambia el estado del sensor | Backend, dashboard | 0 | No | JSON de alerta (abajo) |
| `campus/tanque1/comando` | Dashboard (o `mosquitto_pub`) | ESP32 / simulador | 0 | No | JSON de comando (abajo) |

### 6.1 Telemetría — `campus/tanque1/telemetria`

```json
{"porcentaje":45.2,"cota_cm":79.1,"distancia_cm":120.9,"bomba":false,"modo":"AUTO","sensor_fault":false,"umbral_bajo":50,"umbral_alto":85}
```

| Campo | Tipo | Unidad | Rango | Descripción |
|---|---|---|---|---|
| `porcentaje` | number (1 decimal) | % | 0 – 100 | Nivel de llenado |
| `cota_cm` | number (1 decimal) | cm | 0 – 175 | Altura del agua desde el fondo (`200 − distancia`, recortada) |
| `distancia_cm` | number (1 decimal) | cm | 2 – 400 | Distancia filtrada sensor → agua (mediana de 7) |
| `bomba` | boolean | — | — | `true` = relé/bomba encendida |
| `modo` | string | — | `"AUTO"` \| `"MANUAL"` | Modo de operación |
| `sensor_fault` | boolean | — | — | `true` tras 5 ciclos seguidos sin eco válido |
| `umbral_bajo` | number | % | 0 – 100 | Apaga la bomba (fin de vaciado) |
| `umbral_alto` | number | % | 0 – 100, > `umbral_bajo` | Enciende la bomba (inicio de vaciado) |

**Caso de error:** con `sensor_fault=true`, `porcentaje`, `cota_cm` y `distancia_cm` repiten la **última lectura válida** (no se publican valores basura), el automatismo no actúa y, si la bomba estaba encendida en AUTO, se apaga por seguridad. El backend guarda el flag y el dashboard lo informa.

```json
{"porcentaje":45.2,"cota_cm":79.1,"distancia_cm":120.9,"bomba":false,"modo":"AUTO","sensor_fault":true,"umbral_bajo":50,"umbral_alto":85}
```

### 6.2 Alerta — `campus/tanque1/alerta`

| Situación | Payload |
|---|---|
| El sensor entra en falla | `{"tipo":"sensor_fault","estado":true}` |
| El sensor se recupera | `{"tipo":"sensor_fault","estado":false}` |

### 6.3 Estado de conexión — `campus/tanque1/estado`

| Situación | Payload | Quién lo publica |
|---|---|---|
| El ESP32 se conecta al broker | `online` (retenido) | ESP32 |
| El ESP32 pierde la conexión sin desconectarse (corte de luz, Wi-Fi caído, cuelgue) | `offline` (retenido, QoS 1) | Broker, por *Last Will* al vencer el keepalive (15 s × 1,5 ≈ 20 s) |

Así el backend distingue **"ESP32 sin red"** (`estado = offline`) de **"sensor dañado"** (`estado = online` + `sensor_fault = true`). Se consulta en `GET /api/estado`.

### 6.4 Comandos — `campus/tanque1/comando`

Todos los campos son opcionales y se pueden combinar en un mismo mensaje (`modo` se procesa antes que `bomba`).

| Campo | Tipo | Valores válidos | Efecto |
|---|---|---|---|
| `modo` | string | `"AUTO"`, `"MANUAL"` | Cambia el modo de operación |
| `bomba` | string | `"ON"`, `"OFF"` | Enciende/apaga la bomba. **Solo en MANUAL**; en AUTO se ignora |
| `umbral_bajo` | number | 0 – 100 y < `umbral_alto` | Nuevo umbral bajo (persistido en NVS) |
| `umbral_alto` | number | 0 – 100 y > `umbral_bajo` | Nuevo umbral alto (persistido en NVS) |

Ejemplos: `{"modo":"MANUAL","bomba":"ON"}` · `{"modo":"AUTO"}` · `{"umbral_bajo":40,"umbral_alto":90}`

**Validación en el ESP32:** payload vacío, de 256 bytes o más, o JSON inválido → se descarta y se loguea. Valores de tipo incorrecto (p. ej. `"umbral_bajo":"abc"`) o fuera de rango → se ignoran sin tocar el estado; no se usa `atoi()`/`toInt()`.

---

## 7. Puesta en marcha (desde un clon limpio)

### Requisitos
- [Git](https://git-scm.com/) y [Docker Desktop](https://www.docker.com/products/docker-desktop/) (con Docker Compose v2).
- Solo para correr sin Docker: Python 3.10 o superior.
- Solo para el firmware: [PlatformIO](https://platformio.org/) (VS Code) o Arduino IDE 2.x con soporte ESP32.

### Opción A — Todo con Docker (recomendada)

```bash
git clone https://github.com/forzanijuan22/202611-IC2-cesano-forzani-racca.git
cd 202611-IC2-cesano-forzani-racca

# Broker + backend + dashboard + simulador del ESP32
docker compose --profile sim up -d --build
```

1. Abrir **http://localhost:8000** → dashboard con datos del simulador en vivo (cada 3 s).
2. Cada 60 s el simulador provoca una falla de sensor de 15 s: aparece la alerta en el dashboard y `sensor_fault=true` en la telemetría.
3. Probar la API: http://localhost:8000/api/historial?limit=10 · http://localhost:8000/api/estado · http://localhost:8000/docs
4. Probar la caída del equipo (Last Will): `docker compose stop simulador` → a los ~20 s `GET /api/estado` muestra `"conexion": "offline"` y el dashboard avisa.
5. Detener todo: `docker compose --profile sim down`

Para usar **el ESP32 real** en lugar del simulador, levantar sin el perfil: `docker compose up -d --build`.

### Opción B — Sin Docker (Mosquitto instalado en la PC)

```bash
# 1) Broker: Mosquitto 2.x con la configuración del repo
mosquitto -c mosquitto.conf

# 2) Backend (otra terminal)
cd backend
cp .env.example .env            # Windows: copy .env.example .env
python -m venv .venv
.venv/bin/activate              # Windows: .venv\Scripts\activate
pip install -r requirements.txt
python server.py                # → http://localhost:8000

# 3) Simulador (otra terminal)
cd simulator
pip install -r requirements.txt
python simulador_esp32.py --falla-cada 60
```

### Ver el tráfico MQTT crudo (opcional)

```bash
docker exec -it iot-mosquitto mosquitto_sub -t "campus/tanque1/#" -v
docker exec -it iot-mosquitto mosquitto_pub -t "campus/tanque1/comando" -m '{"modo":"MANUAL","bomba":"ON"}'
```

### Firmware (ESP32 físico)

**PlatformIO:**
```bash
cd firmware
pio run                 # compila
pio run -t upload       # carga en la placa
pio device monitor      # monitor serie a 115200
```

**Arduino IDE:** abrir `firmware/tanque_esp32/tanque_esp32.ino`, placa *ESP32 Dev Module*, instalar desde el Library Manager PubSubClient 2.8, ArduinoJson 7.2.1 y WiFiManager 2.0.17, compilar y cargar.

**Primer arranque:** el ESP32 levanta la red Wi-Fi **`Tanque_ESP32`**. Conectarse desde el celular, elegir la red Wi-Fi del lugar, ingresar su contraseña y la **IP de la PC donde corre el broker** (campo "IP Broker MQTT"). Queda guardado en la memoria NVS.

#### Conexionado

| ESP32 | Componente | Notas |
|---|---|---|
| GPIO 12 | AJ-SR04M TRIG | |
| GPIO 14 | AJ-SR04M ECHO | A través de divisor 1 kΩ / 2 kΩ (ECHO sale a 5 V) |
| GPIO 26 | Relé IN | Activo en HIGH (pendiente de conexión) |
| 5V (VIN) | AJ-SR04M VCC · Relé VCC | |
| GND | AJ-SR04M GND · Relé GND · divisor | Masa común |

El diagrama esquemático formal está en el informe (`docs/`).

---

## 8. Gestión de credenciales y placeholders

- **Backend:** `backend/.env.example` es la plantilla. Copiarla a `backend/.env` (`cp .env.example .env`). Ninguna variable es secreta (broker anónimo, SQLite local); si `.env` no existe se usan los mismos valores por defecto. En Docker las variables se definen en `docker-compose.yml`.
- **Firmware:** **no hay credenciales en el código.** El SSID, la contraseña Wi-Fi y la IP del broker se cargan en el portal cautivo de WiFiManager y quedan en la NVS del ESP32. Por eso no hace falta un `secrets.h`.
- **`.gitignore`** excluye `.env`, `secrets.h`, bases `*.db`, `.pio/`, `__pycache__/` y entornos virtuales.
- Si en el futuro se agrega usuario/contraseña al broker, se cargan en `.env` (backend) y nunca se suben al repositorio.

---

## 9. Limitaciones conocidas (fuera de alcance)

- **MQTT sin TLS ni autenticación:** el broker acepta conexiones anónimas en texto plano (`allow_anonymous true`). Es adecuado solo para una red local de laboratorio.
- **Relé y bomba aún no conectados:** la lógica de control y los comandos están implementados y se publican en `bomba`, pero durante las pruebas el módulo relé no estuvo cableado.
- **Un solo tanque:** los tópicos están fijos en `campus/tanque1`; no hay alta dinámica de dispositivos.
- **QoS 0 en telemetría y comandos:** PubSubClient solo publica con QoS 0. Si se pierde una lectura no se reenvía (llega otra a los 3 s). Un comando perdido hay que reenviarlo desde el dashboard.
- **El backend no guarda alertas ni eventos online/offline:** solo los mantiene en memoria (`/api/estado`). El histórico persistido es únicamente la telemetría (con el flag `sensor_fault`).
- **Medición semi-bloqueante:** cada ciclo toma 7 muestras (~1 s). Durante ese tiempo se sigue atendiendo MQTT (`esperarConMqtt()`), pero `pulseIn()` puede bloquear hasta 50 ms por muestra.
- **Fechas en UTC:** SQLite guarda `fecha_hora` en UTC.
- **Dashboard depende de internet para Chart.js y las fuentes** (CDN con versión fija); el cliente MQTT se sirve localmente.
