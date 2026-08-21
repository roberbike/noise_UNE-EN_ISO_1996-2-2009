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

![ICS-43434 breakout (MRS179A): front and pin side](docs/images/ics43434_mrs179a.png)

Pin labels below match the MRS179A breakout silkscreen (photo above). Other
breakouts may label `LRCL` as `WS`/`LRCLK`, `DOUT` as `SD`, and `SEL` as `L/R`.

| Breakout pin | XIAO ESP32-S3 Pin | Function |
| :--- | :--- | :--- |
| SEL | **GND** | Channel select: low = LEFT (see note) |
| LRCL | GPIO 3 (D2) | Word select (WS / LRCLK) |
| DOUT | GPIO 4 (D3) | Serial data out of the mic |
| BCLK | GPIO 2 (D1) | Bit clock |
| GND | GND | Ground |
| 3V | 3.3V | Supply (1.5-3.6 V — never 5 V) |
| I2C SDA | GPIO 5 (D4) | To master (slave address 0x08) |
| I2C SCL | GPIO 6 (D5) | To master |

**SEL must be LOW (GND).** On this breakout `SEL` is the ICS-43434 `L/R`
channel-select pin, despite some vendor listings describing it as an I2S/PDM
mode selector — the ICS-43434 has no PDM mode. The firmware uses
`I2S_CHANNEL_FMT_ONLY_LEFT`, so:
- SEL low (or floating, thanks to the internal pull-down) → LEFT channel → mic is read. **Verified on bench.**
- SEL high (3.3V) → RIGHT channel → the firmware discards that half-frame → **no readings at all**.

Leaving SEL floating works on the bench but relies on a weak internal
pull-down; tie it to GND for field deployments (humidity and leakage can drift
a floating pin). If a future breakout ties `L/R` high internally and the node
reads silence, switch to `I2S_CHANNEL_FMT_ONLY_RIGHT` in `src/MIC_I2S.cpp`.

Notes for the I2S node:
- Keep the SCK/WS/SD wires short (<10 cm); BCLK runs at ~1 MHz. In a home-made
  harness, run the GND wire between BCLK and DOUT rather than at one edge.
- I2C to the master: SDA-SDA, SCL-SCL and a **common ground between both
  boards** (mandatory even when each has its own supply). Most ESP32 boards
  already carry pull-ups — only if the bus hangs, add 4.7 kΩ from SDA and SCL
  to 3.3V at a single point of the bus. For runs over ~20-30 cm, drop the
  master clock to 100 kHz.
- Pins are overridable via `build_flags` (`-D MIC_I2S_BCLK=...`,
  `-D I2C_SDA=...`). Avoid GPIO 43/44 (USB UART).
- On this node the legacy "mV" fields of `SensorData` (`noise`, `noiseAvg`,
  `noisePeak`, ...) carry micro-full-scale units (µFS) instead of millivolts,
  since a digital mic has no analog voltage. The dB fields are the primary
  output and keep identical semantics on both nodes.
- Expected noise floor: **~34 dB** (measured), vs ~58-60 dB for the MAX4466 node.

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
- **I2C protocol, status contract and master-side integration**: **[docs/COMUNICACION.md](docs/COMUNICACION.md)**.
- **Calibration firmware** (standalone, Serial output of RMS mV and LAeq): run the example in **`examples/calibration/`** (env `lolin_c3_mini`), then use the stable RMS (mV) value as `CALIBRATION_RMS_MV` in the main firmware.

ICS-43434 node (XIAO ESP32-S3):
- The digital mic has a **factory sensitivity spec** (-26 dBFS @ 94 dB SPL), so LAeq
  is computed directly from dBFS and no calibrator is strictly required. Bench
  measurements land at a ~34 dB noise floor, consistent with the datasheet
  (64 dBA SNR), so `MIC_OFFSET_DB` is usually unnecessary.
- **Verification firmware**: run **`examples/calibration_i2s/`** (env `seeed_xiao_esp32s3`).
  With a 94 dB calibrator, if the reported LAeq differs from 94.0, set
  `-D MIC_OFFSET_DB=<94.0 - measured LAeq>` in the main firmware `build_flags`.

## License
GPL-3.0. See `LICENSE`.
