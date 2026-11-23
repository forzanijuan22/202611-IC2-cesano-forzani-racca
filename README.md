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

El ESP32 mide la distancia a la superficie del agua con un sensor ultrasónico, calcula la **cota** (altura de agua en cm) y el **porcentaje de llenado**, y publica la telemetría por MQTT cada 3 s. Un automatismo con **histéresis** enciende la bomba de vaciado al llegar al umbral alto (95 % por defecto, sensor a 25 cm) y la apaga al bajar al umbral bajo (50 %, sensor a 70 cm). El modo (AUTO/MANUAL) y la bomba se comandan por MQTT desde el panel de controles del dashboard, y los umbrales desde su sección Configuración.

El backend guarda cada lectura en SQLite y expone una API REST con el histórico. El dashboard muestra el nivel en vivo (MQTT por WebSocket), el gráfico histórico (API REST), las alertas de falla de sensor y el estado online/offline del equipo.

### Cómo se simula el tanque en el prototipo

No se dispone de un tanque real, así que el nivel se simula con la **distancia del sensor**: el AJ-SR04M se monta fuera de los frascos, apuntando a una superficie, y se acerca o se aleja para representar más o menos agua. El firmware trabaja con un tanque *virtual* de **100 cm útiles**, como un tanque real de ~1 m de alto:

| Distancia medida | Nivel | Qué hace el automatismo (umbrales por defecto) |
|---|---|---|
| 120 cm o más | 0 % (vacío) | — |
| 70 cm | 50 % (umbral bajo) | **Apaga** la bomba al llegar a 70 cm o más (fin del vaciado) |
| 25 cm | 95 % (umbral alto) | **Enciende** la bomba al llegar a 25 cm o menos (inicio del vaciado) |
| 20 cm | 100 % (lleno) | — |
| menos de 20 cm | lectura inválida (zona ciega del sensor) | Se mantiene el último valor; si dura 5 ciclos (~5 s), `sensor_fault` y la bomba se apaga por seguridad |

Fórmula: `nivel % = 120 − distancia (cm)`, recortado entre 0 y 100 (en el código: `(DIST_VACIO − d) / (DIST_VACIO − DIST_LLENO) × 100`, con `DIST_VACIO = 120` y `DIST_LLENO = 20`). Los umbrales se pueden cambiar desde el dashboard: cada punto de umbral equivale a 1 cm. Por la mediana de 7 muestras, un cambio brusco de distancia se refleja en 1 o 2 ciclos de medición (hasta ~2,1 s): un eco espurio aislado no mueve la bomba.

### Componentes físicos

| Componente | Modelo | Función |
|---|---|---|
| Microcontrolador | ESP32 WROOM-32D (placa *ESP32 Dev Module*) | Medición, control y comunicación Wi-Fi/MQTT |
| Sensor de nivel | Ultrasónico impermeable **AJ-SR04M** | Distancia sensor → agua (ver `DIST_VACIO` / `DIST_LLENO`, §6.1) |
| Actuador | Módulo relé 5 V (1 canal), activo en HIGH | Su contacto normal abierto (NA) enciende y apaga la bomba |
| Bomba | Mini bomba sumergible DC, 2,5–6 V, 130–220 mA, 0,4–1,5 W, 80–120 L/h, altura máx. 40–110 cm | Alimentada a 5 V; mientras está encendida pasa agua de un frasco a otro |
| Maqueta del actuador | Dos frascos de ~30 cm unidos por una manguera | Muestran que la bomba funciona; **no** son el tanque medido (ver arriba) |
| Alimentación | Dos puertos USB (5 V) de la PC | **USB 1:** el ESP32, que desde su pin 5V alimenta el riel de la protoboard (sensor y relé). **USB 2:** la bomba, a través del contacto del relé. Las masas se unen en la protoboard |
| Adaptación de nivel | Divisor resistivo: R1 = 1 kΩ en serie desde ECHO, R2 = 2 kΩ a GND | Baja el ECHO de 5 V a 5 · 2/(1+2) ≈ 3,33 V para el GPIO14 |
| Soporte | Pieza impresa diseñada en OpenSCAD | Mantiene el sensor perpendicular a la superficie |
| Pulsador | Botón **BOOT** de la placa (GPIO 0) | Mantenido 3 s abre el portal de configuración Wi-Fi/broker |

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
| **ESP32** | `firmware/tanque_esp32/tanque_esp32.ino`: loop sin `delay()` (1 muestra cada 150 ms con `millis()`, mediana de 7), histéresis, reconexión Wi-Fi/MQTT no bloqueante con backoff, LWT, detección y reporte de falla de sensor, validación de comandos | Placa física (o `simulator/simulador_esp32.py`) |
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

Prefijo común: `campus/tanque1`. Todos los JSON se envían compactos (sin espacios), codificación UTF-8. El firmware arma la telemetría con `snprintf()` y el formato `FORMATO_TELEMETRIA`; el simulador usa exactamente el mismo formato.

| Tópico | Publicador | Suscriptor(es) | QoS | Retain | Payload |
|---|---|---|:---:|:---:|---|
| `campus/tanque1/telemetria` | ESP32 / simulador, cada 3 s | Backend, dashboard | 0 | No | JSON de telemetría (abajo) |
| `campus/tanque1/estado` | ESP32 / simulador al conectar; **broker** (Last Will) si el equipo se cae | Backend, dashboard | `online`: 0 · LWT `offline`: 1 | **Sí** | Texto plano `online` \| `offline` |
| `campus/tanque1/alerta` | ESP32 / simulador, cuando cambia el estado del sensor (y al reconectar si sigue en falla) | Backend, dashboard | 0 | No | JSON de alerta (abajo) |
| `campus/tanque1/comando` | Dashboard (o `mosquitto_pub`) | ESP32 / simulador | 0 | No | JSON de comando (abajo) |

### 6.1 Telemetría — `campus/tanque1/telemetria`

```json
{"porcentaje":45.2,"cota_cm":45.2,"distancia_cm":74.8,"bomba":false,"modo":"AUTO","sensor_fault":false,"umbral_bajo":50.0,"umbral_alto":95.0}
```

| Campo | Tipo | Unidad | Rango | Descripción |
|---|---|---|---|---|
| `porcentaje` | number (1 decimal) | % | 0 – 100 | Nivel de llenado |
| `cota_cm` | number (1 decimal) | cm | 0 – 100 | Altura del agua desde el fondo (`120 − distancia`, recortada) |
| `distancia_cm` | number (1 decimal) | cm | 20 – 400 | Distancia filtrada sensor → agua (mediana de 7). Las lecturas por debajo de 20 cm (zona ciega) se descartan como inválidas |
| `bomba` | boolean | — | — | `true` = relé/bomba encendida |
| `modo` | string | — | `"AUTO"` \| `"MANUAL"` | Modo de operación |
| `sensor_fault` | boolean | — | — | `true` tras 5 ciclos seguidos (≈ 5 s) sin ningún eco válido |
| `umbral_bajo` | number (1 decimal) | % | 0 – 100 | Apaga la bomba (fin de vaciado) |
| `umbral_alto` | number (1 decimal) | % | 0 – 100, > `umbral_bajo` | Enciende la bomba (inicio de vaciado) |

**Casos de error (nunca se publican valores basura):**
- **Sensor sin eco durante la operación:** con `sensor_fault=true`, `porcentaje`, `cota_cm` y `distancia_cm` repiten la **última lectura válida**, el automatismo no actúa y, si la bomba estaba encendida en AUTO, se apaga por seguridad. El backend guarda el flag y el dashboard muestra "Sensor: FALLA".
- **Sensor sin eco desde el arranque:** mientras no haya ninguna lectura válida, `sensor_fault` vale `true`, se publica la alerta (§6.2) y **no se publica telemetría** (no hay ningún valor real que informar).

```json
{"porcentaje":45.2,"cota_cm":45.2,"distancia_cm":74.8,"bomba":false,"modo":"AUTO","sensor_fault":true,"umbral_bajo":50.0,"umbral_alto":95.0}
```

### 6.2 Alerta — `campus/tanque1/alerta`

| Situación | Payload |
|---|---|
| El sensor entra en falla (o arranca sin lectura válida) | `{"tipo":"sensor_fault","estado":true}` |
| El sensor se recupera | `{"tipo":"sensor_fault","estado":false}` |
| El ESP32 se reconecta al broker y el sensor sigue en falla | `{"tipo":"sensor_fault","estado":true}` (reenvío, por si se perdió sin red) |

### 6.3 Estado de conexión — `campus/tanque1/estado`

| Situación | Payload | Quién lo publica |
|---|---|---|
| El ESP32 se conecta al broker | `online` (retenido) | ESP32 |
| El ESP32 pierde la conexión sin desconectarse (corte de luz, Wi-Fi caído, cuelgue) | `offline` (retenido, QoS 1) | Broker, por *Last Will*: al instante si se corta el socket, o al vencer el keepalive (15 s × 1,5 ≈ 20 s) |

Así el backend distingue **"ESP32 sin red"** (`estado = offline`) de **"sensor dañado"** (`estado = online` + `sensor_fault = true`). Se consulta en `GET /api/estado`.

### 6.4 Comandos — `campus/tanque1/comando`

Todos los campos son opcionales y se pueden combinar en un mismo mensaje (`modo` se procesa antes que `bomba`).

| Campo | Tipo | Valores válidos | Efecto |
|---|---|---|---|
| `modo` | string | `"AUTO"`, `"MANUAL"` | Cambia el modo de operación |
| `bomba` | string | `"ON"`, `"OFF"` | Enciende/apaga la bomba. **Solo en MANUAL**; en AUTO se ignora |
| `umbral_bajo` | number | 0 – 100 | Nuevo umbral bajo (persistido en NVS) |
| `umbral_alto` | number | 0 – 100 | Nuevo umbral alto (persistido en NVS) |

Los umbrales se validan **juntos**: el par resultante (los valores enviados, o el actual del que no se envía) tiene que cumplir `0 ≤ bajo < alto ≤ 100`; si no, se rechazan los dos. El dashboard siempre envía ambos.

Ejemplos: `{"modo":"MANUAL","bomba":"ON"}` · `{"modo":"AUTO"}` · `{"umbral_bajo":40,"umbral_alto":90}`

**Validación en el ESP32 (y en el simulador, con los mismos mensajes):**

| Entrada | Resultado |
|---|---|
| Payload de 256 bytes o más | Se descarta: `[MQTT] Comando demasiado largo` |
| Payload vacío o JSON inválido | Se descarta: `[MQTT] Error JSON` |
| JSON que no es un objeto (p. ej. `[1,2]`) | Se descarta: `Comando ignorado: el JSON no es un objeto` |
| `modo` / `bomba` con otro valor o tipo (p. ej. `"auto"`, `1`) | Se ignora ese campo: `Modo inválido` / `Bomba inválida` |
| Umbral no numérico (p. ej. `"abc"`, `true`) o par fuera de rango | Se rechazan los umbrales, sin tocar los actuales |
| Ningún campo conocido | `Comando sin campos reconocidos, ignorado` |

Nunca se convierte texto a número (no se usa `atoi()`/`toInt()`): ArduinoJson verifica el tipo con `is<float>()` / `is<const char*>()` antes de usar cada valor.

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
2. Cada 60 s el simulador provoca una falla de sensor de 15 s: aparece la alerta, el indicador "Sensor" pasa a FALLA y la telemetría trae `sensor_fault=true`.
3. Probar la API: http://localhost:8000/api/historial?limit=10 · http://localhost:8000/api/estado · http://localhost:8000/docs
4. Probar la caída del equipo (Last Will): `docker compose stop simulador` → en unos segundos `GET /api/estado` muestra `"conexion": "offline"` y el indicador "ESP32" del dashboard pasa a Offline. Volver a levantarlo: `docker compose start simulador`.
5. Probar los umbrales: en el dashboard, Configuración → cambiar los umbrales → "Enviar umbrales al ESP32". La telemetría siguiente trae los valores nuevos. Un par inválido (bajo ≥ alto) lo rechaza el dashboard y también el ESP32.
6. Detener todo: `docker compose --profile sim down`

**Sensor desconectado desde el arranque** (opcional, sin Docker o en otra terminal): `python simulator/simulador_esp32.py --sensor-desconectado` → solo se publican `estado=online` y la alerta `sensor_fault=true`; no hay telemetría.

Para usar **el ESP32 real** en lugar del simulador, levantar sin el perfil: `docker compose up -d --build`.

### Opción B — Sin Docker (Mosquitto instalado en la PC)

```bash
# 1) Broker: Mosquitto 2.x con la configuración del repo
mosquitto -c mosquitto.conf

# 2) Backend (otra terminal)
cd backend
cp .env.example .env            # Windows: copy .env.example .env
python -m venv .venv
source .venv/bin/activate       # Windows: .venv\Scripts\activate
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

**Primer arranque:** el ESP32 levanta la red Wi-Fi **`Tanque_ESP32`**. Conectarse desde el celular, elegir la red Wi-Fi del lugar, ingresar su contraseña y la **IP de la PC donde corre el broker** (campo "IP Broker MQTT", se ve con `ipconfig` / `ip a`). Queda guardado en la memoria NVS. Si nadie lo configura en 3 minutos, el portal se cierra y el equipo sigue midiendo en modo local.

Si la placa tenía cargada una versión anterior del firmware, al cargar esta el portal se abre una sola vez para volver a guardar la red, y los umbrales guardados vuelven a los valores por defecto (50 % / 95 %).

**Arranques siguientes:** se conecta solo a la red guardada **sin bloquear**: si el Wi-Fi o el broker no están, mide y controla igual en modo local y reintenta desde el `loop()`.

**Cambiar la red o la IP del broker:** con el equipo funcionando, mantener presionado el botón **BOOT** 3 segundos y vuelve a abrirse el portal `Tanque_ESP32` (mientras está abierto, la bomba se apaga y el control queda en pausa, como máximo 3 minutos).

#### Conexionado

| ESP32 | Componente | Notas |
|---|---|---|
| GPIO 12 | AJ-SR04M TRIG | |
| GPIO 14 | AJ-SR04M ECHO | A través de divisor 1 kΩ / 2 kΩ (ECHO sale a 5 V) |
| GPIO 26 | Relé IN | Activo en HIGH |
| GPIO 0 | Botón BOOT (en la placa) | Pull-up interno; 3 s presionado → portal de configuración |
| 5V (VIN) | Riel + de la protoboard → AJ-SR04M VCC · Relé VCC | El ESP32 recibe 5 V por el USB 1 |
| GND | Riel − de la protoboard → AJ-SR04M GND · Relé GND · R2 · bomba (−) · GND del USB 2 | Todas las masas unidas |

La bomba tiene su propia alimentación, el **USB 2**, y también se conecta en la protoboard:

| Desde | Hasta |
|---|---|
| USB 2 — 5 V | Relé COM |
| Relé NA (NO) | Bomba (+) |
| Bomba (−) | Riel GND de la protoboard (junto con el GND del USB 2) |
| AJ-SR04M ECHO | R1 (1 kΩ) → GPIO 14; GPIO 14 → R2 (2 kΩ) → GND |

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
- **Tanque simulado:** el nivel no sale de agua real; se simula variando la distancia del sensor (§2). Los frascos solo muestran el funcionamiento de la bomba. Para un tanque real habría que montar el sensor en la tapa y ajustar `DIST_VACIO` y `DIST_LLENO` en el firmware y en el simulador.
- **Alimentación desde la PC:** el USB 1 alimenta el ESP32, el sensor y la bobina del relé; el USB 2, solo la bomba (130–220 mA en régimen). Los dos quedan por debajo de los 500 mA de un puerto USB 2.0, pero el arranque del motor puede superar ese valor por instantes. En una instalación real iría una fuente de 5 V dedicada (por ejemplo, de 2 A).
- **Sin diodo de protección en la bomba:** no hay diodo en antiparalelo con el motor, así que al cortar el relé el pico inductivo lo absorben los contactos. En el prototipo no causó problemas; en una versión definitiva conviene agregar un 1N4007.
- **TRIG en GPIO 12:** es un pin de arranque (*strapping*) del ESP32. Si algo lo lleva a nivel alto durante el reinicio, la placa puede no arrancar. Conviene moverlo a un GPIO libre en una próxima versión del hardware.
- **Sin compensación de temperatura:** la velocidad del sonido varía ~0,18 % por °C. El firmware usa 343 m/s (20 °C).
- **Un solo tanque:** los tópicos están fijos en `campus/tanque1`; no hay alta dinámica de dispositivos.
- **QoS 0 en telemetría y comandos:** PubSubClient solo publica con QoS 0. Si se pierde una lectura no se reenvía (llega otra a los 3 s). Un comando perdido hay que reenviarlo desde el dashboard.
- **El backend no guarda alertas ni eventos online/offline:** solo los mantiene en memoria (`/api/estado`). El histórico persistido es únicamente la telemetría (con el flag `sensor_fault`).
- **Bloqueos cortos y acotados en el firmware:** el `loop()` no usa `delay()`, pero `pulseIn()` espera el eco como máximo 26 ms por muestra, y cada intento de conexión al broker (`mqtt.connect()`, uno cada 5–60 s con backoff mientras el broker no responde) puede demorar unos segundos por el timeout TCP de la librería.
- **Portal de configuración:** solo se abre la primera vez o a pedido (BOOT 3 s). Mientras está abierto (máximo 3 minutos) el control queda en pausa con la bomba apagada.
- **Fechas:** SQLite guarda `fecha_hora` en UTC; la API la devuelve en ISO-8601 con `Z` y el dashboard la muestra en hora local.
- **CORS abierto (`*`) en el backend:** pensado para el laboratorio, no para exponerlo a internet.
- **Dashboard depende de internet para Chart.js y las fuentes** (CDN con versión fija); el cliente MQTT se sirve localmente.
