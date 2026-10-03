# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

- Nada todavía.

## [3.3.3] - 2026-10-03 (revisión completa del repositorio)

Revisión de todo el código, los ejemplos y la documentación. Cada fallo se
reprodujo antes de corregirlo, con el código real del nodo compilado en el host
(tareas de muestreo, bus I2C, agregador) o en un simulador de FreeRTOS, y cada
arreglo se verificó de la misma forma.

> **Niveles.** En régimen estacionario no cambia nada a 1 kHz ni con espectros
> de tráfico, así que la calibración y las comparaciones de campo siguen
> valiendo. Cambian tres cosas: el primer segundo tras cada arranque (ya no
> publica un falso impulso), la ponderación del nodo de 48 kHz por encima de
> 8 kHz (antes se quedaba corta: +0,2 dB con ruido rosa, +0,75 dB con espectro
> plano, y más en el LCpeak de impulsos agudos) y el corte del día de
> evaluación, que pasa de medianoche a las 07:00.

### Fixed

- **El primer segundo tras cada arranque publicaba un impulso falso.** El
  seguidor de continua del nodo ADC empezaba en 2048 cuentas con el MAX4466 en
  ~2700, y ese escalón atravesaba los filtros: en una sala tranquila el primer
  segundo salía con LAeq +16 dB, LAFmax +25 dB y LCpeak +49 dB sobre los
  valores reales (simulación con los filtros del firmware). En los dos nodos
  los filtros arrancaban además en frío. Ahora el seguidor arranca con la
  primera muestra y medio segundo recorre toda la cadena sin acumularse; las
  envolventes Fast y Slow salen de ahí sembradas, de modo que LASmax es
  correcto desde el primer segundo (antes salía 2 dB bajo).
- **Los campos `...HoldDb` contaban cada impulso dos veces.** Tras una lectura
  la ventana se reabría con el segundo recién entregado, así que con sondeo a
  1 Hz cada impulso salía en dos tramas publicables. La ventana empieza ahora
  con el segundo siguiente. Además, los latches se alimentaban al calcular el
  segundo y la trama se publicaba unos milisegundos después, bajo otro cerrojo:
  una lectura en ese hueco recibía la trama anterior con el impulso nuevo en el
  latch y la siguiente volvía a dar el impulso como valor de su segundo. Ahora
  se alimentan en `I2C_Comm_Publish()`, dentro de la misma sección crítica que
  la trama. Verificado con el código real en los dos casos: cada impulso sale
  en una sola lectura.
- **En el nodo I2S la saturación se cargaba al segundo equivocado.** Los
  recortes se contaban por bloque DMA antes de procesarlo, y a 48 kHz un bloque
  de 768 muestras cruza uno de cada dos bordes de segundo: un segundo limpio se
  invalidaba y el que de verdad había recortado se publicaba como válido. Ahora
  se cuentan por muestra, en el segundo al que pertenecen (verificado con la
  `sampling_task` real y 20 muestras saturadas justo después de un borde).
- **En el C3 el log serie desaparecía tras el arranque.** La tarea de muestreo
  no se bloquea nunca, así que `loopTask` (prioridad 1) no vuelve a ejecutarse
  en cuanto ella arranca, y el módulo `NodeLog` de la 3.3.1 (R3), que vaciaba su
  cola desde `loop()`, no llegaba a imprimir ninguna línea `[SMART]` ni
  `[WARN]`. Reproducido en un simulador de FreeRTOS. `NodeLog` se elimina y el
  agregador vuelve a imprimir directamente, después de publicar: los drivers
  UART y USB-CDC esperan en un semáforo cuando su búfer está lleno, y mientras
  tanto la tarea de muestreo sigue corriendo (en la simulación, retraso máximo
  de 3,1 ms frente a 2,6 ms sin log; con espera activa serían 9,6 ms). `loop()`
  vuelve a quedar aparcada.
- **El día se cerraba a medianoche y partía la noche.** Ln mezclaba la
  madrugada de una noche con la primera hora de la siguiente, y a las 06:59 Ld
  y Le valían 0. El día de evaluación va ahora de 07:00 a 07:00: justo antes de
  las 07:00 están los cuatro índices del día completo.
- **El nodo de 48 kHz infraponderaba los agudos.** Sus cascadas A y C eran la
  transformada bilineal sin precompensar: exactas hasta 8 kHz (±0,54 dB), pero
  −1,2 dB a 10 kHz, −6,4 dB a 16 kHz y −15,8 dB a 20 kHz, mientras la
  documentación presentaba los 48 kHz como banda hasta 20 kHz. La sección alta
  se ajusta por mínimos cuadrados, como ya se hizo a 16 kHz: ±0,08 dB de 20 Hz a
  20 kHz y 0 dB a 1 kHz, así que la calibración no cambia. La de C queda en
  fase mínima, como la curva analógica, porque LCpeak es un pico: una sección
  de la misma magnitud pero fase no mínima exageraba hasta 0,6 dB el pico de un
  ciclo de 8 kHz. Efecto sobre el LAeq: +0,01 dB con tráfico, +0,2 dB con ruido
  rosa, +0,75 dB con espectro plano. `-D A_WEIGHT_LEGACY_48K` y `-D C_WEIGHT_LEGACY_48K` restauran
  los coeficientes anteriores.
- **El offset de calibración guardado en NVS no se validaba al cargarlo.** Un
  valor corrupto o fuera de rango de una versión anterior (la 3.3.0 aceptaba
  ±327 dB) se aplicaba para siempre a todas las lecturas. Ahora se valida al
  arrancar y, si no vale, se descarta y se borra; NaN e infinitos incluidos.
- **El reloj se rompía en 2038**: `(long)ts` se volvía negativo, la hora caía
  en 1901 y Ld/Le/Ln dejaban de calcularse. Ahora es `time_t`, de 64 bits con el
  core 3.x (IDF 5), y el epoch uint32 del maestro vale hasta 2106. Con el core
  2.0.x `time_t` es de 32 bits y el límite de 2038 es del propio sistema.
- **El nodo C3 no compilaba con el core 2.0.x anterior a la 2.0.15** (IDF hasta
  4.4.6), que solo conoce `ADC_ATTEN_DB_11`; la 3.3.1 lo daba por exigencia del
  core 3.x, pero `ADC_ATTEN_DB_12` existe desde IDF 4.4.7 (core 2.0.15), que
  además marca el nombre antiguo como obsoleto. Ahora se elige el nombre según
  la versión de IDF (es el mismo ajuste de hardware) y el firmware compila con
  todos los cores 2.0.x y 3.x.
- **`-D I2S_SUPPRESS_DEPRECATE_WARN=1` no silenciaba nada**: el driver I2S
  heredado de IDF 5 mira `CONFIG_I2S_SUPPRESS_DEPRECATE_WARN`. Corregido en
  `platformio.ini` y en el del ejemplo `calibration_i2s`.
- **La 3.3.2 se identificaba como 3.3.1** en los metadatos. La versión pasa a
  3.3.3, con un comentario que recuerda mantenerla al día junto con
  `library.json` y `library.properties`.
- Con el muestreo detenido, la tarea agregadora sigue aplicando el reloj y la
  calibración que lleguen por I2C.

**Maestro de ejemplo (`examples/i2c_master`)**

- **Leía los datos aunque el status fuera 0 y los publicaba según un status
  leído antes.** Si el nodo cerraba un segundo inválido entre las dos
  transacciones, publicaba los valores del segundo anterior y repetía su
  impulso. Ahora lee el status, los datos solo si es 1 y el status otra vez, y
  publica solo si los dos valen 1 (simulado con el código real del nodo en
  todas las posiciones posibles del borde de segundo: ningún duplicado). Queda
  un caso residual, en "Pendiente".
- `last_cycles` solo se actualiza con tramas de CRC correcto: una trama
  corrupta podía hacer que la siguiente buena pareciera repetida.
- **La longitud de los metadatos se deduce de la versión del nodo.** Un maestro
  ESP32 rellena la respuesta corta hasta la longitud pedida, así que el número
  de bytes recibidos no distingue un nodo antiguo de uno nuevo; y la 3.3.0 se
  identificaba como 3.2.1.
- **`calibrateNode(referencia, medido)` envía `actual + (referencia − medido)`.**
  El valor enviado sustituye al guardado, y el ejemplo anterior
  (`calibrateNode(-2.5)`) tiraba la corrección que el nodo ya tuviera. Comprueba
  además el rango y el resultado de `endTransmission`. Nuevo
  `setNodeCalibration()` para fijar un valor absoluto.
- Pines por defecto para cualquier otra placa (los `SDA`/`SCL` de su variante),
  casts en los `printf`, motivo concreto de cada descarte, el mensaje de trama
  incompleta mostraba la cifra equivocada y faltaban guardas de offset de los
  campos añadidos en 3.3.1. El README decía que sondeaba cada 5 s y que
  `cycles` debía ser "mayor" (es "distinto": vuelve a empezar al reiniciar el
  nodo).

**Ejemplo de calibración del MAX4466 (`examples/calibration`)**

- **El seguidor de continua se reiniciaba a 2048 cada segundo**: un escalón por
  segundo a través de los filtros, que inflaba el RMS medido. Ahora persiste,
  arranca con la primera muestra y hay medio segundo de calentamiento.
- Usaba la tercera sección A antigua a 16 kHz (la que se hundía por encima de
  5 kHz), muestreaba a 16129 Hz y no contaba recortes. Ahora replica el
  firmware: coeficientes actuales, 16000 Hz exactos, contador de raíles y bias
  con avisos.
- No compilaba con el core 2.0.x (`ADC_ATTEN_DB_12`).
- La prueba de linealidad pedía 114 dB, que el ADC recorta con cualquier
  ganancia útil: ahora 104 dB con `clip` a 0.
- Muestra al arrancar, y con `-D RESET_NVS_CALIB` borra, el offset por unidad
  guardado en NVS, que si no se sumaría a la calibración nueva.
  `CALIBRATION_RMS_MV` se fija en `platformio.ini`, sin editar fuentes.

**Ejemplo de calibración del ICS-43434 (`examples/calibration_i2s`)**

- Muestra el factor de cresta (3,0 dB con un tono limpio) y el pico, y avisa de
  sobrecarga. Ya no propone medir a 114 dB: las unidades de este proyecto
  saturan a ~106 dB.
- Replica también el seguidor de continua, el calentamiento y el nuevo ajuste
  de 48 kHz del firmware.
- Muestra la diferencia con el trim del firmware y el offset NVS. El README
  explica el acoplamiento del calibrador al puerto MEMS (~0,5 dB de más) y
  corrige la salida de ejemplo y la tabla de diagnóstico.

### Changed

- `src/SampleChain.h`: cadena por muestra común a los dos nodos (antes cada
  uno llevaba su copia del bucle). Con el calentamiento a 0 es idéntica bit a
  bit a los bucles anteriores, a 16 y a 48 kHz.
- `I2C_Comm`: el agregador publica con `I2C_Comm_Publish()`, bajo el spinlock
  que ya existía, y esa misma llamada alimenta los latches de retención;
  desaparecen la cola `dataQueue`, `I2cPayloadMessage`, `I2C_Comm_Sync()` e
  `I2C_Comm_AccumulateImpulsive()`. Desaparecen también `MIC_I2S_LastPeak()` y
  `MIC_I2S_LastClipCount()`: pico y recortes se miden por muestra.
- `MIC_MIN_PEAK_FS` separado de `MIC_MIN_RMS_FS` (mismo valor; antes una
  constante servía para dos comprobaciones distintas).
- `CALIBRATION_DB` y `CALIBRATION_RMS_MV` se pueden fijar desde `build_flags`.
- `platformio.ini`: comentarios corregidos; la justificación del trim del
  ICS-43434 pasa a `docs/CALIBRACION.md`.

### Documentation

- README reorganizado (la versión inglesa estaba metida en mitad de la
  española) y corregido: puertos I2S (el S3 tiene dos), BCLK a 3,072 MHz,
  trama de 88 bytes y metadatos de 16, `SET_CALIB`, sensibilidad real y techo
  del ICS-43434, día de evaluación, cores soportados.
- `docs/COMUNICACION.md` reescrito: tabla de offsets, secuencia de lectura y
  caso residual, semántica de `SET_CALIB`, saturación en los dos nodos y
  longitud de los metadatos según la versión.
- `docs/CALIBRACION.md` cubre ahora los dos nodos: dónde vive la calibración,
  reglas del offset NVS, la constante K y su historia, techo de medida.
- `docs/ESTUDIO_TECNICO.md` al día (Slow y LCpeak implementados, exactitud real
  de los filtros); su método de calibración duplicado remite a
  `CALIBRACION.md`, y absorbe lo vigente de `docs/SMART_CITY_CLASE_2.md`, que
  se elimina.
- `docs/RAMA_48KHZ.md`: 48 kHz por defecto desde la 3.2.1 (no la 3.3.0), tabla
  y pasos corregidos.
- `tools/gen_a_weight.py` reproduce el ajuste de 48 kHz; `tools/README.md`
  actualizado.
- Las entradas 3.3.1 y 3.3.2 de este CHANGELOG describen ahora lo que se
  publicó en cada una.

### Pendiente

- **Caso residual en la lectura de los `...HoldDb`.** Si un segundo inválido
  se cierra justo entre la lectura de datos y la segunda lectura de status
  (unos 20 ms por segundo con el maestro de ejemplo), el maestro descarta una
  trama buena que ya había consumido la ventana y esos máximos se pierden. Quitarlo del todo requiere un cambio de protocolo (el
  status de servicio dentro de la trama, o un comando de confirmación); ver
  `docs/COMUNICACION.md` §5.

## [3.3.2] - 2026-10-03

> En los metadatos se identificaba como **3.3.1**; corregido en 3.3.3.

### Fixed

- **A1 · `MIC_OFFSET_DB` = −13,71 dB.** La 3.3.1 salió con el trim a 0 y el
  nodo digital leía **unos 13 dB alto**. Este valor reproduce exactamente el total
  de la 3.3.0, la única configuración verificada contra calibrador hasta
  entonces: la 3.3.0 tal como se publicó marcaba **93,8-94,0 dB con un
  calibrador de 94,0**. Respecto a la 3.3.1 los niveles del nodo digital bajan
  13,71 dB; respecto a la 3.3.0, son los mismos. Verificado después en campo:
  con un calibrador de 94,0 dB marca 94,5-94,6 dB y coincide con un sonómetro
  de referencia. (Las lecturas con calibrador citadas aquí son de sesiones
  distintas, y el acoplamiento al puerto MEMS varía unas décimas entre ellas;
  entre versiones, la diferencia exacta es la de K.)

  `K = 26 + MIC_PEAK_TO_RMS_DB + MIC_OFFSET_DB`, y hace falta K = +15,30:
  la 3.3.0 lo conseguía con 26 − 10,70, y aquí sale de 26 + 3,0103 − 13,71.
  La condición es sobre K, no sobre el trim suelto: sin el término #A2 el trim
  volvería a ser −10,70.

  Con el trim a 0 (K = +29,01) el calibrador marcaba **106,5 dB**. Estos
  micrófonos entregan mucho más nivel del que implica su hoja de datos —del
  orden de −15 dBFS de RMS a 94 dB SPL frente a los −29,01 que saldrían de la
  sensibilidad de −26 dBFS especificada sobre el **pico** de la senoide— y el
  motivo de fondo sigue sin explicar.

  **No es un fallo de firmware**, y se comprobó en las dos versiones ejecutando
  el propio `DSP_ApplyFilter` del repositorio: 0,000 dB de ganancia a 1 kHz en
  ambas y en los dos rates, normalización `s24 / 2^23`,
  `mean_sq = sum_sq_A / samples_count` y `begin()` idéntico. La única
  diferencia de nivel entre 3.3.0 y 3.3.1 son los +3,0103 dB de #A2.

  Dos valores equivocados precedieron a este: **0**, de la conjetura de que el
  −10,7 compensaba el error de 3 dB de #A2 y sobraba al arreglarlo (la
  aritmética nunca lo sostuvo: 10,7 no es una compensación de 3,01); y
  **−15,55**, propuesto y nunca publicado, de tomar al pie de la letra un print
  de RMS de −13,46 dBFS, que se pasa 1,8 dB. El −10,7 de la 3.3.0 era correcto
  para la fórmula sin #A2, no con él.
- **`examples/calibration_i2s` replica ya la conversión del firmware**: medía a
  16 kHz frente a los 48 kHz del nodo, con los coeficientes A antiguos y sin el
  término pico→RMS, y proponía calcular el trim como `94 − LAeq`. Esa
  divergencia es la que llevó a los trims equivocados. Añade `CALIBRATOR_DB` y
  la columna `sens`.
- **Techo de medida documentado**: con 13,8 dB más de sensibilidad que la hoja
  de datos, estas unidades llegan al fondo de escala digital a ~106 dB SPL y no
  a 120. Medido con un calibrador a 94, 104 y 114 dB.

### Added

- **RMS en dBFS en la línea de log del nodo digital.** Es el único campo que no
  depende del trim ni de la constante de sensibilidad —es la salida cruda del
  micrófono—, así que convierte la comprobación de campo en una sola lectura:
  calibrador de 94,0 dB en el puerto, se anota el valor, y el total necesario
  es K = −rms_dBFS.

### Changed

- Metadatos de la librería (`library.json`, `library.properties`): describen
  los dos nodos y sus indicadores actuales.

## [3.3.1] - 2026-10-01 (external review fixes)

> **Publicada con `MIC_OFFSET_DB = 0`.** Con el término #A2 (+3,01 dB) y sin
> trim, K = 29,01, 13,71 dB más que en la 3.3.0 y la 3.3.2, y el nodo digital
> lee **unos 13 dB alto**: un calibrador de 94,0 dB marcaba ~106,5. Su CHANGELOG decía que con el trim a 0 el calibrador
> leería 94,0; corregido en la 3.3.2. Las anotaciones *(3.3.3: …)* marcan lo
> que la revisión de la 3.3.3 encontró mal en esta versión.

Fixes from an independent review (DeepSeek). Items already resolved in 3.3.0
(µFS scaling, sliding L10/L90, NVS calibration, LASmax/LCpeak, 48 kHz on the
digital node) are not repeated here. Timezone handling was deliberately left
out: the master sends LOCAL epoch and the node applies no timezone of its own.

### Added

- **A5 · CRC-16 al final de `SensorData`** (CCITT-FALSE, polinomio 0x1021,
  semilla 0xFFFF, sobre los 84 bytes anteriores; el struct pasa de 76 a 88
  bytes, siempre añadiendo por el final). Las guardas de offset cubren un
  struct desplazado y `cycles` cubre un nodo congelado, pero una trama
  corrompida en tránsito puede llegar con `status = 1` y un `cycles` que
  avanza y pasaría ambas. El nodo lo calcula en cada lectura, así que cubre
  también los campos de retención; el maestro de ejemplo lo verifica y
  descarta la trama si no cuadra. Verificado contra el vector estándar
  (`"123456789"` → 0x29B1) en las dos implementaciones, y detecta los 672
  errores de un bit de la región cubierta. `cycles` queda documentado como el
  número de secuencia, que ya cumplía esa función.
- **A7 · `noiseLASmaxHoldDb` y `noiseLCpeakHoldDb`**: máximos **desde la
  lectura anterior del maestro**, reiniciados por ella. Los campos de siempre
  son máximos del último segundo, así que el maestro de ejemplo, sondeando
  cada 5 s, descartaba cuatro segundos de cada cinco precisamente en los dos
  indicadores creados para el ruido impulsivo. Con los nuevos no se pierde
  ningún evento sea cual sea el periodo de sondeo. `REQUEST_INTERVAL_MS` del
  ejemplo baja de 5000 a 1000 ms, que es la cadencia de agregación del nodo.
  *(3.3.3: al reiniciarse se cargaban con el segundo recién entregado, de modo
  que cada impulso salía en dos lecturas; corregido.)*
- **`window_fill` / `window_size` en los metadatos** (`CMD_GET_METADATA`, que
  pasa de 9 a 13 bytes, añadiendo por el final). Un master no tenía forma de
  distinguir un L10/L90 calculado sobre 12 segundos de uno sobre los 300 de la
  ventana completa, y tras un arranque publicaba el primero como si fuera el
  segundo. Ahora puede marcarlo como provisional.
- **`lden_periods` / `lden_minutes` en los metadatos** (que pasan de 13 a 16
  bytes): el nodo publica Lden desde el primer segundo válido, así que a las
  07:00:02 ya hay un "Lden (24 h)" con dos segundos de día. Ahora el maestro
  sabe qué franjas tienen datos y cuántos minutos se han acumulado, igual que
  `window_fill` ya hacía para L10/L90.

### Fixed

- **Wire format: `SensorData` binary compatibility was broken in 3.3.0.**
  `noiseLASmaxDb` and `noiseLCpeakDb` were inserted in the middle of the
  struct, before `lowNoiseLevel`, which shifted every field after
  `noiseAvgLegalMaxDb` by 8 bytes. A master built against the 3.2.x definition
  (CanAirIO) kept reading the same byte offsets and silently got the wrong
  values: `lowNoiseLevel` read LASmax, `cycles` read LCpeak, and **`Lden` read
  `Le`** — a slow-moving energy average, which is why the published Lden looked
  frozen at a constant value. The two fields are now **appended at the end**,
  so every pre-existing offset is preserved and a master requesting the old
  68-byte size gets exactly the layout it expects. The example master in
  `examples/i2c_master/` carried the same inserted layout and was corrected
  too. `static_assert` guards on the offsets of `noiseAvgDb`, `lowNoiseLevel`,
  `cycles` and `noiseLden` now break the build if a future change inserts a
  field instead of appending it.
- **A2 · Faltaban 3,0103 dB en el nodo digital (pico frente a RMS).** El
  datasheet del ICS-43434 especifica la sensibilidad sobre el **pico** de una
  senoide (94 dB SPL "peaks at −26 dB below full scale"), y el código le pasa
  una amplitud **RMS**; para una senoide difieren en 20·log₁₀(√2). La prueba
  decisiva es el punto de sobrecarga acústica: el datasheet da AOP = 120 dB SPL
  y sensibilidad −26 dBFS, que se diferencian **exactamente** en 26 dB, y eso
  solo es consistente si el fondo de escala lo alcanza el pico de la senoide de
  120 dB. Leído como RMS, esa senoide necesitaría un pico de 1,414 y habría
  recortado 3 dB antes del AOP especificado. Comprobado: con la constante
  corregida una senoide con el pico a fondo de escala se mide en 120,00 dB
  (antes 116,99). **Consecuencia: todas las magnitudes del nodo digital suben
  3,01 dB.** Los umbrales del comentario #B5 se recalculan en consecuencia
  (1e-5 FS pasa de 20,0 a 23,0 dB SPL; el suelo del micrófono queda en
  33-37 dB SPL).
- **Cuatro arreglos que el CHANGELOG de 3.3.0 prometía y el código no tenía.**
  Documentaban la rama `devel`, que no se fundió en `main`, así que la release
  salió con el texto por delante del código. Implementados ahora:
  - **Lden se hundía mientras algún periodo estuviera vacío**: un periodo sin
    datos entraba en la suma energética como 0 dB (energía 1). Con solo
    `Ld = 55` el resultado era **52,0 dB en lugar de 55,0**. Ahora se promedia
    únicamente sobre los periodos que tienen datos, ponderados por sus horas;
    con los tres poblados el divisor vuelve a ser 24 y el valor es idéntico al
    de la definición estándar (verificado: 56,55 dB en ambos casos).
  - **La ventana L10/L90 se estiraba sobre los segundos inválidos**: solo
    avanzaba en segundos válidos, así que "300 s" podía abarcar mucho más
    tiempo de reloj. Ahora avanza una ranura por segundo; los inválidos guardan
    un centinela que envejece con normalidad y queda fuera del percentil.
  - **`DSP_Init()` estaba vacío**: ahora limpia el estado (z1/z2) de las dos
    cascadas, de modo que un reinicio lógico queda bien definido.
  - **El buffer de percentiles era un `static` local de función**: pasa a
    miembro de instancia, para que dos agregadores no lo compartan.
- **A-weighting at 16 kHz rebuilt (ADC node).** The 12194 Hz pole of the analog
  prototype sits above Nyquist at 16 kHz, so the bilinear transform collapsed
  it and the response fell away far too early: −1.50 dB at 5 kHz, −5.72 at
  6.3 kHz, −12.18 at 7 kHz, −50 at 7.9 kHz against IEC 61672-1. The first two
  sections are exact and kept; the third is least-squares fitted over
  20 Hz–7.9 kHz, bringing the whole band within **±0.15 dB**, still 0 dB at
  1 kHz (so `CALIBRATION_RMS_MV` remains valid) and with all poles stable.
  Trade-off: the old section had a zero at Nyquist that masked aliasing from
  8–9 kHz (the ADC has no analog anti-alias filter); the fitted one does not.
  For urban spectra only ~1.3 % of A-weighted energy lies above 8 kHz, so the
  net effect is a clear gain. Build with `-D A_WEIGHT_LEGACY_16K` to restore
  the previous coefficients, and fit an RC low-pass (~8 kHz) at the MAX4466
  output to remove the aliasing at source. *(3.3.3: the 48 kHz node, said here
  to be unaffected, was only accurate up to 8 kHz; refitted in 3.3.3.)*
- **Filtro C a 16 kHz con el mismo defecto que tenía el A.** El prototipo C
  comparte el polo doble de 12194 Hz, que a 16 kHz queda por encima de Nyquist,
  así que la transformada bilineal lo colapsaba igual: −0,51 dB a 4 kHz,
  −1,51 a 5 kHz y **−5,73 a 6,3 kHz** frente a la curva IEC 61672-1. Importa
  porque LCpeak es el indicador de ruido impulsivo y los impulsos llevan
  energía real en 5-8 kHz, de modo que el nodo analógico los subestimaba. La
  sección de 20,6 Hz es exacta y se conserva; la de alta frecuencia se ajusta
  por mínimos cuadrados y deja la banda en **±0,05 dB** (polos 0,65 y 0,045,
  estables; 0 dB exacto a 1 kHz, así que `CALIBRATION_RMS_MV` sigue válido).
  `C_WEIGHT_LEGACY_16K` restaura los coeficientes anteriores, y
  `tools/gen_a_weight.py` reproduce los nuevos.
- **B1 · Sample period was truncated by integer division.** `1000000/16000`
  is 62.5 µs but integer division yielded 62, so the ADC node really ran at
  16129 Hz (+0.81 %) and its "second" lasted 0.992 s. The sampling loop now
  carries the remainder and borrows an extra microsecond when it overflows,
  giving exactly 16000.000 Hz on average (the same fix makes a hypothetical
  48 kHz polling path exact too — it was off by 4.17 %).
- **B4 · No overload detection on the ADC node.** The comment claimed clipping
  was "handled by bias range", which is false: the bias check looks at the DC
  average and a signal pinned at either rail keeps the mean centred. Samples at
  the rails are now counted; more than 10 in a second invalidates the reading
  and the count is published in the metadata, matching the I2S node and the
  overload indication IEC 61672 expects.
- **B9 · `PeriodStats::add` discarded seconds below 10 dB**, removing genuinely
  quiet periods from the energy average and biasing Ld/Le/Ln upwards — exactly
  the periods a night index must capture. Validity is already decided upstream,
  so every valid second now counts.
- **B8 · Day rollover happened after accumulating.** The first second of a new
  day landed in the previous day's accumulators and was then discarded, and the
  published Ld/Le/Ln carried yesterday's values into today. The rollover now
  runs before accumulation and clears the published indices; a period with no
  data yet reports 0 and stays out of Lden. *(3.3.3: the day still rolled over
  at midnight, splitting the night; it now runs 07:00 to 07:00.)*
- **R1 · `settimeofday()` ran inside the I2C slave callback**, where newlib
  locks are unsafe. The callback now only stores the epoch and raises a flag;
  the clock is set in task context by the new `I2C_Comm_Service()`, which the
  aggregator task calls once per second. The same callback also wrote the
  calibration offset straight to NVS, and a flash erase/write blocks for tens
  of milliseconds with the master waiting on the bus; that write is now
  deferred through the same path, and the offset is range-checked before being
  persisted so a corrupted byte cannot store an absurd value that survives
  reboots.
- **R2 · `vTaskDelete(NULL)` on a task that may be subscribed to the TWDT.**
  Both nodes now call `esp_task_wdt_delete(NULL)` and park with
  `vTaskDelay(portMAX_DELAY)` instead of deleting the task.
- **R3 · Muestreo no uniforme en el C3.** El agregador corre a prioridad alta y
  llamaba a `Serial.printf()` directamente; a 115200 baudios esa línea de
  estado son ~110 caracteres, es decir ~9,5 ms de escritura bloqueante que
  desalojaban a la tarea de muestreo una vez por segundo. Nuevo módulo
  `NodeLog`: el agregador solo formatea la línea en una cola y la tarea
  `loop()` —prioridad 1, por debajo del muestreo— hace la escritura. Las dos
  últimas líneas de log fuera de `NodeLog` (`[TIME]` y `[CALIB]`) pasan también
  por la cola. *(3.3.3: revertido. En el C3 `loop()` no vuelve a ejecutarse en
  cuanto arranca el muestreo, así que la cola nunca se vaciaba y el log
  desaparecía; y los drivers serie esperan en un semáforo, no en espera
  activa, de modo que imprimir directamente no frenaba el muestreo.)*
- **Los latches de impulsivos se consumían en lecturas que el maestro
  descarta.** `CMD_GET_DATA` rearmaba `noiseLASmaxHoldDb`/`noiseLCpeakHoldDb`
  en cualquier lectura, incluida una servida con `status = 0`: un maestro que
  sondee incondicionalmente tiraba así todos los impulsos acumulados antes de
  ese segundo inválido. Ahora el rearme ocurre **solo cuando el nodo sirve
  `status = 1`**. *(3.3.3: no era el único caso con pérdidas, como se afirmaba;
  status y datos van en transacciones separadas. Ver la secuencia de lectura
  de la 3.3.3.)*
- **Nit · `window_fill` se congelaba en rachas de segundos inválidos**, porque
  solo se publicaba dentro de la puerta de validez. Ahora se recalcula y se
  publica cada segundo: durante una racha el recuento baja de verdad conforme
  los centinelas entran en la ventana, que es exactamente lo que había que
  poder ver.
- **B10 · The example master never set the clock**, so `time_synced`
  stayed 0 forever and Ld/Le/Ln/Lden never left 0. It now ships
  `setNodeTime()` with the local-epoch requirement spelled out.
- **El maestro de ejemplo no tenía guardas de layout**: la causa raíz del
  fallo de 3.3.0 seguía viva en su lado. Añadidos los mismos `static_assert`
  de offsets y de `sizeof`.
- **El maestro de ejemplo fijaba la longitud de los metadatos en 9 bytes**:
  ahora pide la longitud más larga que conoce y acepta una respuesta más corta
  de un nodo antiguo, y drena el bus en lugar de dejar bytes sin consumir.
  *(3.3.3: en un maestro ESP32 la respuesta corta llega rellenada hasta la
  longitud pedida; la longitud se deduce ahora de la versión.)*
- **C1 · `platformio.ini` claimed compatibility with Arduino core 2.x**, which
  is false: `ADC_ATTEN_DB_12` only exists from IDF 5.0. Documented as a
  requirement. *(3.3.3: wrong as stated — `ADC_ATTEN_DB_12` exists from IDF
  4.4.7, core 2.0.15. The code now picks the name by IDF version and builds on
  every 2.0.x and 3.x core.)*
- `docs/RAMA_48KHZ.md` contradecía al código: decía 1536 frames de DMA y
  +24 KB cuando `MIC_I2S.h` usa 768 (16 ms, +6 KB) porque el driver I2S
  heredado limita `dma_buf_len` a 1024; seguía llamándose "rama experimental"
  cuando 48 kHz es el build por defecto del entorno S3; y su paso 5 presentaba
  como verificación de exactitud un tono de 8-16 kHz que a 16 kHz entra como
  alias por definición.
- Comentario desfasado del contador de recortes ("a 16 kHz un segundo nunca
  llega a 65535") en un nodo que ya muestrea a 48 kHz, ahora con saturación
  explícita; y bloque de comentario duplicado en `ruido_setup()`.

### Changed

- **B3/B6 · Field semantics documented, layout untouched.** `noiseAvgLegal`
  carries L10 in **dB** despite its "(mV)" legacy name; `noisePeak`/`noisePeakDb`
  are the Fast-weighted maximum (LAFmax), not an instantaneous peak — the real
  peak is `noiseLCpeakDb`; `noiseMin`/`noiseMinDb` are duplicates of the average
  and are deprecated. These are corrected in the comments rather than by moving
  fields: the wire layout is frozen and masters read it as a byte block.

### Documentación

- **`CMD_GET_DATA` es de un solo maestro**, y ahora lo dice. Los campos
  `...HoldDb` son destructivos: el nodo los reinicia en la lectura que los
  entrega, así que dos maestros sondeando el mismo nodo se repartirían los
  impulsos y ninguno vería la serie completa. Con varios lectores, que solo
  uno mande `CMD_GET_DATA`; `CMD_GET_STATUS` y `CMD_GET_METADATA` no consumen
  nada. Documentado en `docs/COMUNICACION.md` y en el README del ejemplo.
- `reserved` queda documentado como lo que es: relleno a 0 para que
  `sizeof(SensorData)` sea determinista, **no cubierto por el CRC** (va detrás
  de él) y sin significado alguno. Un maestro no debe interpretarlo.
- El comentario del CRC anota cuándo dejaría de ser gratis: bit a bit sobre 84
  bytes son ~10-20 µs dentro del callback, irrelevante a una lectura por
  segundo; con un sondeo de decenas de Hz tocaría meter la tabla de 256
  entradas.
- El comentario del filtro C a 16 kHz recoge ahora el **trade-off de
  aliasado**, que faltaba. Son filtros digitales: actúan después del muestreo,
  así que ponderan una componente aliasada a la frecuencia en la que
  **aparece**, no a la que traía. El desplome del filtro antiguo enmascaraba el
  alias como efecto colateral (lo que pliega desde 9-11 kHz cae en 5-7 kHz,
  donde la cascada rota atenuaba entre 1,5 y 12 dB), y una cascada exacta lo
  pasa con casi todo su peso. Pega más fuerte en C que en A porque C es
  prácticamente plana entre 2 y 8 kHz y porque LCpeak es un pico: un solo
  impulso aliasado lo mueve. El arreglo es hardware —RC a ~8 kHz en la salida
  del MAX4466—, no coeficientes; enmascarar un error de medida con otro error
  de medida no era una alternativa defendible.
- `docs/RAMA_48KHZ.md`, `docs/COMUNICACION.md` y el README del ejemplo
  actualizados: trama de 88 bytes, metadatos de 16, CRC, campos de retención y
  el aviso de que `setNodeTime()` viene comentado, así que tal cual el ejemplo
  deja `time_synced = 0` y Ld/Le/Ln/Lden en 0 de por vida.

---

## [3.3.0-devel] - 2026-08-26 (code review fixes)

> **Nota (3.3.1):** esta sección describe la rama `devel`, que nunca se fundió
> en `main`. La 3.3.0 publicada **no** incluía cuatro de estos arreglos (Lden
> sobre periodos con datos, centinela de la ventana L10/L90, `DSP_Init()` y el
> buffer de percentiles) ni `CMD_GET_DATA_COMPACT`. Los cuatro primeros se
> implementan por fin en 3.3.1; el comando compacto sigue solo en `devel`.

### Fixed

- **Lden biased low while periods were still empty** (high): a period with no
  data yet entered the energy sum as 0 dB (energy 1) and dragged the result
  down — with only `Ld = 55` the result was 52.0 dB instead of 55.0. Lden is
  now averaged over the periods that actually have data, weighted by their
  hours. With all three periods populated the value is unchanged.
- **NVS write from the I2C callback** (high): `CMD_SET_CALIB` wrote flash
  inside the slave callback, blocking for tens of ms and risking missed
  responses to the master. The value is now applied immediately and the flash
  write is deferred to the aggregator task (`I2C_Comm_ServiceNVS()`).
- **Unvalidated calibration offset** (medium): any int16 was accepted (±327 dB)
  and persisted in NVS, so one corrupted byte could silently ruin every reading
  until reflashing. Offsets are now range-checked to ±30 dB.
- **L10/L90 window stretched over invalid seconds** (medium): the window only
  advanced on valid seconds, so gaps made "300 s" cover more than 300 s of wall
  clock. The window now advances every second; invalid seconds hold a sentinel
  that ages out normally but is excluded from the percentile.
- **`DSP_Init()` was empty** (medium): it now clears the filter state (z1/z2)
  of both the A and C biquad cascades, making a logical restart well-defined.
- **Percentile scratch buffer was a function-local `static`** (low): moved to a
  per-instance member, so two aggregators could never share it.

### Added

- **`CMD_GET_DATA_COMPACT` (0x02)**: optional 18-byte frame with levels as
  int16 tenths of dB, little-endian, plus a XOR checksum — for masters that
  aren't ESP32 or would rather not depend on float layout and struct padding.
  **Strictly additive**: `CMD_GET_DATA` (0x01) is unchanged, so CanAirIO and
  every existing master keep working with no modifications. The example master
  includes a `readCompact()` reference reader.

### Documentation

- README: LASmax and LCpeak added to the indicator table, sliding-window and
  Lden semantics corrected, compact command documented, normative section
  updated (Fast + Slow + C-weighted peak now implemented).
- `docs/COMUNICACION.md`: compact payload layout, calibration range and
  deferred NVS write, window behaviour across invalid seconds.
- `docs/ESTUDIO_TECNICO.md`: Slow weighting and LCpeak reflected as implemented.
- Example master README: metadata is 9 bytes, compact alternative explained.

---

## [3.3.0] - 2026-09-12

### Added

- **LASmax and LCpeak (Qwen review, point 4)**: the aggregator now also reports
  the max level with Slow (1 s) time weighting, `LASmax`, and the absolute
  C-weighted peak, `LCpeak`, required by IEC 61672-1 for traffic and impulsive
  noise. New `SensorData` fields `noiseLASmaxDb` / `noiseLCpeakDb`. C-weighting
  is a separate 2-biquad (4th order) cascade `cWeightingFilters`, with
  coefficients per sample rate (16 kHz and 48 kHz) generated and verified
  against IEC 61672-1 nominal values (|err| < 0.6 dB in the measurable band);
  see `tools/gen_a_weight.py`. On the 16 kHz node the C-weighting is stable and
  accurate to <0.5 dB up to 4 kHz — the bilinear transform maps the 12.2 kHz
  pole below Nyquist without aliasing; only the acquisition band (<8 kHz)
  limits the peak, exactly as for A-weighting.

### Added (earlier in devel)

- **Sliding-window L10/L90 (Qwen review, point 2)**: the percentiles are now
  computed over a circular buffer of the last `AGG_WINDOW_SEC` seconds
  (default 300 s = 5 min, the ISO 1996-2 short-term urban reference), sliding
  every second. The master always reads the percentiles for the last window up
  to its read, regardless of its polling period (`stime`) — this removes both
  the 20 s "staircase" and the stale-block problem when the master's read and
  the node's block boundary didn't align. Configurable via
  `-D AGG_WINDOW_SEC=<seconds>`. Uses `std::nth_element` (O(N)) instead of the
  old O(N²) selection sort, so large windows stay cheap.
- **Persistent calibration in NVS (Qwen review, point 3)**: new I2C command
  `CMD_SET_CALIB` (0x0A) lets the master push a dB offset (int16, hundredths
  of dB) measured with a physical calibrator. The node stores it in NVS
  (survives reboots) and applies it to every level via the amplitude→dB
  conversion — no reflashing to calibrate a node in the field. The offset is
  loaded at boot and exposed in the metadata.
- `NodeMetadata` extended with `calib_offset` (int16, hundredths of dB); it is
  now 9 bytes (was 7). The example master reads the new field and includes a
  `calibrateNode()` reference helper.

### Notes

- RAM for the window buffer is small (2.3 KB at 300 s, 4.7 KB at 600 s) — fine
  on both the C3 and the S3.
- Released as `3.3.0` after on-board validation.

---

## [3.2.1] - 2026-08-21

### Changed

- **README reescrito** en español con tablas comparativas de las dos placas
  (ESP32-C3 vs XIAO ESP32-S3) y los dos micrófonos (MAX4466 vs ICS-43434),
  una sección que explica qué significa cada indicador acústico (LAeq, LAFmax,
  L10, L90, Ld/Le/Ln, Lden) y el encuadre normativo UNE-EN ISO 1996-2 /
  IEC 61672-1 (dónde queda el sistema respecto a Clase 1/2 y para qué es o no
  legalmente válido). Índice navegable.
- **Rama experimental 48 kHz** para el nodo I2S: `seeed_xiao_esp32s3` pasa a
  `SAMPLE_RATE=48000`, se recalculan los coeficientes A y se ajusta el bloque
  DMA para mantener ~32 ms sin perder estabilidad; la documentación queda en
  `docs/RAMA_48KHZ.md` y el cambio queda aislado en el entorno S3.
- READMEs de ejemplos rehechos: `i2c_master` documenta la validación triple y
  los metadatos; `calibration` (ADC) y `calibration_i2s` (I2S) explican el
  suelo de ruido esperado de cada micrófono y remiten al significado de las
  medidas.
- `docs/ESTUDIO_TECNICO.md` corregido para reflejar el estado real del código:
  muestreo a 16 kHz (no 22.05), cascada de 3 biquads, y sólo ponderación
  temporal Fast (la Slow no se reporta).

---

## [3.2.0] - 2026-07-24

### Added

- **Shared `NoiseAggregator`** (`src/NoiseAggregator.{h,cpp}`): the per-second
  ISO 1996-2 math (LAeq/LAFmax, L10/L90, Ld/Le/Ln, Lden, hold-last-valid) now
  lives in one place instead of being duplicated in `main.cpp` and
  `main_i2s.cpp`. Each node injects its own amplitude->dB conversion and keeps
  only its platform-specific sampling task. A fix now lands once, not twice.
- **Clipping detection (I2S)**: `MIC_I2S.cpp` counts samples at full scale
  (>0.99 FS) per read; a second with more than 10 clips is invalidated
  (`mic_ok = 0`), protecting metrological integrity against >120 dB SPL or EMI.
- **Time-sync gate for Ld/Le/Ln/Lden**: period indicators are only accumulated
  once the master has set the clock (`CMD_SET_TIME_LEGACY`). Prevents polluting
  the day/evening/night bands with 1970-epoch data before the first sync.
- **Task watchdog** (`esp_task_wdt`) on both sampling tasks: a hung
  `i2s_read`/ADC loop now resets the chip instead of running mute. Complements
  the v3.1.2 stall detection (which reports the fault; the watchdog recovers).
- **Node metadata over I2C** (`CMD_GET_METADATA`, 0x50): firmware version,
  node type (0x01 ADC / 0x02 I2S), `time_synced` flag and last-second clip
  count. The example master reads and prints it.
- Example master now performs the **triple freshness validation** (complete
  read + status == 1 + `cycles` advancing) and only forwards publishable
  samples — the reference fix for the Grafana flat lines.

### Changed

- **`MIC_MIN_RMS_FS` raised from 1e-6 to 1e-5 FS**: the old threshold (~0 dB
  SPL) never fired; the ICS-43434's real noise floor is ~1e-5 FS (~30 dBA), so
  the new value detects a dead SD line without false positives.
- **I2S DMA block size 256 -> 512 frames** (16 ms -> 32 ms): fewer context
  switches, still well under the 125 ms fast window for LAFmax.

### Polish (post-review)

- Removed dead slow-EMA code in the ADC sampling task (unused after the
  aggregator refactor — the aggregator only consumes the fast max).
- Watchdog: reconfigure the TWDT that Arduino-ESP32 already initializes for
  loop() instead of re-initializing it (avoids ESP_ERR_INVALID_STATE and
  spurious resets, especially on the single-core C3).
- Explicit `uint16_t` cast on the I2S clip count and consistent L10/L90
  rounding/guard in the aggregator.

---

## [3.1.3] - 2026-07-23

### Added

- `docs/COMUNICACION.md`: protocol reference (I2C commands, status contract,
  master-side triple validation, data semantics), now including the I2S mic
  wiring and the master I2C connection notes.
- `docs/images/ics43434_mrs179a.png`: photo of the ICS-43434 breakout, used in
  the README and example docs so the silkscreen labels can be matched directly.
- `examples/calibration_i2s/README.md`: expected output (~34 dB floor with real
  sample trace), explanation of the constant L10/L90 and `Lden = 0.0` on the
  standalone firmware, and a troubleshooting table.

### Changed

- **Wiring documentation corrected**: `SEL` on the MRS179A breakout is the
  ICS-43434 `L/R` channel-select pin, **not** an I2S/PDM mode selector as some
  vendor listings claim (the ICS-43434 has no PDM mode). SEL must be **LOW /
  GND**; tying it to 3.3V selects the right channel and the node stops
  measuring entirely. Bench-verified. Tables now use the breakout's own
  silkscreen labels (SEL/LRCL/DOUT/BCLK/3V) with a mapping note for boards
  using WS/SD/LR.
- README documents the master I2C connection (SDA=D4, SCL=D5, address 0x08,
  common ground, pull-up and cable-length guidance).
- `platformio.ini`: `-D I2S_SUPPRESS_DEPRECATE_WARN=1` and
  `-Wno-deprecated-declarations` on the S3 environment — the legacy I2S API is
  deprecated in IDF 5.x but kept for core 2.x compatibility.

---

## [3.1.2] - 2026-07-18

### Fixed

- **Multi-minute flat readings at arbitrary levels**: the sampling pacing used
  a direct unsigned comparison (`now >= next_sample_time`) which is not safe
  across the `micros()` rollover (every ~71.6 min). A blocking event straddling
  the rollover froze sampling for up to ~71 min; the aggregator then starved
  and the I2C cache served the same struct unchanged, drawing a flat line at
  whatever the last level was. Elapsed-time checks are now wrap-safe
  (`(int32_t)(now - next) >= 0`), with a resync when the task is more than
  100 ms late instead of burst-sampling a compressed second.
- **Stall detection**: the aggregator now times out after 2 s without a
  completed second, logs a warning and drops the status byte to 0 (cycles
  freeze too), so a stalled node is visible to masters instead of silently
  serving frozen data. Applied to both the C3/MAX4466 and S3/ICS-43434 nodes.
- `examples/calibration/` time checks made wrap-safe as well.

---

## [3.1.1] - 2026-07-16

### Fixed

- **Flat readings at the noise floor (e.g. constant 58.7 dB)**: the A-weighted
  RMS was truncated to integer ADC counts and then to integer mV before the
  logarithm, quantizing low-level LAeq into ~2.5 dB steps (2 mV -> ~55.2,
  3 mV -> ~58.7, 4 mV -> ~61.2). The chain is now float end-to-end using a
  calibrated slope (mV/count) without the intercept — passing an AC amplitude
  through `esp_adc_cal_raw_to_voltage()` also wrongly added the calibration
  intercept to a differential quantity.
- **Spurious 0 dB samples**: an invalid second (dead input, RMS below floor)
  published zeros in every dB field, and after a reboot the zeroed boot-time
  struct could be served over I2C. Invalid seconds now hold the last valid
  values (mic_ok reports the fault) and the status byte stays 0 until the
  first aggregation lands, so protocol-following masters never publish
  boot-time zeros. **Masters must gate publishing on the status byte.**
- **L10/L90 were not 20 s percentiles**: `stat_idx` was reset every second, so
  the window never held more than one sample and L10 = L90 = last LAeq. The
  percentiles are now computed over full 20 s blocks (last block value is held
  in between).
- **Torn I2C reads**: `requestEvent` could read `cachedSensorData` while
  `I2C_Comm_Sync` was copying into it, delivering a mixed struct. The cache is
  now guarded by a spinlock and requests serve an atomic snapshot.
- Same hold-last-valid and L10/L90 fixes applied to the ICS-43434 I2S node.
- `examples/calibration/` updated to the same slope-based float conversion.
  **`CALIBRATION_RMS_MV` must be re-measured after updating** (the mV scale no
  longer includes the ADC calibration intercept).

---

## [3.1.0] - 2026-07-15

### Added

- **New acoustic node: XIAO ESP32-S3 + ICS-43434 (I2S digital MEMS mic)**:
  `src/main_i2s.cpp` + `src/MIC_I2S.{h,cpp}`, PlatformIO env `seeed_xiao_esp32s3`.
  Same DSP chain (16 kHz A-weighting), ISO 1996-2 indicators (LAeq, LAFmax,
  L10/L90, Ld/Le/Ln, Lden) and I2C slave protocol as the C3 node — existing
  masters work unchanged.
- **dBFS-based level computation**: LAeq derived from the ICS-43434 factory
  sensitivity (-26 dBFS @ 94 dB SPL); optional `MIC_OFFSET_DB` build flag for
  field trim. No acoustic calibrator strictly required.
- **I2S DMA sampling task pinned to core 1** (S3 is dual-core): the acoustic
  chain never competes with I2C/radio on core 0; blocking `i2s_read()` replaces
  the ADC polling loop.
- **Mic-alive detection for the digital path**: silence below the ICS-43434
  noise floor flags the SD line as disconnected.
- **Verification example** `examples/calibration_i2s/` (Serial LAeq + RMS dBFS).
- **Conditional I2C slave pins**: XIAO ESP32-S3 uses SDA=5/SCL=6; C3 keeps 8/10.

### Changed

- `platformio.ini` now uses `build_src_filter` to select the node variant per
  environment (`lolin_c3_mini` vs `seeed_xiao_esp32s3`).
- On the I2S node the legacy mV fields of `SensorData` carry µFS units
  (documented in README); dB fields keep identical semantics on both nodes.

---

## [3.0.0] - 2026-05-25

### Added

- **Modular RTOS architecture**: Firmware refactored to a modular ESP32 task-based design with improved task isolation and signal handling.
- **ADC sampling migration**: ADC capture moved to ESP32 hardware timer/async queue logic for more stable and reliable measurements.
- **I2C reliability improvements**: fixed slave read timeout by yielding in the polling loop and strengthened initial detection.
- **ISO 1996-2 compliance**: preserved last valid Ld/Le/Ln values after daily reset and reset `stat_idx` correctly during L10/L90 calculation.
- **Library metadata**: `library.properties` updated to `version=3.0.0` and repository URL corrected.

### Fixed

- **Daily statistics reset**: Correct handling of long-term level indices across midnight resets.
- **Legacy compatibility**: Improved command handling and backward-compatible I2C responses.

### Changed

- **Release version**: Official release version set to `3.0.0`.

---

## [2.2.0] - 2026-03-15

### Added

- **Calibration**: Full procedure in `docs/CALIBRACION.md` (ISO 1996-2, Decreto 213/2012): MAX4466 wiring, potentiometer adjustment, calibration firmware usage, registry template.
- **Calibration example**: Standalone firmware in `examples/calibration/` (same ADC + A-weighting chain as main firmware) for measuring RMS (mV) with 94 dB calibrator; outputs via Serial.
- **library.properties**: Version and metadata for Arduino/PlatformIO when using the repo as a library or installing from GitHub by tag.

### Fixed

- **Ld / Le at night**: Day and evening level indices no longer drop to 0 dB during the night. After midnight reset of period statistics, the last valid Ld/Le/Ln values are retained until new data exists for each period (ISO 1996-2 compliant).

### Changed

- **Documentation**: `docs/METODO_CALIBRACION.md` removed as it is a duplicate of `docs/CALIBRACION.md`. Field calibration and maintenance notes moved into `docs/CALIBRACION.md` (sections 10 and 11).
- **README**: Calibration section updated with links to `docs/CALIBRACION.md` and `examples/calibration/`.

---

[3.3.3]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v3.3.3
[3.3.2]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v3.3.2
[3.3.1]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v3.3.1
[3.3.0]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v3.3.0
[3.2.1]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v3.2.1
[3.0.0]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v3.0.0
[2.2.0]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v2.2.0
