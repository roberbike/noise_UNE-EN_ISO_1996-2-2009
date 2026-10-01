# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [3.3.1] - 2026-10-01 (external review fixes)

Fixes from an independent review (DeepSeek). Items already resolved in 3.3.0
(µFS scaling, sliding L10/L90, NVS calibration, LASmax/LCpeak, 48 kHz on the
digital node) are not repeated here. Timezone handling was deliberately left
out: the master sends LOCAL epoch and the node applies no timezone of its own.

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
