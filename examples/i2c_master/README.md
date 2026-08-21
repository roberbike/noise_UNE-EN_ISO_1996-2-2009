# Ejemplo: Maestro I2C (ESP32-S2 / ESP32-S3)

Maestro I2C de referencia que lee los indicadores de ruido de un nodo sensor
(esclavo en `0x08`) y muestra cómo integrarlo correctamente. Sirve igual para
el nodo analógico (ESP32-C3 + MAX4466) y el digital (XIAO ESP32-S3 + ICS-43434):
el protocolo es idéntico en ambos.

## Mapeo de pines

| Placa maestra | SDA | SCL |
| :--- | :--- | :--- |
| XIAO ESP32-S3 | GPIO 5 | GPIO 6 |
| Lolin S2 Mini | GPIO 8 | GPIO 9 |

Lado esclavo: ESP32-C3 → SDA GPIO 8 / SCL GPIO 10; XIAO ESP32-S3 → SDA GPIO 5 /
SCL GPIO 6. **Masa común entre ambas placas** siempre. Pull-ups de 4.7 kΩ a
3.3 V en SDA/SCL solo si las placas no los llevan y el bus falla.

## Flujo de lectura (el patrón correcto)

1. Enviar `GET_STATUS` (`0x20`) y leer 1 byte.
2. Si status == 1, enviar `GET_DATA` (`0x01`) y leer `sizeof(SensorData)` bytes.
3. **Validación triple antes de publicar** (las tres, no basta una):
   - Lectura completa (`sizeof(SensorData)` bytes recibidos).
   - status == 1.
   - `cycles` mayor que el de la lectura anterior.
4. Opcional: `GET_METADATA` (`0x50`) → 7 bytes con versión de firmware, tipo de
   nodo (0x01 ADC / 0x02 I2S), time_synced y clip_count del último segundo.

**Por qué la validación triple.** Una lectura completa no basta: el esclavo I2C
de arduino-esp32 puede rellenar con padding una respuesta corta y hacer que
`requestFrom` devuelva la longitud completa. Y un esclavo con el muestreo
colgado devuelve una estructura válida pero **congelada** — solo el avance de
`cycles` lo detecta. Sin esta comprobación, un nodo colgado pinta líneas planas
creíbles en el dashboard. Detalle completo en
[docs/COMUNICACION.md](../../docs/COMUNICACION.md).

## Qué se lee

El maestro imprime LAeq, LAFmax, L10, L90, Lden y `cycles`. El significado de
cada indicador está en el README principal (§ Qué significan las medidas).
Recuerda: **LAeq es el nivel del último segundo**, no el promedio del intervalo
de sondeo; si necesitas el promedio del intervalo, acumúlalo en el maestro.

## Comportamiento del ejemplo

- Inicializa I2C con los pines de la placa.
- Sondea el esclavo `0x08` cada 5 segundos.
- Envía `GET_STATUS` (`0x20`), con reserva legacy (`0x00`).
- Envía `GET_DATA` (`0x01`), aplica la validación triple y solo reenviaría a la
  nube las muestras publicables (punto marcado en el código para integrar).
- Lee y muestra los metadatos del nodo.

## Diagnóstico

- `I2C Connection Error: 2`: el esclavo no hace ACK — normalmente cableado,
  alimentación o GND común.
- Detecta el dispositivo pero no hay datos: desajuste de comando o de layout de
  `SensorData` entre maestro y esclavo (deben compilar con la misma versión).
- Valores planos en el dashboard con el nodo respondiendo: revisa que aplicas la
  validación por `cycles`; si `cycles` no avanza, el muestreo del nodo está
  colgado (la v3.1.2+ lo reporta con status 0 y el watchdog lo reinicia).
