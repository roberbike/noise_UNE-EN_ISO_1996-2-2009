# Monitor de Ruido Ambiental — UNE-EN ISO 1996-2

Red distribuida de nodos de medida de ruido ambiental con salida por I2C. El
sistema admite **dos variantes de nodo sensor intercambiables** que comparten
la misma cadena de procesado (ponderación A, indicadores ISO 1996-2) y el mismo
protocolo I2C, de modo que para el nodo maestro son indistinguibles:

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

---

## Arquitectura del sistema

Cada nodo sensor es un esclavo I2C (dirección `0x08`) que muestrea el
micrófono, calcula los indicadores acústicos una vez por segundo y los mantiene
en una caché que el maestro lee bajo demanda. El código está organizado así:

- `src/NoiseAggregator.{h,cpp}` — toda la matemática ISO 1996-2 (común a ambos nodos).
- `src/main.cpp` — nodo ADC (ESP32-C3 + MAX4466): muestreo por polling a 16 kHz.
- `src/main_i2s.cpp` — nodo I2S (XIAO ESP32-S3 + ICS-43434): muestreo por DMA.
- `src/MIC_I2S.{h,cpp}` — driver del micrófono digital I2S.
- `src/DSP_Engine.{h,cpp}` — filtros de ponderación A (biquads a 16 kHz) y tipos compartidos.
- `src/I2C_Comm.{h,cpp}` — protocolo esclavo I2C, contrato de estado y metadatos.
- `examples/` — firmware maestro y firmwares de calibración/verificación.

El único código específico de plataforma es la tarea de muestreo (ADC vs I2S) y
la conversión de amplitud a dB SPL, que cada nodo inyecta en el agregador
común. Un cambio en la lógica acústica se aplica una sola vez.

---

## Las dos placas

| Característica | ESP32-C3 (nodo analógico) | XIAO ESP32-S3 (nodo digital) |
| :--- | :--- | :--- |
| Núcleo | RISC-V mononúcleo @ 160 MHz | Xtensa LX7 **doble núcleo** @ 240 MHz |
| FPU (coma flotante HW) | **No** (emulada por software) | **Sí** |
| Entrada de audio | ADC 12 bits (GPIO 4) | I2S digital (DMA) |
| Reparto de carga | DSP + I2C comparten el único núcleo | DSP en un núcleo, radio/I2C en el otro |
| Puertos I2S | 1 | 1 |
| Uso recomendado | Micrófono analógico (MAX4466) | Micrófono digital I2S (ICS-43434) |

**Por qué cada placa con su micro.** El nodo digital exige procesar audio en
coma flotante de forma continua; en el S3 (con FPU y doble núcleo) la tarea de
audio va anclada a un núcleo sin competir con nada. En el C3 (sin FPU) esa misma
cadena se emula por software y va al límite, por eso se reserva para la entrada
analógica por ADC, más ligera.

> El ICS-43434 **también funciona eléctricamente en el C3** (tiene puerto I2S),
> pero el DSP en coma flotante a 16 kHz queda muy justo sin FPU. Si se necesita
> I2S en el C3, habría que bajar el muestreo o reescribir los filtros en punto
> fijo. Para el nodo digital, el S3 es la placa adecuada.

---

## Los dos micrófonos

| Característica | MAX4466 (electret analógico) | ICS-43434 (MEMS digital I2S) |
| :--- | :--- | :--- |
| Tipo | Cápsula electret + preamplificador | MEMS con salida I2S de 24 bits |
| Salida | Analógica (se digitaliza en el ADC del ESP32) | Digital I2S (inmune al ruido de alimentación/ADC) |
| Suelo de ruido del nodo | **~58-60 dB** (limitado por ADC + previo) | **~34 dB** (medido; datasheet ~33 dBA, SNR 64 dBA) |
| Respuesta en frecuencia | No plana; cae en graves (<100 Hz) y agudos (>10 kHz) | Plana (±1 dB típico) |
| Sensibilidad | Variable con ganancia; **requiere calibrador** | **De fábrica**: -26 dBFS @ 94 dB SPL (calibración opcional) |
| Coste | Muy bajo | Bajo |

**La diferencia práctica es el suelo de ruido.** El MAX4466 no puede medir por
debajo de ~58 dB: en zonas tranquilas o de noche, el nodo mide su propio ruido,
no el ambiente. El ICS-43434 baja ese suelo a ~34 dB, habilitando la medida de
noches urbanas reales. Por eso es el micrófono recomendado, y la razón de que
proyectos como el DNMS de sensor.community lo usen.

> **Unidades del campo "mV" en el nodo I2S.** En `SensorData`, los campos
> heredados `noise`/`noiseAvg`/`noisePeak` llevan µFS (micro-fracciones de fondo
> de escala) en el nodo digital, no milivoltios (un micrófono digital no tiene
> tensión analógica). **Los campos en dB son la salida primaria** y tienen
> significado idéntico en ambos nodos.

---

## Qué significan las medidas

Todos los niveles se expresan en **dB(A)** (ponderación A, que imita la
sensibilidad del oído humano atenuando graves y agudos). El firmware calcula:

| Indicador | Campo `SensorData` | Qué es | Para qué sirve |
| :--- | :--- | :--- | :--- |
| **LAeq,1s** | `noiseAvgDb` | Nivel continuo **equivalente** en 1 s: la energía sonora media del segundo | Es la medida base; casi todo lo demás se deriva de ella |
| **LAFmax** | `noisePeakDb` | Nivel **máximo** con ponderación temporal *Fast* (125 ms) en el intervalo | Detectar eventos puntuales (un claxon, un portazo) |
| **L10** | `noiseAvgLegalDb` | Nivel superado el **10 %** del tiempo | Caracteriza los eventos ruidosos / picos frecuentes |
| **L90** | `lowNoiseLevel` | Nivel superado el **90 %** del tiempo | Es el **ruido de fondo** real de la zona |
| **Ld / Le / Ln** | `Ld` `Le` `Ln` | Promedios energéticos de **día** (7-19 h), **tarde** (19-23 h) y **noche** (23-7 h) | Indicadores legales por franja horaria |
| **Lden** | `noiseLden` | Índice global día-tarde-noche, con penalización de **+5 dB** a la tarde y **+10 dB** a la noche | Indicador europeo de molestia (Directiva 2002/49/CE) |

Notas importantes para interpretar los datos:

- **LAeq,1s es el nivel del último segundo**, no el promedio del intervalo entre
  lecturas del maestro. Si el maestro lee cada 60 s, obtiene 1 segundo de cada
  60: válido como tendencia, no es el LAeq,60s.
- **L10 y L90 se recalculan cada bloque completo de 20 s** y se mantienen
  constantes entre bloques (no es un error: es su cadencia de actualización).
- **Ld/Le/Ln y Lden solo se calculan con hora sincronizada.** Hasta que el
  maestro envía la hora por I2C, Lden se queda en 0.0 (comportamiento correcto,
  no un fallo). Ver el contrato en [docs/COMUNICACION.md](docs/COMUNICACION.md).
- **Corrección por ruido de fondo (UNE-EN ISO 1996-2).** Si el nivel medido está
  a menos de 10 dB del ruido de fondo, la norma exige corregir:
  `Lcorr = 10·log10(10^(Lmed/10) − 10^(Lfondo/10))`. Aquí es donde el suelo de
  ~34 dB del ICS-43434 marca la diferencia frente a los ~58 dB del MAX4466.

---

## Normativa UNE-EN ISO 1996-2 e IEC 61672-1

**Qué exige la normativa** (UNE-EN ISO 1996-2:2009 y Decreto 213/2012 del País
Vasco, o equivalentes como el Decreto 266/2004 de C. Valenciana):

- Niveles expresados en dB(A) → **ponderación A: implementada** (biquads IIR a 16 kHz).
- Ponderación temporal Fast (125 ms) / Slow (1 s) → **Fast implementada** (LAFmax).
- Indicadores LAeq, Ld/Le/Ln, Lden, L10/L90 → **todos implementados**.
- Instrumento de **Clase 1** (medidas legales de precisión) o **Clase 2**
  (medidas de campo) según IEC 61672-1.

**Dónde queda este sistema.** El equipo **no tiene certificación de clase**. Con
el MAX4466 (cápsula electret + ADC de 12 bits) queda por debajo de Clase 2 por
respuesta en frecuencia y rango dinámico. Con el ICS-43434 mejora
sustancialmente (respuesta plana, mayor SNR, salida digital), y con protección
adecuada se aproxima a tolerancias de Clase 2, pero **la certificación formal
requiere ensayo en laboratorio acreditado** — no basta con el hardware.

**Uso legítimo:** monitorización preventiva, mapas de ruido, detección de
tendencias y pre-evaluación. **Uso no válido:** certificar superaciones de
límites legales en un procedimiento sancionador sin verificación metrológica
oficial del instrumento.

**Camino hacia Clase 2 de campo** (ver [docs/SMART_CITY_CLASE_2.md](docs/SMART_CITY_CLASE_2.md)):
micrófono ICS-43434, pantalla antiviento de espuma (≥60 mm, obligatoria en
exterior; reduce hasta 20-30 dB de ruido de viento), caja IP65/IP67, y
validación cruzada 24 h contra un sonómetro Clase 1 (desviación objetivo < ±1.4 dB).

El análisis completo de conformidad está en [docs/ESTUDIO_TECNICO.md](docs/ESTUDIO_TECNICO.md).

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

Buenas prácticas I2S: cables BCLK/WS/SD cortos (<10 cm, BCLK va a ~1 MHz); en un
latiguillo casero intercala el hilo de GND entre BCLK y DOUT. Pines
sobreescribibles con `-D MIC_I2S_BCLK=...` en `build_flags`; evita GPIO 43/44
(UART del USB).

### Conexión I2C nodo ↔ maestro

SDA-SDA, SCL-SCL y **masa común entre ambas placas** (imprescindible aunque cada
una tenga su propia alimentación). La mayoría de placas ESP32 ya llevan
pull-ups; solo si el bus se cuelga, añade 4.7 kΩ de SDA y SCL a 3.3 V en un
único punto del bus. Para latiguillos de más de 20-30 cm, baja el clock del
maestro a 100 kHz.

Mapeo de pines del maestro de ejemplo: XIAO ESP32-S3 → SDA GPIO 5 / SCL GPIO 6;
Lolin S2 Mini → SDA GPIO 8 / SCL GPIO 9.

---

## Protocolo I2C

Dirección de esclavo: `0x08`. Comandos (1 byte):

| Comando | Valor | Respuesta |
| :--- | :--- | :--- |
| `GET_STATUS` | `0x20` | 1 byte: `1` = OK y datos listos, `0` = no publicar |
| `GET_STATUS` (legacy) | `0x00` | Igual (compatibilidad) |
| `GET_DATA` | `0x01` | Estructura `SensorData` completa |
| `GET_METADATA` | `0x50` | `NodeMetadata` (7 bytes): versión fw, tipo de nodo, time_synced, clip_count |
| `GET_DB` (legacy) | `0x10` | float de 4 bytes |
| `GET_RAW_MV` (legacy) | `0x30` | uint32 de 4 bytes |
| `GET_LMAX` (legacy) | `0x40` | float de 4 bytes |
| `GET_L10` (legacy) | `0x60` | float de 4 bytes |
| `GET_L90` (legacy) | `0x70` | float de 4 bytes |

- `0x09` responde identificación y también acepta "set time" (epoch) con 4 bytes
  adicionales; ese set-time es lo que habilita Ld/Le/Ln/Lden.
- **Contrato de estado:** status `0` significa "no publiques", sea por arranque,
  fallo de micrófono, saturación (clipping) o muestreo detenido. El nodo nunca
  publica ceros: en segundos inválidos conserva el último valor válido.
- **Validación en el maestro:** antes de publicar, exigir lectura completa +
  status == 1 + `cycles` avanzando respecto a la lectura anterior. Detalle y
  razonamiento en [docs/COMUNICACION.md](docs/COMUNICACION.md).

---

## Compilar y flashear (PlatformIO)

---

## English version

# Environmental Noise Monitor — UNE-EN ISO 1996-2

A distributed network of environmental noise measurement nodes with I2C output. The system supports two interchangeable sensor node variants that share the same signal-processing chain (A-weighting, ISO 1996-2 indicators) and the same I2C protocol, so the master node treats them as identical:

- Analog node — ESP32-C3 + **MAX4466** electret microphone (analog + ADC)
- Digital node — XIAO ESP32-S3 + **ICS-43434** MEMS I2S microphone (recommended)

A **master** node (ESP32-S2/S3, typically integrated with CanAirIO) reads the indicators over I2C and publishes them (InfluxDB/Grafana, MQTT, etc.).

> Regulatory note (summary): this device is a monitoring and prevention instrument for noise maps and trend detection. It is not a certified Class 1/2 sound level meter (IEC 61672-1), and the data are not legally binding for sanctions without official metrological verification. See the normative section below.

## System architecture

Each sensor node is an I2C slave (address `0x08`) that samples the microphone, calculates acoustic indicators once per second, and keeps them in a cache that the master reads on demand. The code is organized as follows:

- `src/NoiseAggregator.{h,cpp}` — all ISO 1996-2 math, shared by both node types
- `src/main.cpp` — ADC node (ESP32-C3 + MAX4466): polling-based sampling at 16 kHz
- `src/main_i2s.cpp` — I2S node (XIAO ESP32-S3 + ICS-43434): DMA sampling
- `src/MIC_I2S.{h,cpp}` — digital I2S microphone driver
- `src/DSP_Engine.{h,cpp}` — A-weighting filters (biquads at 16 kHz) and shared types
- `src/I2C_Comm.{h,cpp}` — I2C slave protocol, status contract and metadata
- `examples/` — master firmware and calibration/verification firmware

The only platform-specific code is the sampling task (ADC vs I2S) and the amplitude-to-SPL conversion, which each node injects into the common aggregator. A change in acoustic logic is implemented in one place only.

## Why the two boards

The digital node must process audio in floating point continuously, which the S3 handles well thanks to the FPU and dual-core architecture. The C3 has no hardware FPU and must emulate the chain in software, which is why it is reserved for the lighter analog ADC input path.

## Practical difference between microphones

The MAX4466 cannot measure below approximately 58–60 dB, while the ICS-43434 lowers the floor to around 34 dB, enabling real urban night measurements. This is why the digital MEMS microphone is the recommended option.

## Meaning of the measurements

All levels are expressed in **dB(A)**. The firmware calculates:

- **LAeq,1s** — equivalent continuous level in 1 second
- **LAFmax** — maximum level with Fast weighting in the interval
- **L10 / L90** — percentile-based noise characterization
- **Ld / Le / Ln** — day/evening/night energy averages
- **Lden** — day-evening-night global index with the standard penalty factors

The details and formulas are documented in the Spanish sections above, while the code and the example firmware implement the same behavior for both node types.

## Build and flash (PlatformIO)

Use PlatformIO with the project environment matching the target board:

- `lolin_c3_mini` for the analog node
- `seeed_xiao_esp32s3` for the digital node

Then build and upload the firmware to the selected board. The project already includes the needed configuration and compile flags in [platformio.ini](platformio.ini).

---


**Nodo analógico** (ESP32-C3 + MAX4466): entorno `lolin_c3_mini`.

**Nodo digital** (XIAO ESP32-S3 + ICS-43434): entorno `seeed_xiao_esp32s3`.
Ajuste fino opcional frente a un sonómetro de referencia: `-D MIC_OFFSET_DB=<dB>`.
Los avisos de API I2S obsoleta en core Arduino 3.x / IDF 5.x están silenciados
en el `platformio.ini` (son avisos, no errores).

**Maestro de ejemplo:** abrir `examples/i2c_master/`, entorno
`seeed_xiao_esp32s3` o `lolin_s2_mini`.

---

## Calibración y verificación

**Nodo MAX4466 (requiere calibrador).** Flashea `examples/calibration/`, ajusta
el potenciómetro con un calibrador a 94 dB, anota el RMS (mV) estable y cópialo
en `CALIBRATION_RMS_MV` de `src/main.cpp`. Procedimiento completo en
[docs/CALIBRACION.md](docs/CALIBRACION.md).

**Nodo ICS-43434 (calibración opcional).** La sensibilidad de fábrica
(-26 dBFS @ 94 dB SPL) hace que el LAeq salga directo del dBFS sin calibrador;
las medidas de banco caen en ~34 dB, coherente con el datasheet. Flashea
`examples/calibration_i2s/` para verificar; solo si con calibrador a 94 dB el
LAeq difiere de 94.0, define `-D MIC_OFFSET_DB=<94.0 − LAeq_medido>`.

---

## Ejemplos y documentación

| Recurso | Contenido |
| :--- | :--- |
| [examples/i2c_master/](examples/i2c_master/) | Maestro I2C de referencia con validación triple y lectura de metadatos |
| [examples/calibration/](examples/calibration/) | Verificación/calibración del nodo MAX4466 (ADC) |
| [examples/calibration_i2s/](examples/calibration_i2s/) | Verificación del nodo ICS-43434 (I2S) |
| [docs/COMUNICACION.md](docs/COMUNICACION.md) | Protocolo I2C, contrato de estado, integración del maestro |
| [docs/CALIBRACION.md](docs/CALIBRACION.md) | Procedimiento de calibración y normativa |
| [docs/ESTUDIO_TECNICO.md](docs/ESTUDIO_TECNICO.md) | Análisis técnico y de conformidad legal |
| [docs/SMART_CITY_CLASE_2.md](docs/SMART_CITY_CLASE_2.md) | Requisitos para despliegue exterior Clase 2 |

---

## Licencia

GPL-3.0. Ver `LICENSE`.
