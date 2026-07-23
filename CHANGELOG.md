# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

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
