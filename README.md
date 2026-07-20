# Environmental Noise Meter (ESP32 Slave + ESP32-S2/S3 Master)

This repository contains an I2C distributed noise monitoring setup with two
interchangeable acoustic node variants sharing the same DSP chain, ISO 1996-2
indicators and I2C slave protocol:
- `src/main.cpp`: ESP32-C3 node with **MAX4466 analog mic** (ADC sampling).
- `src/main_i2s.cpp`: **XIAO ESP32-S3 node with ICS-43434 digital I2S MEMS mic**
  (recommended: ~30 dBA noise floor vs ~55-60 dB of the analog chain, factory
  sensitivity spec, immune to supply/ADC noise).
- `examples/`: ESP32-S2/ESP32-S3 firmware acting as I2C master/reader.

## Features
- ADC (MAX4466) or I2S DMA (ICS-43434) sampling and DSP processing for acoustic indicators.
- A-weighting (16 kHz cascaded biquads), LAeq(1s), LAFmax, L10/L90.
- I2C slave protocol compatible with current and legacy masters.
- Structured payload (`SensorData`) for robust host integration.
- Basic long-term indicators (`Ld`, `Le`, `Ln`, `Lden`) when device time is available.

## Hardware Wiring

### 1a) Sensor Node (ESP32-C3 + MAX4466, I2C slave)
| Signal | ESP32-C3 Pin |
| :--- | :--- |
| MIC OUT (MAX4466) | GPIO 4 |
| I2C SDA | GPIO 8 |
| I2C SCL | GPIO 10 |

### 1b) Sensor Node (XIAO ESP32-S3 + ICS-43434, I2C slave)
| Signal | XIAO ESP32-S3 Pin |
| :--- | :--- |
| ICS-43434 SCK (BCLK) | GPIO 2 (D1) |
| ICS-43434 WS (LRCLK) | GPIO 3 (D2) |
| ICS-43434 SD | GPIO 4 (D3) |
| ICS-43434 L/R | GND |
| ICS-43434 VDD | 3.3V |
| I2C SDA | GPIO 5 (D4) |
| I2C SCL | GPIO 6 (D5) |

Notes for the I2S node:
- Keep the SCK/WS/SD wires short (<10 cm); BCLK runs at ~1 MHz.
- On this node the legacy "mV" fields of `SensorData` (`noise`, `noiseAvg`,
  `noisePeak`, ...) carry micro-full-scale units (µFS) instead of millivolts,
  since a digital mic has no analog voltage. The dB fields are the primary
  output and keep identical semantics on both nodes.

### 2) Master Node

For ESP32-S2 default example mapping:
| Signal | ESP32-S2 Pin |
| :--- | :--- |
| I2C SDA | GPIO 8 |
| I2C SCL | GPIO 9 |

For XIAO ESP32-S3 default example mapping:
| Signal | ESP32-S3 Pin |
| :--- | :--- |
| I2C SDA | GPIO 5 |
| I2C SCL | GPIO 6 |

Important:
- Use common GND between boards.
- Use 3.3V logic.
- Add pull-up resistors (typically 4.7k to 3.3V on SDA/SCL) if your boards do not include them.

## I2C Protocol
- Slave address: `0x08`

Commands (1 byte):
| Command | Value | Response |
| :--- | :--- | :--- |
| `GET_STATUS` | `0x20` | 1 byte (`1` mic OK, `0` mic error) |
| `GET_STATUS` legacy | `0x00` | Same as above (backward compatibility) |
| `GET_DATA` | `0x01` | Full `SensorData` struct |
| `GET_DB` legacy | `0x10` | 4-byte float |
| `GET_RAW_MV` legacy | `0x30` | 4-byte uint32 |
| `GET_LMAX` legacy | `0x40` | 4-byte float |
| `GET_L10` legacy | `0x60` | 4-byte float |
| `GET_L90` legacy | `0x70` | 4-byte float |

Notes:
- Command `0x09` is reserved for identify response and also accepted as legacy "set time" when sent with 4 additional bytes.
- Master example polls every 5 seconds by default.

## Build and Flash (PlatformIO)

Slave (ESP32-C3 + MAX4466):
- Open repository root in VSCode/PlatformIO.
- Build/upload environment: `lolin_c3_mini`.

Slave (XIAO ESP32-S3 + ICS-43434):
- Open repository root in VSCode/PlatformIO.
- Build/upload environment: `seeed_xiao_esp32s3`.
- Optional fine trim vs a reference meter: `-D MIC_OFFSET_DB=<dB>` in `build_flags`.

Master examples:
- Open `examples/` in VSCode/PlatformIO.
- Build/upload environment: `seeed_xiao_esp32s3` or `lolin_s2_mini`.

## Calibration

- **Constants** in `src/main.cpp`: `CALIBRATION_DB` (e.g. 94.0), `CALIBRATION_RMS_MV` (measured with calibrator).
- **Procedure and MAX4466 setup** (wiring, potentiometer gain, ISO 1996-2 / Decreto 213/2012): **[docs/CALIBRACION.md](docs/CALIBRACION.md)**.
- **Calibration firmware** (standalone, Serial output of RMS mV and LAeq): run the example in **`examples/calibration/`** (env `lolin_c3_mini`), then use the stable RMS (mV) value as `CALIBRATION_RMS_MV` in the main firmware.

ICS-43434 node (XIAO ESP32-S3):
- The digital mic has a **factory sensitivity spec** (-26 dBFS @ 94 dB SPL), so LAeq
  is computed directly from dBFS and no calibrator is strictly required.
- **Verification firmware**: run **`examples/calibration_i2s/`** (env `seeed_xiao_esp32s3`).
  With a 94 dB calibrator, if the reported LAeq differs from 94.0, set
  `-D MIC_OFFSET_DB=<94.0 - measured LAeq>` in the main firmware `build_flags`.

## License
GPL-3.0. See `LICENSE`.
