# Code Quality Report

Date: 2026-02-14

## Scope
Reviewed:
- `src/main.cpp` (ESP32-C3 slave firmware)
- `examples/src/main.cpp` (ESP32-S2/S3 master example)
- `README.md`, `examples/README.md`

## Findings and Fixes Applied

1. I2C callback data races (high)
- Problem: slave DSP task updated shared metrics while I2C callbacks could read partially updated values.
- Fix: added critical-section protection for shared state and atomic snapshot copies before `Wire.write`.

2. Potential blocking time call in DSP loop (high)
- Problem: `getLocalTime(&timeinfo)` can block; this risks starving real-time sampling.
- Fix: changed to non-blocking `getLocalTime(&timeinfo, 0)`.

3. Protocol clarity and compatibility (medium)
- Problem: mixed use of status command values (`0x20` vs `0x00`) and ambiguous docs.
- Fix: slave keeps modern command `0x20` and maps legacy `0x00`; master now uses `0x20` with legacy fallback.

4. Incomplete payload fields (medium)
- Problem: some `SensorData` fields remained stale/zero despite valid samples.
- Fix: now updates min/legal-max fields consistently and keeps long-term values coherent.

5. Sampling robustness edge case (low)
- Problem: `sub_sample_trigger` could be zero if config changed.
- Fix: added lower bound to `>= 1`.

6. Documentation drift (medium)
- Problem: docs had outdated addresses/commands/interval and encoding artifacts.
- Fix: rewrote root and example READMEs with current wiring, protocol, and behavior.

## Residual Risks
- `CMD_IDENTIFY (0x09)` is still dual-use for identify and legacy set-time, differentiated by payload length.
- Calibration constants must be validated in deployment environment.

## Recommended Next Validation
1. Build both projects in PlatformIO and verify no warnings/errors.
2. Run master/slave on hardware and confirm stable `cycles` increments and expected `GET_DATA` parsing.
3. Validate calibration with reference source and document final constants.
