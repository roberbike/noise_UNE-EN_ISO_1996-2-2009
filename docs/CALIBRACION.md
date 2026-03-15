# Procedimiento de calibración — Monitor de ruido ambiental

## 1. Alcance y normativa

Este documento describe el procedimiento de calibración del monitor de ruido basado en **ESP32-C3** y **MAX4466**, de forma compatible con:

- **UNE-EN ISO 1996-2:2009** (Acústica. Medición del ruido ambiental. Parte 2: Determinación de los niveles de presión sonora).
- **Decreto 213/2012** (País Vasco), o equivalentes (p. ej. Decreto 266/2004, C. Valenciana), en lo referente a indicadores \(L_{Aeq}\), \(L_d\), \(L_e\), \(L_n\), \(L_{den}\), \(L_{10}\) y \(L_{90}\) en dB(A).

La calibración establece la relación entre la tensión RMS a la salida del preamplificador (en mV) y el nivel de presión sonora en dB(A), de modo que el firmware reporte valores coherentes con un calibrador acústico de referencia.

---

## 2. Instrumentación necesaria

| Elemento | Especificación | Uso |
|----------|----------------|-----|
| **Calibrador acústico** | Clase 1 o 2 (IEC 60942), 94,0 dB a 1 kHz (y opcionalmente 114 dB) | Referencia de nivel para el punto de calibración. |
| **Sonómetro de referencia** (opcional) | Clase 1 o 2 (IEC 61672-1) | Verificación cruzada en campo. |

El calibrador debe estar en fecha de verificación metrológica. A 1 kHz la ponderación A es ≈ 0 dB, por lo que 94 dB SPL se consideran 94 dB(A) para el procedimiento.

---

## 3. Hardware: MAX4466

### 3.1 Descripción

El **MAX4466** es un preamplificador de micrófono electreto con:

- **Salida:** tensión analógica (AC sobre nivel de continua).
- **Ganancia:** ajustable mediante potenciómetro (típ. 25×–125×).
- **Alimentación:** 2,4 V–5 V (usar 3,3 V con ESP32).
- **Ancho de banda:** suficiente para el rango audible (20 Hz–20 kHz).

La cápsula electreto no es un transductor de medición certificado; el sistema es adecuado para **monitorización orientativa** (mapas de ruido, Smart City) según el estudio técnico del proyecto, no para informes legales vinculantes sin verificación metrológica.

### 3.2 Conexión al ESP32-C3

| Señal MAX4466 | Pin ESP32-C3 | Notas |
|---------------|--------------|--------|
| **VCC** | 3,3 V | Alimentación estable. |
| **GND** | GND | Referencia común con el ESP32. |
| **OUT** | **GPIO 4** (ADC1_CHANNEL_4) | Entrada analógica; en el firmware es el canal usado para LAeq. |

- Mantener cables de micrófono cortos y alejados de líneas de alimentación o digitales para reducir ruido y acoplo.
- No conectar la salida del MAX4466 a tensiones superiores a VCC ni inferiores a GND (riesgo de daño).

### 3.3 Ajuste del potenciómetro (ganancia)

El potenciómetro del módulo MAX4466 regula la **ganancia** del preamplificador. Objetivo en calibración:

- Con el **calibrador a 94 dB** y la cápsula correctamente colocada en el calibrador, la señal en el ADC debe ser **suficientemente alta** para una buena relación señal/ruido, pero **sin saturar** el ADC (0–3,3 V en ESP32-C3 con atenuación por defecto).

Pasos recomendados:

1. **Condiciones iniciales**
   - Alimentar el módulo a 3,3 V.
   - Sin calibrador, en ambiente tranquilo: ganancia baja (potenciómetro hacia la posición de **mínima ganancia**).

2. **Subir ganancia hasta ver señal útil**
   - Con el firmware de calibración en ejecución (véase sección 5), observar **RMS (mV)** en el monitor serie.
   - En silencio, el valor suele ser bajo (decenas de mV o menos).  
   - Colocar el calibrador sobre el micrófono (94 dB, 1 kHz), asegurando buen acoplamiento acústico.
   - Girar el potenciómetro **hacia mayor ganancia** hasta que RMS (mV) suba de forma estable (p. ej. en el rango 100–250 mV para 94 dB, según cápsula y módulo).

3. **Evitar saturación**
   - Comprobar que, con 94 dB, **RMS (mV)** no se acerca al máximo del ADC (≈ 3300 mV con referencia 3,3 V). Dejar margen (p. ej. < 2000 mV) para picos y para la prueba a 114 dB.
   - Si se satura (valores máximos planos o inestables), **reducir ganancia** (potenciómetro hacia mínima ganancia) y repetir.

4. **Punto de trabajo recomendado**
   - Objetivo orientativo: con **94 dB** en el calibrador, **RMS (mV)** estable entre **100 mV y 400 mV** (por ejemplo 150–250 mV).  
   - Anotar el valor medio estable de **RMS (mV)** que se usará como `CALIBRATION_RMS_MV` en el firmware (sección 6).

Resumen del potenciómetro:

- **Giro hacia mayor resistencia en la rama de ganancia** → más ganancia → más mV a la salida para el mismo SPL.
- **Giro hacia menor ganancia** → menos mV; usar si hay saturación o si en 94 dB los mV son demasiado altos.

---

## 4. Preparación mecánica y acústica

1. **Colocación del calibrador**
   - Colocar la cápsula del micrófono **dentro del casquillo del calibrador** según las instrucciones del fabricante del calibrador.
   - Asegurar un **sellado acústico** adecuado (sin fugas). Cualquier fuga reduce el nivel aplicado al micrófono y falsea la calibración.

2. **Entorno**
   - Evitar ruidos externos fuertes durante la toma del valor de referencia (habitación tranquila o calibrador bien acoplado).
   - El calibrador debe estar encendido y estabilizado (normalmente unos segundos).

3. **Temperatura**
   - La sensibilidad del electreto puede variar con la temperatura. Documentar la temperatura ambiente en el acta de calibración.

---

## 5. Uso del firmware de calibración

En el repositorio se incluye un **ejemplo de calibración** que ejecuta únicamente la cadena de medida (ADC + ponderación A + RMS) y envía por **Serial** el valor de **RMS (mV)** y **LAeq (dB)** cada segundo, sin I2C ni lógica de esclavo.

### 5.1 Flashing del ejemplo

1. Abrir en PlatformIO la carpeta **`examples/calibration`** como proyecto (o desde la raíz del repo, compilar el ejemplo de calibración si tu flujo lo permite).
2. Seleccionar el entorno **lolin_c3_mini** (nodo sensor ESP32-C3).
3. Compilar y subir el firmware a la placa.
4. Conectar el ESP32-C3 por USB y abrir **Monitor Serie** a **115200 baud**.

Véase también **`examples/calibration/README.md`**.

### 5.2 Qué hacer en calibración

1. Con el **calibrador apagado** y el micrófono en ambiente tranquilo, comprobar que se imprimen valores de RMS (mV) coherentes (bajos).
2. **Encender el calibrador a 94 dB** (1 kHz) y colocar el micrófono correctamente en el calibrador.
3. Esperar **estabilización** (varios segundos). El firmware muestra cada segundo: **RMS (mV)** y **LAeq (dB)**.
4. **Anotar el valor medio estable de RMS (mV)** (por ejemplo, promediar 5–10 lecturas estables). Ese valor se usará como `CALIBRATION_RMS_MV` en el firmware principal.
5. Comprobar que **LAeq (dB)** se acerca a **94,0 dB** una vez actualizado `CALIBRATION_RMS_MV` en el código de calibración (o tras volver a flashear el firmware principal con ese valor y repetir la medida).

Si el ejemplo de calibración ya tiene definidos `CALIBRATION_DB = 94.0f` y un `CALIBRATION_RMS_MV` de prueba, al introducir el RMS (mV) medido como nueva constante, LAeq debería acercarse a 94 dB. Cualquier desviación persistente indica que hay que revisar acoplamiento acústico, ganancia del potenciómetro o linealidad del ADC.

---

## 6. Ajuste de constantes en el firmware principal

En el firmware del **nodo sensor** (`src/main.cpp`), las constantes que definen la calibración son:

```c
#define CALIBRATION_DB 94.0f       // Nivel de referencia del calibrador [dB(A)]
#define CALIBRATION_RMS_MV 166.0f  // Tensión RMS [mV] medida con el calibrador a CALIBRATION_DB
```

- **CALIBRATION_DB:** nivel que genera el calibrador en la posición de calibración (normalmente **94.0** dB a 1 kHz).
- **CALIBRATION_RMS_MV:** valor **medio estable** de RMS (mV) obtenido con el firmware de calibración cuando el calibrador está a 94 dB y el micrófono bien acoplado (y, si aplica, tras el ajuste del potenciómetro descrito en 3.3).

Fórmula utilizada en el firmware (coherente con ISO 1996-2 en cuanto a nivel equivalente):

\[
L_{Aeq} = 20\,\log_{10}\frac{V_{rms}}{V_{ref}} + L_{ref}
\]

con \(V_{ref} = \texttt{CALIBRATION\_RMS\_MV}\) y \(L_{ref} = \texttt{CALIBRATION\_DB}\).

Tras modificar `CALIBRATION_RMS_MV` (y si se usa otro nivel, `CALIBRATION_DB`), recompilar y flashear el firmware principal del nodo sensor.

---

## 7. Verificación

1. **Punto 94 dB**
   - Con el calibrador a 94 dB y el mismo acoplamiento que en la calibración, el sistema debe indicar **LAeq ≈ 94 dB** (tolerancia típica ±1 dB según estabilidad del calibrador y del ADC).

2. **Linealidad (opcional)**
   - Si el calibrador puede generar **114 dB**, repetir la medida. El sistema debería indicar aproximadamente **114 dB** (±1–2 dB). Desviaciones mayores pueden indicar saturación del ADC o del preamplificador; en ese caso reducir ganancia (potenciómetro) y repetir la calibración a 94 dB.

3. **Consistencia con la norma**
   - Los indicadores \(L_{Aeq,1s}\), \(L_d\), \(L_e\), \(L_n\), \(L_{den}\), \(L_{10}\), \(L_{90}\) se calculan a partir del mismo canal y de la misma referencia; una calibración correcta a 94 dB asegura que todos ellos estén en la misma escala dB(A), conforme a los requisitos orientativos del Decreto 213/2012 y de la UNE-EN ISO 1996-2:2009 para la cadena de medida.

---

## 8. Registro de calibración (plantilla)

Recomendación: rellenar un registro como el siguiente en cada calibración.

| Campo | Valor |
|-------|--------|
| Fecha | |
| Operador | |
| Calibrador (marca/modelo) | |
| Nivel del calibrador | 94,0 dB @ 1 kHz |
| RMS (mV) anotado | |
| CALIBRATION_RMS_MV usado | |
| CALIBRATION_DB usado | 94,0 |
| LAeq observado tras calibración | |
| Temperatura ambiente | |
| Observaciones | |

---

## 9. Resumen rápido (checklist)

- [ ] MAX4466 conectado: VCC 3,3 V, GND, OUT → GPIO 4.
- [ ] Potenciómetro ajustado: con 94 dB, RMS (mV) estable en rango útil (p. ej. 100–400 mV) sin saturación.
- [ ] Calibrador 94 dB @ 1 kHz, micrófono bien acoplado y sellado.
- [ ] Firmware de calibración ejecutándose; anotar RMS (mV) medio estable.
- [ ] Actualizar `CALIBRATION_RMS_MV` en `src/main.cpp` y, si procede, `CALIBRATION_DB`.
- [ ] Reflashear firmware principal y verificar LAeq ≈ 94 dB.
- [ ] (Opcional) Verificar linealidad a 114 dB.
- [ ] Documentar en registro de calibración.

Mantener este procedimiento garantiza la compatibilidad del sistema con los requisitos de la **UNE-EN ISO 1996-2:2009** y del **Decreto 213/2012** en lo referente a la relación entre señal eléctrica y nivel en dB(A).

---

## 10. Calibración aproximada en campo (sin calibrador)

Si no se dispone de calibrador acústico:

1. Colocar un sonómetro de referencia (Clase 1 o 2) junto al micrófono.
2. Exponer ambos a un ruido estable (tráfico, ruido blanco).
3. Ajustar `CALIBRATION_RMS_MV` en el firmware hasta que el LAeq del monitor coincida con el del sonómetro.

Esta práctica es orientativa; para trazabilidad metrológica se recomienda calibración con calibrador (secciones 1–7).

---

## 11. Mantenimiento

- Recalibrar periódicamente (recomendación: cada **6 meses**).
- Recalibrar siempre que se cambie la carcasa del micrófono, la posición del sensor o el módulo MAX4466.
