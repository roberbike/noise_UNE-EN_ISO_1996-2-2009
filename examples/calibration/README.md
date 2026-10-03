# Ejemplo: Calibración del nodo MAX4466 (ADC)

Firmware **autónomo** para el nodo analógico (ESP32-C3 + MAX4466). Ejecuta la
misma cadena de medida que el firmware principal (ADC a 16 kHz → eliminación
de continua → ponderación A → RMS) y muestra por **Serie** cada segundo, sin
usar I2C:

- **RMS (mV)**: tensión RMS ponderada A a la entrada del ADC (salida del MAX4466).
- **LAeq (dB)**: el nivel que daría el firmware con las constantes de
  calibración vigentes.
- **bias (mV)**: la continua del MAX4466, que debe rondar VCC/2 (~1650 mV).
  Fuera de 800–2600 mV el módulo está sin alimentar, desconectado o en corto.
- **clip**: muestras en los raíles del ADC en ese segundo. Tiene que ser 0.

Este micrófono **requiere calibración con calibrador acústico**: su
sensibilidad depende del potenciómetro de ganancia y no está especificada de
fábrica (a diferencia del ICS-43434 del nodo digital).

La cadena es copia exacta de la del firmware (mismo reloj de muestreo, mismo
seguidor de continua, mismos coeficientes de ponderación A, misma conversión
a mV). Si alguna vez divergieran, el valor medido aquí estaría desviado en esa
diferencia.

## Uso

1. Conectar el **MAX4466**: OUT → GPIO 4, VCC → 3.3 V, GND → GND.
2. En PlatformIO, entorno **lolin_c3_mini** de este ejemplo: compilar y subir.
3. Abrir el **Monitor Serie** a **115200** baudios.
4. Acoplar el **calibrador a 94 dB** (1 kHz) y ajustar el **potenciómetro**
   hasta un RMS estable de **100–400 mV** con **`clip: 0`**.
5. Anotar el **RMS (mV) estable**: es `CALIBRATION_RMS_MV`.
6. Ponerlo en el `platformio.ini` de la raíz, entorno `lolin_c3_mini`:
   `build_flags = -D CALIBRATION_RMS_MV=<valor>`. Recompilar y flashear el
   firmware de producción. No hay que editar ningún fuente: el valor de
   `src/DSP_Engine.h` solo se usa si no se define otro.
7. **Offset en NVS.** El firmware de producción suma a todos los niveles el
   offset por unidad que guarda la NVS del chip (`CMD_SET_CALIB`), y ese
   offset sobrevive al reflasheo. Este sketch lo muestra al arrancar. Con un
   `CALIBRATION_RMS_MV` nuevo, el offset antiguo ya no significa nada:
   bórralo compilando este sketch con `-D RESET_NVS_CALIB`, o enviando
   `CMD_SET_CALIB` con 0 desde el maestro.

Comprobación: con el firmware de producción y el calibrador puesto, el nodo
debe marcar 94,0 ±0,3 dB. Un retoque posterior de una unidad concreta se hace
con `CMD_SET_CALIB` (función `calibrateNode()` del ejemplo `i2c_master`), sin
recompilar.

Formato de la línea (valores ilustrativos):

```text
RMS: 166.20 mV | LAeq: 94.0 dB(A) | bias: 1652 mV | clip: 0
```

## Resultado esperado

Suelo de ruido del nodo: **~58-60 dB** (limitado por el ADC del ESP32 y el
previo del MAX4466). Es la razón por la que este micrófono no puede medir
noches urbanas tranquilas: para eso está el nodo ICS-43434.

## Linealidad y techo

El techo de este nodo lo pone el ADC, no el micrófono. La salida del MAX4466
está centrada en ~1,65 V y el ADC del C3, con esta atenuación, llega hasta
~2,5 V: la semionda positiva se queda sin margen a unos 0,85 V de pico, es
decir ~0,6 V RMS. Con 166 mV a 94 dB eso son unos **105 dB**; con 400 mV,
unos 97 dB. Por encima, el contador `clip` lo marca y el firmware invalida el
segundo.

- **104 dB** (si tu calibrador tiene esa posición) es una buena prueba de
  linealidad: debe marcar 104,0 ±0,5 dB, **siempre que `clip` siga en 0**.
- **114 dB** recorta con cualquier ganancia útil (`clip` > 0) y la lectura no
  vale. No es falta de linealidad, es falta de rango.

## Documentación completa

Procedimiento detallado, conexión del MAX4466, ajuste del potenciómetro y
normativa (UNE-EN ISO 1996-2:2009, Decreto 213/2012):
**[docs/CALIBRACION.md](../../docs/CALIBRACION.md)**.

---

## English version

# Example: calibration of the MAX4466 node (ADC)

**Standalone** firmware for the analog node (ESP32-C3 + MAX4466). It runs the same measurement chain as the main firmware (16 kHz ADC → DC removal → A-weighting → RMS) and prints over **Serial** every second, without using I2C:

- **RMS (mV)**: A-weighted RMS voltage at the ADC input (MAX4466 output).
- **LAeq (dB)**: the level the firmware would report with the calibration constants in use.
- **bias (mV)**: the MAX4466 DC level, which should sit near VCC/2 (~1650 mV). Outside 800–2600 mV the module is unpowered, disconnected or shorted.
- **clip**: samples at the ADC rails in that second. It must be 0.

This microphone **requires acoustic calibration**: its sensitivity depends on the gain trimmer and is not specified by the factory (unlike the ICS-43434 of the digital node).

The chain is an exact copy of the firmware's (same sample clock, same DC tracker, same A-weighting coefficients, same conversion to mV). Should the two ever diverge, the value measured here would be off by the difference.

## Usage

1. Connect the **MAX4466**: OUT → GPIO 4, VCC → 3.3 V, GND → GND.
2. In PlatformIO, this example's **lolin_c3_mini** environment: build and upload.
3. Open the **Serial Monitor** at **115200** baud.
4. Couple the **94 dB calibrator** (1 kHz) and turn the **trimmer** until RMS is stable at **100–400 mV** with **`clip: 0`**.
5. Note the **stable RMS (mV)**: that is `CALIBRATION_RMS_MV`.
6. Put it in the root `platformio.ini`, `lolin_c3_mini` environment: `build_flags = -D CALIBRATION_RMS_MV=<value>`. Rebuild and flash the production firmware. No source file needs editing: the value in `src/DSP_Engine.h` is only used when none is defined.
7. **NVS offset.** The production firmware adds to every level the per-unit offset stored in the chip's NVS (`CMD_SET_CALIB`), and that offset survives reflashing. This sketch prints it at boot. With a new `CALIBRATION_RMS_MV` the old offset no longer means anything: clear it by building this sketch with `-D RESET_NVS_CALIB`, or by sending `CMD_SET_CALIB` with 0 from the master.

Check: with the production firmware and the calibrator on, the node must read 94.0 ±0.3 dB. A later touch-up of a single unit goes through `CMD_SET_CALIB` (`calibrateNode()` in the `i2c_master` example), with no rebuild.

Line format (illustrative values):

```text
RMS: 166.20 mV | LAeq: 94.0 dB(A) | bias: 1652 mV | clip: 0
```

## Expected result

Noise floor of the node: **~58–60 dB** (limited by the ESP32 ADC and the MAX4466 preamp). This is why this microphone cannot measure quiet urban nights: that is the role of the ICS-43434 node.

## Linearity and ceiling

The ceiling of this node is set by the ADC, not the microphone. The MAX4466 output sits at ~1.65 V and the C3's ADC, at this attenuation, reaches ~2.5 V: the positive half-wave runs out of room at about 0.85 V peak, i.e. ~0.6 V RMS. With 166 mV at 94 dB that is about **105 dB**; with 400 mV, about 97 dB. Above that the `clip` counter flags it and the firmware invalidates the second.

- **104 dB** (if your calibrator has that setting) is a good linearity check: it must read 104.0 ±0.5 dB, **as long as `clip` stays at 0**.
- **114 dB** clips at any useful gain (`clip` > 0) and the reading is not valid. That is a lack of range, not of linearity.

## Full documentation

Detailed procedure, MAX4466 wiring, trimmer adjustment and regulations (UNE-EN ISO 1996-2:2009, Decree 213/2012):
**[docs/CALIBRACION.md](../../docs/CALIBRACION.md)**.
