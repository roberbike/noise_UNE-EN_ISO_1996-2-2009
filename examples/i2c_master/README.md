# Ejemplo: Maestro I2C

Maestro I2C de referencia que lee los indicadores de ruido de un nodo sensor
(esclavo en `0x08`) y muestra cómo integrarlo correctamente. Sirve igual para
el nodo analógico (ESP32-C3 + MAX4466) y el digital (XIAO ESP32-S3 + ICS-43434):
el protocolo es idéntico en ambos.

## Mapeo de pines

| Placa maestra | SDA | SCL |
| :--- | :--- | :--- |
| XIAO ESP32-S3 | GPIO 5 | GPIO 6 |
| Lolin S2 Mini | GPIO 8 | GPIO 9 |
| Cualquier otra ESP32 | `SDA` de su variante | `SCL` de su variante |

Lado esclavo: ESP32-C3 → SDA GPIO 8 / SCL GPIO 10; XIAO ESP32-S3 → SDA GPIO 5 /
SCL GPIO 6. **Masa común entre ambas placas** siempre. Pull-ups de 4.7 kΩ a
3.3 V en SDA/SCL solo si las placas no los llevan y el bus falla.

## Flujo de lectura (el patrón correcto)

1. Enviar `GET_STATUS` (`0x20`) y leer 1 byte. **Si no es 1, no leas los
   datos** en este ciclo: no hay nada nuevo que publicar, y saltarlo no pierde
   nada, porque el nodo solo reinicia la ventana de los `...HoldDb` cuando
   sirve una lectura con status 1.
2. Enviar `GET_DATA` (`0x01`) y leer los 88 bytes de `SensorData`.
3. Volver a leer `GET_STATUS`.
4. **Publicar solo si se cumple todo:**
   - Llegaron los 88 bytes.
   - El CRC-16 de la trama es correcto.
   - Las dos lecturas de status valen 1.
   - `cycles` es **distinto** del de la última trama con CRC correcto.
     Distinto, no mayor: tras un reinicio del nodo vuelve a empezar desde 1.
5. Opcional: `GET_METADATA` (`0x50`) → **16 bytes** desde 3.3.1: versión de
   firmware, tipo de nodo (0x01 ADC / 0x02 I2S), `time_synced`, `clip_count`
   del último segundo, offset de calibración, llenado de la ventana L10/L90 y
   cuánto lleva acumulado Lden. La trama ha crecido siempre por el final
   (7 → 9 → 16). **La longitud real se deduce de la versión**, no de los bytes
   recibidos: un maestro ESP32 rellena una respuesta corta hasta la longitud
   pedida. Y la 3.3.0 se identificaba como 3.2.1, así que en un nodo anterior
   a 3.3.1 solo son seguros los 7 primeros bytes.

**Por qué dos lecturas de status.** Status y datos van en transacciones
distintas, y entre ellas el nodo puede cerrar un segundo. Si ese segundo es
inválido (saturación, fallo del micrófono), la trama se sirve con status 0 y
lleva los valores del segundo anterior: publicarla repetiría un punto y un
impulso ya publicados. La segunda lectura lo detecta. Queda un caso residual:
si el segundo inválido se cierra justo **después** de leer los datos, se
descarta una trama buena y, con ella, los `...HoldDb` que llevaba. Es raro
(una ventana de unos 20 ms por segundo, y solo cuando el segundo siguiente es
inválido) y se quitaría del todo con un cambio de protocolo; ver
[docs/COMUNICACION.md](../../docs/COMUNICACION.md).

**La hora hay que enviarla, y en epoch LOCAL.** En este ejemplo la llamada a
`setNodeTime()` está **comentada a propósito**, porque solo tú sabes de dónde
sacas la hora. Pero si la dejas comentada, el nodo nunca tiene reloj: los
metadatos devuelven `time_synced = 0` y **Ld, Le, Ln y Lden se quedan en 0 de
por vida**. El nodo no aplica ninguna zona horaria: usa el epoch que recibe tal
cual, así que hay que enviarle **hora local, no UTC** (en España UTC+1 en
invierno y UTC+2 en verano) y reenviarla tras cada cambio de horario. Si envías
UTC, los índices por franja salen desplazados una o dos horas sin ningún aviso.

**Integridad de la trama.** Los 88 bytes terminan en un CRC-16 que cubre todo
lo anterior. Compruébalo antes de publicar: las guardas de layout detectan un
struct desplazado y `cycles` detecta un nodo congelado, pero una trama
corrompida en tránsito puede traer un `cycles` que avanza y pasaría las dos.

**Ruido impulsivo: usa los campos `...HoldDb`.** `noiseLASmaxDb` y
`noiseLCpeakDb` son máximos **del último segundo**, así que un maestro que
sondee cada 5 s descarta cuatro segundos de cada cinco justo en el indicador
que existe para cazar impulsos. `noiseLASmaxHoldDb` y `noiseLCpeakHoldDb`
retienen el máximo **desde la lectura anterior** y se reinician con ella, de
modo que no se pierde ningún evento sea cual sea tu periodo de sondeo, ni se
cuenta dos veces.

**Un solo maestro por nodo si usas los `...HoldDb`.** Esos dos campos se
reinician en la lectura que los entrega, así que dos maestros sondeando el
mismo nodo se repartirían los impulsos y ninguno vería la serie completa. Con
varios lectores, que solo uno mande `CMD_GET_DATA`; `CMD_GET_STATUS` y
`CMD_GET_METADATA` no consumen nada.

**Por qué tantas comprobaciones.** Una lectura completa no basta: el esclavo
I2C de arduino-esp32 puede rellenar con padding una respuesta corta y hacer que
`requestFrom` devuelva la longitud completa. Y un esclavo con el muestreo
colgado devuelve una estructura válida pero **congelada**: solo el avance de
`cycles` lo detecta. Sin esta comprobación, un nodo colgado pinta líneas planas
creíbles en el dashboard. Detalle completo en
[docs/COMUNICACION.md](../../docs/COMUNICACION.md).

## Calibración desde el maestro

- `calibrateNode(referencia, medido)`: corrige el nodo contra una referencia
  (calibrador o sonómetro). Lee el offset actual de los metadatos y envía
  `actual + (referencia − medido)`, porque el valor que se envía **sustituye**
  al guardado y los niveles del nodo ya lo incluyen. Ejemplo: calibrador a
  94,0 dB y el nodo marca 94,6 → `calibrateNode(94.0, 94.6)`. Necesita un nodo
  3.3.1 o posterior.
- `setNodeCalibration(offset)`: fija un offset absoluto (±30 dB como máximo).

El nodo guarda el offset en NVS: sobrevive a reinicios **y a reflasheos**.
Llámalas una vez, nunca desde `loop()`.

## Qué se lee

El maestro imprime LAeq, LAFmax, LASmax y LCpeak (del último segundo y desde la
lectura anterior), L10, L90, Ld/Le/Ln, Lden y `cycles`, y después los
metadatos. El significado de cada indicador está en el README principal
(§ Qué significan las medidas). Recuerda: **LAeq es el nivel del último
segundo**, no el promedio del intervalo de sondeo; si necesitas el promedio del
intervalo, acumúlalo en el maestro.

## Comportamiento del ejemplo

- Inicializa I2C con los pines de la placa.
- Sondea el esclavo `0x08` cada segundo.
- Envía `GET_STATUS` (`0x20`), con reserva legacy (`0x00`). Con status 0 no
  lee los datos y muestra los metadatos (`clips` dice si fue por saturación).
- Envía `GET_DATA` (`0x01`), vuelve a leer el status, aplica las
  comprobaciones y solo reenviaría a la nube las tramas publicables (punto
  marcado en el código para integrar). Cuando descarta una, dice por qué.
- Lee y muestra los metadatos del nodo.

## Diagnóstico

- `I2C error 2 on the status command`: el esclavo no hace ACK; normalmente
  cableado, alimentación o GND común.
- Detecta el dispositivo pero no hay datos: desajuste de comando o de layout de
  `SensorData` entre maestro y esclavo (deben compilar con la misma versión).
- `SKIP: CRC mismatch` repetido: ruido en el bus. Cables más cortos, pull-ups o
  menos velocidad.
- `SKIP: same second as the previous read` de vez en cuando es normal (dos
  sondeos dentro del mismo segundo del nodo). Si se repite siempre, el muestreo
  del nodo está colgado; el nodo lo reporta con status 0 a los 2 s y el
  watchdog lo reinicia.

---

## English version

# Example: I2C master

Reference I2C master that reads the noise indicators from a sensor node (slave at `0x08`) and shows how to integrate it properly. It works the same for the analog node (ESP32-C3 + MAX4466) and the digital node (XIAO ESP32-S3 + ICS-43434): the protocol is identical.

## Pin mapping

| Master board | SDA | SCL |
| :--- | :--- | :--- |
| XIAO ESP32-S3 | GPIO 5 | GPIO 6 |
| Lolin S2 Mini | GPIO 8 | GPIO 9 |
| Any other ESP32 | its variant's `SDA` | its variant's `SCL` |

Slave side: ESP32-C3 → SDA GPIO 8 / SCL GPIO 10; XIAO ESP32-S3 → SDA GPIO 5 / SCL GPIO 6. **Common ground between both boards is mandatory.** 4.7 kΩ pull-ups to 3.3 V on SDA/SCL only if the boards do not include them and the bus fails.

## Read flow (the correct pattern)

1. Send `GET_STATUS` (`0x20`) and read 1 byte. **If it is not 1, do not read the data** this cycle: there is nothing new to publish, and skipping loses nothing, because the node only restarts the `...HoldDb` window when it serves a read with status 1.
2. Send `GET_DATA` (`0x01`) and read the 88 bytes of `SensorData`.
3. Read `GET_STATUS` again.
4. **Publish only if all of these hold:**
   - All 88 bytes arrived.
   - The frame's CRC-16 is correct.
   - Both status reads are 1.
   - `cycles` is **different** from that of the last frame with a correct CRC. Different, not greater: it restarts from 1 when the node reboots.
5. Optional: `GET_METADATA` (`0x50`) → **16 bytes** as of 3.3.1: firmware version, node type (0x01 ADC / 0x02 I2S), `time_synced`, last-second `clip_count`, calibration offset, L10/L90 window fill, and how much Lden has accumulated. The frame has only ever grown at the end (7 → 9 → 16). **Take the real length from the version**, not from the bytes received: an ESP32 master pads a short reply to the requested length. And 3.3.0 identified itself as 3.2.1, so on a node older than 3.3.1 only the first 7 bytes are certain.

**Why two status reads.** Status and data travel in separate transactions, and the node can close a second in between. If that second is invalid (clipping, microphone fault), the frame is served with status 0 and carries the previous second's values: publishing it would repeat a point and an impulse already published. The second read catches that. One residual case remains: if the invalid second closes right **after** the data read, a good frame is dropped, and with it the `...HoldDb` values it carried. It is rare (a window of about 20 ms per second, and only when the next second is invalid) and a protocol change would remove it entirely; see [docs/COMUNICACION.md](../../docs/COMUNICACION.md).

**The clock must be sent, as a LOCAL epoch.** The `setNodeTime()` call is commented out **on purpose**, because only you know your time source. Leave it commented and the node never gets a clock: metadata reports `time_synced = 0` and **Ld, Le, Ln and Lden stay 0 forever**. The node applies no timezone of its own and uses the epoch exactly as received, so send **local time, not UTC** (in Spain UTC+1 in winter and UTC+2 in summer) and resend it after each DST change. Sending UTC shifts every period index by one or two hours with no warning.

**Frame integrity.** The 88 bytes end in a CRC-16 covering everything before it. Check it before publishing: the layout guards catch a shifted struct and `cycles` catches a frozen node, but a frame corrupted in transit can carry an advancing `cycles` and would pass both.

**Impulsive noise: use the `...HoldDb` fields.** `noiseLASmaxDb` and `noiseLCpeakDb` are maxima **over the last second**, so a master polling every 5 s throws away four seconds in five in the very indicator meant to catch impulses. `noiseLASmaxHoldDb` and `noiseLCpeakHoldDb` hold the maximum **since the previous read** and are reset by it, so no event is lost whatever your polling period, and none is counted twice.

**One master per node if you use the `...HoldDb` fields.** They are reset by the read that delivers them, so two masters polling the same node would split the impulses between them and neither would see the full series. With several readers, let only one send `CMD_GET_DATA`; `CMD_GET_STATUS` and `CMD_GET_METADATA` consume nothing.

**Why so many checks.** A complete read alone is not enough: the arduino-esp32 I2C slave can pad a short reply and make `requestFrom` return the full length. And a slave whose sampling has stalled returns a valid but **frozen** structure: only the advance of `cycles` detects it. Without this check a stuck node draws flat, believable lines in the dashboard. Full details in [docs/COMUNICACION.md](../../docs/COMUNICACION.md).

## Calibration from the master

- `calibrateNode(reference, measured)`: corrects the node against a reference (calibrator or sound level meter). It reads the current offset from the metadata and sends `current + (reference − measured)`, because the value sent **replaces** the stored one and the node's levels already include it. Example: calibrator at 94.0 dB and the node reads 94.6 → `calibrateNode(94.0, 94.6)`. Needs a 3.3.1 or later node.
- `setNodeCalibration(offset)`: sets an absolute offset (±30 dB at most).

The node keeps the offset in NVS: it survives reboots **and reflashing**. Call them once, never from `loop()`.

## What is read

The master prints LAeq, LAFmax, LASmax and LCpeak (over the last second and since the previous read), L10, L90, Ld/Le/Ln, Lden and `cycles`, then the metadata. The meaning of each indicator is in the main README (§ What the measurements mean). Remember: **LAeq is the level of the last second**, not the average over the polling interval; if you need the interval average, accumulate it on the master.

## Example behavior

- Initializes I2C with the board's pins.
- Polls slave `0x08` every second.
- Sends `GET_STATUS` (`0x20`) with legacy fallback (`0x00`). On status 0 it does not read the data and shows the metadata instead (`clips` tells whether clipping was the cause).
- Sends `GET_DATA` (`0x01`), reads the status again, applies the checks and would only forward publishable frames to the cloud (integration point marked in the code). When it drops one, it says why.
- Reads and displays the node metadata.

## Diagnosis

- `I2C error 2 on the status command`: the slave does not ACK; usually wiring, power or common ground.
- Device detected but no data: command or `SensorData` layout mismatch between master and slave (both must be built from the same version).
- Repeated `SKIP: CRC mismatch`: noise on the bus. Shorter wires, pull-ups or a lower speed.
- An occasional `SKIP: same second as the previous read` is normal (two polls within the same node second). If it happens every time, the node's sampling has stalled; the node reports it with status 0 after 2 s and the watchdog resets it.
