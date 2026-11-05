#include <Arduino.h> 
/*
 * ============================================================================
 *  SISTEMA DE TELEMETRÍA Y CONTROL DE NIVEL — TANQUE DE AGUA
 *  Firmware ESP32 WROOM-32D — v2.0
 * ============================================================================
 *  Hardware conectado:
 *    - Sensor ultrasónico AJ-SR04M   (TRIG=GPIO12, ECHO=GPIO14)
 *      ⚠ Si el sensor se alimenta a 5V, el ECHO sale a ~5V: usar un
 *        divisor resistivo (ej. 1kΩ/2kΩ) antes de GPIO14. El ESP32
 *        NO tolera 5V en sus entradas.
 *
 *  Hardware pendiente (lógica lista, módulo aún no conectado):
 *    - Relé 5V                       (IN=GPIO26, activo en HIGH)
 *
 *  Dependencias (instalar desde Library Manager):
 *    - PubSubClient        (Nick O'Leary)
 *    - ArduinoJson         (Benoît Blanchon, v7+)
 *    - WiFiManager         (tzapu, v2.0.17)
 *    - Preferences          (incluida en el core de ESP32)
 *  Versiones exactas: ver firmware/platformio.ini
 *
 *  Credenciales Wi-Fi: NO están en el código. Se cargan con el portal
 *  cautivo de WiFiManager (AP "Tanque_ESP32") y quedan guardadas en NVS.
 *
 *  Cambios v2.0 respecto a v1.0:
 *    - Corregido el comentario de pines: no coincidía con los #define reales
 *      (decía GPIO5/18, el código usaba GPIO12/14)
 *    - Umbrales de histéresis persistentes en NVS (Preferences) → sobreviven
 *      a un corte de luz o reinicio
 *    - Validación cruzada de umbrales al recibirlos por MQTT (bajo < alto),
 *      antes se podían setear al revés y el automatismo quedaba trabado
 *    - Detección de fallas consecutivas del sensor: si el eco falla varias
 *      veces seguidas, sensor_fault=true y el automatismo NO actúa a ciegas
 *    - Last Will Testament de MQTT: publica "offline"/"online" (retenido) en
 *      campus/tanque1/estado si el ESP32 se cuelga o pierde la conexión
 *    - Wi-Fi: limpieza de estado previo + WiFi.setSleep(false) para reducir
 *      reconexiones fantasma y latencia de reconexión
 *    - mqtt.loop() se sigue llamando durante el muestreo del sensor (antes
 *      quedaba ~420ms por ciclo sin atender MQTT, podía demorar comandos)
 *
 *  Compilar con: Arduino IDE (Board: ESP32 Dev Module) o PlatformIO
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFiManager.h>

// ─── CONFIGURACIÓN MQTT ─────────────────────────────────────────────────────
char        mqtt_broker[40] = "192.168.0.3";   // Valor por defecto, se lee desde NVS
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "esp32_tanque1";
const char* MQTT_USER     = "";                 // Dejar vacío si no requiere auth
const char* MQTT_PASS     = "";

const char* TOPIC_TELEMETRIA = "campus/tanque1/telemetria";
const char* TOPIC_COMANDO    = "campus/tanque1/comando";
const char* TOPIC_ESTADO     = "campus/tanque1/estado";   // online/offline (retenido)
const char* TOPIC_ALERTA     = "campus/tanque1/alerta";   // alertas instantáneas

// ─── PINES DE HARDWARE ─────────────────────────────────────────────────────
#define PIN_TRIG    12
#define PIN_ECHO    14
#define PIN_RELAY   26

// ─── PARÁMETROS DEL TANQUE ──────────────────────────────────────────────────
const float DIST_VACIO   = 200.0;  // cm — distancia sensor → fondo (tanque vacío)
const float DIST_LLENO   =  25.0;  // cm — distancia sensor → nivel 100% (zona ciega)
const float RANGO_UTIL   = DIST_VACIO - DIST_LLENO;  // 175 cm útiles

// ─── HISTÉRESIS DEL AUTOMATISMO (persistente en NVS, ajustable vía MQTT) ────
float UMBRAL_BAJO  = 50.0;   // % → apaga bomba (fin vaciado)
float UMBRAL_ALTO  = 85.0;   // % → enciende bomba (inicio vaciado)

// ─── FILTRADO (MEDIANA DE N MUESTRAS) ───────────────────────────────────────
#define NUM_MUESTRAS  7
#define DELAY_ENTRE_MUESTRAS_MS  150

// ─── DETECCIÓN DE FALLAS DEL SENSOR ─────────────────────────────────────────
#define MAX_FALLOS_CONSECUTIVOS  5   // ciclos de medición sin ningún eco válido

// ─── INTERVALOS DE TEMPORIZACIÓN ────────────────────────────────────────────
const unsigned long INTERVALO_TELEMETRIA_MS = 3000;  // publicar cada 3 s
const unsigned long INTERVALO_RECONEXION_MS = 5000;  // reintentar conexión cada 5 s

// ─── OBJETOS GLOBALES ───────────────────────────────────────────────────────
WiFiClient          espClient;
PubSubClient        mqtt(espClient);
Preferences         prefs;

// ─── VARIABLES DE ESTADO ────────────────────────────────────────────────────
float   distancia_cm  = 0.0;
float   cota_cm       = 0.0;
float   porcentaje    = 0.0;
bool    bomba_on      = false;     // Sin relé instalado todavía, sin efecto físico
bool    modo_auto     = true;      // true = AUTO, false = MANUAL
bool    sensor_fault  = false;     // true si hay fallas consecutivas de lectura
bool    sensor_fault_anterior = false;  // para detectar transiciones

int     lecturas_fallidas_consecutivas = 0;

unsigned long ultimo_telemetria = 0;
unsigned long ultimo_reconexion = 0;
unsigned long intervalo_reconexion_actual = INTERVALO_RECONEXION_MS;
const unsigned long MAX_INTERVALO_RECONEXION_MS = 60000;  // tope: 1 minuto

bool          wifi_reconectando = false;
unsigned long wifi_inicio_reconexion = 0;
const unsigned long WIFI_TIMEOUT_MS = 20000;  // 20 s máximo esperando WiFi

// ═══════════════════════════════════════════════════════════════════════════
//  UTILIDAD: ESPERA QUE NO BLOQUEA MQTT
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Reemplaza a delay() en los tramos largos (muestreo del sensor) para que
 * mqtt.loop() se siga llamando y no se acumule latencia en la recepción
 * de comandos ni en el keepalive de la conexión.
 */
void esperarConMqtt(unsigned long ms) {
  unsigned long inicio = millis();
  while (millis() - inicio < ms) {
    mqtt.loop();
    delay(5);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  FUNCIONES DE MEDICIÓN
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Lee una sola distancia del AJ-SR04M (en cm).
 * Devuelve -1.0 si hay timeout o lectura fuera de rango.
 */
float leerDistanciaRaw() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(15);
  digitalWrite(PIN_TRIG, LOW);

  long duracion = pulseIn(PIN_ECHO, HIGH, 50000);  // timeout 50 ms (~8 m)
  if (duracion == 0) {
    Serial.println("[SENSOR] Error: Timeout (sin eco)");
    return -1.0;
  }

  float dist = (duracion * 0.0343) / 2.0;
  if (dist < 2.0 || dist > 400.0) {
    // Si esto se repite seguido, revisar el puente R19 del sensor
    // (define el modo de zona ciega) y la alimentación/cableado del ECHO.
    Serial.printf("[SENSOR] Error: Fuera de rango (%.1f cm)\n", dist);
    return -1.0;
  }
  return dist;
}

/**
 * Ordenamiento burbuja para array de floats (usado por la mediana).
 */
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

/**
 * Toma NUM_MUESTRAS lecturas, descarta inválidas, aplica mediana.
 * Devuelve distancia filtrada en cm, o la última lectura válida si falla.
 * Lleva la cuenta de fallas consecutivas para detectar sensor_fault.
 */
float medirDistanciaFiltrada() {
  float muestras[NUM_MUESTRAS];
  int   validas = 0;

  for (int i = 0; i < NUM_MUESTRAS; i++) {
    float d = leerDistanciaRaw();
    if (d > 0) {
      muestras[validas++] = d;
    }
    esperarConMqtt(DELAY_ENTRE_MUESTRAS_MS);
  }

  if (validas == 0) {
    lecturas_fallidas_consecutivas++;
    return distancia_cm;  // mantener última lectura válida
  }

  lecturas_fallidas_consecutivas = 0;
  ordenarArray(muestras, validas);
  return muestras[validas / 2];  // mediana
}

/**
 * A partir de la distancia filtrada, calcula cota de agua, porcentaje
 * y actualiza el flag de falla del sensor.
 */
void calcularNivel() {
  distancia_cm = medirDistanciaFiltrada();
  sensor_fault = (lecturas_fallidas_consecutivas >= MAX_FALLOS_CONSECUTIVOS);

  // Alerta instantánea solo en transiciones de sensor_fault
  if (sensor_fault && !sensor_fault_anterior) {
    if (mqtt.connected()) {
      mqtt.publish(TOPIC_ALERTA, "{\"tipo\":\"sensor_fault\",\"estado\":true}", false);
    }
    Serial.println("[ALERTA] Sensor en falla — alerta publicada");
  } else if (!sensor_fault && sensor_fault_anterior) {
    if (mqtt.connected()) {
      mqtt.publish(TOPIC_ALERTA, "{\"tipo\":\"sensor_fault\",\"estado\":false}", false);
    }
    Serial.println("[ALERTA] Sensor recuperado — alerta publicada");
  }
  sensor_fault_anterior = sensor_fault;

  // Cota de agua = qué tan alto está el agua desde el fondo
  cota_cm = DIST_VACIO - distancia_cm;
  if (cota_cm < 0)          cota_cm = 0;
  if (cota_cm > RANGO_UTIL) cota_cm = RANGO_UTIL;

  // Porcentaje
  porcentaje = (cota_cm / RANGO_UTIL) * 100.0;
  if (porcentaje < 0)   porcentaje = 0;
  if (porcentaje > 100) porcentaje = 100;
}

// ═══════════════════════════════════════════════════════════════════════════
//  CONTROL DE BOMBA (preparado, sin efecto hasta conectar relé)
// ═══════════════════════════════════════════════════════════════════════════

void activarBomba() {
  bomba_on = true;
  digitalWrite(PIN_RELAY, HIGH); // Módulo relé se activa en estado ALTO
  Serial.println("[BOMBA] Activada (relé ON - pin HIGH)");
}

void desactivarBomba() {
  bomba_on = false;
  digitalWrite(PIN_RELAY, LOW); // Módulo relé se desactiva en estado BAJO
  Serial.println("[BOMBA] Desactivada (relé OFF - pin LOW)");
}

/**
 * Lógica de automatismo con histéresis.
 * Solo actúa si estamos en modo AUTO y el sensor está confiable.
 */
void controlAutomatico() {
  if (!modo_auto) return;  // Solo operar en AUTO

  if (sensor_fault) {
    // BUGFIX: Si el sensor falla (por ejemplo por ruido eléctrico al prender la bomba),
    // debemos apagar la bomba por seguridad para evitar que se desborde o funcione en seco,
    // en lugar de simplemente ignorarlo y dejarla prendida para siempre.
    if (bomba_on) {
      desactivarBomba();
      Serial.println("[AUTO] Falla de sensor → bomba APAGADA por seguridad");
    }
    return;
  }

  // LÓGICA DE VACIADO
  if (porcentaje >= UMBRAL_ALTO && !bomba_on) {
    activarBomba();
    Serial.println("[AUTO] Nivel alto → bomba ENCENDIDA (vaciando)");
  }
  else if (porcentaje <= UMBRAL_BAJO && bomba_on) {
    desactivarBomba();
    Serial.println("[AUTO] Nivel bajo → bomba APAGADA (fin vaciado)");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CONECTIVIDAD Wi-Fi
// ═══════════════════════════════════════════════════════════════════════════

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifi_reconectando) {
      Serial.print("[WiFi] Reconectado — IP: ");
      Serial.println(WiFi.localIP());
      wifi_reconectando = false;
    }
    return;
  }

  // Si ya se disparó reconexión, verificar timeout
  if (wifi_reconectando) {
    if (millis() - wifi_inicio_reconexion > WIFI_TIMEOUT_MS) {
      Serial.println("[WiFi] Timeout, se reintentará...");
      wifi_reconectando = false;
    }
    return;
  }

  // Disparar reconexión (no bloqueante)
  Serial.println("[WiFi] Conexión perdida. Intentando reconectar...");
  wifi_reconectando = true;
  wifi_inicio_reconexion = millis();
  WiFi.reconnect(); // Usa las credenciales guardadas por WiFiManager
}

// ═══════════════════════════════════════════════════════════════════════════
//  MQTT — Callback y Conexión
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Callback invocado al recibir un mensaje en los tópicos suscritos.
 * Procesa comandos JSON: {"bomba":"ON"|"OFF", "modo":"AUTO"|"MANUAL",
 *                          "umbral_bajo":N, "umbral_alto":N}
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char buffer[256];
  if (length >= sizeof(buffer)) return;
  memcpy(buffer, payload, length);
  buffer[length] = '\0';

  Serial.print("[MQTT RX] ");
  Serial.print(topic);
  Serial.print(" → ");
  Serial.println(buffer);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, buffer);
  if (err) {
    Serial.print("[MQTT] Error JSON: ");
    Serial.println(err.c_str());
    return;
  }

  // Comando de modo
  if (doc["modo"].is<const char*>()) {
    const char* m = doc["modo"].as<const char*>();
    if (m && strcmp(m, "AUTO") == 0) {
      modo_auto = true;
      Serial.println("[CMD] Modo → AUTO");
    } else if (m && strcmp(m, "MANUAL") == 0) {
      modo_auto = false;
      Serial.println("[CMD] Modo → MANUAL");
    }
  }

  // Comando de bomba (solo efectivo en MANUAL)
  if (doc["bomba"].is<const char*>()) {
    const char* b = doc["bomba"].as<const char*>();
    if (!modo_auto) {
      if (b && strcmp(b, "ON") == 0) {
        activarBomba();
        Serial.println("[CMD] Bomba → ON (manual)");
      } else if (b && strcmp(b, "OFF") == 0) {
        desactivarBomba();
        Serial.println("[CMD] Bomba → OFF (manual)");
      }
    } else {
      Serial.println("[CMD] Comando de bomba ignorado (modo AUTO)");
    }
  }

  // Umbral bajo: válido, en rango y estrictamente menor al umbral alto.
  // Se persiste en NVS para sobrevivir a un reinicio/corte de luz.
  if (doc["umbral_bajo"].is<float>()) {
    float nuevo = doc["umbral_bajo"].as<float>();
    if (nuevo >= 0 && nuevo <= 100 && nuevo < UMBRAL_ALTO) {
      UMBRAL_BAJO = nuevo;
      prefs.putFloat("umbral_bajo", UMBRAL_BAJO);
      Serial.printf("[CMD] Umbral bajo → %.1f%%\n", UMBRAL_BAJO);
    } else {
      Serial.println("[CMD] Umbral bajo rechazado (fuera de rango o >= umbral alto)");
    }
  }

  // Umbral alto: misma validación cruzada.
  if (doc["umbral_alto"].is<float>()) {
    float nuevo = doc["umbral_alto"].as<float>();
    if (nuevo >= 0 && nuevo <= 100 && nuevo > UMBRAL_BAJO) {
      UMBRAL_ALTO = nuevo;
      prefs.putFloat("umbral_alto", UMBRAL_ALTO);
      Serial.printf("[CMD] Umbral alto → %.1f%%\n", UMBRAL_ALTO);
    } else {
      Serial.println("[CMD] Umbral alto rechazado (fuera de rango o <= umbral bajo)");
    }
  }
}

/**
 * Intenta conectar al broker MQTT y suscribirse al tópico de comandos.
 * Configura Last Will Testament para publicar "offline" retenido si el
 * ESP32 se desconecta sin avisar, y publica "online" retenido al conectar.
 * No bloquea: retorna inmediatamente si falla.
 */
void conectarMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long ahora = millis();
  if (ahora - ultimo_reconexion < intervalo_reconexion_actual) return;
  ultimo_reconexion = ahora;

  Serial.print("[MQTT] Conectando a ");
  Serial.print(mqtt_broker);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS, TOPIC_ESTADO, 1, true, "offline");
  } else {
    ok = mqtt.connect(MQTT_CLIENT, TOPIC_ESTADO, 1, true, "offline");
  }

  if (ok) {
    Serial.println("[MQTT] Conectado ✓");
    intervalo_reconexion_actual = INTERVALO_RECONEXION_MS;  // reset backoff
    mqtt.publish(TOPIC_ESTADO, "online", true);  // retenido
    mqtt.subscribe(TOPIC_COMANDO);
    Serial.print("[MQTT] Suscrito a ");
    Serial.println(TOPIC_COMANDO);
  } else {
    intervalo_reconexion_actual = min(intervalo_reconexion_actual * 2, MAX_INTERVALO_RECONEXION_MS);
    Serial.printf("[MQTT] Fallo, rc=%d — próximo reintento en %lu ms\n", mqtt.state(), intervalo_reconexion_actual);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLICACIÓN DE TELEMETRÍA
// ═══════════════════════════════════════════════════════════════════════════

void publicarTelemetria() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["porcentaje"]   = round(porcentaje * 10.0) / 10.0;
  doc["cota_cm"]      = round(cota_cm * 10.0) / 10.0;
  doc["distancia_cm"] = round(distancia_cm * 10.0) / 10.0;
  doc["bomba"]        = bomba_on;
  doc["modo"]         = modo_auto ? "AUTO" : "MANUAL";
  doc["sensor_fault"] = sensor_fault;
  doc["umbral_bajo"]  = UMBRAL_BAJO;
  doc["umbral_alto"]  = UMBRAL_ALTO;

  char buffer[256];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  if (n == 0 || n >= sizeof(buffer)) {
    Serial.println("[MQTT] Error: telemetría no entra en el buffer");
    return;
  }

  mqtt.publish(TOPIC_TELEMETRIA, buffer);

  Serial.print("[MQTT TX] ");
  Serial.println(buffer);
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  SISTEMA DE TELEMETRÍA — TANQUE v2.0");
  Serial.println("  (Sensor ultrasónico AJ-SR04M + MQTT)");
  Serial.println("========================================");

  // ─── Pines del sensor ─────────────────────────────────────────────────
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  // ─── Pines de actuadores ──────────────────────────────────────────────
  pinMode(PIN_RELAY,  OUTPUT);
  digitalWrite(PIN_RELAY, LOW);  // Bomba apagada al inicio

  // ─── Umbrales persistentes (sobreviven a reinicios/cortes de luz) ──────
  prefs.begin("tanque", false);
  UMBRAL_BAJO = prefs.getFloat("umbral_bajo", UMBRAL_BAJO);
  UMBRAL_ALTO = prefs.getFloat("umbral_alto", UMBRAL_ALTO);
  
  if (UMBRAL_BAJO >= UMBRAL_ALTO) {   // guarda contra NVS corrupta o primer uso
    UMBRAL_BAJO = 50.0;
    UMBRAL_ALTO = 85.0;
  }
  Serial.printf("[NVS] Umbrales cargados: bajo=%.1f%% alto=%.1f%%\n", UMBRAL_BAJO, UMBRAL_ALTO);

  // ─── Wi-Fi & Configuración Cautiva ────────────────────────────────────
  String broker_guardado = prefs.getString("mqtt_broker", mqtt_broker);
  strncpy(mqtt_broker, broker_guardado.c_str(), sizeof(mqtt_broker) - 1);
  mqtt_broker[sizeof(mqtt_broker) - 1] = '\0';

  WiFiManager wm;
  // Sin estos timeouts autoConnect() puede quedar bloqueado para siempre en
  // el portal cautivo. Si nadie lo configura en 3 min, se reinicia y reintenta.
  wm.setConnectTimeout(20);          // s esperando conectar a la red guardada
  wm.setConfigPortalTimeout(180);    // s máximos con el portal abierto
  WiFiManagerParameter custom_mqtt_broker("broker", "IP Broker MQTT", mqtt_broker, 40);
  wm.addParameter(&custom_mqtt_broker);

  Serial.println("[WiFi] Conectando a red guardada o levantando AP (Tanque_ESP32)...");
  if (!wm.autoConnect("Tanque_ESP32")) {
    Serial.println("[WiFi] Fallo al conectar y timeout. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.print("[WiFi] Conectado! IP: ");
  Serial.println(WiFi.localIP());

  // Guardar nueva IP si el usuario la cambió en el portal
  strncpy(mqtt_broker, custom_mqtt_broker.getValue(), sizeof(mqtt_broker) - 1);
  mqtt_broker[sizeof(mqtt_broker) - 1] = '\0';
  if (strcmp(mqtt_broker, broker_guardado.c_str()) != 0) {
    Serial.printf("[NVS] Guardando nuevo broker MQTT: %s\n", mqtt_broker);
    prefs.putString("mqtt_broker", mqtt_broker);
  }

  // ─── MQTT ─────────────────────────────────────────────────────────────
  mqtt.setServer(mqtt_broker, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);

  delay(500);
}

void loop() {
  unsigned long ahora = millis();

  // Mantener conexiones vivas (no bloqueante)
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  conectarMQTT();
  mqtt.loop();

  // Medir nivel con ultrasónico
  calcularNivel();

  // Control automático (respeta sensor_fault)
  controlAutomatico();

  // Publicar telemetría cada INTERVALO_TELEMETRIA_MS
  if (ahora - ultimo_telemetria >= INTERVALO_TELEMETRIA_MS) {
    ultimo_telemetria = ahora;
    publicarTelemetria();

    Serial.printf("[SENSOR] Dist: %.1f cm | Cota: %.1f cm | Nivel: %.1f%% | Fault: %s\n",
                  distancia_cm, cota_cm, porcentaje, sensor_fault ? "SI" : "no");
  }
}
