# Verificación del nodo I2S (XIAO ESP32-S3 + ICS-43434)

Firmware autónomo que ejecuta solo la cadena de medida (I2S + ponderación A + RMS)
y muestra por Serial el LAeq (dB) y el nivel RMS (dBFS) cada segundo. No usa I2C.

## Cableado

| ICS-43434 | XIAO ESP32-S3 |
| :--- | :--- |
| SCK (BCLK) | GPIO 2 (D1) |
| WS (LRCLK) | GPIO 3 (D2) |
| SD | GPIO 4 (D3) |
| VDD | 3.3V |
| GND | GND |
| L/R | GND |

## Uso

1. Compilar y flashear: entorno `seeed_xiao_esp32s3`.
2. Abrir Monitor Serie a 115200 baud.
3. El LAeq mostrado ya es dB SPL directos (sensibilidad de fábrica -26 dBFS @ 94 dB SPL).
4. Verificación opcional con calibrador acústico a 94 dB (1 kHz): acoplar el
   micrófono y anotar el LAeq estable. Si difiere de 94.0, definir en el firmware
   principal `-D MIC_OFFSET_DB=<94.0 - LAeq_medido>` en `build_flags`.
