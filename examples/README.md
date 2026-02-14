# Master Example (ESP32-S2 / ESP32-S3)

This example is an I2C master that reads `SensorData` from the ESP32-C3 slave at address `0x08`.

## Default Pin Mapping

### XIAO ESP32-S3
| Signal | Pin |
| :--- | :--- |
| SDA | GPIO 5 |
| SCL | GPIO 6 |

### Lolin S2 Mini
| Signal | Pin |
| :--- | :--- |
| SDA | GPIO 8 |
| SCL | GPIO 9 |

Slave side (ESP32-C3):
- SDA: GPIO 8
- SCL: GPIO 10

## Wiring Notes
- Connect GND between both boards.
- Keep I2C at 3.3V logic.
- Add pull-ups (for example 4.7k to 3.3V on SDA/SCL) when needed.

## Behavior
- Initializes I2C with board-specific pins.
- Polls slave `0x08` every 5 seconds.
- Sends `GET_STATUS` (`0x20`) and falls back to legacy (`0x00`) if needed.
- Sends `GET_DATA` (`0x01`) and prints parsed `SensorData` fields.

## Troubleshooting
- `I2C Connection Error: 2`: no ACK from slave address, usually wiring/power/GND issue.
- No data but device detected: command mismatch or struct/protocol mismatch between master and slave firmware.
