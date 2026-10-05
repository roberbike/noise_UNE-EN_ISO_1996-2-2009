# Monitor de Ruido Ambiental — UNE-EN ISO 1996-2

Red distribuida de nodos de medida de ruido ambiental con salida por I2C. El
sistema admite **dos variantes de nodo sensor intercambiables** que comparten
la misma cadena de procesado (ponderaciones A y C, indicadores ISO 1996-2) y el
mismo protocolo I2C, de modo que para el nodo maestro son indistinguibles:

- **Nodo analógico** — ESP32-C3 + micrófono **MAX4466** (electret + ADC).
- **Nodo digital** — XIAO ESP32-S3 + micrófono MEMS I2S **ICS-43434** (recomendado).

Un nodo **maestro** (ESP32-S2/S3, típicamente integrado con CanAirIO) lee los
indicadores por I2C y los publica (InfluxDB/Grafana, MQTT, etc.).

> **Aviso normativo (resumen).** Este equipo es un instrumento de
> **monitorización y prevención**, útil para mapas de ruido y detección de
> tendencias. **No es un sonómetro certificado** de Clase 1/2 (IEC 61672-1) y
> sus datos no son legalmente vinculantes para sanciones sin verificación
> metrológica oficial. Ver [§ Normativa](#normativa-une-en-iso-1996-2-e-iec-61672-1).

---

## Índice

- [Arquitectura del sistema](#arquitectura-del-sistema)
- [Las dos placas: ESP32-C3 vs XIAO ESP32-S3](#las-dos-placas)
- [Los dos micrófonos: MAX4466 vs ICS-43434](#los-dos-micrófonos)
- [Qué significan las medidas](#qué-significan-las-medidas)
- [Normativa UNE-EN ISO 1996-2 e IEC 61672-1](#normativa-une-en-iso-1996-2-e-iec-61672-1)
- [Cableado](#cableado)
- [Protocolo I2C](#protocolo-i2c)
- [Compilar y flashear](#compilar-y-flashear-platformio)
- [Calibración y verificación](#calibración-y-verificación)
- [Ejemplos y documentación](#ejemplos-y-documentación)
- [English version](#english-version)

---

## Arquitectura del sistema

Cada nodo sensor es un esclavo I2C (dirección `0x08`) que muestrea el
micrófono, calcula los indicadores acústicos una vez por segundo y los deja en
una copia que el maestro lee bajo demanda. El código está organizado así:

- `src/main.cpp` — nodo ADC (ESP32-C3 + MAX4466): muestreo por polling a 16 kHz.
- `src/main_i2s.cpp` — nodo I2S (XIAO ESP32-S3 + ICS-43434): muestreo por DMA a 48 kHz.
- `src/SampleChain.h` — cadena por muestra, común a los dos nodos: eliminación
  de continua, ponderaciones A y C, envolventes Fast y Slow, pico y recortes.
- `src/NoiseAggregator.{h,cpp}` — matemática por segundo de ISO 1996-2, común:
  LAeq, máximos, L10/L90, Ld/Le/Ln, Lden y validez.
- `src/DSP_Engine.{h,cpp}` — coeficientes de las ponderaciones A y C a 16 y
  48 kHz, y la estructura `SensorData` que viaja por I2C.
- `src/MIC_I2S.{h,cpp}` — driver del micrófono digital I2S.
- `src/I2C_Comm.{h,cpp}` — protocolo esclavo I2C, contrato de estado,
  metadatos, hora y calibración persistente.
- `examples/` — maestro de referencia, firmwares de calibración/verificación y **calibración automática I2S**.

Cada nodo tiene dos tareas: una de muestreo, que pasa cada muestra por la
cadena común, y una agregadora, que una vez por segundo calcula los
indicadores y los publica para el maestro. Lo único específico de cada
plataforma es la adquisición (ADC frente a I2S) y la conversión de amplitud a
dB SPL, que cada nodo inyecta en el agregador. Un cambio en la lógica acústica
se aplica una sola vez.

---

## Las dos placas

| Característica | ESP32-C3 (nodo analógico) | XIAO ESP32-S3 (nodo digital) |
| :--- | :--- | :--- |
| Núcleo | RISC-V mononúcleo @ 160 MHz | Xtensa LX7 **doble núcleo** @ 240 MHz |
| FPU (coma flotante HW) | **No** (emulada por software) | **Sí** |
| Entrada de audio | ADC 12 bits (GPIO 4) | I2S digital (DMA) |
| Reparto de carga | Muestreo, DSP e I2C comparten el único núcleo | Audio anclado a un núcleo; I2C y radio en el otro |
| Puertos I2S | 1 | 2 |
| Uso en este proyecto | Micrófono analógico (MAX4466) | Micrófono digital I2S (ICS-43434) |

**Por qué cada placa con su micro.** La cadena de procesado es en coma
flotante. En el C3, sin FPU, se emula por software: a 16 kHz cabe (es lo que
hace el nodo analógico), pero el nodo digital trabaja a 48 kHz, el triple de
muestras. En el S3, con FPU y dos núcleos, la tarea de audio va anclada a un
núcleo sin competir con nada.

> El ICS-43434 **también funciona eléctricamente en el C3** (tiene un puerto
> I2S), pero este firmware no lo soporta: habría que adaptar `main_i2s.cpp`
> (sin el anclaje a núcleo) y comprobar la carga a 48 kHz, o bajar a 16 kHz.
> Para el nodo digital, el S3 es la placa adecuada.

---

## Los dos micrófonos

| Característica | MAX4466 (electret analógico) | ICS-43434 (MEMS digital I2S) |
| :--- | :--- | :--- |
| Tipo | Cápsula electret + preamplificador | MEMS con salida I2S de 24 bits |
| Salida | Analógica (se digitaliza en el ADC del ESP32) | Digital I2S (inmune al ruido de alimentación/ADC) |
| Suelo de ruido del nodo | **~58-60 dB** (limitado por ADC + previo) | **~34 dB** (medido en banco; datasheet: SNR 65 dBA) |
| Techo | ~97-109 dB según la ganancia (el ADC) | **~106 dB SPL** en las unidades de este proyecto |
| Respuesta en frecuencia | No plana; cae en graves (<100 Hz) y agudos (>10 kHz) | Plana (±1 dB típico) |
| Sensibilidad | Variable con la ganancia; **requiere calibrador** | Especificada (−26 dBFS @ 94 dB SPL), pero estas unidades leen **13,8 dB más**: el firmware lo corrige con un trim verificado |
| Coste | Muy bajo | Bajo |

**La diferencia práctica es el suelo de ruido.** El MAX4466 no puede medir por
debajo de ~58 dB: en zonas tranquilas o de noche, el nodo mide su propio ruido,
no el ambiente. El ICS-43434 baja ese suelo a ~34 dB, habilitando la medida de
noches urbanas reales. Por eso es el micrófono recomendado, y la razón de que
proyectos como el DNMS de sensor.community lo usen. Su límite es el techo: las
unidades de este proyecto entregan más nivel del que dice su hoja de datos y
llegan al fondo de escala digital a ~106 dB SPL en lugar de a 120.

> **Unidades de los campos lineales en el nodo I2S.** En `SensorData`, los
> campos heredados `noise`/`noiseAvg`/`noisePeak` llevan µFS (millonésimas del
> fondo de escala) en el nodo digital, no milivoltios: un micrófono digital no
> tiene tensión analógica. **Los campos en dB son la salida primaria** y tienen
> significado idéntico en ambos nodos.

---

## Qué significan las medidas

Los niveles se expresan en **dB(A)** (ponderación A, que imita la sensibilidad
del oído atenuando graves y agudos), salvo el pico, que va en dB(C). El
firmware calcula:

| Indicador | Campo `SensorData` | Qué es | Para qué sirve |
| :--- | :--- | :--- | :--- |
| **LAeq,1s** | `noiseAvgDb` | Nivel continuo **equivalente** en 1 s: la energía sonora media del segundo | Es la medida base; casi todo lo demás se deriva de ella |
| **LAFmax** | `noisePeakDb` | Nivel **máximo** con ponderación temporal *Fast* (125 ms) en el segundo | Eventos puntuales (un claxon, un portazo) |
| **LASmax** | `noiseLASmaxDb` | Nivel máximo con ponderación *Slow* (1 s) | Ruido de tráfico; menos sensible a transitorios |
| **LCpeak** | `noiseLCpeakDb` | **Pico** instantáneo con ponderación C, sin constante de tiempo | Ruido impulsivo (obras, impactos) |
| **…desde la última lectura** | `noiseLASmaxHoldDb`, `noiseLCpeakHoldDb` | Máximos de LASmax y LCpeak desde la lectura anterior del maestro | Que ningún impulso se pierda sea cual sea el periodo de sondeo |
| **L10** | `noiseAvgLegalDb` | Nivel superado el **10 %** del tiempo | Caracteriza los eventos ruidosos frecuentes |
| **L90** | `lowNoiseLevel` | Nivel superado el **90 %** del tiempo | Es el **ruido de fondo** real de la zona |
| **Ld / Le / Ln** | `Ld` `Le` `Ln` | Promedios energéticos de **día** (7-19 h), **tarde** (19-23 h) y **noche** (23-7 h) | Indicadores legales por franja horaria |
| **Lden** | `noiseLden` | Índice global día-tarde-noche, con penalización de **+5 dB** a la tarde y **+10 dB** a la noche | Indicador europeo de molestia (Directiva 2002/49/CE) |

Notas importantes para interpretar los datos:

- **LAeq,1s es el nivel del último segundo**, no el promedio del intervalo entre
  lecturas del maestro. Si el maestro lee cada 60 s, obtiene 1 segundo de cada
  60: válido como tendencia, no es el LAeq,60s.
- **L10 y L90 se calculan sobre una ventana deslizante** de los últimos
  `AGG_WINDOW_SEC` segundos (300 s por defecto), que avanza cada segundo. El
  maestro obtiene siempre el percentil de los últimos 300 s hasta su lectura,
  sea cual sea su periodo de sondeo.
- **Ld/Le/Ln y Lden son los del día de evaluación en curso, de 07:00 a 07:00**,
  para que la noche de 23:00 a 07:00 quede entera. A las 07:00 se reinician; el
  día completo es el último valor publicado antes de esa hora.
- **Lden solo promedia los periodos que ya tienen datos**, para que un periodo
  aún sin muestras no hunda el resultado.
- **La hora la envía el maestro en epoch LOCAL**, no UTC: el nodo no aplica
  zona horaria, así que las franjas día/tarde/noche se leen tal cual del epoch
  recibido. Enviar UTC desplaza todos los índices una o dos horas en España.
- **Ld/Le/Ln y Lden solo se calculan con hora sincronizada.** Hasta que el
  maestro envía la hora por I2C se quedan en 0,0 (comportamiento correcto, no
  un fallo). Ver el contrato en [docs/COMUNICACION.md](docs/COMUNICACION.md).
- **Corrección por ruido de fondo (UNE-EN ISO 1996-2).** Si el nivel medido está
  a menos de 10 dB del ruido de fondo, la norma exige corregir:
  `Lcorr = 10·log10(10^(Lmed/10) − 10^(Lfondo/10))`. Aquí es donde el suelo de
  ~34 dB del ICS-43434 marca la diferencia frente a los ~58 dB del MAX4466.

---

## Normativa UNE-EN ISO 1996-2 e IEC 61672-1

**Qué exige la normativa** (UNE-EN ISO 1996-2:2009 y Decreto 213/2012 del País
Vasco, o equivalentes como el Decreto 266/2004 de C. Valenciana):

- Niveles expresados en dB(A) → **ponderación A implementada y verificada**
  contra IEC 61672-1: ±0,15 dB hasta 7,9 kHz en el nodo de 16 kHz y ±0,08 dB de
  20 Hz a 20 kHz en el de 48 kHz. En los dos, la sección alta del filtro está
  ajustada por mínimos cuadrados porque la transformada bilineal directa se
  quedaba corta en agudos.
- Ponderación temporal Fast (125 ms) / Slow (1 s) → **ambas implementadas**
  (LAFmax y LASmax), más el **pico con ponderación C** (LCpeak) para ruido
  impulsivo, con la misma exactitud que la A.
- Indicadores LAeq, LAFmax, LASmax, LCpeak, Ld/Le/Ln, Lden, L10/L90 → **todos implementados**.
- Instrumento de **Clase 1** (medidas legales de precisión) o **Clase 2**
  (medidas de campo) según IEC 61672-1.

**Dónde queda este sistema.** El equipo **no tiene certificación de clase**. Con
el MAX4466 (cápsula electret + ADC de 12 bits) queda por debajo de Clase 2 por
respuesta en frecuencia y rango dinámico. Con el ICS-43434 mejora
sustancialmente (respuesta plana, mayor SNR, salida digital), y con protección
adecuada se aproxima a tolerancias de Clase 2, pero **la certificación formal
requiere ensayo en laboratorio acreditado**: no basta con el hardware.

**Uso legítimo:** monitorización preventiva, mapas de ruido, detección de
tendencias y pre-evaluación. **Uso no válido:** certificar superaciones de
límites legales en un procedimiento sancionador sin verificación metrológica
oficial del instrumento.

**Camino hacia Clase 2 de campo:** micrófono ICS-43434, pantalla antiviento de
espuma (≥60 mm, obligatoria en exterior; reduce hasta 20-30 dB de ruido de
viento), caja IP65/IP67, y validación cruzada 24 h contra un sonómetro Clase 1
(desviación objetivo < ±1,4 dB).

El análisis completo de conformidad y el plan de despliegue exterior están en
[docs/ESTUDIO_TECNICO.md](docs/ESTUDIO_TECNICO.md).

---

## Cableado

### Nodo analógico — ESP32-C3 + MAX4466

| Señal | Pin ESP32-C3 |
| :--- | :--- |
| MIC OUT (MAX4466) | GPIO 4 |
| VCC | 3.3 V |
| GND | GND |
| I2C SDA (al maestro) | GPIO 8 |
| I2C SCL (al maestro) | GPIO 10 |

La ganancia se ajusta con el potenciómetro del MAX4466 durante la calibración
(ver [docs/CALIBRACION.md](docs/CALIBRACION.md)).

### Nodo digital — XIAO ESP32-S3 + ICS-43434

![Módulo ICS-43434 (MRS179A): cara frontal y de pines](docs/images/ics43434_mrs179a.png)

Las etiquetas siguen la serigrafía del breakout **MRS179A** (foto). Otros
módulos rotulan `LRCL` como `WS`/`LRCLK`, `DOUT` como `SD` y `SEL` como `L/R`.

| Pin del módulo | Pin XIAO ESP32-S3 | Función |
| :--- | :--- | :--- |
| SEL | **GND** | Selección de canal: bajo = izquierdo (ver nota) |
| LRCL | GPIO 3 (D2) | Word select (WS / LRCLK) |
| DOUT | GPIO 4 (D3) | Salida de datos del micrófono |
| BCLK | GPIO 2 (D1) | Reloj de bit |
| GND | GND | Masa |
| 3V | 3.3 V | Alimentación (1.5-3.6 V, **nunca 5 V**) |
| I2C SDA (al maestro) | GPIO 5 (D4) | Dirección esclavo 0x08 |
| I2C SCL (al maestro) | GPIO 6 (D5) | |

**SEL debe ir a GND.** En este breakout `SEL` es el pin `L/R` de selección de
canal del ICS-43434, **no** un selector I2S/PDM como afirman algunas
descripciones de vendedor (el ICS-43434 no tiene modo PDM). Con
`I2S_CHANNEL_FMT_ONLY_LEFT` en el firmware:
- SEL a GND (o al aire, por el pull-down interno) → canal izquierdo → **el micro se lee** (verificado en banco, suelo ~34 dB).
- SEL a 3.3 V → canal derecho → el firmware descarta esa media trama → **no mide nada**.

Al aire funciona en la mesa por el pull-down interno, pero en despliegue de
campo **átalo a GND** (humedad y fugas pueden derivar un pin flotante). Si un
módulo futuro trae `L/R` fijado en alto y el nodo lee silencio, cambia a
`I2S_CHANNEL_FMT_ONLY_RIGHT` en `src/MIC_I2S.cpp`.

Buenas prácticas I2S: cables BCLK/WS/SD cortos (<10 cm: a 48 kHz, BCLK va a
3,072 MHz); en un latiguillo casero intercala el hilo de GND entre BCLK y DOUT.
Pines sobreescribibles con `-D MIC_I2S_BCLK=...` en `build_flags`; evita
GPIO 43/44 (UART del USB).

### Conexión I2C nodo ↔ maestro

SDA-SDA, SCL-SCL y **masa común entre ambas placas** (imprescindible aunque cada
una tenga su propia alimentación). La mayoría de placas ESP32 ya llevan
pull-ups; solo si el bus se cuelga, añade 4.7 kΩ de SDA y SCL a 3.3 V en un
único punto del bus. Para latiguillos de más de 20-30 cm, baja el clock del
maestro a 100 kHz.

Mapeo de pines del maestro de ejemplo: XIAO ESP32-S3 → SDA GPIO 5 / SCL GPIO 6;
Lolin S2 Mini → SDA GPIO 8 / SCL GPIO 9; cualquier otra placa ESP32 → los pines
I2C por defecto de su variante.

---

## Protocolo I2C

Dirección de esclavo: `0x08`. Cada lectura va precedida de una escritura de un
byte con el comando:

| Comando | Valor | Respuesta |
| :--- | :--- | :--- |
| `GET_STATUS` | `0x20` | 1 byte: `1` = último segundo válido, `0` = no publicar |
| `GET_STATUS` (legacy) | `0x00` | Igual (compatibilidad) |
| `GET_DATA` | `0x01` | `SensorData`: 88 bytes, terminados en un CRC-16 |
| `GET_METADATA` | `0x50` | `NodeMetadata` (16 bytes): versión, tipo de nodo, `time_synced`, `clip_count`, offset de calibración, llenado de la ventana L10/L90 y de Lden |
| `IDENTIFY` / `SET_TIME` | `0x09` | Identificación (5 bytes); con 4 bytes más (epoch LOCAL, uint32 LE) fija el reloj, que habilita Ld/Le/Ln/Lden |
| `SET_CALIB` | `0x0A` | + int16 LE en centésimas de dB: offset de calibración persistente (NVS) |
| `GET_DB` (legacy) | `0x10` | float de 4 bytes |
| `GET_RAW_MV` (legacy) | `0x30` | uint32 de 4 bytes |
| `GET_LMAX` (legacy) | `0x40` | float de 4 bytes |
| `GET_L10` (legacy) | `0x60` | float de 4 bytes |
| `GET_L90` (legacy) | `0x70` | float de 4 bytes |

- **Contrato de estado:** status `0` significa "no publiques", sea por
  arranque, fallo de micrófono, saturación o muestreo detenido. En un segundo
  inválido el nodo conserva los últimos valores válidos (no los pone a cero) y
  la señal es exclusivamente el status. Ld/Le/Ln/Lden sí valen 0 mientras su
  periodo no tenga datos.
- **Lectura en el maestro:** status → si es 1, datos → status otra vez.
  Publicar solo si llegaron los 88 bytes, el CRC cuadra, los dos status son 1
  y `cycles` ha cambiado respecto a la última trama buena.
- **`SET_CALIB` sustituye el offset guardado**, no se suma: para corregir un
  nodo hay que enviar `actual + (referencia − medido)`.
- El formato de `SensorData` solo crece por el final, con guardas de offset en
  el código, para no romper maestros antiguos.

Tabla de offsets, razonamiento de cada regla y casos límite en
[docs/COMUNICACION.md](docs/COMUNICACION.md).

---

## Compilar y flashear (PlatformIO)

**Nodo analógico** (ESP32-C3 + MAX4466): entorno `lolin_c3_mini`. Su
calibración va en `build_flags` (`-D CALIBRATION_RMS_MV=<mV>`).

**Nodo digital** (XIAO ESP32-S3 + ICS-43434): entorno `seeed_xiao_esp32s3`, que
ya trae `SAMPLE_RATE=48000` y el trim `MIC_OFFSET_DB=-13.71` de estas unidades.

El firmware compila con el core de Arduino 2.0.x (IDF 4.4) que instala la
plataforma `espressif32` de PlatformIO y con el core 3.x (IDF 5). Los avisos de
API I2S obsoleta del core 3.x están silenciados en `platformio.ini` (son
avisos, no errores).

**Maestro de ejemplo:** abrir `examples/i2c_master/`, entorno
`seeed_xiao_esp32s3` o `lolin_s2_mini`.

---

## Calibración y verificación

**Nodo MAX4466 (requiere calibrador).** Flashea `examples/calibration/`, ajusta
el potenciómetro con un calibrador a 94 dB hasta un RMS estable de 100-400 mV
sin recortes, y pon ese valor en `platformio.ini`:
`-D CALIBRATION_RMS_MV=<valor>`.

**Nodo ICS-43434.** El firmware ya trae el trim de estas unidades
(`MIC_OFFSET_DB=-13.71`), verificado con calibrador y contra un sonómetro de
referencia: con un calibrador de 94,0 dB marca 94,5-94,6 dB, por el
acoplamiento del calibrador al puerto MEMS, y en campo libre coincide con el
sonómetro. Para comprobar una unidad, `examples/calibration_i2s/`. Calibra con
la posición de 94 dB, nunca con 114: estas unidades saturan a ~106 dB.

**En los dos nodos**, el ajuste fino de una unidad concreta se hace sin
recompilar con `SET_CALIB` (función `calibrateNode()` del maestro de ejemplo),
que se guarda en la NVS del nodo y sobrevive a reflasheos; al recalibrar desde
cero, ponlo a 0.

Procedimiento completo, historia del trim y techo de medida en
[docs/CALIBRACION.md](docs/CALIBRACION.md).

---

## Ejemplos y documentación

| Recurso | Contenido |
| :--- | :--- |
| [examples/i2c_master/](examples/i2c_master/) | Maestro I2C de referencia: secuencia de lectura, CRC, metadatos, hora y calibración |
| [examples/calibration/](examples/calibration/) | Calibración del nodo MAX4466 (ADC) |
| [examples/calibration_i2s/](examples/calibration_i2s/) | Verificación del nodo ICS-43434 (I2S) |
| [docs/COMUNICACION.md](docs/COMUNICACION.md) | Protocolo I2C, contrato de estado, integración del maestro |
| [docs/CALIBRACION.md](docs/CALIBRACION.md) | Procedimiento de calibración de los dos nodos |
| [docs/ESTUDIO_TECNICO.md](docs/ESTUDIO_TECNICO.md) | Análisis técnico y de conformidad; despliegue exterior hacia Clase 2 |
| [docs/RAMA_48KHZ.md](docs/RAMA_48KHZ.md) | Por qué y cómo el nodo digital muestrea a 48 kHz |
| [tools/](tools/) | Generador y verificación de los coeficientes A y C |
| [CHANGELOG.md](CHANGELOG.md) | Cambios por versión |

---

## Licencia

GPL-3.0. Ver `LICENSE`.

---

## English version

# Environmental Noise Monitor — UNE-EN ISO 1996-2

A distributed network of environmental noise measurement nodes with I2C output. The system supports **two interchangeable sensor node variants** that share the same processing chain (A- and C-weighting, ISO 1996-2 indicators) and the same I2C protocol, so the master treats them as identical:

- **Analog node** — ESP32-C3 + **MAX4466** electret microphone (analog + ADC).
- **Digital node** — XIAO ESP32-S3 + **ICS-43434** MEMS I2S microphone (recommended).

A **master** node (ESP32-S2/S3, typically integrated with CanAirIO) reads the indicators over I2C and publishes them (InfluxDB/Grafana, MQTT, etc.).

> **Regulatory note (summary).** This device is a **monitoring and prevention** instrument, useful for noise maps and trend detection. It is **not a certified** Class 1/2 sound level meter (IEC 61672-1), and its data are not legally binding for sanctions without official metrological verification.

### System architecture

Each sensor node is an I2C slave (address `0x08`) that samples the microphone, computes the acoustic indicators once per second and keeps them in a copy the master reads on demand:

- `src/main.cpp` — ADC node (ESP32-C3 + MAX4466): polled sampling at 16 kHz.
- `src/main_i2s.cpp` — I2S node (XIAO ESP32-S3 + ICS-43434): DMA sampling at 48 kHz.
- `src/SampleChain.h` — per-sample chain shared by both nodes: DC removal, A- and C-weighting, Fast and Slow envelopes, peak and clipping.
- `src/NoiseAggregator.{h,cpp}` — shared per-second ISO 1996-2 math: LAeq, maxima, L10/L90, Ld/Le/Ln, Lden and validity.
- `src/DSP_Engine.{h,cpp}` — A- and C-weighting coefficients at 16 and 48 kHz, and the `SensorData` structure sent over I2C.
- `src/MIC_I2S.{h,cpp}` — I2S digital microphone driver.
- `src/I2C_Comm.{h,cpp}` — I2C slave protocol, status contract, metadata, clock and persistent calibration.
- `examples/` — reference master and calibration/verification firmware.

Each node runs a sampling task, which feeds every sample through the shared chain, and an aggregator task, which computes the indicators once per second and publishes them for the master. Only acquisition (ADC vs I2S) and the amplitude-to-SPL conversion are platform-specific.

### The two boards

The processing chain is floating point. The C3 has no FPU and emulates it in software: that fits at 16 kHz (the analog node does exactly that), but the digital node runs at 48 kHz, three times the samples. The S3 has an FPU and two cores, and its audio task is pinned to one of them. The C3 has one I2S port and the S3 two; the ICS-43434 would work electrically on the C3, but this firmware does not support it.

### The two microphones

| | MAX4466 (analog electret) | ICS-43434 (digital MEMS I2S) |
| :--- | :--- | :--- |
| Node noise floor | **~58-60 dB** (ADC + preamp) | **~34 dB** (bench-measured; datasheet SNR 65 dBA) |
| Ceiling | ~97-109 dB depending on gain (the ADC) | **~106 dB SPL** on this project's units |
| Sensitivity | Depends on gain; **needs a calibrator** | Specified (−26 dBFS @ 94 dB SPL), but these units read **13.8 dB hotter**; the firmware corrects it with a verified trim |

The practical difference is the noise floor: the MAX4466 cannot measure quiet nights, the ICS-43434 can. Its limit is the ceiling. On the I2S node the linear fields (`noise`, `noiseAvg`, `noisePeak`) carry µFS instead of mV; the dB fields mean the same on both nodes.

### Meaning of the measurements

Levels are in **dB(A)**, except the peak, in dB(C):

- **LAeq,1s** (`noiseAvgDb`) — equivalent continuous level of the last second.
- **LAFmax** (`noisePeakDb`) and **LASmax** (`noiseLASmaxDb`) — maxima with Fast (125 ms) and Slow (1 s) time weighting.
- **LCpeak** (`noiseLCpeakDb`) — instantaneous C-weighted peak, for impulsive noise.
- `noiseLASmaxHoldDb`, `noiseLCpeakHoldDb` — maxima since the master's previous read, so no impulse is lost whatever the polling period.
- **L10 / L90** — levels exceeded 10 % / 90 % of the time, over a 300 s sliding window that advances every second.
- **Ld / Le / Ln** — day (7-19 h), evening (19-23 h) and night (23-7 h) energy averages of the current evaluation day, which runs **07:00 to 07:00** so the night stays whole; they reset at 07:00.
- **Lden** — day-evening-night index with +5 dB evening and +10 dB night penalties, averaged over the periods that already have data.

The master must send the clock as a **LOCAL** epoch: the node applies no timezone, and until it gets a clock Ld/Le/Ln/Lden stay 0.

### Standards

A-weighting is verified against IEC 61672-1 within ±0.15 dB up to 7.9 kHz on the 16 kHz node and within ±0.08 dB from 20 Hz to 20 kHz on the 48 kHz node; C-weighting likewise. Fast and Slow weighting and the C-weighted peak are implemented. The system has **no class certification**; with the ICS-43434 and proper outdoor protection it can approach Class 2 tolerances, but certification needs an accredited laboratory. Details and the outdoor deployment plan in [docs/ESTUDIO_TECNICO.md](docs/ESTUDIO_TECNICO.md).

### Wiring

- Analog node: MAX4466 OUT → GPIO 4, VCC 3.3 V, GND; I2C SDA GPIO 8, SCL GPIO 10.
- Digital node: ICS-43434 SEL → **GND**, LRCL → GPIO 3 (D2), DOUT → GPIO 4 (D3), BCLK → GPIO 2 (D1), 3V → 3.3 V (never 5 V); I2C SDA GPIO 5 (D4), SCL GPIO 6 (D5). `SEL` is the `L/R` channel pin, not an I2S/PDM selector; tied to 3.3 V the node measures nothing. Keep the I2S wires short: BCLK runs at 3.072 MHz.
- Node ↔ master: SDA-SDA, SCL-SCL and a **common ground**; 4.7 kΩ pull-ups only if the bus hangs.

### I2C protocol

Slave address `0x08`; commands `GET_STATUS` (`0x20`, legacy `0x00`), `GET_DATA` (`0x01`, 88 bytes ending in a CRC-16), `GET_METADATA` (`0x50`, 16 bytes), `IDENTIFY`/`SET_TIME` (`0x09`, + uint32 LE local epoch), `SET_CALIB` (`0x0A`, + int16 LE hundredths of dB) and the legacy single-value reads. Read status → data if status is 1 → status again; publish only if all 88 bytes arrived, the CRC matches, both status reads are 1 and `cycles` changed. `SET_CALIB` replaces the stored offset rather than adding to it. Full details in [docs/COMUNICACION.md](docs/COMUNICACION.md) (Spanish).

### Build and flash (PlatformIO)

- `lolin_c3_mini` — analog node; its calibration goes in `build_flags` (`-D CALIBRATION_RMS_MV=<mV>`).
- `seeed_xiao_esp32s3` — digital node, with `SAMPLE_RATE=48000` and these units' `MIC_OFFSET_DB=-13.71`.

It builds on the Arduino core 2.0.x (IDF 4.4) shipped with PlatformIO's `espressif32` platform and on core 3.x (IDF 5).

### Calibration

- **MAX4466:** flash `examples/calibration/`, set the gain with a 94 dB calibrator until RMS is stable at 100-400 mV with no clipping, and put the value in `platformio.ini` (`-D CALIBRATION_RMS_MV=<value>`).
- **ICS-43434:** the firmware ships the trim of these units, verified with a calibrator and against a reference meter (a 94.0 dB calibrator reads 94.5-94.6 dB because of its coupling to the MEMS port; in free field it matches the meter). Check a unit with `examples/calibration_i2s/`. Calibrate at 94 dB, never 114: these units saturate at ~106 dB.
- **Both:** fine-tune a single unit with `SET_CALIB` (`calibrateNode()` in the example master), stored in NVS and kept across reflashing; reset it to 0 when calibrating from scratch.

Full procedure in [docs/CALIBRACION.md](docs/CALIBRACION.md) (Spanish).

### License

GPL-3.0. See `LICENSE`.
