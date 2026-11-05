-- ============================================================================
--  IOT CAMPUS — Esquema de la base de datos de telemetría
--  Motor: SQLite 3 (archivo iot_campus.db)
--  Referencia: server.py crea esta tabla automáticamente al iniciar, no hace
--  falta ejecutar este script a mano.
-- ============================================================================

CREATE TABLE IF NOT EXISTS historial_tanque (
  id              INTEGER     PRIMARY KEY AUTOINCREMENT,
  fecha_hora      DATETIME    DEFAULT CURRENT_TIMESTAMP,   -- UTC
  porcentaje      REAL        NOT NULL,                    -- 0..100 %
  cota_cm         REAL        NOT NULL,                    -- 0..175 cm
  distancia_cm    REAL        NOT NULL,                    -- cm sensor → agua
  estado_bomba    BOOLEAN     NOT NULL DEFAULT 0,
  modo_operacion  VARCHAR(10) NOT NULL DEFAULT 'AUTO',     -- AUTO | MANUAL
  sensor_fault    BOOLEAN     NOT NULL DEFAULT 0           -- 1 = lectura no confiable
);

CREATE INDEX IF NOT EXISTS idx_fecha ON historial_tanque (fecha_hora DESC);
