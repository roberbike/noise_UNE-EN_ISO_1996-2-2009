# Comunicación e inicialización y obtención de datos del sensor de ruido

## 1) Visión general

- El firmware implementa un nodo sensor (I2C slave, dirección `0x08`) que publica métricas acústicas agregadas por segundo.
- La adquisición puede hacerse desde un micrófono analógico MAX4466 (ADC, ESP32-C3) o digital ICS-43434 (I2S, XIAO ESP32-S3). El procesamiento DSP aplica ponderación A y calcula LAeq, LAFmax, L10, L90, Ld/Le/Ln y Lden.
- Ambas variantes exponen el mismo protocolo I2C y la misma estructura `SensorData`: para el master son indistinguibles (salvo la nota de unidades de §7).

## 2) Inicialización de comunicaciones (lado sensor)

### I2C (esclavo)
- Función: `I2C_Comm_Init()` en [src/I2C_Comm.cpp](../src/I2C_Comm.cpp).
- Configura pines con `Wire.setPins(I2C_SDA, I2C_SCL)` y registra los callbacks `Wire.onReceive(receiveEvent)` y `Wire.onRequest(requestEvent)` **antes** de `Wire.begin(addr)`, para que el hardware pueda atender la primera transacción.
- Pines por defecto: SDA=8/SCL=10 en el C3; SDA=5/SCL=6 (D4/D5) en el XIAO S3. Sobreescribibles por `build_flags`.
- El esclavo no fija el clock del bus (lo controla el master).

### I2S (micrófono digital)
- `MIC_I2S_Init()` en [src/MIC_I2S.cpp](../src/MIC_I2S.cpp) instala el driver I2S (24 bits en trama de 32, canal izquierdo, 16 kHz), fija pines (BCLK=2, WS=3, SD=4) y buffers DMA. La lectura es bloqueante con `i2s_read()`.

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
como selector I2S/PDM — el ICS-43434 no tiene modo PDM. Con
`I2S_CHANNEL_FMT_ONLY_LEFT` en el firmware: SEL bajo (o al aire, por el
pull-down interno) → canal izquierdo → se lee correctamente (verificado en
banco, suelo de ~34 dB); SEL a 3.3 V → canal derecho → el firmware descarta esa
media trama → **no mide nada**. Al aire funciona pero depende de un pull-down
débil: en despliegue de campo, atarlo a masa.

Otros breakouts pueden etiquetar `LRCL` como `WS`/`LRCLK`, `DOUT` como `SD` y
`SEL` como `L/R`. Si un módulo tiene `L/R` fijado a nivel alto internamente y el
nodo lee silencio, cambiar a `I2S_CHANNEL_FMT_ONLY_RIGHT` en `MIC_I2S.cpp`.

#### Conexión I2C con el master (nodo I2S)

SDA = GPIO 5 (D4), SCL = GPIO 6 (D5), dirección `0x08`. SDA-SDA, SCL-SCL y
**masa común entre ambas placas**, imprescindible aunque cada una tenga su
propia alimentación. La mayoría de placas ESP32 ya llevan pull-ups; solo si el
bus falla o se cuelga, añadir 4.7 kΩ de SDA y SCL a 3.3 V en un único punto del
bus. Para latiguillos de más de 20-30 cm, bajar el clock del master a 100 kHz.
Pines sobreescribibles con `-D I2C_SDA=x -D I2C_SCL=y`; evitar GPIO 43/44 (UART
del USB) y dejar libres GPIO 2/3/4 para el micrófono.

#### Avisos de compilación (core Arduino 3.x / IDF 5.x)

El driver `driver/i2s.h` está marcado como obsoleto en IDF 5.x, y los campos
`dma_buf_count`/`dma_buf_len` son alias de `dma_desc_num`/`dma_frame_num`. Son
avisos, no errores: el binario funciona correctamente. Para silenciarlos,
añadir al entorno S3 en `platformio.ini`:

```ini
build_flags =
    -D I2S_SUPPRESS_DEPRECATE_WARN=1
    -Wno-deprecated-declarations
```

La migración a la API nueva (`driver/i2s_std.h`) queda pendiente; ataría el
proyecto a core 3.x, mientras que la API legacy mantiene compatibilidad con
cores 2.x.

### ADC (micrófono analógico)
- En [src/main.cpp](../src/main.cpp) se configura el ADC con `adc1_config_channel_atten()` y `esp_adc_cal_characterize()`, y se deriva una pendiente `adc_mv_per_count` (solo pendiente, sin offset) para convertir amplitudes AC en float.

## 3) Flujo de obtención de datos (lado sensor)

- **Muestreo**: `sampling_task()` — polling wrap-safe a 16 kHz con `adc1_get_raw()` en el nodo ADC; bloqueo en `MIC_I2S_Read()` (DMA) en el nodo I2S.
- **DSP**: cascada de 3 biquads de ponderación A ([src/DSP_Engine.cpp](../src/DSP_Engine.cpp)), RMS ponderado A, máximos con EMA rápida (125 ms), percentiles L10/L90 sobre bloques completos de 20 s, y acumulación por periodos (Ld 7-19 h, Le 19-23 h, Ln 23-7 h) con Lden cuando hay hora válida.
- **Agregación y entrega**: cada segundo se construye un `SensorData` y se publica vía cola (`xQueueOverwrite(dataQueue, ...)`). `I2C_Comm_Sync()` vacía la cola y copia a `cachedSensorData` bajo sección crítica (`portENTER_CRITICAL_SAFE`), marcando `data_ready`. `requestEvent()` sirve un snapshot atómico de la caché.

## 4) Protocolo y comandos

Definidos en [src/I2C_Comm.h](../src/I2C_Comm.h):

| Comando | Código | Respuesta |
| :--- | :--- | :--- |
| `CMD_GET_STATUS` | 0x20 | 1 byte: 1 = OK, 0 = no publicar (ver §6) |
| `CMD_GET_DATA` | 0x01 | `SensorData` completo empaquetado |
| `CMD_GET_METADATA` | 0x50 | `NodeMetadata` (16 bytes): versión fw, tipo de nodo, time_synced, clip_count, calib_offset, llenado de la ventana L10/L90 |
| `CMD_SET_CALIB` | 0x0A | Escribe offset de calibración (int16 LE, centésimas de dB); se guarda en NVS |
| `CMD_IDENTIFY` | — | Identificación del nodo |
| `CMD_LEGACY_*` | — | Lecturas puntuales simples (compatibilidad) |

**Contrato del byte de estado.** Status 0 significa "no publiques este dato", sea cual sea la causa:
- Arranque: aún no ha aterrizado la primera agregación (`data_ready == 0`).
- Fallo de micrófono: bias fuera de rango (nodo ADC) o silencio absoluto bajo el suelo del micro (nodo I2S).
- Segundo inválido: RMS por debajo del umbral mínimo.
- **Muestreo detenido** (desde v3.1.2): si el agregador no recibe ningún segundo completo en 2 s, baja el status a 0, congela `cycles` y emite `[WARN] No samples for 2 s` por Serial. Un nodo colgado se ve como fallo, nunca como dato plano creíble.

En segundos inválidos el esclavo conserva los últimos valores válidos en la estructura (nunca publica ceros); la señal de invalidez es exclusivamente el status.

**Regla del formato de cable: sólo se añade al final.** Los maestros mantienen
su propia copia de `SensorData` y la leen como un bloque de bytes, así que el
desplazamiento de cada campo forma parte del protocolo. Un campo nuevo va
**siempre al final**: insertarlo en medio desplaza todo lo posterior y un
maestro con la definición antigua no falla, sino que lee valores equivocados en
silencio. Un maestro que pida sólo el tamaño antiguo (68 bytes) sigue
recibiendo exactamente el layout que espera. Hay `static_assert` en
`DSP_Engine.h` que rompen la compilación si alguien inserta un campo en medio.

**Hora: la envía el maestro en epoch LOCAL.** El nodo **no aplica ninguna zona
horaria**: toma el epoch recibido por `CMD_SET_TIME_LEGACY` (0x09 + 4 bytes LE)
tal cual, y de ahí salen directamente las franjas día (7-19 h), tarde (19-23 h)
y noche (23-7 h). Por tanto el maestro debe enviar **hora local, no UTC** — en
España UTC+1 en invierno y UTC+2 en verano — y reenviarla tras cada cambio de
horario. Si se envía UTC, todos los índices Ld/Le/Ln/Lden quedan desplazados
una o dos horas. El byte `time_synced` de los metadatos confirma que el nodo ya
tiene hora; hasta entonces Lden permanece en 0. El nodo no aplica el reloj
dentro del callback I2C: lo difiere a la tarea agregadora (ver §6).

**Semántica real de algunos campos de `SensorData`.** El layout está congelado
(los maestros lo leen como bloque de bytes), así que varios nombres heredados
no describen lo que llevan:

| Campo | Nombre sugiere | Lleva realmente |
| :--- | :--- | :--- |
| `noiseAvgLegal` | media legal en mV | **L10 en dB** |
| `noisePeak` / `noisePeakDb` | pico | **LAFmax** (máximo con ponderación Fast, es un RMS) |
| `noiseMin` / `noiseMinDb` | mínimo | **duplicado de la media** — obsoleto, no usar |
| `noiseLCpeakDb` | — | el **pico real** (ponderación C, sin constante de tiempo) |

**Metadatos del nodo (`CMD_GET_METADATA`).** Devuelve 16 bytes empaquetados.
La trama ha crecido **siempre por el final**, igual que `SensorData`: 7 bytes
en origen, 9 al añadirse la calibración y 16 desde la 3.3.1. Un master debe
pedir la longitud más larga que conozca y **aceptar una respuesta más corta**
de un nodo antiguo, nunca fijar una longitud única:

| Offset | Campo | Tipo | Significado |
| :--- | :--- | :--- | :--- |
| 0 | fw_major | uint8 | Versión de firmware (mayor) |
| 1 | fw_minor | uint8 | Versión (menor) |
| 2 | fw_patch | uint8 | Versión (parche) |
| 3 | node_type | uint8 | 0x01 = ADC/MAX4466, 0x02 = I2S/ICS-43434 |
| 4 | time_synced | uint8 | 1 cuando el master ya ha fijado la hora |
| 5-6 | clip_count | uint16 LE | Muestras a fondo de escala en el último segundo (nodo I2S) |
| 7-8 | calib_offset | int16 LE | Offset de calibración persistente (NVS), centésimas de dB |
| 9-10 | window_fill | uint16 LE | Segundos válidos que contiene ahora la ventana L10/L90 |
| 11-12 | window_size | uint16 LE | Longitud de la ventana con la que se compiló (`AGG_WINDOW_SEC`) |
| 13 | lden_periods | uint8 | Bits: 0 = día con datos, 1 = tarde, 2 = noche |
| 14-15 | lden_minutes | uint16 LE | Minutos acumulados en los periodos con datos |

**`window_fill` / `window_size`: cuándo fiarse de L10 y L90.** Un percentil
sobre 12 segundos no es el mismo estadístico que uno sobre 300, y hasta ahora
el nodo publicaba el primero con la misma apariencia que el segundo. Tras un
arranque, o tras una racha de segundos inválidos, `window_fill < window_size`:
el L10/L90 es provisional y conviene marcarlo como tal en Grafana en lugar de
tratarlo como un percentil de 5 minutos. La ventana avanza **una ranura por
segundo de reloj**, válido o no: los segundos inválidos guardan un centinela
que envejece con normalidad pero queda fuera del percentil, de modo que
"los últimos 300 s" son 300 s reales y no 300 muestras repartidas en una hora.

**Integridad: CRC-16 al final de la trama (3.3.1).** `SensorData` pasa de 76 a
88 bytes y los dos últimos útiles son un CRC-16/CCITT-FALSE (polinomio 0x1021,
semilla 0xFFFF) sobre los 84 bytes anteriores. El nodo lo calcula en cada
lectura, así que cubre también los campos de retención. Las guardas de layout
detectan un struct desplazado y `cycles` detecta un nodo congelado, pero una
trama corrompida en tránsito puede llegar con `status = 1` y un `cycles` que
avanza, y pasaría las dos comprobaciones: el CRC es lo que la descarta. Un
maestro que no lo compruebe sigue funcionando igual que antes.

**Ruido impulsivo: `noiseLASmaxHoldDb` y `noiseLCpeakHoldDb`.** Los campos
`noiseLASmaxDb` y `noiseLCpeakDb` son máximos **del último segundo**. Un
maestro que sondee cada 5 s ve uno de cada cinco segundos y descarta el resto,
justo en los dos indicadores que existen para cazar impulsos. Los campos
`...HoldDb` retienen el máximo **desde la lectura anterior del maestro** y
`CMD_GET_DATA` los reinicia, de modo que no se pierde ningún evento sea cual
sea el periodo de sondeo. Tras reiniciarse llevan el valor del último segundo,
nunca 0.

**`CMD_GET_DATA` es de un solo maestro.** Los campos `noiseLASmaxHoldDb` y
`noiseLCpeakHoldDb` son **destructivos**: el nodo los reinicia en la misma
lectura que los entrega. Eso los hace exactos para un maestro, pero significa
que **dos maestros sondeando el mismo nodo se robarían los máximos entre sí**:
cada uno recibiría solo los impulsos ocurridos desde la lectura del otro, y
ninguno de los dos vería la serie completa. Si necesitas varios lectores, que
solo uno use `CMD_GET_DATA` y los demás se queden en `CMD_GET_STATUS` y
`CMD_GET_METADATA`, que no consumen nada; o ignora los campos `...HoldDb` y
quédate con los de último segundo, que no son destructivos. El resto de los
campos de `SensorData` se puede leer desde tantos maestros como quieras.

El rearme ocurre **solo cuando el nodo sirve `status = 1`**. Un maestro que
sondee incondicionalmente sigue recibiendo `status = 0` mientras el nodo no
está listo y descarta la trama, sin que eso tire los impulsos acumulados antes
de ese segundo inválido. El único caso en que se pierde algo es una trama con
CRC malo: el nodo no puede saber que el maestro la descartó.

**`reserved` no lo interpretes.** Son los dos últimos bytes de la trama, van
detrás del CRC y por tanto **no están cubiertos por él**; el nodo los pone a 0.
Existen solo para que `sizeof(SensorData)` sea determinista frente al relleno
del compilador. No les des significado ni los uses como campo libre.

**`lden_periods` / `lden_minutes`: sobre qué se apoya el Lden.** El nodo
publica Lden desde el primer segundo válido, así que a las 07:00:02 ya hay un
"Lden (24 h)" calculado con dos segundos de día. Estos dos campos dicen qué
franjas tienen datos y cuántos minutos se han acumulado en total, para que el
maestro pueda tratarlo como provisional en lugar de almacenarlo como índice de
24 horas. Es lo mismo que `window_fill` hace para L10/L90.

**Cuidado al pedir menos bytes de los que ofrece `CMD_GET_DATA`.** El nodo
escribe los 88 bytes de `SensorData` en el búfer de transmisión del esclavo.
Un master antiguo que pida solo los 68 de la definición 3.2.x obtiene
exactamente los campos que espera —por eso los campos nuevos van al final—,
pero deja 20 bytes sin consumir en el FIFO del esclavo. En las pruebas hechas
hasta ahora el HAL del ESP32 reinicia ese búfer en cada `onRequest`, así que no
se arrastran a la lectura siguiente; aun así **la recomendación es pedir
`sizeof(SensorData)` completo** y quedarse con los campos que interesen, que es
lo que hace el maestro de ejemplo. Si integras un master que pide una longitud
fija menor, verifícalo en banco antes de desplegarlo.

El master puede usarlo para etiquetar la serie en InfluxDB por tipo de nodo,
verificar que la hora está sincronizada antes de fiarse de Ld/Le/Ln, y detectar
saturación acústica (`clip_count` alto de forma sostenida indica un nodo mal
ubicado o con ganancia excesiva).

**Detección de clipping (nodo I2S).** El firmware cuenta las muestras que tocan
fondo de escala (>0.99 FS); si en un segundo hay más de 10, la lectura se
invalida (status 0) en lugar de publicar un LAeq falseado por saturación. Un
`clip_count` distinto de cero pero por debajo del umbral es una señal temprana
de que el nivel se acerca al máximo del micrófono (~120 dB SPL).

**Calibración persistente (`CMD_SET_CALIB`).** El maestro puede inyectar un
offset de calibración en dB (int16 little-endian, centésimas de dB) medido con
un calibrador acústico físico. El payload es 1 byte de comando + 2 bytes del
offset. El nodo lo guarda en NVS, sobrevive a reinicios, y lo aplica a todos los
niveles en la conversión a dB. Ejemplo: con un calibrador emitiendo 94.0 dB, si
el nodo mide 96.5 dB, el maestro envía −250 (−2.50 dB). El offset vigente se
puede leer en los metadatos (`calib_offset`). Un offset de 0 significa nodo sin
calibrar (valor de fábrica).

**Ventana deslizante L10/L90.** Los percentiles se calculan sobre los últimos
`AGG_WINDOW_SEC` segundos (por defecto 300 s = 5 min, intervalo de referencia
corto habitual en ISO 1996-2 urbano), en un buffer circular que desliza cada
segundo. Esto garantiza que, sea cual sea el periodo de sondeo del maestro
(`stime`), el L10/L90 que lee corresponde a los últimos `AGG_WINDOW_SEC`
segundos hasta ese instante — sin los saltos por bloques ni el problema de leer
un bloque recién reseteado. Configurable por build flag `-D AGG_WINDOW_SEC=N`.

## 5) Lado master: flujo recomendado

Ejemplo de referencia: [examples/i2c_master/src/main.cpp](../examples/i2c_master/src/main.cpp).

1. Inicializar I2C: `Wire.begin(SDA, SCL)` y `Wire.setTimeOut(100)`.
2. Enviar `CMD_GET_STATUS` (`beginTransmission` + `write` + `endTransmission`), leer 1 byte con `requestFrom`.
3. Si status == 1, enviar `CMD_GET_DATA` y leer `sizeof(SensorData)` bytes en una estructura del mismo layout.
4. **Validación triple antes de publicar** (las tres condiciones, no basta una):
   - Lectura completa: se recibieron exactamente `sizeof(SensorData)` bytes. Atención: con el esclavo I2C de arduino-esp32, si el esclavo entrega menos bytes el hardware puede rellenar con padding y `requestFrom` devolver la longitud completa igualmente — por eso esta condición sola no es suficiente.
   - Status == 1.
   - **`cycles` mayor que el de la lectura anterior**: detecta un esclavo que responde correctamente pero con datos congelados (struct válido y rancio). Sin esta condición, un nodo colgado pasa los otros dos filtros y el dashboard pinta líneas planas.
5. Si cualquier condición falla: descartar la muestra (no publicar el buffer anterior) y reintentar en el siguiente ciclo.

## 6) Sincronización y seguridad de datos

- Cola FreeRTOS de tamaño 1 (`xQueueOverwrite`) entre agregador y capa I2C: el master siempre lee lo último.
- Spinlock (`portMUX` / `portENTER_CRITICAL_SAFE`) alrededor de la caché compartida: `requestEvent` sirve un snapshot atómico, eliminando lecturas rasgadas (torn reads) si la petición llega en mitad de una actualización.
- `data_ready` impide servir status OK con la estructura a ceros del arranque.

## 7) Semántica de los datos publicados

- **LASmax** (`noiseLASmaxDb`): nivel máximo con ponderación temporal Slow (1 s), dB(A). Menos sensible a transitorios que LAFmax; requerido para ruido de tráfico.
- **LCpeak** (`noiseLCpeakDb`): pico absoluto con ponderación C, dB(C). Sin constante de tiempo: capta el valor instantáneo de presión, indicador de ruido impulsivo (obras, impactos). En el nodo de 16 kHz cubre hasta 8 kHz; el de 48 kHz cubre la banda completa.
- **`noiseAvgDb` es el LAeq del último segundo**, no el promedio del intervalo entre lecturas del master. Un master que lee cada 60 s está muestreando 1 segundo de cada 60: válido como indicador de tendencia, pero no es el LAeq,60s. Si se necesita el promedio del intervalo, debe acumularse en el esclavo (cambio de protocolo pendiente de diseño).
- `noiseAvgLegalDb` (L10) y `lowNoiseLevel` (L90) se recalculan cada bloque completo de 20 s y se mantienen entre bloques.
- Ld/Le/Ln solo se actualizan dentro de su franja horaria; fuera de ella conservan el último valor del periodo. Lden requiere hora válida en el nodo.
- **Unidades del campo legacy de mV en el nodo I2S**: `noise`, `noiseAvg`, `noisePeak`, etc. llevan µFS (micro-fracciones de fondo de escala) en lugar de milivoltios, porque un micrófono digital no tiene tensión analógica. Los campos en dB tienen semántica idéntica en ambas variantes y son la salida primaria.
- `cycles` se incrementa una vez por segundo agregado (válido o no) y se congela si el muestreo se detiene: es el latido del nodo.

## 8) Puntos importantes para integradores

- Respetar timeouts I2C (`Wire.setTimeOut`) y no bloquear ante fallos de bus.
- No publicar jamás con status 0, y registrar (log o campo extra) status y `cycles` en cada lectura: son el diagnóstico diferencial entre "nodo colgado" y "pipeline de publicación defectuoso".
- Tras actualizar de versiones ≤3.1.0 a ≥3.1.1, **recalibrar** `CALIBRATION_RMS_MV` en el nodo ADC (la escala de mV cambió al eliminar el offset de la conversión). Ver `examples/calibration/`.

## Referencias de implementación

- Protocolo I2C: [src/I2C_Comm.cpp](../src/I2C_Comm.cpp), [src/I2C_Comm.h](../src/I2C_Comm.h)
- Nodo ADC (C3 + MAX4466): [src/main.cpp](../src/main.cpp)
- Nodo I2S (S3 + ICS-43434): [src/main_i2s.cpp](../src/main_i2s.cpp), [src/MIC_I2S.cpp](../src/MIC_I2S.cpp)
- DSP: [src/DSP_Engine.cpp](../src/DSP_Engine.cpp)
- Master de ejemplo: [examples/i2c_master/src/main.cpp](../examples/i2c_master/src/main.cpp)
- Calibración: [examples/calibration/](../examples/calibration/) (ADC), [examples/calibration_i2s/](../examples/calibration_i2s/) (I2S)
