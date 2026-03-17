# Environmental Noise Meter (ESP32-C3 Slave + ESP32-S2/S3 Master)

This repository contains an I2C distributed noise monitoring setup:
- `src/`: ESP32-C3 firmware acting as I2C slave and acoustic processing node.
- `examples/`: ESP32-S2/ESP32-S3 firmware acting as I2C master/reader.

## Features
- ADC sampling and DSP processing for acoustic indicators.
- I2C slave protocol compatible with current and legacy masters.
- Structured payload (`SensorData`) for robust host integration.
- Basic long-term indicators (`Ld`, `Le`, `Ln`, `Lden`) when device time is available.

## Hardware Wiring

### 1) Sensor Node (ESP32-C3, I2C slave)
| Signal | ESP32-C3 Pin |
| :--- | :--- |
| MIC OUT (MAX4466) | GPIO 4 |
| I2C SDA | GPIO 8 |
| I2C SCL | GPIO 10 |

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

Slave (ESP32-C3):
- Open repository root in VSCode/PlatformIO.
- Build/upload environment: `lolin_c3_mini`.

Master examples:
- Open `examples/` in VSCode/PlatformIO.
- Build/upload environment: `seeed_xiao_esp32s3` or `lolin_s2_mini`.

## Calibration

- **Constants** in `src/main.cpp`: `CALIBRATION_DB` (e.g. 94.0), `CALIBRATION_RMS_MV` (measured with calibrator).
- **Procedure and MAX4466 setup** (wiring, potentiometer gain, ISO 1996-2 / Decreto 213/2012): **[docs/CALIBRACION.md](docs/CALIBRACION.md)**.
- **Calibration firmware** (standalone, Serial output of RMS mV and LAeq): run the example in **`examples/calibration/`** (env `lolin_c3_mini`), then use the stable RMS (mV) value as `CALIBRATION_RMS_MV` in the main firmware.

## License
GPL-3.0. See `LICENSE`.
