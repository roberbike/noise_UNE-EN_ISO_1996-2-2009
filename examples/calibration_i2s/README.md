# Verificación del nodo I2S (XIAO ESP32-S3 + ICS-43434)

Firmware autónomo que ejecuta solo la cadena de medida (I2S + ponderación A + RMS)
y muestra por Serial el LAeq (dB) y el nivel RMS (dBFS) cada segundo. No usa I2C.

## Cableado

![Módulo ICS-43434 MRS179A](../../docs/images/ics43434_mrs179a.png)

Etiquetas según la serigrafía del breakout MRS179A (foto). Otros módulos usan
`WS`/`LRCLK` por `LRCL`, `SD` por `DOUT` y `L/R` por `SEL`.

| Pin del módulo | XIAO ESP32-S3 |
| :--- | :--- |
| SEL | **GND** |
| LRCL | GPIO 3 (D2) |
| DOUT | GPIO 4 (D3) |
| BCLK | GPIO 2 (D1) |
| GND | GND |
| 3V | 3.3V |

**SEL a GND**: es el pin `L/R` del ICS-43434 (no un selector I2S/PDM: el chip no
tiene modo PDM). Con `I2S_CHANNEL_FMT_ONLY_LEFT`, SEL a 3.3 V deja el nodo sin
medir. Al aire funciona por el pull-down interno, pero en campo debe ir a masa.

## Uso

1. Compilar y flashear: entorno `seeed_xiao_esp32s3`.
2. Abrir Monitor Serie a 115200 baud.
3. El LAeq mostrado ya es dB SPL directos (sensibilidad de fábrica -26 dBFS @ 94 dB SPL).
4. Verificación opcional con calibrador acústico a 94 dB (1 kHz): acoplar el
   micrófono y anotar el LAeq estable. Si difiere de 94.0, definir en el firmware
   principal `-D MIC_OFFSET_DB=<94.0 - LAeq_medido>` en `build_flags`.

## Resultado esperado

Suelo de ruido en interior tranquilo: **~34 dB**, con variación segundo a
segundo (frente a ~58-60 dB clavados del nodo MAX4466). Ejemplo real:

```
[ICS43434] LAeq:34.8 | LAFmx:36.1 | L10:34.9 | L90:34.0 | RMS:55uFS | Lden:0.0
[ICS43434] LAeq:40.7 | LAFmx:47.2 | L10:34.7 | L90:34.0 | RMS:108uFS | Lden:0.0
```

Notas sobre esa salida:
- **L10/L90 se mantienen constantes** entre bloques: se recalculan cada 20 s
  completos y conservan el valor del último bloque.
- **Lden = 0.0** es correcto aquí: requiere hora válida en el nodo, que llega
  del master vía comando legacy de set-time. En el firmware autónomo nunca se
  puebla.

## Diagnóstico

| Síntoma | Causa probable |
| :--- | :--- |
| `[WARN] Silencio absoluto` | SEL a 3.3 V, o línea DOUT sin conectar |
| Silencio con SEL a GND | Módulo con `L/R` fijado en alto: usar `I2S_CHANNEL_FMT_ONLY_RIGHT` |
| Lecturas erráticas | Cables largos (>10 cm) en BCLK/DOUT, o masa mal referenciada |
| Avisos de compilación sobre API obsoleta | Normal en core 3.x; ver `platformio.ini` (flags de supresión) |
