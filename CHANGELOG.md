# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [3.3.1] - 2026-10-01 (external review fixes)

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
  ningún evento sea cual sea el periodo de sondeo; tras reiniciarse llevan el
  valor del último segundo, nunca 0. `REQUEST_INTERVAL_MS` del ejemplo baja de
  5000 a 1000 ms, que es la cadencia de agregación del nodo.
- **`lden_periods` / `lden_minutes` en los metadatos** (que pasan de 13 a 16
  bytes): el nodo publica Lden desde el primer segundo válido, así que a las
  07:00:02 ya hay un "Lden (24 h)" con dos segundos de día. Ahora el maestro
  sabe qué franjas tienen datos y cuántos minutos se han acumulado, igual que
  `window_fill` ya hacía para L10/L90.

### Fixed

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
- **A1 · `MIC_OFFSET_DB` vuelve a 0.** El ⋅10,7 dB del nodo digital era un
  parche de campo heredado: estaba compensando —y de más— el error de 3,01 dB
  de A2, y 10,7 dB es diez veces la tolerancia ±1 dB del ICS-43434, así que no
  podía ser dispersión de la pieza. Con la constante del datasheet ya correcta,
  la conversión da el nivel bueno por sí sola y el trim de compilación no hace
  falta. Para un ajuste por unidad lo correcto es `CMD_SET_CALIB` (0x0A), que
  persiste en NVS y no obliga a reflashear.

  > **ATENCIÓN, esto mueve los niveles publicados.** Entre A2 (+3,01) y este
  > cambio (+10,7), el nodo digital pasa a leer **+13,71 dB** respecto a
  > v3.3.0. Un calibrador de 94,0 dB leía 80,29 dB y ahora lee 94,00. La serie
  > histórica del nodo S3 en Grafana tendrá un escalón de 13,71 dB en el
  > momento del flasheo: hay que anotarlo, y los datos anteriores de ese nodo
  > no son comparables con los posteriores sin sumarles esos 13,71 dB. El nodo
  > analógico (C3/MAX4466) **no** se ve afectado: su cadena de calibración es
  > independiente y no cambia.

  Comprobación sin sonómetro, con un calibrador de 94,0 dB: el campo `raw
  noise` debe salir en ~35 400 µFS — es independiente del trim y de la
  constante, así que valida la extracción de los 24 bits del I2S— y el LAeq
  en ~94,0 dB. Si el raw sale muy distinto de 35 400, el problema no es ninguna
  de las dos cosas sino el desplazamiento de bits.
- **R3 · Muestreo no uniforme en el C3.** El agregador corre a prioridad alta y
  llamaba a `Serial.printf()` directamente; a 115200 baudios esa línea de
  estado son ~110 caracteres, es decir ~9,5 ms de escritura bloqueante que
  desalojaban a la tarea de muestreo una vez por segundo. Las muestras no se
  perdían —el bucle recupera desde `next_sample_time`— pero llegaban en
  ráfaga al final, y una ráfaga no es muestreo uniforme: sin filtro antialias
  analógico delante del ADC eso es error de medida, no solo jitter. Nuevo
  módulo `NodeLog`: el agregador solo formatea la línea en una cola (sin E/S,
  sin bloqueo) y la tarea `loop()` —prioridad 1, por debajo del muestreo— hace
  la escritura, que ahora sí es desalojable. Una cola llena **descarta** la
  línea: un log nunca debe retrasar una medida. De paso `loop()` deja de estar
  aparcada y hace algo útil, sin dejar de bloquearse en vez de girar en vacío.
- **Nit · `window_fill` se congelaba en rachas de segundos inválidos**, porque
  solo se publicaba dentro de la puerta de validez. Ahora se recalcula y se
  publica cada segundo: durante una racha el recuento baja de verdad conforme
  los centinelas entran en la ventana, que es exactamente lo que había que
  poder ver.
- `docs/RAMA_48KHZ.md`, `docs/COMUNICACION.md` y el README del ejemplo
  actualizados: trama de 88 bytes, metadatos de 16, CRC, campos de retención y
  el aviso de que `setNodeTime()` viene comentado, así que tal cual el ejemplo
  deja `time_synced = 0` y Ld/Le/Ln/Lden en 0 de por vida.

### Documentación

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

### Added

- **`window_fill` / `window_size` en los metadatos** (`CMD_GET_METADATA`, que
  pasa de 9 a 13 bytes, añadiendo por el final). Un master no tenía forma de
  distinguir un L10/L90 calculado sobre 12 segundos de uno sobre los 300 de la
  ventana completa, y tras un arranque publicaba el primero como si fuera el
  segundo. Ahora puede marcarlo como provisional.

### Fixed

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
- **El maestro de ejemplo no tenía guardas de layout** — la causa raíz del
  fallo de 3.3.0 seguía viva en su lado: ambos extremos compilaban, la lectura
  devolvía el número de bytes esperado y los valores eran silenciosamente
  erróneos. Añadidos los mismos `static_assert` de offsets y de `sizeof`.
- **El maestro de ejemplo fijaba la longitud de los metadatos en 9 bytes**:
  ahora pide la longitud más larga que conoce y acepta una respuesta más corta
  de un nodo antiguo, y drena el bus en lugar de dejar bytes sin consumir.
- `docs/RAMA_48KHZ.md` contradecía al código: decía 1536 frames de DMA y
  +24 KB cuando `MIC_I2S.h` usa 768 (16 ms, +6 KB) porque el driver I2S
  heredado limita `dma_buf_len` a 1024; seguía llamándose "rama experimental"
  cuando 48 kHz es el build por defecto del entorno S3; y su paso 5 presentaba
  como verificación de exactitud un tono de 8-16 kHz que a 16 kHz entra como
  alias por definición.
- Comentario desfasado del contador de recortes ("a 16 kHz un segundo nunca
  llega a 65535") en un nodo que ya muestrea a 48 kHz, ahora con saturación
  explícita; y bloque de comentario duplicado en `ruido_setup()`.

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
  data yet reports 0 and stays out of Lden.
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
- **B10 · The example master never set the clock**, so `time_synced` stayed 0
  forever and Ld/Le/Ln/Lden never left 0. It now ships `setNodeTime()` with the
  local-epoch requirement spelled out.
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
  output to remove the aliasing at source. The 48 kHz node is unaffected
  (already within 0.52 dB to 7.9 kHz).
- **C1 · `platformio.ini` claimed compatibility with Arduino core 2.x**, which
  is false: `ADC_ATTEN_DB_12` only exists from IDF 5.0. Documented as a
  requirement.

### Changed

- **B3/B6 · Field semantics documented, layout untouched.** `noiseAvgLegal`
  carries L10 in **dB** despite its "(mV)" legacy name; `noisePeak`/`noisePeakDb`
  are the Fast-weighted maximum (LAFmax), not an instantaneous peak — the real
  peak is `noiseLCpeakDb`; `noiseMin`/`noiseMinDb` are duplicates of the average
  and are deprecated. These are corrected in the comments rather than by moving
  fields: the wire layout is frozen and masters read it as a byte block.

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

## [3.3.0-devel] - 2026-08-25

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

## [Unreleased]

- None.

[2.2.0]: https://github.com/YOUR_ORG/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v2.2.0
[3.0.0]: https://github.com/roberbike/noise_UNE-EN_ISO_1996-2-2009/releases/tag/v3.0.0
