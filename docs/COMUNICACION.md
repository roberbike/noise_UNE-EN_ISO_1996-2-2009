# Comunicación, inicialización y obtención de datos del sensor de ruido

## 1) Visión general

- El firmware implementa un nodo sensor (esclavo I2C, dirección `0x08`) que publica métricas acústicas agregadas por segundo.
- La adquisición puede hacerse desde un micrófono analógico MAX4466 (ADC a 16 kHz, ESP32-C3) o digital ICS-43434 (I2S a 48 kHz, XIAO ESP32-S3). Las dos variantes comparten la cadena de procesado: ponderación A y C, ponderaciones temporales Fast y Slow, LAeq, LAFmax, LASmax, LCpeak, L10, L90, Ld/Le/Ln y Lden.
- Ambas exponen el mismo protocolo I2C y la misma estructura `SensorData`: para el maestro son indistinguibles, salvo las unidades de los campos lineales (§7).

## 2) Inicialización de comunicaciones (lado sensor)

### I2C (esclavo)
- Función: `I2C_Comm_Init()` en [src/I2C_Comm.cpp](../src/I2C_Comm.cpp).
- Carga de NVS el offset de calibración y lo descarta (y borra) si está fuera de ±30 dB (§4, calibración).
- Configura pines con `Wire.setPins(I2C_SDA, I2C_SCL)` y registra los callbacks `Wire.onReceive(receiveEvent)` y `Wire.onRequest(requestEvent)` **antes** de `Wire.begin(addr)`, para que la primera transacción ya los encuentre.
- Pines por defecto: SDA=8/SCL=10 en el C3; SDA=5/SCL=6 (D4/D5) en el XIAO S3. Sobreescribibles por `build_flags`.
- El esclavo no fija el clock del bus (lo controla el maestro).

### I2S (micrófono digital)
- `MIC_I2S_Init()` en [src/MIC_I2S.cpp](../src/MIC_I2S.cpp) instala el driver I2S heredado (24 bits en trama de 32, canal izquierdo, 48 kHz en el entorno S3), fija pines (BCLK=2, WS=3, SD=4) y buffers DMA de 768 frames. La lectura es bloqueante con `i2s_read()`. BCLK va a 48000 × 32 × 2 = 3,072 MHz.

#### Cableado del módulo ICS-43434 (breakout MRS179A)

![Módulo ICS-43434 MRS179A](images/ics43434_mrs179a.png)

| Pin del módulo | XIAO ESP32-S3 | Función |
| :--- | :--- | :--- |
| SEL | **GND** | Selección de canal: bajo = izquierdo |
| LRCL | GPIO 3 (D2) | Word select (WS / LRCLK) |
| DOUT | GPIO 4 (D3) | Salida de datos del micrófono |
| BCLK | GPIO 2 (D1) | Reloj de bit |
| GND | GND | Masa |
| 3V | 3.3V | Alimentación (1.5-3.6 V, nunca 5 V) |

**SEL debe ir a GND.** En este breakout `SEL` es el pin `L/R` (selección de
canal) del ICS-43434, pese a que algunas descripciones de vendedor lo presenten
como selector I2S/PDM: el ICS-43434 no tiene modo PDM. Con
`I2S_CHANNEL_FMT_ONLY_LEFT` en el firmware: SEL bajo (o al aire, por el
pull-down interno) → canal izquierdo → se lee correctamente (verificado en
banco, suelo de ~34 dB); SEL a 3.3 V → canal derecho → el firmware descarta esa
media trama → **no mide nada**. Al aire funciona pero depende de un pull-down
débil: en despliegue de campo, atarlo a masa.

Otros breakouts pueden etiquetar `LRCL` como `WS`/`LRCLK`, `DOUT` como `SD` y
`SEL` como `L/R`. Si un módulo tiene `L/R` fijado a nivel alto internamente y el
nodo lee silencio, cambiar a `I2S_CHANNEL_FMT_ONLY_RIGHT` en `MIC_I2S.cpp`.

#### Conexión I2C con el maestro (nodo I2S)

SDA = GPIO 5 (D4), SCL = GPIO 6 (D5), dirección `0x08`. SDA-SDA, SCL-SCL y
**masa común entre ambas placas**, imprescindible aunque cada una tenga su
propia alimentación. La mayoría de placas ESP32 ya llevan pull-ups; solo si el
bus falla o se cuelga, añadir 4.7 kΩ de SDA y SCL a 3.3 V en un único punto del
bus. Para latiguillos de más de 20-30 cm, bajar el clock del maestro a 100 kHz.
Pines sobreescribibles con `-D I2C_SDA=x -D I2C_SCL=y`; evitar GPIO 43/44 (UART
del USB) y dejar libres GPIO 2/3/4 para el micrófono.

#### Core de Arduino y avisos de compilación

El firmware compila con el core 2.0.x (IDF 4.4) que trae la plataforma
`espressif32` de PlatformIO y con el core 3.x (IDF 5). En IDF 5 el driver
`driver/i2s.h` está marcado como obsoleto y `dma_buf_count`/`dma_buf_len` son
alias de `dma_desc_num`/`dma_frame_num`: son avisos, no errores, y el entorno S3
de `platformio.ini` ya los silencia (`-D CONFIG_I2S_SUPPRESS_DEPRECATE_WARN=1`,
`-Wno-deprecated-declarations`). Migrar a la API nueva (`driver/i2s_std.h`)
ataría el proyecto al core 3.x; la API heredada funciona en los dos.

### ADC (micrófono analógico)
- En [src/main.cpp](../src/main.cpp) se configura el ADC con `adc1_config_channel_atten()` (atenuación de 11/12 dB: el mismo ajuste, que hasta IDF 4.4.6 se llama `ADC_ATTEN_DB_11` y desde IDF 4.4.7 `ADC_ATTEN_DB_12`) y `esp_adc_cal_characterize()`, y se deriva una pendiente `adc_mv_per_count` (solo pendiente, sin offset) para convertir amplitudes AC en float.

## 3) Flujo de obtención de datos (lado sensor)

- **Muestreo**: `sampling_task()`. En el nodo ADC, polling a 16 kHz exactos con `adc1_get_raw()` (periodo de 62/63 µs alternados, comprobación wrap-safe de `micros()`); en el nodo I2S, bloqueo en `MIC_I2S_Read()`, que marca el ritmo del DMA.
- **Cadena por muestra** ([src/SampleChain.h](../src/SampleChain.h), común a los dos nodos): eliminación de continua, ponderación A (3 biquads) y C (2 biquads, [src/DSP_Engine.cpp](../src/DSP_Engine.cpp)), envolventes Fast (125 ms) y Slow (1 s), pico C, pico de entrada y recuento de muestras recortadas. Al arrancar, medio segundo pasa por los filtros sin acumularse, para que su transitorio no llegue a ningún segundo publicado.
- **Agregación por segundo** ([src/NoiseAggregator.cpp](../src/NoiseAggregator.cpp)): validez del segundo, LAeq, LAFmax, LASmax, LCpeak, ventana deslizante L10/L90 y acumulación por periodos (Ld 7-19 h, Le 19-23 h, Ln 23-7 h) con Lden cuando hay hora válida.
- **Entrega**: cada segundo el agregador llama a `I2C_Comm_Publish()`, que copia el `SensorData` y su status bajo un spinlock; `requestEvent()` sirve una instantánea atómica de esa copia.

## 4) Protocolo y comandos

Definidos en [src/I2C_Comm.h](../src/I2C_Comm.h). Cada lectura va precedida de
una escritura de 1 byte con el comando:

| Comando | Código | Respuesta |
| :--- | :--- | :--- |
| `CMD_GET_STATUS` | `0x20` | 1 byte: 1 = el último segundo es válido, 0 = no publicar (ver contrato) |
| `CMD_GET_STATUS_LEGACY` | `0x00` | Igual que `0x20` (compatibilidad) |
| `CMD_GET_DATA` | `0x01` | `SensorData`: 88 bytes con CRC-16 al final |
| `CMD_GET_METADATA` | `0x50` | `NodeMetadata`: 16 bytes |
| `CMD_IDENTIFY` | `0x09` | 5 bytes de identificación (`01 02 01 01 08`) |
| `CMD_SET_TIME_LEGACY` | `0x09` + uint32 LE | Fija el reloj del nodo (epoch LOCAL). Sin respuesta |
| `CMD_SET_CALIB` | `0x0A` + int16 LE | Fija el offset de calibración (centésimas de dB). Sin respuesta |
| `CMD_LEGACY_GET_DB` | `0x10` | float: LAeq,1s |
| `CMD_LEGACY_GET_RAW_MV` | `0x30` | uint32: `noise` |
| `CMD_LEGACY_GET_LMAX` | `0x40` | float: LAFmax |
| `CMD_LEGACY_GET_L10` | `0x60` | float: L10 |
| `CMD_LEGACY_GET_L90` | `0x70` | float: L90 |

Un comando desconocido responde un único byte 0.

**Contrato del byte de estado.** Status 0 significa "no publiques este dato", sea cual sea la causa:
- Arranque: aún no ha aterrizado el primer segundo (`data_ready == 0`), así que nunca se sirve la estructura a ceros del arranque como válida.
- Fallo de micrófono: bias fuera de rango (nodo ADC) o silencio absoluto por debajo del suelo del micrófono (nodo I2S).
- Saturación: más de 10 muestras recortadas en el segundo (en los dos nodos; §7).
- Segundo inválido: RMS por debajo del umbral mínimo.
- **Muestreo detenido** (desde v3.1.2): si el agregador no recibe ningún segundo completo en 2 s, baja el status a 0, congela `cycles` y emite `[WARN] No samples for 2 s` por Serial. El watchdog reinicia el chip si persiste. Un nodo colgado se ve como fallo, nunca como dato plano creíble.

En un segundo inválido el nodo conserva en la estructura los últimos valores
válidos (no los pone a cero) y la señal de invalidez es exclusivamente el
status. Ld/Le/Ln y Lden sí valen 0 legítimamente mientras su periodo no tenga
datos: sin hora sincronizada, y en los primeros segundos de cada día de
evaluación (§7).

### Estructura `SensorData` (88 bytes, little-endian)

**Regla del formato de cable: solo se añade al final.** Los maestros mantienen
su propia copia de `SensorData` y la leen como un bloque de bytes, así que el
desplazamiento de cada campo forma parte del protocolo. Un campo nuevo va
**siempre al final**: insertarlo en medio desplaza todo lo posterior y un
maestro con la definición antigua no falla, sino que lee valores equivocados en
silencio (eso pasó en la 3.3.0). Un maestro que pida solo el tamaño antiguo
(68 bytes, layout 3.2.x) sigue recibiendo exactamente lo que espera. Los
`static_assert` de `DSP_Engine.h` rompen la compilación si alguien inserta un
campo en medio.

| Offset | Campo | Tipo | Contenido |
| ---: | :--- | :--- | :--- |
| 0 | `noise` | uint32 | RMS A del último segundo (mV en ADC, µFS en I2S), redondeado |
| 4 | `noiseAvg` | float | El mismo RMS, sin redondear |
| 8 | `noiseAvgDb` | float | **LAeq,1s**, dB(A) |
| 12 | `noisePeak` | float | LAFmax en unidades lineales |
| 16 | `noisePeakDb` | float | **LAFmax**, dB(A) (no es un pico; ver abajo) |
| 20 | `noiseMin` | float | Obsoleto: duplicado de `noiseAvg` |
| 24 | `noiseMinDb` | float | Obsoleto: duplicado de `noiseAvgDb` |
| 28 | `noiseAvgLegal` | float | **L10**, dB(A) (pese al nombre, no mV) |
| 32 | `noiseAvgLegalDb` | float | **L10**, dB(A) |
| 36 | `noiseAvgLegalMax` | float | LAFmax en unidades lineales |
| 40 | `noiseAvgLegalMaxDb` | float | LAFmax, dB(A) |
| 44 | `lowNoiseLevel` | uint16 | **L90**, dB(A) redondeado |
| 46 | — | 2 bytes | Relleno de alineación |
| 48 | `cycles` | uint32 | Segundos agregados (válidos o no): latido del nodo |
| 52 | `Ld` | float | Día (7-19 h), dB(A) |
| 56 | `Le` | float | Tarde (19-23 h), dB(A) |
| 60 | `Ln` | float | Noche (23-7 h), dB(A) |
| 64 | `noiseLden` | float | Lden, dB |
| 68 | `noiseLASmaxDb` | float | **LASmax** del último segundo, dB(A) (3.3.1) |
| 72 | `noiseLCpeakDb` | float | **LCpeak** del último segundo, dB(C) (3.3.1) |
| 76 | `noiseLASmaxHoldDb` | float | Máximo LASmax desde la lectura anterior (3.3.1) |
| 80 | `noiseLCpeakHoldDb` | float | Máximo LCpeak desde la lectura anterior (3.3.1) |
| 84 | `crc16` | uint16 | CRC-16/CCITT-FALSE de los bytes 0-83 (3.3.1) |
| 86 | `reserved` | uint16 | 0, fuera del CRC; no interpretarlo |

**Semántica real de algunos nombres heredados.** El layout está congelado, así
que varios nombres no describen lo que llevan: `noiseAvgLegal` es L10 en dB,
no una media en mV; `noisePeak`/`noisePeakDb` son el máximo con ponderación
Fast (un RMS de 125 ms), no un pico; `noiseMin`/`noiseMinDb` duplican la media y
están obsoletos. El **pico real** es `noiseLCpeakDb` (ponderación C, sin
constante de tiempo).

**Integridad: CRC-16 al final de la trama.** CRC-16/CCITT-FALSE (polinomio
0x1021, semilla 0xFFFF, valor de comprobación de `"123456789"` = 0x29B1) sobre
los 84 bytes anteriores. El nodo lo calcula en cada lectura, así que cubre
también los campos de retención. Las guardas de layout detectan un struct
desplazado y `cycles` detecta un nodo congelado, pero una trama corrompida en
tránsito puede traer un `cycles` que avanza y pasaría las dos comprobaciones:
el CRC es lo que la descarta. Un maestro que no lo compruebe sigue funcionando
igual que antes.

**`reserved` no lo interpretes.** Son los dos últimos bytes, van detrás del CRC
y por tanto **no están cubiertos por él**; el nodo los pone a 0. Existen solo
para que `sizeof(SensorData)` sea determinista frente al relleno del
compilador.

**Ruido impulsivo: `noiseLASmaxHoldDb` y `noiseLCpeakHoldDb`.** Los campos
`noiseLASmaxDb` y `noiseLCpeakDb` son máximos **del último segundo**: un
maestro que sondee cada 5 s ve uno de cada cinco segundos y descarta el resto,
justo en los dos indicadores que existen para cazar impulsos. Los campos
`...HoldDb` retienen el máximo **desde la lectura anterior** y la lectura que
los entrega abre una ventana nueva, que empieza con el siguiente segundo: así
ningún evento se pierde ni sale en dos lecturas. La ventana solo se consume
cuando el nodo sirve la lectura con status 1. Si se lee otra vez antes de que
llegue un segundo nuevo, repiten los valores del último segundo en lugar de
dar 0; el maestro descarta esa trama porque `cycles` no ha cambiado.

**`CMD_GET_DATA` es de un solo maestro.** Esos dos campos son **destructivos**:
dos maestros sondeando el mismo nodo se repartirían los impulsos y ninguno
vería la serie completa. Con varios lectores, que solo uno use `CMD_GET_DATA` y
los demás se queden en `CMD_GET_STATUS` y `CMD_GET_METADATA`, que no consumen
nada; o que ignoren los `...HoldDb` y usen los del último segundo. El resto de
campos se puede leer desde tantos maestros como se quiera.

**Cuidado al pedir menos bytes de los que ofrece `CMD_GET_DATA`.** El nodo
escribe los 88 bytes en el búfer de transmisión del esclavo. Un maestro antiguo
que pida solo los 68 de la definición 3.2.x obtiene exactamente los campos que
espera, pero deja 20 bytes sin consumir en el FIFO del esclavo. En las pruebas
hechas hasta ahora el HAL del ESP32 reinicia ese búfer en cada `onRequest`, así
que no se arrastran a la lectura siguiente; aun así **la recomendación es pedir
los 88 bytes** y quedarse con los campos que interesen. Si integras un maestro
que pide una longitud fija menor, verifícalo en banco antes de desplegarlo.

### Metadatos del nodo (`CMD_GET_METADATA`, 16 bytes)

| Offset | Campo | Tipo | Significado |
| ---: | :--- | :--- | :--- |
| 0 | fw_major | uint8 | Versión de firmware (mayor) |
| 1 | fw_minor | uint8 | Versión (menor) |
| 2 | fw_patch | uint8 | Versión (parche) |
| 3 | node_type | uint8 | 0x01 = ADC/MAX4466, 0x02 = I2S/ICS-43434 |
| 4 | time_synced | uint8 | 1 cuando el maestro ya ha fijado la hora |
| 5-6 | clip_count | uint16 LE | Muestras recortadas en el último segundo (los dos nodos) |
| 7-8 | calib_offset | int16 LE | Offset de calibración persistente (NVS), centésimas de dB |
| 9-10 | window_fill | uint16 LE | Segundos válidos que contiene ahora la ventana L10/L90 |
| 11-12 | window_size | uint16 LE | Longitud de la ventana con la que se compiló (`AGG_WINDOW_SEC`) |
| 13 | lden_periods | uint8 | Bits: 0 = día con datos, 1 = tarde, 2 = noche |
| 14-15 | lden_minutes | uint16 LE | Minutos acumulados en los periodos con datos |

**Longitud según la versión.** La trama ha crecido siempre por el final: 7
bytes en origen, 9 al añadirse la calibración (3.3.0) y 16 desde la 3.3.1. Un
maestro ESP32 rellena una respuesta corta hasta la longitud pedida, así que el
número de bytes recibidos no dice nada: la longitud real hay que deducirla de
la versión. Y la 3.3.0 se identificaba como 3.2.1, de modo que en un nodo
anterior a 3.3.1 solo son seguros los 7 primeros bytes. (La 3.3.2 también
decía ser la anterior, 3.3.1; desde la 3.3.3 la versión va al día.)

**`window_fill` / `window_size`: cuándo fiarse de L10 y L90.** Un percentil
sobre 12 segundos no es el mismo estadístico que uno sobre 300. Tras un
arranque, o tras una racha de segundos inválidos, `window_fill < window_size`:
el L10/L90 es provisional y conviene marcarlo como tal en Grafana en lugar de
tratarlo como un percentil de 5 minutos. La ventana avanza **una ranura por
segundo de reloj**, válido o no: los segundos inválidos guardan un centinela
que envejece con normalidad pero queda fuera del percentil, de modo que
"los últimos 300 s" son 300 s reales.

**`lden_periods` / `lden_minutes`: sobre qué se apoya el Lden.** El nodo publica
Lden desde el primer segundo válido del día, así que a las 07:00:02 ya hay un
"Lden" calculado con dos segundos de día. Estos dos campos dicen qué franjas
tienen datos y cuántos minutos se han acumulado, para que el maestro pueda
tratarlo como provisional en lugar de almacenarlo como índice de 24 horas.

El maestro puede usar los metadatos para etiquetar la serie por tipo de nodo,
comprobar que la hora está sincronizada antes de fiarse de Ld/Le/Ln, y detectar
saturación acústica (`clip_count` alto de forma sostenida indica un nodo mal
ubicado o con demasiada ganancia).

### Hora (`CMD_SET_TIME_LEGACY`)

**La envía el maestro en epoch LOCAL.** El nodo **no aplica ninguna zona
horaria**: toma el epoch recibido (`0x09` + 4 bytes LE) tal cual, y de ahí
salen directamente las franjas día (7-19 h), tarde (19-23 h) y noche (23-7 h).
El maestro debe enviar **hora local, no UTC** (en España UTC+1 en invierno y
UTC+2 en verano) y reenviarla tras cada cambio de horario. Si se envía UTC,
todos los índices Ld/Le/Ln/Lden quedan desplazados una o dos horas. El byte
`time_synced` de los metadatos confirma que el nodo ya tiene hora; hasta
entonces Ld/Le/Ln/Lden permanecen en 0. El epoch es un uint32, que llegaría
hasta 2106; en la práctica el límite lo pone `time_t` del nodo: 64 bits con el
core 3.x (IDF 5), 32 bits y por tanto 2038 con el core 2.0.x.
El nodo no ajusta el reloj dentro del callback I2C: lo difiere a la tarea
agregadora (§6).

### Calibración persistente (`CMD_SET_CALIB`)

`0x0A` + int16 little-endian en centésimas de dB. El nodo suma ese offset a
todos los niveles, lo guarda en NVS y lo conserva tras reinicios **y tras
reflashear**. Reglas:

- **El valor sustituye al guardado, no se suma a él**, y los niveles que
  publica el nodo ya lo incluyen. Para corregir un nodo que marca `medido`
  frente a una referencia `ref` (calibrador o sonómetro), hay que enviar
  `nuevo = actual + (ref − medido)`, con `actual` leído de `calib_offset` en
  los metadatos. Solo si `actual` es 0 se reduce a `ref − medido`. El maestro
  de ejemplo lo hace en `calibrateNode(ref, medido)`.
- Se aceptan valores entre −30 y +30 dB. Fuera de ese rango se ignoran: en
  silencio si llegan por el bus; si es el valor guardado en NVS (por ejemplo,
  uno corrupto de una versión anterior), el arranque lo avisa por Serial, lo
  descarta y lo borra.
- Se aplica al instante; la escritura en flash se hace después, desde la tarea
  agregadora, porque borrar flash bloquea decenas de ms y el maestro está
  esperando en el bus.
- Al recalibrar un nodo desde cero (un `CALIBRATION_RMS_MV` nuevo, o un cambio
  de `MIC_OFFSET_DB`), el offset antiguo deja de tener sentido: hay que ponerlo
  a 0. Ver [docs/CALIBRACION.md](CALIBRACION.md).

### Ventana deslizante L10/L90

Los percentiles se calculan sobre los últimos `AGG_WINDOW_SEC` segundos (por
defecto 300 s = 5 min, intervalo de referencia corto habitual en ISO 1996-2
urbano), en un buffer circular que avanza cada segundo. Sea cual sea el periodo
de sondeo del maestro, el L10/L90 que lee corresponde a los últimos
`AGG_WINDOW_SEC` segundos hasta ese instante. Configurable con
`-D AGG_WINDOW_SEC=N` (10 a 3600).

## 5) Lado maestro: flujo recomendado

Ejemplo de referencia: [examples/i2c_master/src/main.cpp](../examples/i2c_master/src/main.cpp).

1. Inicializar I2C: `Wire.begin(SDA, SCL)` y `Wire.setTimeOut(100)`.
2. Enviar `CMD_GET_STATUS` y leer 1 byte. **Si no es 1, no leer los datos** en
   este ciclo: no hay nada nuevo que publicar, y saltarlo no pierde nada,
   porque el nodo solo consume la ventana de los `...HoldDb` en una lectura que
   sirve con status 1.
3. Enviar `CMD_GET_DATA` y leer los 88 bytes en una estructura con el mismo
   layout (y las mismas guardas de offset).
4. Volver a leer `CMD_GET_STATUS`.
5. **Publicar solo si se cumple todo:**
   - Llegaron los 88 bytes. No basta por sí solo: el esclavo I2C de
     arduino-esp32 puede rellenar una respuesta corta con padding y
     `requestFrom` devolver la longitud completa igualmente.
   - El CRC es correcto.
   - Las dos lecturas de status valen 1.
   - **`cycles` es distinto del de la última trama con CRC correcto.** Detecta
     un esclavo que responde bien pero con datos congelados, y dos sondeos dentro
     del mismo segundo del nodo. Distinto, no mayor: tras un reinicio del nodo
     `cycles` vuelve a empezar.
6. Si algo falla: descartar la trama (nunca republicar la anterior) y seguir en
   el ciclo siguiente.

**Por qué dos lecturas de status.** Status y datos van en transacciones
distintas, y el nodo puede cerrar un segundo entre ellas. Si ese segundo es
inválido, la trama se sirve con status 0: no consume la ventana de retención y
lleva los valores del último segundo válido, que ya se publicaron. Un maestro
que la publicara repetiría un punto de LAeq y, en los `...HoldDb`, un impulso
ya contado. La segunda lectura de status lo detecta.

**Caso residual.** Si el segundo inválido se cierra justo **después** de la
lectura de datos, la trama era buena y ya había consumido la ventana, pero la
segunda lectura de status vale 0 y el maestro la descarta: se pierden los
`...HoldDb` de esa ventana, igual que con una trama de CRC malo. Hace falta que
coincidan las dos cosas: que el borde de segundo caiga en los ~20 ms que median
entre la lectura de datos y la de status (con el maestro de ejemplo a 100 kHz:
la propia trama, el comando y su espera), y que el segundo nuevo sea inválido.
Una simulación con el código real del nodo, recorriendo todas las posiciones
posibles del borde, lo confirma: el flujo antiguo (leer status y datos siempre
y publicar si el status era 1) **duplica** el impulso cada vez que el borde cae
entre el status y los datos con un segundo inválido detrás; este flujo no
duplica nunca y solo pierde en el caso residual.

Eliminar también ese caso exige un cambio de protocolo, pendiente de decidir:
que la trama lleve el status con el que se sirvió (por ejemplo en `reserved`,
con un byte de comprobación, porque el CRC no lo cubre), o un comando de
confirmación con el que el maestro consuma la ventana solo después de validar
CRC y status. Los dos serían compatibles con los maestros actuales.

## 6) Sincronización y seguridad de datos

- El agregador publica una vez por segundo con `I2C_Comm_Publish()`; no hay
  cola intermedia.
- Un spinlock (`portMUX`, `portENTER_CRITICAL_SAFE`) protege juntos la copia
  servida, su status, `data_ready` y los latches de retención: `requestEvent`
  sirve una instantánea atómica, sin lecturas rasgadas si la petición llega en
  mitad de una actualización.
- Los callbacks I2C corren en la tarea esclava I2C del core de Arduino mientras
  el maestro espera en el bus, así que solo guardan valores y levantan
  indicadores: el ajuste del reloj (`settimeofday`) y la escritura en NVS los
  hace `I2C_Comm_Service()`, que la tarea agregadora llama cada segundo.
- `data_ready` impide servir status 1 con la estructura a ceros del arranque.

## 7) Semántica de los datos publicados

- **`noiseAvgDb` es el LAeq del último segundo**, no el promedio del intervalo
  entre lecturas del maestro. Un maestro que lee cada 60 s está muestreando 1
  segundo de cada 60: válido como indicador de tendencia, pero no es el
  LAeq,60s. Para el promedio de un intervalo, el maestro puede leer cada
  segundo y acumular la energía (`10^(L/10)`) de los segundos publicados.
- **LASmax** (`noiseLASmaxDb`): nivel máximo con ponderación temporal Slow
  (1 s), dB(A). Menos sensible a transitorios que LAFmax; requerido para ruido
  de tráfico.
- **LCpeak** (`noiseLCpeakDb`): pico absoluto con ponderación C, dB(C). Sin
  constante de tiempo: capta el valor instantáneo de presión, indicador de
  ruido impulsivo (obras, impactos). En el nodo de 16 kHz la ponderación C es
  exacta (±0,05 dB) hasta 7,9 kHz y no hay banda por encima; en el de 48 kHz,
  ±0,08 dB de 20 Hz a 20 kHz (desde la 3.3.3; antes caía −6,4 dB a 16 kHz).
  Es un pico de **muestra**, sin sobremuestreo: con contenido muy agudo el pico
  real puede caer entre dos muestras, hasta ~1,2 dB por encima del medido a
  8 kHz en el nodo de 48 kHz, y hasta 3 dB a 4 kHz en el de 16 kHz.
- **L10** (`noiseAvgLegalDb`) y **L90** (`lowNoiseLevel`) se recalculan cada
  segundo sobre la ventana deslizante.
- **Día de evaluación de 07:00 a 07:00.** Ld/Le/Ln y Lden son los índices del
  día de evaluación en curso, que empieza a las 07:00, con el periodo de día,
  y termina a las 07:00 siguientes: así la noche de 23:00 a 07:00 queda entera
  dentro del mismo día. A las 07:00 los acumuladores y los cuatro índices se
  reinician. Para guardar el día completo, el maestro debe quedarse con los
  valores publicados justo antes de las 07:00 (el reinicio se ve en los
  metadatos: `lden_periods` pasa de 7 a 1). Cada periodo solo cambia dentro de su franja;
  fuera de ella conserva su valor. Requieren hora válida en el nodo.
- **Lden se calcula sobre los periodos que ya tienen datos**, ponderados por
  sus horas, para que un periodo aún vacío no hunda el resultado. Con los tres
  periodos poblados es exactamente el Lden estándar.
- **Unidades de los campos lineales en el nodo I2S**: `noise`, `noiseAvg`,
  `noisePeak`, `noiseMin` y `noiseAvgLegalMax` llevan µFS (millonésimas de
  fondo de escala) en lugar de milivoltios, porque un micrófono digital no
  tiene tensión analógica. Los campos en dB tienen semántica idéntica en ambas
  variantes y son la salida primaria.
- `cycles` se incrementa una vez por segundo agregado (válido o no) y se
  congela si el muestreo se detiene: es el latido del nodo.

**Detección de saturación (los dos nodos).** El nodo I2S cuenta las muestras
por encima de 0,99 del fondo de escala; el nodo ADC, las que tocan los raíles
del ADC (≥4090 o ≤5). Con más de 10 en un segundo, ese segundo se invalida
(status 0) en lugar de publicar un nivel falseado por la saturación. Un
`clip_count` distinto de cero pero por debajo del umbral avisa de que el nivel
se acerca al techo. En las unidades ICS-43434 de este proyecto el fondo de
escala digital llega a **~106 dB SPL** (no a los 120 dB nominales), porque
leen 13,8 dB por encima de su hoja de datos; en el nodo ADC el techo depende de
la ganancia, del orden de 97 a 109 dB (ver
[docs/CALIBRACION.md](CALIBRACION.md)).

## 8) Puntos importantes para integradores

- Respetar timeouts I2C (`Wire.setTimeOut`) y no bloquearse ante fallos de bus.
- No publicar jamás con status 0, y registrar (log o campo extra) status y
  `cycles` en cada lectura: son el diagnóstico diferencial entre "nodo colgado"
  y "pipeline de publicación defectuoso".
- Tras actualizar desde versiones ≤3.1.0, **recalibrar** `CALIBRATION_RMS_MV`
  en el nodo ADC (la escala de mV cambió al eliminar el offset de la
  conversión). Ver `examples/calibration/`.
- Comprobar la versión del nodo en los metadatos: los niveles del nodo digital
  cambiaron entre versiones (la 3.3.1 leía unos 13 dB alto; ver el CHANGELOG).

## Referencias de implementación

- Protocolo I2C: [src/I2C_Comm.cpp](../src/I2C_Comm.cpp), [src/I2C_Comm.h](../src/I2C_Comm.h)
- Nodo ADC (C3 + MAX4466): [src/main.cpp](../src/main.cpp)
- Nodo I2S (S3 + ICS-43434): [src/main_i2s.cpp](../src/main_i2s.cpp), [src/MIC_I2S.cpp](../src/MIC_I2S.cpp)
- Cadena por muestra: [src/SampleChain.h](../src/SampleChain.h); agregación: [src/NoiseAggregator.cpp](../src/NoiseAggregator.cpp)
- DSP: [src/DSP_Engine.cpp](../src/DSP_Engine.cpp)
- Maestro de ejemplo: [examples/i2c_master/src/main.cpp](../examples/i2c_master/src/main.cpp)
- Calibración: [examples/calibration/](../examples/calibration/) (ADC), [examples/calibration_i2s/](../examples/calibration_i2s/) (I2S)
