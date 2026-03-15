# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

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
