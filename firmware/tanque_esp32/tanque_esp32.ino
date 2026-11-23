#include <Arduino.h>
/*
 * ============================================================================
 *  SISTEMA DE TELEMETRÍA Y CONTROL DE NIVEL — TANQUE DE AGUA
 *  Firmware ESP32 WROOM-32D — v2.1
 * ============================================================================
 *  Hardware conectado:
 *    - Sensor ultrasónico AJ-SR04M   (TRIG=GPIO12, ECHO=GPIO14)
 *      ⚠ Si el sensor se alimenta a 5V, el ECHO sale a ~5V: usar un
 *        divisor resistivo (ej. 1kΩ/2kΩ) antes de GPIO14. El ESP32
 *        NO tolera 5V en sus entradas.
 *    - Módulo relé 5V optoacoplado  (IN=GPIO26, activo en HIGH): su
 *      contacto NA conmuta una mini bomba sumergible de 5 V (130–220 mA).
 *    - Alimentación: el ESP32 por un puerto USB de la PC, y desde su pin
 *      5V la protoboard (sensor y relé); la bomba por otro puerto USB a
 *      través del relé. Masas unidas en la protoboard.
 *    - Botón BOOT de la placa (GPIO0): mantenerlo 3 s con el equipo
 *      funcionando abre el portal de configuración Wi-Fi/broker.
 *
 *  Prototipo: tanque "virtual" de 100 cm útiles (como un tanque real de
 *  ~1 m de alto). El sensor se monta fuera de los frascos, apuntando a una
 *  superficie, y el nivel se simula cambiando su distancia:
 *      120 cm = vacío (0 %)   ·  70 cm = 50 % → la bomba se APAGA
 *       25 cm = 95 % → la bomba se ENCIENDE  ·  20 cm = lleno (100 %)
 *  (20 cm es el límite por la zona ciega del AJ-SR04M: las lecturas por
 *  debajo se descartan como inválidas). Nivel % = 120 − d.
 *  La bomba, sumergida en un frasco, pasa agua a otro frasco por una
 *  manguera mientras está encendida.
 *
 *  Dependencias (versiones exactas en firmware/platformio.ini):
 *    - PubSubClient        (Nick O'Leary, v2.8)
 *    - ArduinoJson         (Benoît Blanchon, v7.2.1)
 *    - WiFiManager         (tzapu, v2.0.17)
 *    - Preferences         (incluida en el core de ESP32)
 *
 *  Credenciales Wi-Fi: NO están en el código. Se cargan con el portal
 *  cautivo de WiFiManager (AP "Tanque_ESP32") y quedan guardadas en NVS.
 *
 *  Contrato MQTT: ver README.md, sección 6. El simulador
 *  (simulator/simulador_esp32.py) implementa exactamente el mismo.
 *
 *  Diseño del loop (sin delay(); la única espera es pulseIn(), máx. 26 ms):
 *    - Medición: 1 muestra cada 150 ms (millis); cada 7 muestras se
 *      calcula la mediana, el nivel y se ejecuta el control automático.
 *    - Wi-Fi y MQTT: reconexión por estados con millis(); MQTT con
 *      backoff exponencial de 5 s a 60 s.
 *    - El control local (sensor + bomba) sigue funcionando sin red.
 *
 *  Cambios v2.1 respecto a v2.0:
 *    - Medición no bloqueante (se eliminó la espera de ~1 s por ciclo).
 *    - Arranque sin bloqueo: con una red ya configurada no se abre el
 *      portal ni se reinicia si el Wi-Fi no está; se arranca en modo local
 *      y se reintenta desde el loop. El portal solo se abre la primera vez
 *      o a pedido con el botón BOOT.
 *    - No se publica telemetría hasta tener una lectura válida del sensor
 *      (antes, al arrancar sin sensor se publicaba 100 % de nivel).
 *    - Telemetría armada con snprintf() en un buffer fijo (sin heap).
 *    - Umbrales recibidos juntos se validan como par (bajo < alto).
 *    - Se registra por Serial todo comando o valor rechazado.
 *    - Al (re)conectar al broker se reenvía la alerta si el sensor sigue
 *      en falla.
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFiManager.h>

// ─── CONFIGURACIÓN MQTT ─────────────────────────────────────────────────────
char        mqtt_broker[40] = "192.168.0.3";   // Valor por defecto; se lee desde NVS
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "esp32_tanque1";
const char* MQTT_USER     = "";                 // Dejar vacío si no requiere auth
const char* MQTT_PASS     = "";

const char* TOPIC_TELEMETRIA = "campus/tanque1/telemetria";
const char* TOPIC_COMANDO    = "campus/tanque1/comando";
const char* TOPIC_ESTADO     = "campus/tanque1/estado";   // online/offline (retenido)
const char* TOPIC_ALERTA     = "campus/tanque1/alerta";   // alertas instantáneas

const char* ALERTA_FALLA_ON  = "{\"tipo\":\"sensor_fault\",\"estado\":true}";
const char* ALERTA_FALLA_OFF = "{\"tipo\":\"sensor_fault\",\"estado\":false}";

// ─── PINES DE HARDWARE ─────────────────────────────────────────────────────
#define PIN_TRIG    12
#define PIN_ECHO    14
#define PIN_RELAY   26
#define PIN_BOOT     0   // botón BOOT de la placa (a GND al presionar)

// ─── PARÁMETROS DEL TANQUE ──────────────────────────────────────────────────
const float DIST_VACIO   = 120.0;  // cm — distancia sensor → fondo (tanque vacío, 0 %)
const float DIST_LLENO   =  20.0;  // cm — distancia sensor → nivel 100 % (límite de zona ciega)
const float RANGO_UTIL   = DIST_VACIO - DIST_LLENO;  // 100 cm útiles → nivel % = 120 − distancia

// ─── HISTÉRESIS DEL AUTOMATISMO (persistente en NVS, ajustable vía MQTT) ────
const float UMBRAL_BAJO_DEFECTO = 50.0;   // % → 70 cm: apaga bomba (fin vaciado)
const float UMBRAL_ALTO_DEFECTO = 95.0;   // % → 25 cm: enciende bomba (inicio vaciado)
const int   VERSION_CONFIG      = 2;      // subir si cambian los valores por defecto
float UMBRAL_BAJO  = UMBRAL_BAJO_DEFECTO;
float UMBRAL_ALTO  = UMBRAL_ALTO_DEFECTO;

// ─── MEDICIÓN (MEDIANA DE N MUESTRAS, NO BLOQUEANTE) ───────────────────────
#define NUM_MUESTRAS             7
#define INTERVALO_MUESTRA_MS     150
#define TIMEOUT_ECO_US           26000UL  // ~4,4 m ida y vuelta: acota pulseIn()
#define DIST_MIN_VALIDA_CM       20.0     // zona ciega del AJ-SR04M: debajo, la lectura no es confiable
#define DIST_MAX_VALIDA_CM       400.0

// ─── DETECCIÓN DE FALLAS DEL SENSOR ─────────────────────────────────────────
#define MAX_FALLOS_CONSECUTIVOS  5   // ciclos de medición seguidos sin ningún eco válido

// ─── INTERVALOS DE TEMPORIZACIÓN ────────────────────────────────────────────
const unsigned long INTERVALO_TELEMETRIA_MS     = 3000;   // publicar cada 3 s
const unsigned long INTERVALO_RECONEXION_MS     = 5000;   // 1er reintento MQTT
const unsigned long MAX_INTERVALO_RECONEXION_MS = 60000;  // tope del backoff MQTT
const unsigned long WIFI_TIMEOUT_MS             = 20000;  // espera por intento Wi-Fi
const unsigned long PULSACION_PORTAL_MS         = 3000;   // BOOT presionado → portal
const unsigned long PORTAL_TIMEOUT_S            = 180;    // portal abierto como máximo

// ─── OBJETOS GLOBALES ───────────────────────────────────────────────────────
WiFiClient          espClient;
PubSubClient        mqtt(espClient);
Preferences         prefs;

// ─── VARIABLES DE ESTADO ────────────────────────────────────────────────────
float   distancia_cm  = 0.0;
float   cota_cm       = 0.0;
float   porcentaje    = 0.0;
bool    bomba_on      = false;
bool    modo_auto     = true;      // true = AUTO, false = MANUAL
bool    sensor_fault  = false;     // true si la lectura no es confiable
bool    sensor_fault_anterior = false;
bool    hay_lectura_valida    = false;  // false hasta la 1ra medición válida
int     lecturas_fallidas_consecutivas = 0;

// Medición no bloqueante
float         muestras[NUM_MUESTRAS];
int           muestras_tomadas = 0;
int           muestras_validas = 0;
unsigned long ultima_muestra   = 0;

// Conectividad
unsigned long ultimo_telemetria = 0;
unsigned long ultimo_reconexion = 0;
unsigned long intervalo_reconexion_actual = INTERVALO_RECONEXION_MS;
bool          wifi_reconectando = false;
unsigned long wifi_inicio_reconexion = 0;

// Botón BOOT
bool          boot_presionado = false;
unsigned long boot_presionado_desde = 0;

// ═══════════════════════════════════════════════════════════════════════════
//  MEDICIÓN
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Lee una sola distancia del AJ-SR04M (en cm).
 * Devuelve -1.0 si no hay eco o la lectura está fuera de rango.
 * pulseIn() bloquea como máximo TIMEOUT_ECO_US (26 ms).
 */
float leerDistanciaRaw() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(15);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracion = pulseIn(PIN_ECHO, HIGH, TIMEOUT_ECO_US);
  if (duracion == 0) {
    Serial.println("[SENSOR] Error: Timeout (sin eco)");
    return -1.0;
  }

  float dist = (duracion * 0.0343) / 2.0;
  if (dist < DIST_MIN_VALIDA_CM || dist > DIST_MAX_VALIDA_CM) {
    // Debajo de 20 cm (zona ciega) o por encima de 400 cm la lectura no es
    // confiable. Si se repite seguido, revisar la posición del sensor, el
    // puente R19 (modo 1) y la alimentación/cableado del ECHO.
    Serial.printf("[SENSOR] Error: Fuera de rango (%.1f cm)\n", dist);
    return -1.0;
  }
  return dist;
}

/** Ordenamiento burbuja para array de floats (usado por la mediana). */
void ordenarArray(float arr[], int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (arr[j] > arr[j + 1]) {
        float tmp  = arr[j];
        arr[j]     = arr[j + 1];
        arr[j + 1] = tmp;
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  BOMBA Y CONTROL AUTOMÁTICO
// ═══════════════════════════════════════════════════════════════════════════

void activarBomba() {
  bomba_on = true;
  digitalWrite(PIN_RELAY, HIGH);  // módulo relé activo en HIGH
  Serial.println("[BOMBA] Activada (relé ON - pin HIGH)");
}

void desactivarBomba() {
  bomba_on = false;
  digitalWrite(PIN_RELAY, LOW);
  Serial.println("[BOMBA] Desactivada (relé OFF - pin LOW)");
}

/**
 * Histéresis de vaciado. Solo actúa en modo AUTO y con lectura confiable;
 * si el sensor falla, apaga la bomba por seguridad.
 */
void controlAutomatico() {
  if (!modo_auto) return;

  if (sensor_fault) {
    if (bomba_on) {
      desactivarBomba();
      Serial.println("[AUTO] Falla de sensor → bomba APAGADA por seguridad");
    }
    return;
  }

  if (porcentaje >= UMBRAL_ALTO && !bomba_on) {
    activarBomba();
    Serial.println("[AUTO] Nivel alto → bomba ENCENDIDA (vaciando)");
  } else if (porcentaje <= UMBRAL_BAJO && bomba_on) {
    desactivarBomba();
    Serial.println("[AUTO] Nivel bajo → bomba APAGADA (fin vaciado)");
  }
}

/** Publica la alerta de falla/recuperación del sensor (si hay conexión). */
void publicarAlertaSensor(bool en_falla) {
  bool enviada = mqtt.connected() &&
                 mqtt.publish(TOPIC_ALERTA, en_falla ? ALERTA_FALLA_ON : ALERTA_FALLA_OFF, false);
  Serial.printf("[ALERTA] %s — %s\n",
                en_falla ? "Sensor en falla" : "Sensor recuperado",
                enviada ? "alerta publicada" : "sin conexión MQTT, no se pudo publicar");
}

/**
 * Se ejecuta al completar cada ciclo de NUM_MUESTRAS muestras.
 * sensor_fault = true si todavía no hubo ninguna lectura válida desde el
 * arranque, o tras MAX_FALLOS_CONSECUTIVOS ciclos seguidos sin eco válido.
 * Mientras dura la falla se conserva la última lectura válida.
 */
void actualizarNivel() {
  sensor_fault = !hay_lectura_valida ||
                 (lecturas_fallidas_consecutivas >= MAX_FALLOS_CONSECUTIVOS);

  if (sensor_fault != sensor_fault_anterior) {
    publicarAlertaSensor(sensor_fault);
    sensor_fault_anterior = sensor_fault;
  }

  if (!hay_lectura_valida) return;   // nada que calcular todavía

  cota_cm = DIST_VACIO - distancia_cm;
  if (cota_cm < 0)          cota_cm = 0;
  if (cota_cm > RANGO_UTIL) cota_cm = RANGO_UTIL;

  porcentaje = (cota_cm / RANGO_UTIL) * 100.0;
  if (porcentaje < 0)   porcentaje = 0;
  if (porcentaje > 100) porcentaje = 100;
}

/**
 * Máquina de estados de medición: toma 1 muestra cada INTERVALO_MUESTRA_MS.
 * Al juntar NUM_MUESTRAS aplica la mediana de las válidas, actualiza el
 * nivel y ejecuta el control automático. No usa delay().
 */
void tareaMedicion(unsigned long ahora) {
  if (ahora - ultima_muestra < INTERVALO_MUESTRA_MS) return;
  ultima_muestra = ahora;

  float d = leerDistanciaRaw();
  if (d > 0) {
    muestras[muestras_validas++] = d;
  }
  muestras_tomadas++;
  if (muestras_tomadas < NUM_MUESTRAS) return;

  // Ciclo completo
  if (muestras_validas == 0) {
    lecturas_fallidas_consecutivas++;
  } else {
    lecturas_fallidas_consecutivas = 0;
    ordenarArray(muestras, muestras_validas);
    distancia_cm = muestras[muestras_validas / 2];   // mediana
    hay_lectura_valida = true;
  }
  muestras_tomadas = 0;
  muestras_validas = 0;

  actualizarNivel();
  controlAutomatico();
}

// ═══════════════════════════════════════════════════════════════════════════
//  CONFIGURACIÓN Wi-Fi (portal cautivo) Y BOTÓN BOOT
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Abre el portal de WiFiManager (AP "Tanque_ESP32") como máximo
 * PORTAL_TIMEOUT_S segundos. Solo se usa la primera vez (sin red guardada)
 * o a pedido del usuario con el botón BOOT. Mientras está abierto el
 * control queda en pausa, por eso antes se apaga la bomba.
 */
void abrirPortalConfiguracion() {
  if (bomba_on) desactivarBomba();

  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(20);
  WiFiManagerParameter param_broker("broker", "IP Broker MQTT", mqtt_broker, sizeof(mqtt_broker) - 1);
  wm.addParameter(&param_broker);

  Serial.printf("[WiFi] Portal de configuración abierto (AP \"Tanque_ESP32\", máx. %lu s)\n", PORTAL_TIMEOUT_S);
  if (wm.startConfigPortal("Tanque_ESP32")) {
    prefs.putBool("wifi_ok", true);
    const char* nuevo = param_broker.getValue();
    if (strlen(nuevo) > 0 && strcmp(nuevo, mqtt_broker) != 0) {
      strncpy(mqtt_broker, nuevo, sizeof(mqtt_broker) - 1);
      mqtt_broker[sizeof(mqtt_broker) - 1] = '\0';
      prefs.putString("mqtt_broker", mqtt_broker);
      mqtt.setServer(mqtt_broker, MQTT_PORT);
      mqtt.disconnect();   // reconectar contra el broker nuevo
      Serial.printf("[NVS] Nuevo broker MQTT guardado: %s\n", mqtt_broker);
    }
    Serial.print("[WiFi] Configurado y conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Portal cerrado sin configurar: se sigue en modo local");
  }
  WiFi.mode(WIFI_STA);
  wifi_reconectando = false;   // conectarWiFi() retoma en el próximo loop
}

/** BOOT presionado PULSACION_PORTAL_MS seguidos → abre el portal. */
void revisarBotonPortal(unsigned long ahora) {
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!boot_presionado) {
      boot_presionado = true;
      boot_presionado_desde = ahora;
    } else if (ahora - boot_presionado_desde >= PULSACION_PORTAL_MS) {
      boot_presionado = false;
      Serial.println("[BOOT] Botón mantenido: abriendo portal de configuración");
      abrirPortalConfiguracion();
    }
  } else {
    boot_presionado = false;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CONECTIVIDAD Wi-Fi (no bloqueante)
// ═══════════════════════════════════════════════════════════════════════════

void conectarWiFi(unsigned long ahora) {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifi_reconectando) {
      Serial.print("[WiFi] Conectado — IP: ");
      Serial.println(WiFi.localIP());
      wifi_reconectando = false;
    }
    return;
  }

  if (wifi_reconectando) {
    if (ahora - wifi_inicio_reconexion < WIFI_TIMEOUT_MS) return;   // seguir esperando
    Serial.println("[WiFi] Timeout de conexión, se reintenta");
  } else {
    Serial.println("[WiFi] Sin conexión. Intentando conectar a la red guardada...");
  }
  wifi_reconectando = true;
  wifi_inicio_reconexion = ahora;
  WiFi.begin();   // usa las credenciales guardadas en NVS; no bloquea
}

// ═══════════════════════════════════════════════════════════════════════════
//  MQTT — Callback de comandos
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Comandos JSON en campus/tanque1/comando (todos los campos opcionales):
 *   {"modo":"AUTO"|"MANUAL", "bomba":"ON"|"OFF",
 *    "umbral_bajo":0..100, "umbral_alto":0..100}
 * Todo valor inválido se rechaza explícitamente y se registra por Serial;
 * nunca se convierte texto a número (no se usa atoi()/toInt()).
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char buffer[256];
  if (length >= sizeof(buffer)) {
    Serial.printf("[MQTT] Comando demasiado largo (%u bytes), ignorado\n", length);
    return;
  }
  memcpy(buffer, payload, length);
  buffer[length] = '\0';

  Serial.print("[MQTT RX] ");
  Serial.print(topic);
  Serial.print(" → ");
  Serial.println(buffer);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, buffer, length);
  if (err) {
    Serial.print("[MQTT] Error JSON: ");
    Serial.println(err.c_str());
    return;
  }
  if (!doc.is<JsonObject>()) {
    Serial.println("[MQTT] Comando ignorado: el JSON no es un objeto");
    return;
  }

  bool reconocido = false;

  // ── Modo ──
  if (!doc["modo"].isNull()) {
    reconocido = true;
    const char* m = doc["modo"].is<const char*>() ? doc["modo"].as<const char*>() : nullptr;
    if (m && strcmp(m, "AUTO") == 0) {
      modo_auto = true;
      Serial.println("[CMD] Modo → AUTO");
    } else if (m && strcmp(m, "MANUAL") == 0) {
      modo_auto = false;
      Serial.println("[CMD] Modo → MANUAL");
    } else {
      Serial.println("[CMD] Modo inválido (se espera \"AUTO\" o \"MANUAL\"), ignorado");
    }
  }

  // ── Bomba (solo efectiva en MANUAL) ──
  if (!doc["bomba"].isNull()) {
    reconocido = true;
    const char* b = doc["bomba"].is<const char*>() ? doc["bomba"].as<const char*>() : nullptr;
    bool on  = b && strcmp(b, "ON") == 0;
    bool off = b && strcmp(b, "OFF") == 0;
    if (!on && !off) {
      Serial.println("[CMD] Bomba inválida (se espera \"ON\" u \"OFF\"), ignorado");
    } else if (modo_auto) {
      Serial.println("[CMD] Comando de bomba ignorado (modo AUTO)");
    } else if (on) {
      activarBomba();
      Serial.println("[CMD] Bomba → ON (manual)");
    } else {
      desactivarBomba();
      Serial.println("[CMD] Bomba → OFF (manual)");
    }
  }

  // ── Umbrales: se validan juntos (rango 0..100 y bajo < alto) ──
  bool trae_bajo = !doc["umbral_bajo"].isNull();
  bool trae_alto = !doc["umbral_alto"].isNull();
  if (trae_bajo || trae_alto) {
    reconocido = true;
    bool tipos_ok = (!trae_bajo || doc["umbral_bajo"].is<float>()) &&
                    (!trae_alto || doc["umbral_alto"].is<float>());
    if (!tipos_ok) {
      Serial.println("[CMD] Umbrales rechazados: deben ser numéricos");
    } else {
      float nuevo_bajo = trae_bajo ? doc["umbral_bajo"].as<float>() : UMBRAL_BAJO;
      float nuevo_alto = trae_alto ? doc["umbral_alto"].as<float>() : UMBRAL_ALTO;
      bool en_rango = nuevo_bajo >= 0 && nuevo_bajo <= 100 && nuevo_alto >= 0 && nuevo_alto <= 100;
      if (!en_rango || nuevo_bajo >= nuevo_alto) {
        Serial.printf("[CMD] Umbrales rechazados (bajo=%.1f, alto=%.1f): deben estar entre 0 y 100 y bajo < alto\n",
                      nuevo_bajo, nuevo_alto);
      } else {
        UMBRAL_BAJO = nuevo_bajo;
        UMBRAL_ALTO = nuevo_alto;
        prefs.putFloat("umbral_bajo", UMBRAL_BAJO);   // persistentes en NVS
        prefs.putFloat("umbral_alto", UMBRAL_ALTO);
        Serial.printf("[CMD] Umbrales → bajo=%.1f%% alto=%.1f%%\n", UMBRAL_BAJO, UMBRAL_ALTO);
      }
    }
  }

  if (!reconocido) {
    Serial.println("[CMD] Comando sin campos reconocidos, ignorado");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  MQTT — Conexión (no bloqueante, con backoff)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Intenta conectar al broker como máximo una vez por intervalo, con
 * backoff exponencial (5 s → 60 s). Configura Last Will para que el broker
 * publique "offline" retenido si el ESP32 se cae, y publica "online"
 * retenido al conectar.
 */
void conectarMQTT(unsigned long ahora) {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (ahora - ultimo_reconexion < intervalo_reconexion_actual) return;
  ultimo_reconexion = ahora;

  Serial.printf("[MQTT] Conectando a %s:%d\n", mqtt_broker, MQTT_PORT);

  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS, TOPIC_ESTADO, 1, true, "offline");
  } else {
    ok = mqtt.connect(MQTT_CLIENT, TOPIC_ESTADO, 1, true, "offline");
  }

  if (ok) {
    Serial.println("[MQTT] Conectado ✓");
    intervalo_reconexion_actual = INTERVALO_RECONEXION_MS;   // reset backoff
    mqtt.publish(TOPIC_ESTADO, "online", true);              // retenido
    mqtt.subscribe(TOPIC_COMANDO);
    Serial.printf("[MQTT] Suscrito a %s\n", TOPIC_COMANDO);
    if (sensor_fault) publicarAlertaSensor(true);            // la alerta pudo perderse sin red
  } else {
    intervalo_reconexion_actual = min(intervalo_reconexion_actual * 2, MAX_INTERVALO_RECONEXION_MS);
    Serial.printf("[MQTT] Fallo, rc=%d — próximo reintento en %lu ms\n", mqtt.state(), intervalo_reconexion_actual);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLICACIÓN DE TELEMETRÍA (buffer estático + snprintf)
// ═══════════════════════════════════════════════════════════════════════════

// Mismo formato exacto que usa el simulador (FORMATO_TELEMETRIA).
#define FORMATO_TELEMETRIA "{\"porcentaje\":%.1f,\"cota_cm\":%.1f,\"distancia_cm\":%.1f," \
                           "\"bomba\":%s,\"modo\":\"%s\",\"sensor_fault\":%s," \
                           "\"umbral_bajo\":%.1f,\"umbral_alto\":%.1f}"

void publicarTelemetria() {
  if (!mqtt.connected()) return;
  if (!hay_lectura_valida) {
    Serial.println("[MQTT] Telemetría no publicada: todavía no hay ninguna lectura válida del sensor");
    return;
  }

  char buffer[256];
  int n = snprintf(buffer, sizeof(buffer), FORMATO_TELEMETRIA,
                   porcentaje, cota_cm, distancia_cm,
                   bomba_on ? "true" : "false",
                   modo_auto ? "AUTO" : "MANUAL",
                   sensor_fault ? "true" : "false",
                   UMBRAL_BAJO, UMBRAL_ALTO);
  if (n < 0 || n >= (int)sizeof(buffer)) {
    Serial.println("[MQTT] Error: telemetría no entra en el buffer");
    return;
  }
  if (!mqtt.publish(TOPIC_TELEMETRIA, buffer)) {
    Serial.println("[MQTT] Error al publicar telemetría");
    return;
  }
  Serial.print("[MQTT TX] ");
  Serial.println(buffer);
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  SISTEMA DE TELEMETRÍA — TANQUE v2.1");
  Serial.println("  (Sensor ultrasónico AJ-SR04M + MQTT)");
  Serial.println("========================================");

  // ─── Pines ────────────────────────────────────────────────────────────
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);   // bomba apagada al inicio
  pinMode(PIN_BOOT, INPUT_PULLUP);

  // ─── Configuración persistente (NVS) ──────────────────────────────────
  prefs.begin("tanque", false);
  // Si la NVS tiene umbrales de una versión anterior (escala vieja), se
  // reemplazan por los valores por defecto de esta versión.
  if (prefs.getInt("cfg_ver", 0) != VERSION_CONFIG) {
    prefs.putFloat("umbral_bajo", UMBRAL_BAJO_DEFECTO);
    prefs.putFloat("umbral_alto", UMBRAL_ALTO_DEFECTO);
    prefs.putInt("cfg_ver", VERSION_CONFIG);
    Serial.println("[NVS] Umbrales reiniciados a los valores por defecto de esta versión");
  }
  UMBRAL_BAJO = prefs.getFloat("umbral_bajo", UMBRAL_BAJO_DEFECTO);
  UMBRAL_ALTO = prefs.getFloat("umbral_alto", UMBRAL_ALTO_DEFECTO);
  if (UMBRAL_BAJO < 0 || UMBRAL_ALTO > 100 || UMBRAL_BAJO >= UMBRAL_ALTO) {   // NVS corrupta
    UMBRAL_BAJO = UMBRAL_BAJO_DEFECTO;
    UMBRAL_ALTO = UMBRAL_ALTO_DEFECTO;
  }
  Serial.printf("[NVS] Umbrales: bajo=%.1f%% alto=%.1f%%\n", UMBRAL_BAJO, UMBRAL_ALTO);

  prefs.getString("mqtt_broker", mqtt_broker, sizeof(mqtt_broker));   // si no existe, queda el default
  mqtt_broker[sizeof(mqtt_broker) - 1] = '\0';
  Serial.printf("[NVS] Broker MQTT: %s\n", mqtt_broker);

  // ─── MQTT ─────────────────────────────────────────────────────────────
  mqtt.setServer(mqtt_broker, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);

  // ─── Wi-Fi ────────────────────────────────────────────────────────────
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // menos latencia y reconexiones fantasma
  if (prefs.getBool("wifi_ok", false)) {
    // Red ya configurada: NO se bloquea. Si el Wi-Fi no está, el equipo
    // arranca en modo local y conectarWiFi() reintenta desde el loop.
    Serial.println("[WiFi] Conectando a la red guardada (no bloqueante)...");
    WiFi.begin();
    wifi_reconectando = true;
    wifi_inicio_reconexion = millis();
  } else {
    // Primera vez: hace falta cargar la red y la IP del broker.
    abrirPortalConfiguracion();
  }
}

void loop() {
  unsigned long ahora = millis();

  // Conectividad (no bloqueante)
  conectarWiFi(ahora);
  conectarMQTT(ahora);
  mqtt.loop();

  // Medición + control automático (cada ciclo de 7 muestras)
  tareaMedicion(ahora);

  // Portal de configuración a pedido (BOOT 3 s)
  revisarBotonPortal(ahora);

  // Telemetría cada INTERVALO_TELEMETRIA_MS
  if (ahora - ultimo_telemetria >= INTERVALO_TELEMETRIA_MS) {
    ultimo_telemetria = ahora;
    publicarTelemetria();
    Serial.printf("[SENSOR] Dist: %.1f cm | Cota: %.1f cm | Nivel: %.1f%% | Fault: %s\n",
                  distancia_cm, cota_cm, porcentaje, sensor_fault ? "SI" : "no");
  }
}
