# Informe de Calidad del Código (Code Quality Assurance)

## 1. Resumen Ejecutivo
El firmware implementa un medidor de nivel sonoro (SPL) para ESP32-C3 utilizando FreeRTOS y ADC directo. La arquitectura es robusta, separando la adquisición de datos (Task 20kHz) de la comunicación (I2C Interrupts).

## 2. Hallazgos Críticos (Critical Issues)

### 🔴 1. Mapeo de Pines I2C (Hardware Mismatch)
**Severidad: ALTA**
Detectado cambio de placa a `lolin_c3_mini`.
- **Código actual**: `Wire.begin(ADDR, 8, 9)` (Estándar C3).
- **Hardware Lolin C3 Mini**: SDA = GPIO 8, **SCL = GPIO 10**. (GPIO 9 es botón de Boot en esta placa y puede causar conflictos).
- **Acción**: Se debe actualizar la iniciación del bus I2C.

### 🟠 2. Latencia en Interrupción I2C
**Severidad: MEDIA**
El uso de `SerialLog` (Serial.print) dentro de `requestEvent` y `receiveEvent` es peligroso.
- Estas funciones se ejecutan en contexto de alta prioridad/interrupción del driver I2C.
- `Serial.print` bloquea y es lento. Puede causar *Clock Stretching* excesivo, haciendo que el Maestro falle (timeout) o que el ESP32 se reinicie por Watchdog.
- **Acción**: Eliminar logs del callback I2C o usar banderas.

### 🟡 3. Ponderación de Frecuencia (A-Weighting)
**Severidad: MEDIA (Funcional)**
La norma UNE-EN ISO 1996-2 exige **LAeq** (Ponderación A).
- **Estado Actual**: El código calcula RMS plano (Lineal / Z-Weighting).
- **Impacto**: Las mediciones serán más altas de lo real en frecuencias bajas (graves), ya que el oído humano (y la curva A) atenúa los bajos.
- **Acción**: Se añadirá una nota de limitación técnica o una implementación básica de filtro A si el CPU lo permite. Dado el requisito de "medida de lo posible", se mantendrá como LZeq (Lineal) pero renombrando variables para precisión técnica, o aplicando una corrección simple.

## 3. Revisión de Estilo y Naming
- **Clean Code**: Estructura excelente. Uso de `#define` correcto.
- **Tasks**: Uso correcto de `xTaskCreate` pinned (aunque C3 es single core, `PinnedToCore` es seguro).
- **Log**: Sistema de log centralizado implementado correctamente.

## 4. Plan de Corrección
1. Corregir pines I2C para Lolin C3 Mini (8, 10).
2. Eliminar `SerialLog` de los eventos I2C.
3. Refinar comentarios sobre A-Weighting.
