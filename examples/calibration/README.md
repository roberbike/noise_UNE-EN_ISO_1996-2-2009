# Ejemplo: Calibración

Firmware **standalone** para el nodo de ruido (ESP32-C3). Ejecuta la misma cadena de medida que el firmware principal (ADC + ponderación A + RMS) y envía por **Serial** cada segundo:

- **RMS (mV)** — tensión RMS en la entrada del ADC (salida del MAX4466).
- **LAeq (dB)** — nivel equivalente en dB(A) usando las constantes de calibración.

## Uso

1. Conectar el **MAX4466**: OUT → GPIO 4, VCC → 3,3 V, GND → GND.
2. Ajustar el **potenciómetro** del MAX4466 según [docs/CALIBRACION.md](../../docs/CALIBRACION.md) (con calibrador a 94 dB, RMS estable en ~100–400 mV, sin saturación).
3. En PlatformIO: seleccionar entorno **lolin_c3_mini**, compilar y subir.
4. Abrir **Monitor Serie** a **115200** baud.
5. Con el **calibrador a 94 dB** (1 kHz) y el micrófono bien acoplado, anotar el valor **estable** de **RMS (mV)**.
6. Copiar ese valor a la constante **`CALIBRATION_RMS_MV`** en **`src/main.cpp`** del firmware principal (raíz del repo), recompilar y flashear el firmware de producción.

## Documentación completa

Procedimiento detallado, conexión del MAX4466, ajuste del potenciómetro y normativa (UNE-EN ISO 1996-2:2009, Decreto 213/2012): **[docs/CALIBRACION.md](../../docs/CALIBRACION.md)**.
