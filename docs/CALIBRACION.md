# Procedimiento de calibración — Monitor de ruido ambiental

## 1. Alcance y normativa

Este documento describe cómo calibrar y verificar los dos nodos sensores del
proyecto:

- **Nodo analógico**: ESP32-C3 + **MAX4466** (cápsula electret y ADC). Su
  sensibilidad depende del potenciómetro de ganancia, así que **siempre**
  necesita calibrador.
- **Nodo digital**: XIAO ESP32-S3 + **ICS-43434** (MEMS con salida I2S). Su
  sensibilidad viene especificada de fábrica, pero las unidades de este
  proyecto se apartan mucho de ella y el firmware lleva un trim verificado
  (§5).

El procedimiento es compatible con:

- **UNE-EN ISO 1996-2:2009** (Acústica. Medición del ruido ambiental. Parte 2: Determinación de los niveles de presión sonora).
- **Decreto 213/2012** (País Vasco), o equivalentes (p. ej. Decreto 266/2004, C. Valenciana), en lo referente a indicadores \(L_{Aeq}\), \(L_d\), \(L_e\), \(L_n\), \(L_{den}\), \(L_{10}\) y \(L_{90}\) en dB(A).

La calibración establece la relación entre lo que entrega el micrófono (mV en
el nodo analógico, fracción del fondo de escala en el digital) y el nivel de
presión sonora en dB(A), de modo que el firmware reporte valores coherentes con
un calibrador acústico de referencia.

---

## 2. Instrumentación necesaria

| Elemento | Especificación | Uso |
|----------|----------------|-----|
| **Calibrador acústico** | Clase 1 o 2 (IEC 60942), 94,0 dB a 1 kHz (104 dB opcional) | Referencia de nivel y comprobación de linealidad. |
| **Sonómetro de referencia** | Clase 1 o 2 (IEC 61672-1) | Verificación en campo libre; imprescindible para el ajuste fino del nodo digital (§5.4). |

El calibrador debe estar en fecha de verificación metrológica. A 1 kHz la
ponderación A es 0 dB, por lo que 94 dB SPL se consideran 94 dB(A) para el
procedimiento.

---

## 3. Dónde vive la calibración

Cada nodo tiene una constante de compilación, común a todas las unidades del
mismo montaje, y un offset por unidad que se guarda en la NVS del chip:

| Nodo | Constante de compilación (`platformio.ini`) | Offset por unidad |
| :--- | :--- | :--- |
| Analógico | `-D CALIBRATION_RMS_MV=<mV>` (y `CALIBRATION_DB`, 94 por defecto) en `[env:lolin_c3_mini]` | `CMD_SET_CALIB` |
| Digital | `-D MIC_OFFSET_DB=-13.71` en `[env:seeed_xiao_esp32s3]` | `CMD_SET_CALIB` |

No hay que editar ningún fuente: los valores por defecto de `src/DSP_Engine.h`
(`CALIBRATION_RMS_MV` 166,0) y de `src/main_i2s.cpp` (`MIC_OFFSET_DB` 0, la
sensibilidad del datasheet) solo se usan si `platformio.ini` no define otros.

Fórmulas que aplica el firmware:

- Analógico: \(L = 20\log_{10}(V_{rms}/\texttt{CALIBRATION\_RMS\_MV}) + \texttt{CALIBRATION\_DB} + \text{offset}_{NVS}\)
- Digital: \(L = 94 + 20\log_{10}(\text{rms}_{FS}) + 26 + 3{,}0103 + \texttt{MIC\_OFFSET\_DB} + \text{offset}_{NVS}\)

**El offset por unidad (`CMD_SET_CALIB`).** El maestro lo envía en centésimas
de dB (ver [docs/COMUNICACION.md](COMUNICACION.md)). Cuatro cosas que conviene
tener presentes:

1. **Sustituye** al guardado, no se suma. Como los niveles del nodo ya lo
   incluyen, para corregir un nodo que marca `medido` frente a una referencia
   `ref` hay que enviar `actual + (ref − medido)`. La función
   `calibrateNode(ref, medido)` del maestro de ejemplo lo hace sola.
2. Solo se aceptan valores entre −30 y +30 dB, al recibirlo y al cargarlo.
3. Se guarda en NVS: sobrevive a reinicios **y a reflasheos**.
4. Por eso, al recalibrar desde cero (un `CALIBRATION_RMS_MV` nuevo, un
   `MIC_OFFSET_DB` distinto), **el offset antiguo hay que ponerlo a 0**: si no,
   se sumaría a la calibración nueva. Los dos firmwares de calibración muestran
   al arrancar el offset guardado en el chip y lo borran si se compilan con
   `-D RESET_NVS_CALIB`; también se puede enviar `CMD_SET_CALIB` con 0.

---

## 4. Nodo analógico (MAX4466)

### 4.1 Descripción

El **MAX4466** es un preamplificador de micrófono electreto con:

- **Salida:** tensión analógica (AC sobre una continua de VCC/2).
- **Ganancia:** ajustable mediante potenciómetro (típ. 25×–125×).
- **Alimentación:** 2,4 V–5 V (usar 3,3 V con ESP32).
- **Ancho de banda:** suficiente para el rango audible (20 Hz–20 kHz).

La cápsula electreto no es un transductor de medición certificado; el sistema es adecuado para **monitorización orientativa** (mapas de ruido, Smart City) según el estudio técnico del proyecto, no para informes legales vinculantes sin verificación metrológica.

### 4.2 Conexión al ESP32-C3

| Señal MAX4466 | Pin ESP32-C3 | Notas |
|---------------|--------------|--------|
| **VCC** | 3,3 V | Alimentación estable. |
| **GND** | GND | Referencia común con el ESP32. |
| **OUT** | **GPIO 4** (ADC1_CHANNEL_4) | Entrada analógica del nodo. |

- Mantener cables de micrófono cortos y alejados de líneas de alimentación o digitales para reducir ruido y acoplo.
- No conectar la salida del MAX4466 a tensiones superiores a VCC ni inferiores a GND (riesgo de daño).

### 4.3 Ajuste del potenciómetro (ganancia) y techo de medida

El potenciómetro regula la **ganancia** del preamplificador, y con ella dos
cosas a la vez: el suelo y el techo del nodo en dB SPL.

El techo lo pone el ADC. La salida del MAX4466 está centrada en ~1,65 V y el
ADC del C3, con la atenuación que usa el firmware, llega hasta ~2,5 V: la
semionda positiva se queda sin margen a unos **0,85 V de pico (~0,6 V RMS)**.
A partir de ahí el contador de recortes del firmware marca el segundo como
inválido. Por tanto:

| RMS a 94 dB (`CALIBRATION_RMS_MV`) | Techo aproximado |
| ---: | ---: |
| 100 mV | ~109,5 dB |
| 166 mV | ~105 dB |
| 250 mV | ~102 dB |
| 400 mV | ~97,5 dB |

Más ganancia baja el suelo en dB SPL (el ruido del ADC pesa menos) pero baja
también el techo. Para ruido urbano, entre 100 y 250 mV a 94 dB es un buen
compromiso.

Pasos recomendados, con el firmware de calibración en marcha (§4.5):

1. **Condiciones iniciales**: alimentar a 3,3 V; en ambiente tranquilo y sin
   calibrador, potenciómetro hacia la **mínima ganancia**.
2. **Subir ganancia hasta ver señal útil**: colocar el calibrador (94 dB,
   1 kHz) con buen acoplamiento y girar hacia **mayor ganancia** hasta que el
   RMS suba de forma estable al rango elegido.
3. **Evitar la saturación**: el contador `clip` del firmware de calibración
   tiene que quedarse en **0** y el `bias` dentro de 800–2600 mV. Si `clip`
   sube, reducir ganancia y repetir.
4. **Anotar** el valor medio estable de **RMS (mV)**: será `CALIBRATION_RMS_MV`.

### 4.4 Preparación mecánica y acústica

1. **Colocación del calibrador**
   - Colocar la cápsula del micrófono **dentro del casquillo del calibrador** según las instrucciones de su fabricante.
   - Asegurar un **sellado acústico** adecuado (sin fugas). Cualquier fuga reduce el nivel aplicado al micrófono y falsea la calibración.
2. **Entorno**
   - Evitar ruidos externos fuertes durante la toma del valor de referencia.
   - El calibrador debe estar encendido y estabilizado (normalmente unos segundos).
3. **Temperatura**
   - La sensibilidad del electreto varía con la temperatura. Anotarla en el registro de calibración.

### 4.5 Firmware de calibración

[examples/calibration/](../examples/calibration/) ejecuta la misma cadena de
medida que el firmware principal (ADC a 16 kHz, eliminación de continua,
ponderación A, RMS) y muestra cada segundo, sin I2C:

```text
RMS: 166.20 mV | LAeq: 94.0 dB(A) | bias: 1652 mV | clip: 0
```

1. Abrir en PlatformIO la carpeta **`examples/calibration`** como proyecto,
   entorno **lolin_c3_mini**, compilar y subir.
2. Abrir el **Monitor Serie** a **115200 baudios**.
3. Sin calibrador, comprobar que el RMS es bajo y el `bias` está en rango.
4. Con el **calibrador a 94 dB**, ajustar el potenciómetro (§4.3) y esperar a
   que el valor se estabilice.
5. **Anotar el RMS (mV) medio estable** (promediar 5-10 lecturas).
6. Al arrancar, el sketch muestra el offset por unidad guardado en la NVS del
   chip. Si no es 0, bórralo (§3, punto 4).

### 4.6 Ajuste del firmware principal

En el `platformio.ini` de la raíz, entorno `lolin_c3_mini`:

```ini
build_flags =
    -D CALIBRATION_RMS_MV=<RMS anotado>
```

`CALIBRATION_DB` solo hay que definirlo si el calibrador no está a 94,0 dB.
Recompilar y flashear el firmware de producción.

### 4.7 Verificación

1. **Punto 94 dB**: con el calibrador a 94 dB y el mismo acoplamiento, el nodo
   debe marcar **94,0 ±0,3 dB**.
2. **Linealidad (opcional)**: si el calibrador tiene 104 dB, debe marcar
   **104,0 ±0,5 dB**, siempre que `clip` siga en 0. A **114 dB** el ADC recorta
   con cualquier ganancia útil (ver el techo de §4.3) y la lectura no vale: es
   falta de rango, no de linealidad.
3. **Consistencia**: \(L_{Aeq,1s}\), \(L_d\), \(L_e\), \(L_n\), \(L_{den}\),
   \(L_{10}\) y \(L_{90}\) salen del mismo canal y de la misma referencia; una
   calibración correcta a 94 dB los deja a todos en la misma escala dB(A).

---

## 5. Nodo digital (ICS-43434)

### 5.1 Conversión

El datasheet del ICS-43434 da una sensibilidad de **−26 dBFS a 94 dB SPL**,
especificada sobre el **pico** de la senoide. El firmware mide un RMS, y para
una senoide pico y RMS difieren en 20·log₁₀(√2) = **3,0103 dB**
(`MIC_PEAK_TO_RMS_DB`). La comprobación es el punto de sobrecarga del datasheet:
AOP = 120 dB SPL y sensibilidad = −26 dBFS se diferencian exactamente en 26 dB,
lo que solo cuadra si el fondo de escala lo alcanza el pico de la senoide de
120 dB.

Todo se resume en una constante:

\[
K = -\texttt{MIC\_SENSITIVITY\_DBFS} + \texttt{MIC\_PEAK\_TO\_RMS\_DB} + \texttt{MIC\_OFFSET\_DB} = 26 + 3{,}0103 - 13{,}71 = 15{,}30\ \text{dB}
\]

\[
L_{Aeq} = 94 + \text{dBFS(A)} + K \quad (= 109{,}30 + \text{dBFS(A)})
\]

donde dBFS(A) es el RMS ponderado A que el firmware imprime en su línea de log
(`RMS: … dBFS`). Esa cifra no depende de ninguna constante: es la salida cruda
del micrófono.

### 5.2 Por qué `MIC_OFFSET_DB` = −13,71

Las unidades de este proyecto entregan **13,8 dB más** de lo que implica su
hoja de datos: a 94 dB SPL dan unos −12,2 dBFS de pico frente a los −26
especificados. El motivo de fondo no está explicado; se ha medido, no supuesto.
−13,71 deja K = 15,30 dB, el mismo total que tenía la 3.3.0, la única
configuración que se había verificado contra calibrador antes de esta.

Verificación con la 3.3.2: con un calibrador de 94,0 dB el nodo marca
**94,5-94,6 dB**, y en campo libre **coincide con un sonómetro de
referencia**. La diferencia con el calibrador es del acoplamiento: un
calibrador diseñado para cápsulas de 1/2", aplicado al puerto de un MEMS en una
cavidad pequeña, entrega algo más de nivel del nominal. Por eso en este nodo el
sonómetro, y no el calibrador, es la referencia para el ajuste fino.

La condición es sobre K, no sobre el trim suelto. Historia, para no repetir los
errores:

| Versión | Término pico→RMS | `MIC_OFFSET_DB` | K | Con un calibrador de 94,0 dB |
| :--- | :---: | ---: | ---: | :--- |
| 3.3.0 | no | −10,7 | 15,30 | 93,8-94,0 dB (verificado) |
| 3.3.1 | sí | 0 | 29,01 | ~106,5 dB: **unos 13 dB alto** |
| (descartado) | sí | −15,55 | 13,46 | ~1,8 dB bajo; nunca se publicó |
| 3.3.2 y posteriores | sí | −13,71 | 15,30 | 94,5-94,6 dB; coincide con el sonómetro |

Las lecturas con calibrador son de sesiones distintas, y el acoplamiento del
calibrador al puerto MEMS varía unas décimas de una a otra; entre versiones, la
diferencia exacta es la de K (13,71 dB entre la 3.3.1 y las demás).

- **0** salió de la conjetura de que el −10,7 compensaba el error de 3 dB del
  término pico→RMS y sobraba al arreglarlo. La aritmética nunca lo sostuvo.
- **−15,55** salió de tomar como RMS un −13,46 dBFS impreso por una versión
  antigua del ejemplo de calibración (que medía a 16 kHz, con coeficientes
  viejos y sin el término pico→RMS).

No es un fallo de firmware: entre la 3.3.0 y las siguientes, la ganancia de la
cadena a 1 kHz es 0,000 dB en los dos rates, y la única diferencia de nivel es
el término pico→RMS.

### 5.3 Techo de medida: ~106 dB SPL

Esos 13,8 dB de más se pagan con 13,8 dB menos de margen: el fondo de escala
digital llega a **~106 dB SPL**, no a los 120 dB del AOP nominal. Medido con un
calibrador en tres posiciones:

| Posición | `dBFS(A)` | Pico de un seno con ese RMS |
| :--- | ---: | ---: |
| 94 dB | −15,21 | 0,25 del fondo de escala |
| 104 dB | −5,37 | 0,76 del fondo de escala |
| 114 dB | −9,01 | (la onda ya no es un seno) |

De 94 a 104 la cadena es lineal (la entrada subió 10,00 dB y la medida 9,84).
A 114 dB el micrófono está unos 8 dB por encima de su fondo de escala y su
salida no recorta limpiamente sino que se degrada: lee **menos** que a 104.
Consecuencias:

- **Calibrar con 94 dB** (y comprobar linealidad, si se quiere, con 104).
  **Nunca con 114** en estas unidades.
- Al desplegar, tener presente el techo: un impulso puede pasar de 106 dB. El
  firmware cuenta las muestras por encima de 0,99 del fondo de escala y, con
  más de 10 en un segundo, lo invalida (status 0), así que la sobrecarga queda
  señalada en lugar de publicarse como una medida buena.

### 5.4 Comprobar una unidad

**Con el firmware de producción** (lo más directo): calibrador de 94,0 dB en el
puerto y leer el LAeq de la línea de log. Una unidad igual a la de referencia
marca **94,5 ±0,5 dB** con este tipo de acoplamiento (§5.2).

**Con [examples/calibration_i2s/](../examples/calibration_i2s/)**, que replica
la conversión sin ningún trim y muestra además el factor de cresta (`crest`:
3,0 dB con un tono limpio; otra cosa indica distorsión o fugas), la
sensibilidad de la unidad y el `MIC_OFFSET_DB` que la haría marcar el
calibrador, junto con su diferencia respecto al del firmware. Con el calibrador
acoplado, una unidad igual a la de referencia imprime entre −13,8 y −14,3.

Si una unidad se aparta más de ~1 dB del modelo:

- **No cambies `MIC_OFFSET_DB`**: es el trim común a todas las unidades.
- Corrige **esa unidad** con `CMD_SET_CALIB`, preferiblemente comparándola en
  campo libre con un sonómetro de referencia: `calibrateNode(sonómetro, nodo)`
  desde el maestro de ejemplo.

---

## 6. Registro de calibración (plantilla)

| Campo | Valor |
|-------|--------|
| Fecha | |
| Operador | |
| Nodo (ADC / I2S) y número de serie | |
| Calibrador (marca/modelo) y nivel | 94,0 dB @ 1 kHz |
| ADC: RMS (mV) anotado / `CALIBRATION_RMS_MV` usado | |
| I2S: dBFS(A) con el calibrador / `MIC_OFFSET_DB` del firmware | |
| Offset NVS antes / después | |
| LAeq observado tras calibración | |
| Comparación con sonómetro de referencia (si se hizo) | |
| Temperatura ambiente | |
| Observaciones | |

---

## 7. Resumen rápido (checklist)

**Nodo analógico**

- [ ] MAX4466 conectado: VCC 3,3 V, GND, OUT → GPIO 4.
- [ ] Firmware de calibración: con 94 dB, RMS estable en el rango elegido, `clip: 0`, `bias` en rango.
- [ ] `-D CALIBRATION_RMS_MV=<valor>` en `platformio.ini`; reflashear el firmware principal.
- [ ] Offset NVS a 0.
- [ ] Verificar 94,0 ±0,3 dB (y, si se puede, 104 dB con `clip: 0`).
- [ ] Rellenar el registro.

**Nodo digital**

- [ ] `MIC_OFFSET_DB` del modelo en `platformio.ini` (−13,71).
- [ ] Con el calibrador de 94 dB: 94,5 ±0,5 dB, o `crest` 3,0 dB y `MIC_OFFSET_DB` impreso entre −13,8 y −14,3 en el ejemplo.
- [ ] Si se aparta más de ~1 dB: offset por unidad con `CMD_SET_CALIB`, contra sonómetro en campo libre.
- [ ] Rellenar el registro.

---

## 8. Calibración aproximada en campo (sin calibrador)

Si no se dispone de calibrador acústico:

1. Colocar un sonómetro de referencia (Clase 1 o 2) junto al micrófono.
2. Exponer ambos a un ruido estable (tráfico, ruido rosa) durante al menos un
   minuto y comparar el LAeq de ese intervalo.
3. Corregir el nodo con `calibrateNode(sonómetro, nodo)` desde el maestro: no
   hace falta recompilar. En el nodo analógico, si la diferencia es grande,
   conviene repetir antes el ajuste de ganancia y `CALIBRATION_RMS_MV`.

Esta práctica es orientativa; para trazabilidad metrológica se recomienda la
calibración con calibrador (§4 y §5).

---

## 9. Mantenimiento

- Recalibrar periódicamente (recomendación: cada **6 meses**).
- Recalibrar siempre que se cambie la carcasa del micrófono, la posición del
  sensor, el módulo MAX4466 o la unidad ICS-43434.
