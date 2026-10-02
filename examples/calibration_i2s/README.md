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
3. Acoplar un calibrador de **94,0 dB / 1 kHz** al puerto del micrófono y
   dejar que la lectura se estabilice unos segundos.
4. **Mira primero la columna `sens`.** Es la sensibilidad de esta unidad
   referida a 94 dB SPL, así que tiene que salir **igual con cualquier posición
   del calibrador**. Mide en 94 y en 114 (recompilando con
   `-D CALIBRATOR_DB=114.0` para la segunda): si las dos no coinciden, el
   acoplamiento no está entregando el nivel que marca el calibrador y nada de
   lo que salga de ahí sirve. Solo si coinciden, copia el `MIC_OFFSET_DB` que
   imprime la línea al `build_flags` del firmware principal. **No lo calcules
   a mano como `94 − LAeq`**: eso solo vale si este sketch y el firmware
   comparten la misma conversión, que es justo lo que el valor impreso ya tiene
   en cuenta. Y si usas la posición de 114 dB, **recompila con
   `-D CALIBRATOR_DB=114.0`** o el valor sugerido saldrá 20 dB desviado.
5. Contrastar en **campo libre** contra un sonómetro de referencia. Un
   calibrador diseñado para cápsula de 1/2" acoplado a un puerto MEMS en una
   cavidad pequeña entrega más SPL que el nominal, así que el paso 4 por sí
   solo puede sobrecorregir.

**Este sketch replica la conversión del firmware exactamente**: mismo sample
rate (48 kHz), mismos coeficientes de ponderación A y mismo término
pico→RMS (+3,0103 dB). Si alguna vez divergen, el trim que salga de aquí
estará equivocado en esa diferencia. Versiones anteriores de este ejemplo
fallaban en las tres cosas a la vez —16 kHz frente a los 48 kHz del nodo, los
coeficientes A antiguos y sin el término pico→RMS—, y así un 106,5 dB medido
se convertía en un trim 3 dB desviado.

No aplica ningún trim propio, a propósito: su trabajo es mostrar el nivel sin
corregir para poder derivarlo.

**Las dos cifras de RMS.** `dBFS(A)` es lo que publica la línea de log del
firmware; `dBFS(Z)` es sin ponderar e incluye el retumbe de baja frecuencia que
la ponderación A quita, así que en una sala real sale más alto. Para comparar
contra el firmware, usa `dBFS(A)`.

## Usa la posición de 94 dB: por encima de ~106 dB este nodo satura

Medido sobre una unidad real de este proyecto, con tres posiciones del
calibrador:

| Posición | `dBFS(A)` | pico | del fondo de escala |
| :--- | ---: | ---: | ---: |
| 94 dB | −15,21 | −12,20 | 0,25 |
| 104 dB | −5,37 | −2,36 | **0,76** |
| 114 dB | −9,01 | −6,00 | 0,50 |

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
separan 2,1 dB cuando en las otras dos coinciden — la onda ya está sucia.

La predicción lo confirma: partiendo solo de la medida de 94 dB, a 104 dB
corresponde un pico de 0,78 del fondo de escala y se midió **0,76**. A 114 dB
corresponderían 2,45 veces el fondo de escala, que es imposible.

**En la práctica:** calibra con la posición de **94 dB** y nada más. Y ten
presente el techo al desplegar — un nodo que satura a ~106 dB no puede medir
eventos más fuertes, y el `LCpeak` de un impulso sí puede pasarse de ahí. El
contador `clip` del firmware (umbral 0,99 de fondo de escala) marca el segundo
como inválido cuando eso ocurre, así que la sobrecarga queda señalada en lugar
de publicarse como una medida buena.

## Resultado esperado

Este sketch imprime el nivel **sin corregir**, así que sus LAeq salen más altos
que los del firmware en la misma sala: la diferencia es exactamente el
`MIC_OFFSET_DB` que acabes poniendo. Una línea por segundo:

```
LAeq: 54.70 dB | dBFS(A): -68.31 | dBFS(Z): -59.40 | if calibrator: MIC_OFFSET_DB=39.30
LAeq: 108.28 dB | dBFS(A): -14.73 | dBFS(Z): -13.46 | if calibrator: MIC_OFFSET_DB=-14.28
```

La primera línea es una sala tranquila (unos 41 dB reales una vez aplicado el
trim); la segunda, con el calibrador de 94,0 dB acoplado. **La columna
`MIC_OFFSET_DB` solo significa algo en la segunda situación** — con el
calibrador puesto. En ambiente da un número sin sentido, porque el sketch no
sabe a qué nivel estás realmente.

Nota sobre las unidades: estas unidades leen muy por encima de lo que implica
su hoja de datos. Un ICS-43434 que cumpliera los −26 dBFS daría
`dBFS(A) ≈ -29` con el calibrador y un `MIC_OFFSET_DB` cercano a 0; las
medidas reales dan unos 14 dB más de nivel. Mide antes de dar por bueno
cualquier valor, incluido el que trae el firmware.

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
3. Couple a **94.0 dB / 1 kHz** calibrator to the microphone port and let the reading settle for a few seconds.
4. **Check the `sens` column first.** It is this unit's sensitivity referred to 94 dB SPL, so it must come out **the same whichever calibrator level you use**. Measure at 94 and at 114 (rebuilding with `-D CALIBRATOR_DB=114.0` for the latter): if the two disagree, the coupling is not delivering the level the calibrator is set to and nothing derived from it is usable. Only if they agree, copy the printed `MIC_OFFSET_DB` into the main firmware's `build_flags`. **Do not compute it by hand as `94 - LAeq`**: that only holds if this sketch and the firmware share the same conversion, which is exactly what the printed value already accounts for. And if you use the 114 dB setting, **rebuild with `-D CALIBRATOR_DB=114.0`** or the suggested value comes out 20 dB wrong.
5. Verify in **free field** against a reference sound level meter. A calibrator designed for a 1/2" capsule, coupled to a MEMS port in a small cavity, delivers more SPL than nominal, so step 4 alone can over-correct.

**This sketch mirrors the firmware's conversion exactly**: same sample rate (48 kHz), same A-weighting coefficients, same peak-to-RMS term (+3.0103 dB). If the two ever diverge, the trim derived here is wrong by the difference. Earlier versions of this example got all three wrong at once — 16 kHz against the node's 48 kHz, the superseded A coefficients, and no peak-to-RMS term — which is how a measured 106.5 dB turned into a trim 3 dB off.

It applies no trim of its own, by design: its job is to show the untrimmed level so the trim can be derived from it.

**The two RMS figures.** `dBFS(A)` is what the firmware's log line reports; `dBFS(Z)` is unweighted and includes low-frequency rumble that A-weighting removes, so it reads higher in a real room. Use `dBFS(A)` when comparing against the firmware.

## Use the 94 dB setting: above ~106 dB this node saturates

Measured on a real unit of this project, at three calibrator settings:

| Setting | `dBFS(A)` | peak | of full scale |
| :--- | ---: | ---: | ---: |
| 94 dB | −15.21 | −12.20 | 0.25 |
| 104 dB | −5.37 | −2.36 | **0.76** |
| 114 dB | −9.01 | −6.00 | 0.50 |

From 94 to 104 the chain is **linear**: the input went up 10.00 dB and the measurement 9.84. The coupling and the firmware are fine.

The 114 dB setting is the one that does not work, and the reason is headroom. This unit has a sensitivity of **−12.20 dBFS peak at 94 dB SPL** against the datasheet's −26 dBFS — it delivers 13.8 dB more level, and that is paid for with 13.8 dB less usable range, so **digital full scale is reached at ~106 dB SPL** instead of the ICS-43434's nominal 120 dB AOP. At 114 dB the microphone is some 8 dB past full scale and its output does not clip cleanly but degrades: that is why it reads **less** than at 104 (−9.01 against −5.37), and why on that plateau `dBFS(A)` and `dBFS(Z)` diverge by 2.1 dB where on the other two they agree — the waveform is already dirty.

The prediction confirms it: from the 94 dB measurement alone, 104 dB implies a peak at 0.78 of full scale and 0.76 was measured. 114 dB would imply 2.45 times full scale, which is impossible.

**In practice:** calibrate with the **94 dB** setting and nothing else. And keep the ceiling in mind when deploying — a node that saturates at ~106 dB cannot measure louder events, and an impulse's `LCpeak` can exceed that. The firmware's `clip` counter (threshold 0.99 of full scale) invalidates the second when it happens, so overload is flagged rather than published as a good reading.

## Expected result

This sketch prints the **untrimmed** level, so its LAeq reads higher than the firmware's in the same room — the difference is exactly the `MIC_OFFSET_DB` you end up setting. One line per second:

```text
LAeq: 54.70 dB | dBFS(A): -68.31 | dBFS(Z): -59.40 | if calibrator: MIC_OFFSET_DB=39.30
LAeq: 108.28 dB | dBFS(A): -14.73 | dBFS(Z): -13.46 | if calibrator: MIC_OFFSET_DB=-14.28
```

The first line is a quiet room (about 41 dB once the trim is applied); the second has a 94.0 dB calibrator coupled. **The `MIC_OFFSET_DB` column only means anything in the second case** — with the calibrator on. In ambient it prints a meaningless number, because the sketch has no idea what level you are actually in.

A note on the parts: these units read far above what their datasheet implies. An ICS-43434 meeting its −26 dBFS spec would give `dBFS(A) ≈ -29` with the calibrator and a `MIC_OFFSET_DB` near 0; real measurements come out about 14 dB hotter. Measure before trusting any value, including the one the firmware ships with.

## Diagnosis

| Symptom | Likely cause |
| :--- | :--- |
| `[WARN] Silent` | SEL at 3.3 V or DOUT line disconnected |
| Silence with SEL at GND | Module with `L/R` hardwired high: use `I2S_CHANNEL_FMT_ONLY_RIGHT` |
| `[WARN] Clipping detected` with high `clip:` | Level > ~120 dB SPL or interference; the second is invalidated |
| Erratic readings | Long cables (>10 cm) on BCLK/DOUT, or poor ground reference |
| Compilation warnings about deprecated API | Normal on core 3.x; check `platformio.ini` suppression flags |