# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

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
