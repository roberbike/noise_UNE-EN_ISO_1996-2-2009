# Verificación del nodo I2S (XIAO ESP32-S3 + ICS-43434)

Firmware autónomo que ejecuta solo la cadena de medida (I2S + ponderación A + RMS)
y muestra por Serial el LAeq (dB) y el nivel RMS (dBFS) cada segundo. No usa I2C.

## Cableado

![Módulo ICS-43434 MRS179A](../../docs/images/ics43434_mrs179a.png)

Etiquetas según la serigrafía del breakout MRS179A (foto). Otros módulos usan
`WS`/`LRCLK` por `LRCL`, `SD` por `DOUT` y `L/R` por `SEL`.

| Pin del módulo | XIAO ESP32-S3 |
| :--- | :--- |
| SEL | **GND** |
| LRCL | GPIO 3 (D2) |
| DOUT | GPIO 4 (D3) |
| BCLK | GPIO 2 (D1) |
| GND | GND |
| 3V | 3.3V |

**SEL a GND**: es el pin `L/R` del ICS-43434 (no un selector I2S/PDM: el chip no
tiene modo PDM). Con `I2S_CHANNEL_FMT_ONLY_LEFT`, SEL a 3.3 V deja el nodo sin
medir. Al aire funciona por el pull-down interno, pero en campo debe ir a masa.

## Uso

1. Compilar y flashear: entorno `seeed_xiao_esp32s3`.
2. Abrir Monitor Serie a 115200 baud.
3. El LAeq mostrado ya es dB SPL directos (sensibilidad de fábrica -26 dBFS @ 94 dB SPL).
4. Verificación opcional con calibrador acústico a 94 dB (1 kHz): acoplar el
   micrófono y anotar el LAeq estable. Si difiere de 94.0, definir en el firmware
   principal `-D MIC_OFFSET_DB=<94.0 - LAeq_medido>` en `build_flags`.

## Resultado esperado

Suelo de ruido en interior tranquilo: **~34 dB**, con variación segundo a
segundo (frente a ~58-60 dB clavados del nodo MAX4466). Ejemplo real:

```
[ICS43434] LAeq:34.8 | LAFmx:36.1 | L10:34.9 | L90:34 | RMS:55uFS | Lden:0.0 | clip:0 | cyc:21
[ICS43434] LAeq:40.7 | LAFmx:47.2 | L10:34.7 | L90:34 | RMS:108uFS | Lden:0.0 | clip:0 | cyc:22
```

El significado de cada indicador (LAeq, LAFmax, L10, L90, Lden) está en el
README principal, sección "Qué significan las medidas".

Notas sobre esa salida:
- **L10/L90 se mantienen constantes** entre bloques: se recalculan cada 20 s
  completos y conservan el valor del último bloque.
- **Lden = 0.0** es correcto aquí: requiere hora válida en el nodo, que llega
  del master vía comando legacy de set-time. En el firmware autónomo nunca se
  puebla.

## Diagnóstico

| Síntoma | Causa probable |
| :--- | :--- |
| `[WARN] Silencio absoluto` | SEL a 3.3 V, o línea DOUT sin conectar |
| Silencio con SEL a GND | Módulo con `L/R` fijado en alto: usar `I2S_CHANNEL_FMT_ONLY_RIGHT` |
| `[WARN] Clipping detected` con `clip:` alto | Nivel > ~120 dB SPL o interferencia; el segundo se invalida (integridad) |
| Lecturas erráticas | Cables largos (>10 cm) en BCLK/DOUT, o masa mal referenciada |
| Avisos de compilación sobre API obsoleta | Normal en core 3.x; ver `platformio.ini` (flags de supresión) |

---

## English version

# Verification of the I2S node (XIAO ESP32-S3 + ICS-43434)

Standalone firmware that runs only the measurement chain (I2S + A-weighting + RMS) and prints LAeq (dB) and RMS (dBFS) every second over Serial. It does not use I2C.

## Wiring

![ICS-43434 module MRS179A](../../docs/images/ics43434_mrs179a.png)

Labels follow the silkscreen of the MRS179A breakout (photo). Other modules use `WS`/`LRCLK` instead of `LRCL`, `SD` instead of `DOUT`, and `L/R` instead of `SEL`.

| Module pin | XIAO ESP32-S3 |
| :--- | :--- |
| SEL | **GND** |
| LRCL | GPIO 3 (D2) |
| DOUT | GPIO 4 (D3) |
| BCLK | GPIO 2 (D1) |
| GND | GND |
| 3V | 3.3V |

**SEL to GND**: this is the `L/R` pin of the ICS-43434 (not an I2S/PDM selector; the chip has no PDM mode). With `I2S_CHANNEL_FMT_ONLY_LEFT`, a 3.3 V SEL leaves the node silent. It may work floating due to the internal pull-down, but in the field it should be tied to ground.

## Usage

1. Compile and flash: `seeed_xiao_esp32s3` environment.
2. Open the Serial Monitor at 115200 baud.
3. The LAeq shown is already direct dB SPL (factory sensitivity -26 dBFS @ 94 dB SPL).
4. Optional verification with a 94 dB acoustic calibrator (1 kHz): couple the microphone and note the stable LAeq value. If it differs from 94.0, set `-D MIC_OFFSET_DB=<94.0 - measured_LAeq>` in the main firmware `build_flags`.

## Expected result

Quiet indoor noise floor: **~34 dB**, with second-to-second variation (as opposed to ~58–60 dB of the MAX4466 node). Example real output:

```text
[ICS43434] LAeq:34.8 | LAFmx:36.1 | L10:34.9 | L90:34 | RMS:55uFS | Lden:0.0 | clip:0 | cyc:21
[ICS43434] LAeq:40.7 | LAFmx:47.2 | L10:34.7 | L90:34 | RMS:108uFS | Lden:0.0 | clip:0 | cyc:22
```

The meaning of each indicator (LAeq, LAFmax, L10, L90, Lden) is described in the main README under "What the measurements mean".

## Diagnosis

| Symptom | Likely cause |
| :--- | :--- |
| `[WARN] Silent` | SEL at 3.3 V or DOUT line disconnected |
| Silence with SEL at GND | Module with `L/R` hardwired high: use `I2S_CHANNEL_FMT_ONLY_RIGHT` |
| `[WARN] Clipping detected` with high `clip:` | Level > ~120 dB SPL or interference; the second is invalidated |
| Erratic readings | Long cables (>10 cm) on BCLK/DOUT, or poor ground reference |
| Compilation warnings about deprecated API | Normal on core 3.x; check `platformio.ini` suppression flags |
