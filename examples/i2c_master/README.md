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
4. Opcional: `GET_METADATA` (`0x50`) → **16 bytes** desde 3.3.1: versión de
   firmware, tipo de nodo (0x01 ADC / 0x02 I2S), `time_synced`, `clip_count`
   del último segundo, offset de calibración, llenado de la ventana L10/L90 y
   cuánto lleva acumulado Lden. La trama ha crecido siempre por el final
   (7 → 9 → 16), así que pide la longitud más larga que conozcas y **acepta
   una respuesta más corta** de un nodo antiguo.

**La hora hay que enviarla, y en epoch LOCAL.** En este ejemplo la llamada a
`setNodeTime()` está **comentada a propósito**, porque solo tú sabes de dónde
sacas la hora. Pero si la dejas comentada, el nodo nunca tiene reloj: los
metadatos devuelven `time_synced = 0` y **Ld, Le, Ln y Lden se quedan en 0 de
por vida**. El nodo no aplica ninguna zona horaria: usa el epoch que recibe tal
cual, así que hay que enviarle **hora local, no UTC** (en España UTC+1 en
invierno y UTC+2 en verano) y reenviarla tras cada cambio de horario. Si envías
UTC, los índices por franja salen desplazados una o dos horas sin ningún aviso.

**Integridad de la trama (3.3.1).** Los 88 bytes terminan en un CRC-16 que
cubre todo lo anterior. Compruébalo antes de publicar: las guardas de layout
detectan un struct desplazado y `cycles` detecta un nodo congelado, pero una
trama corrompida en tránsito puede traer `status = 1` y un `cycles` que avanza,
y pasaría las dos. El ejemplo lo verifica y descarta la trama si no cuadra.

**Ruido impulsivo: usa los campos `...HoldDb`.** `noiseLASmaxDb` y
`noiseLCpeakDb` son máximos **del último segundo**, así que un maestro que
sondee cada 5 s descarta cuatro segundos de cada cinco justo en el indicador
que existe para cazar impulsos. `noiseLASmaxHoldDb` y `noiseLCpeakHoldDb`
retienen el máximo **desde tu lectura anterior** y se reinician con ella, de
modo que no se pierde ningún evento sea cual sea tu periodo de sondeo.

**Un solo maestro por nodo si usas los `...HoldDb`.** Esos dos campos se
reinician en la lectura que los entrega, así que dos maestros sondeando el
mismo nodo se repartirían los impulsos y ninguno vería la serie completa. Con
varios lectores, que solo uno mande `CMD_GET_DATA`; `CMD_GET_STATUS` y
`CMD_GET_METADATA` no consumen nada. El nodo solo rearma cuando sirve
`status = 1`, así que sondear incondicionalmente no tira nada.

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

---

## English version

# Example: I2C master (ESP32-S2 / ESP32-S3)

Reference I2C master that reads noise indicators from a sensor node (slave at `0x08`) and shows how to integrate it properly. It works the same for the analog node (ESP32-C3 + MAX4466) and the digital node (XIAO ESP32-S3 + ICS-43434): the protocol is identical.

## Pin mapping

| Master board | SDA | SCL |
| :--- | :--- | :--- |
| XIAO ESP32-S3 | GPIO 5 | GPIO 6 |
| Lolin S2 Mini | GPIO 8 | GPIO 9 |

Slave side: ESP32-C3 → SDA GPIO 8 / SCL GPIO 10; XIAO ESP32-S3 → SDA GPIO 5 / SCL GPIO 6. **Common ground between both boards is mandatory.** 4.7 kΩ pull-ups to 3.3 V on SDA/SCL are needed only if the boards do not include them and the bus hangs.

## Read flow (the correct pattern)

1. Send `GET_STATUS` (`0x20`) and read 1 byte.
2. If `status == 1`, send `GET_DATA` (`0x01`) and read `sizeof(SensorData)` bytes.
3. **Triple validation before publishing** (all three are required):
   - Full read (`sizeof(SensorData)` bytes received)
   - `status == 1`
   - `cycles` greater than the previous read
4. Optional: `GET_METADATA` (`0x50`) → **16 bytes** as of 3.3.1: firmware version, node type (0x01 ADC / 0x02 I2S), `time_synced`, last-second `clip_count`, calibration offset, L10/L90 window fill, and how much Lden has accumulated. The frame has only ever grown at the end (7 → 9 → 16), so request the longest length you know and **accept a shorter reply** from an older node.

**The clock must be sent, as a LOCAL epoch.** The `setNodeTime()` call is commented out on purpose — only you know your time source. Leave it commented and the node never gets a clock: metadata reports `time_synced = 0` and **Ld, Le, Ln and Lden stay 0 forever**. The node applies no timezone of its own, so send **local time, not UTC**, and resend it after each DST change; sending UTC shifts every period index by one or two hours with no warning.

**Frame integrity (3.3.1).** The 88 bytes end in a CRC-16 covering everything before it. Check it before publishing: the layout guards catch a shifted struct and `cycles` catches a frozen node, but a frame corrupted in transit can arrive with `status = 1` and an advancing `cycles` and would pass both.

**One master per node if you use the `...HoldDb` fields.** They are reset by the read that delivers them, so two masters polling the same node would split the impulses between them and neither would see the full series. With several readers, let only one send `CMD_GET_DATA`; `CMD_GET_STATUS` and `CMD_GET_METADATA` consume nothing. The node only rearms when it serves `status = 1`, so polling unconditionally loses nothing.

**Impulsive noise: use the `...HoldDb` fields.** `noiseLASmaxDb` and `noiseLCpeakDb` are maxima over the last second only, so a master polling every 5 s throws away four seconds in five in the very indicator meant to catch impulses. `noiseLASmaxHoldDb` and `noiseLCpeakHoldDb` hold the maximum since your previous read and are reset by it.

**Why the triple validation.** A full read alone is not enough: the ESP32 I2C slave HAL can pad a short reply to the full length. A stalled slave can also return a valid but frozen structure; only the advance of `cycles` detects that. Without this check, a stuck node can generate flat but believable lines in the dashboard.

## What is read

The master prints LAeq, LAFmax, L10, L90, Lden and `cycles`. The meaning of each indicator is described in the main README under "What the measurements mean".

## Example behavior

- Initializes I2C with the board pins
- Polls slave `0x08` every 5 seconds
- Sends `GET_STATUS` (`0x20`) with legacy fallback (`0x00`)
- Sends `GET_DATA` (`0x01`), applies the triple validation, and only forwards publishable samples to the cloud
- Reads and displays node metadata

## Diagnosis

- `I2C Connection Error: 2`: slave does not ACK, usually due to wiring, power or common ground
- Device detected but no data: command mismatch or `SensorData` layout mismatch between master and slave (must be compiled from the same version)
- Flat values in the dashboard while the node responds: verify that you are checking `cycles`; if `cycles` does not move, the node sampling is stuck and should be marked as not valid
