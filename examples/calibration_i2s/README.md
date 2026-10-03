# Verificación del nodo I2S (XIAO ESP32-S3 + ICS-43434)

Firmware autónomo que ejecuta solo la cadena de medida (I2S + ponderación A +
RMS) y muestra por Serie, cada segundo, el LAeq sin corregir, el RMS en dBFS,
el factor de cresta y el pico de la señal, la sensibilidad de la unidad y el
`MIC_OFFSET_DB` que la haría marcar el calibrador. No usa I2C.

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

1. Compilar y flashear: entorno `seeed_xiao_esp32s3` de este ejemplo.
2. Abrir el Monitor Serie a 115200 baudios.
3. Acoplar un calibrador de **94,0 dB / 1 kHz** al puerto del micrófono y
   dejar que la lectura se estabilice unos segundos.
4. **Mira primero `crest`.** Con un tono limpio vale **3,0 dB** (±0,3). Si sale
   otra cosa, el tono está distorsionado (demasiado nivel para la unidad) o el
   acoplamiento deja pasar ruido de la sala, y nada de esa línea sirve.
5. **Compara el `MIC_OFFSET_DB` impreso con el del firmware** (`platformio.ini`
   raíz, −13,71; la línea muestra la diferencia). Con el calibrador acoplado,
   una unidad igual a la de referencia imprime **entre −13,8 y −14,3**: un
   calibrador diseñado para cápsulas de 1/2", aplicado al puerto de un MEMS en
   una cavidad pequeña, entrega hasta ~0,5 dB más que en campo libre. Si la
   unidad se aparta más de ~1 dB, corrige **esa unidad** con `CMD_SET_CALIB`
   (función `calibrateNode()` del ejemplo `i2c_master`) en vez de cambiar el
   `build_flags` común. **No lo calcules a mano como `94 − LAeq`**: el valor
   impreso ya usa la misma conversión que el firmware.
6. Para el ajuste fino, contrasta en **campo libre** contra un sonómetro de
   referencia: es la referencia con la que se validó el −13,71 (con él, el
   calibrador marca 94,5-94,6 y el sonómetro coincide). Ver
   [docs/CALIBRACION.md](../../docs/CALIBRACION.md) §5.

**Offset en NVS.** El firmware de producción suma a `MIC_OFFSET_DB` el offset
por unidad guardado en la NVS del chip (`CMD_SET_CALIB`), que sobrevive al
reflasheo. Este sketch lo muestra al arrancar; para empezar de cero, compílalo
con `-D RESET_NVS_CALIB` o envía `CMD_SET_CALIB` con 0 desde el maestro.

**Este sketch replica la conversión del firmware exactamente**: mismo sample
rate (48 kHz), mismos coeficientes de ponderación A, mismo término pico→RMS
(+3,0103 dB) y mismo seguidor de continua. Si alguna vez divergen, el trim que
salga de aquí estará equivocado en esa diferencia. Versiones anteriores de este
ejemplo fallaban en tres de esas cosas a la vez —16 kHz frente a los 48 kHz del
nodo, los coeficientes A antiguos y sin el término pico→RMS—, y así un
106,5 dB medido se convertía en un trim 3 dB desviado.

No aplica ningún trim propio, a propósito: su trabajo es mostrar el nivel sin
corregir para poder derivarlo.

**Las dos cifras de RMS.** `dBFS(A)` es lo que publica la línea de log del
firmware; `dBFS(Z)` es sin ponderar e incluye el retumbe de baja frecuencia que
la ponderación A quita, así que en una sala real sale más alto. Con el tono del
calibrador (1 kHz, donde la ponderación A vale 0 dB) coinciden.

## Usa la posición de 94 dB: por encima de ~106 dB este nodo satura

Medido sobre una unidad real de este proyecto, con tres posiciones del
calibrador. El pico es el de un seno con ese RMS (`dBFS(A)` + 3,01).

| Posición | `dBFS(A)` | pico (seno) | del fondo de escala |
| :--- | ---: | ---: | ---: |
| 94 dB | −15,21 | −12,20 | 0,25 |
| 104 dB | −5,37 | −2,36 | **0,76** |
| 114 dB | −9,01 | (no es un seno) | — |

De 94 a 104 la cadena es **lineal**: la entrada subió 10,00 dB y la medida
9,84. El acoplamiento y el firmware están bien.

La posición de 114 es la que no vale, y la razón es el margen por arriba. Esta
unidad tiene una sensibilidad de **−12,20 dBFS de pico a 94 dB SPL**, frente a
los −26 dBFS del datasheet: entrega 13,8 dB más nivel. Eso se paga con 13,8 dB
menos de rango útil, así que **el fondo de escala digital se alcanza a
~106 dB SPL** en lugar de los 120 dB del AOP nominal del ICS-43434. A 114 dB
el micrófono está unos 8 dB por encima de su fondo de escala, y su salida no
recorta limpiamente sino que se degrada: por eso lee **menos** que en 104
(−9,01 frente a −5,37) y por eso en esa meseta `dBFS(A)` y `dBFS(Z)` se
separan 2,1 dB cuando en las otras dos coinciden: la onda ya está sucia.

La predicción lo confirma: partiendo solo de la medida de 94 dB, a 104 dB
corresponde un pico de 0,78 del fondo de escala y salió **0,76**. A 114 dB
corresponderían 2,45 veces el fondo de escala, que es imposible.

**En la práctica:** calibra con la posición de **94 dB** (y, si quieres
comprobar la linealidad, con 104 compilando con `-D CALIBRATOR_DB=104.0`).
Nunca con 114 en estas unidades. Y ten presente el techo al desplegar: un nodo
que satura a ~106 dB no puede medir eventos más fuertes, y el `LCpeak` de un
impulso sí puede pasarse de ahí. El firmware cuenta las muestras por encima de
0,99 del fondo de escala y, con más de 10 en un segundo, lo invalida, así que
la sobrecarga queda señalada en lugar de publicarse como una medida buena.

## Resultado esperado

Una línea por segundo. Este ejemplo usa los valores de la meseta de 94 dB de la
tabla (los de `crest` y `dBFS(Z)` son los de un seno limpio):

```text
LAeq: 107.80 dB | dBFS(A): -15.21 | dBFS(Z): -15.21 | crest: 3.01 dB | peak: 0.25 FS | sens: -12.20 dBFS | @94dB: MIC_OFFSET_DB=-13.80 (firmware -13.71, diff -0.09)
```

El LAeq sale **sin corregir**, así que en una misma sala marca más que el
firmware: la diferencia es exactamente el `MIC_OFFSET_DB`. La columna
`MIC_OFFSET_DB` solo significa algo **con el calibrador puesto**; en ambiente
da un número sin sentido, porque el sketch no sabe a qué nivel estás. Lo mismo
vale para `crest`: en una sala, con ruido en vez de un tono, sale muy por
encima de 3 dB y es lo normal.

Nota sobre las unidades: estas unidades leen muy por encima de lo que implica
su hoja de datos. Un ICS-43434 que cumpliera los −26 dBFS daría
`dBFS(A) ≈ −29` con el calibrador y un `MIC_OFFSET_DB` cercano a 0; las
medidas reales dan unos 14 dB más de nivel. Mide antes de dar por bueno
cualquier valor, incluido el que trae el firmware.

## Diagnóstico

| Síntoma | Causa probable |
| :--- | :--- |
| `[WARN] Absolute silence` | SEL a 3.3 V, o línea DOUT sin conectar |
| Silencio con SEL a GND | Módulo con `L/R` fijado en alto: usar `I2S_CHANNEL_FMT_ONLY_RIGHT` |
| `crest` lejos de 3,0 dB con el calibrador | Tono distorsionado (nivel por encima de ~104 dB en estas unidades) o acoplamiento que deja pasar ruido |
| `[WARN] ... samples at full scale` | Sobrecarga: nivel por encima de ~106 dB SPL en estas unidades, o interferencia |
| Lecturas erráticas | Cables largos (>10 cm) en BCLK/DOUT, o masa mal referenciada |
| Avisos de compilación sobre API obsoleta | Normal en core 3.x; `platformio.ini` ya los silencia |

---

## English version

# Verification of the I2S node (XIAO ESP32-S3 + ICS-43434)

Standalone firmware that runs only the measurement chain (I2S + A-weighting + RMS) and prints over Serial, every second, the untrimmed LAeq, the RMS in dBFS, the crest factor and peak of the signal, the unit's sensitivity and the `MIC_OFFSET_DB` that would make it read the calibrator. It does not use I2C.

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

**SEL to GND**: this is the `L/R` pin of the ICS-43434 (not an I2S/PDM selector; the chip has no PDM mode). With `I2S_CHANNEL_FMT_ONLY_LEFT`, a 3.3 V SEL leaves the node silent. It may work floating thanks to the internal pull-down, but in the field it must be tied to ground.

## Usage

1. Build and flash: this example's `seeed_xiao_esp32s3` environment.
2. Open the Serial Monitor at 115200 baud.
3. Couple a **94.0 dB / 1 kHz** calibrator to the microphone port and let the reading settle for a few seconds.
4. **Check `crest` first.** With a clean tone it reads **3.0 dB** (±0.3). Anything else means the tone is distorted (too loud for the unit) or the coupling lets room noise in, and nothing on that line is usable.
5. **Compare the printed `MIC_OFFSET_DB` with the firmware's** (root `platformio.ini`, −13.71; the line shows the difference). With the calibrator coupled, a unit like the reference one prints **between −13.8 and −14.3**: a calibrator designed for 1/2" capsules, applied to a MEMS port in a small cavity, delivers up to ~0.5 dB more than in free field. If the unit is off by more than ~1 dB, correct **that unit** with `CMD_SET_CALIB` (`calibrateNode()` in the `i2c_master` example) instead of changing the shared `build_flags`. **Do not compute it by hand as `94 − LAeq`**: the printed value already uses the firmware's conversion.
6. For fine adjustment, compare in **free field** against a reference sound level meter: that is the reference −13.71 was validated against (with it the calibrator reads 94.5–94.6 and the meter agrees). See [docs/CALIBRACION.md](../../docs/CALIBRACION.md) §5.

**NVS offset.** The production firmware adds to `MIC_OFFSET_DB` the per-unit offset stored in the chip's NVS (`CMD_SET_CALIB`), which survives reflashing. This sketch prints it at boot; to start from scratch, build it with `-D RESET_NVS_CALIB` or send `CMD_SET_CALIB` with 0 from the master.

**This sketch mirrors the firmware's conversion exactly**: same sample rate (48 kHz), same A-weighting coefficients, same peak-to-RMS term (+3.0103 dB) and same DC tracker. If the two ever diverge, the trim derived here is wrong by the difference. Earlier versions of this example got three of those wrong at once — 16 kHz against the node's 48 kHz, the superseded A coefficients, and no peak-to-RMS term — which is how a measured 106.5 dB turned into a trim 3 dB off.

It applies no trim of its own, by design: its job is to show the untrimmed level so the trim can be derived from it.

**The two RMS figures.** `dBFS(A)` is what the firmware's log line reports; `dBFS(Z)` is unweighted and includes low-frequency rumble that A-weighting removes, so it reads higher in a real room. With the calibrator tone (1 kHz, where A-weighting is 0 dB) they agree.

## Use the 94 dB setting: above ~106 dB this node saturates

Measured on a real unit of this project, at three calibrator settings. The peak is that of a sine with that RMS (`dBFS(A)` + 3.01).

| Setting | `dBFS(A)` | peak (sine) | of full scale |
| :--- | ---: | ---: | ---: |
| 94 dB | −15.21 | −12.20 | 0.25 |
| 104 dB | −5.37 | −2.36 | **0.76** |
| 114 dB | −9.01 | (not a sine) | — |

From 94 to 104 the chain is **linear**: the input went up 10.00 dB and the measurement 9.84. The coupling and the firmware are fine.

The 114 dB setting is the one that does not work, and the reason is headroom. This unit has a sensitivity of **−12.20 dBFS peak at 94 dB SPL** against the datasheet's −26 dBFS: it delivers 13.8 dB more level, and that is paid for with 13.8 dB less usable range, so **digital full scale is reached at ~106 dB SPL** instead of the ICS-43434's nominal 120 dB AOP. At 114 dB the microphone is some 8 dB past full scale and its output does not clip cleanly but degrades: that is why it reads **less** than at 104 (−9.01 against −5.37), and why on that plateau `dBFS(A)` and `dBFS(Z)` diverge by 2.1 dB where on the other two they agree: the waveform is already dirty.

The prediction confirms it: from the 94 dB measurement alone, 104 dB implies a peak at 0.78 of full scale and 0.76 came out. 114 dB would imply 2.45 times full scale, which is impossible.

**In practice:** calibrate with the **94 dB** setting (and, to check linearity, 104 by building with `-D CALIBRATOR_DB=104.0`). Never 114 on these units. And keep the ceiling in mind when deploying: a node that saturates at ~106 dB cannot measure louder events, and an impulse's `LCpeak` can exceed that. The firmware counts samples above 0.99 of full scale and, with more than 10 in one second, invalidates it, so an overload is flagged rather than published as a good reading.

## Expected result

One line per second. This example uses the values of the 94 dB plateau in the table (`crest` and `dBFS(Z)` are those of a clean sine):

```text
LAeq: 107.80 dB | dBFS(A): -15.21 | dBFS(Z): -15.21 | crest: 3.01 dB | peak: 0.25 FS | sens: -12.20 dBFS | @94dB: MIC_OFFSET_DB=-13.80 (firmware -13.71, diff -0.09)
```

The LAeq is **untrimmed**, so in the same room it reads higher than the firmware: the difference is exactly `MIC_OFFSET_DB`. The `MIC_OFFSET_DB` column only means anything **with the calibrator on**; in ambient it prints a meaningless number, because the sketch has no idea what level you are in. The same goes for `crest`: in a room, with noise instead of a tone, it reads well above 3 dB, and that is normal.

A note on the parts: these units read far above what their datasheet implies. An ICS-43434 meeting its −26 dBFS spec would give `dBFS(A) ≈ −29` with the calibrator and a `MIC_OFFSET_DB` near 0; real measurements come out about 14 dB hotter. Measure before trusting any value, including the one the firmware ships with.

## Diagnosis

| Symptom | Likely cause |
| :--- | :--- |
| `[WARN] Absolute silence` | SEL at 3.3 V, or DOUT line disconnected |
| Silence with SEL at GND | Module with `L/R` hardwired high: use `I2S_CHANNEL_FMT_ONLY_RIGHT` |
| `crest` far from 3.0 dB with the calibrator | Distorted tone (level above ~104 dB on these units) or a coupling that lets noise in |
| `[WARN] ... samples at full scale` | Overload: level above ~106 dB SPL on these units, or interference |
| Erratic readings | Long cables (>10 cm) on BCLK/DOUT, or poor ground reference |
| Compilation warnings about deprecated API | Normal on core 3.x; `platformio.ini` already silences them |
