# Ejemplo: Calibración del nodo MAX4466 (ADC)

Firmware **autónomo** para el nodo analógico (ESP32-C3 + MAX4466). Ejecuta la
misma cadena de medida que el firmware principal (ADC → ponderación A → RMS) y
envía por **Serial** cada segundo, sin usar I2C:

- **RMS (mV)** — tensión RMS a la entrada del ADC (salida del MAX4466).
- **LAeq (dB)** — nivel equivalente en dB(A) con las constantes de calibración.

Este micrófono **requiere calibración con calibrador acústico**: su
sensibilidad depende del ajuste de ganancia del potenciómetro y no está
especificada de fábrica (a diferencia del ICS-43434 del nodo digital).

## Uso

1. Conectar el **MAX4466**: OUT → GPIO 4, VCC → 3.3 V, GND → GND.
2. Ajustar el **potenciómetro** del MAX4466 con calibrador a 94 dB hasta un RMS
   estable de ~100-400 mV sin saturación (ver [docs/CALIBRACION.md](../../docs/CALIBRACION.md)).
3. En PlatformIO: entorno **lolin_c3_mini**, compilar y subir.
4. Abrir **Monitor Serie** a **115200** baud.
5. Con el **calibrador a 94 dB** (1 kHz) y el micrófono bien acoplado, anotar el
   valor **estable** de **RMS (mV)**.
6. Copiar ese valor en **`CALIBRATION_RMS_MV`** de **`src/main.cpp`** (raíz del
   repo), recompilar y flashear el firmware de producción.

## Resultado esperado

Suelo de ruido del nodo: **~58-60 dB** (limitado por el ADC del ESP32 y el
previo del MAX4466). Es la razón por la que este micrófono no puede medir
noches urbanas tranquilas — para eso está el nodo ICS-43434, con suelo ~34 dB.

## Verificación de linealidad (opcional)

Cambiar el calibrador a 114 dB: el sistema debería marcar 114.0 ±0.5 dB. Una
desviación mayor indica no linealidad del ADC o saturación del previo.

## Documentación completa

Procedimiento detallado, conexión del MAX4466, ajuste del potenciómetro y
normativa (UNE-EN ISO 1996-2:2009, Decreto 213/2012):
**[docs/CALIBRACION.md](../../docs/CALIBRACION.md)**.

---

## English version

# Example: calibration of the MAX4466 node (ADC)

Standalone firmware for the analog node (ESP32-C3 + MAX4466). It runs the same measurement chain as the main firmware (ADC → A-weighting → RMS) and outputs via **Serial** every second without using I2C:

- **RMS (mV)** — RMS voltage at the ADC input (MAX4466 output)
- **LAeq (dB)** — equivalent level in dB(A) using the calibration constants

This microphone **requires acoustic calibration**: its sensitivity depends on the gain adjustment of the potentiometer and is not specified by the factory (unlike the ICS-43434 used by the digital node).

## Usage

1. Connect the **MAX4466**: OUT → GPIO 4, VCC → 3.3 V, GND → GND.
2. Adjust the **potentiometer** of the MAX4466 with a calibrator at 94 dB until a stable RMS of ~100–400 mV is obtained without saturation.
3. In PlatformIO: use the **lolin_c3_mini** environment, compile and upload.
4. Open the **Serial Monitor** at **115200** baud.
5. With the **94 dB calibrator** (1 kHz) and the microphone properly coupled, note the stable **RMS (mV)** value.
6. Copy that value into **`CALIBRATION_RMS_MV`** in **`src/main.cpp`** (project root), rebuild and flash the production firmware.

## Expected result

Noise floor of the node: **~58–60 dB** (limited by the ESP32 ADC and the MAX4466 preamp). This is why this microphone cannot measure quiet urban nights — that is the role of the ICS-43434 node, with a floor around 34 dB.

## Linearity check (optional)

Change the calibrator to 114 dB: the system should read 114.0 ± 0.5 dB. A larger deviation indicates ADC nonlinearity or preamp saturation.

## Full documentation

Detailed procedure, MAX4466 wiring, potentiometer adjustment and regulations (UNE-EN ISO 1996-2:2009, Decree 213/2012):
**[docs/CALIBRACION.md](../../docs/CALIBRACION.md)**.
