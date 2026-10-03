# Estudio técnico y de conformidad: Monitor de ruido ambiental

## 1. Introducción
Este documento analiza la viabilidad técnica y el encaje normativo de los dos nodos del proyecto, ESP32-C3 + MAX4466 (analógico) y XIAO ESP32-S3 + ICS-43434 (digital), frente al **Decreto 213/2012** (País Vasco) o equivalentes (como el Decreto 266/2004 en C. Valenciana) y la norma **UNE-EN ISO 1996-2:2009**.

## 2. Análisis del hardware y capacidad de medida

### 2.1. Micrófono analógico (MAX4466)
El módulo MAX4466 utiliza una cápsula de electreto conectada a un preamplificador con ganancia ajustable.
- **Respuesta en frecuencia:** el amplificador tiene un GBW de 600 kHz, de sobra para el rango audible (20 Hz-20 kHz). La cápsula de electreto estándar, en cambio, no es plana: cae de forma apreciable por debajo de 100 Hz y por encima de 10 kHz.
- **Relación señal/ruido:** el MAX4466 tiene un PSRR excelente (112 dB), lo que minimiza el ruido de la fuente de alimentación. El ruido térmico y el del ADC del ESP32 limitan el rango dinámico por abajo: el suelo del nodo queda en ~58-60 dB.

### 2.2. Digitalización (ADC del ESP32-C3)
- **Resolución:** 12 bits (4096 niveles). Teóricamente ~72 dB de rango dinámico; en la práctica, por la no linealidad del ADC del ESP32 y el ruido electrónico, el rango efectivo es de unos **50-60 dB**.
- **Techo:** la salida del MAX4466 está centrada en ~1,65 V y el ADC llega a ~2,5 V con la atenuación usada, así que la semionda positiva se queda sin margen a ~0,85 V de pico. Con la ganancia habitual el techo queda entre ~97 y ~109 dB (ver [CALIBRACION.md](CALIBRACION.md) §4.3).
- **Frecuencia de muestreo:** **16 kHz** exactos (límite superior útil ~8 kHz por Nyquist), suficiente para la banda de interés del ruido urbano, cuya energía se concentra muy por debajo.

### 2.3. Micrófono digital (ICS-43434)
- MEMS con salida I2S de 24 bits y su propio filtro de decimación, así que no tiene el problema de aliasing del nodo ADC (§6).
- Datasheet: sensibilidad −26 dBFS a 94 dB SPL (sobre el pico de la senoide), AOP 120 dB SPL, SNR 65 dBA.
- Las unidades de este proyecto leen **13,8 dB por encima** de esa sensibilidad. El firmware lo corrige (`MIC_OFFSET_DB` = −13,71, verificado con calibrador y con sonómetro de referencia), pero el margen perdido no se recupera: el fondo de escala digital llega a **~106 dB SPL**. Ver [CALIBRACION.md](CALIBRACION.md) §5.
- Suelo medido en banco: **~34 dB**. Es lo que permite medir noches urbanas tranquilas.
- Frecuencia de muestreo: **48 kHz**, con la tarea de audio anclada a un núcleo del S3, que tiene FPU.

## 3. Conformidad con el Decreto y la normativa

### 3.1. Clases de precisión (IEC 61672-1)
El Decreto y la norma UNE 1996-2 exigen instrumentos de **Clase 1** (mediciones de precisión o legales) o **Clase 2** (mediciones de campo generales).
- **Estado actual:** ninguno de los dos nodos tiene certificación de clase. El nodo analógico, por su ADC y su cápsula, es un instrumento de monitorización, no apto para denuncias ni informes oficiales vinculantes. El digital mejora sustancialmente (respuesta plana, más rango dinámico, salida digital) y con la protección adecuada (§7) puede aproximarse a tolerancias de Clase 2, pero **la certificación formal requiere ensayo en laboratorio acreditado**.
- **Uso legal:** monitorización preventiva y mapas de ruido, sí; certificar superaciones de límites legales en un procedimiento sancionador sin verificación metrológica oficial, no.

### 3.2. Ponderación frecuencial (A y C)
La normativa exige niveles en dB(A); el pico impulsivo se da con ponderación C.
- **Estado actual: implementadas y verificadas** frente a las curvas de IEC 61672-1, normalizadas a 0 dB a 1 kHz. A: 3 biquads (6.º orden); C: 2 biquads (4.º orden).
- **Nodo de 16 kHz:** A dentro de **±0,15 dB** y C dentro de **±0,05 dB** hasta 7,9 kHz. El polo de 12194 Hz del prototipo analógico queda por encima de Nyquist, así que la sección alta no es la bilineal directa (que caía −12 dB a 7 kHz) sino un ajuste por mínimos cuadrados sobre 20 Hz-7,9 kHz.
- **Nodo de 48 kHz:** A y C dentro de **±0,08 dB de 20 Hz a 20 kHz** desde la 3.3.3, con la sección alta ajustada igual. Hasta la 3.3.2 era la bilineal directa sin precompensación, exacta solo hasta 8 kHz (±0,54 dB) y corta por encima: −1,2 dB a 10 kHz, −2,7 dB a 12,5 kHz, −6,4 dB a 16 kHz. Sobre el LAeq de un espectro de tráfico aquello era despreciable (−0,01 dB); con ruido rosa, −0,2 dB; con un siseo de espectro plano, −0,75 dB.
- Los coeficientes son reproducibles con `tools/gen_a_weight.py`.

### 3.3. Ponderación temporal (Fast/Slow)
- **Fast (F):** 125 ms → **LAFmax**.
- **Slow (S):** 1 s → **LASmax**.
- **Estado actual: las dos implementadas**, como envolventes exponenciales del cuadrado de la señal ponderada A, según IEC 61672-1. Además, **LCpeak**: el pico absoluto de la señal ponderada C, sin constante de tiempo, para ruido impulsivo.

## 4. Indicadores calculados

### 4.1. Parámetros de ruido ambiental (Decreto 213/2012)
1. **$L_{Aeq,1s}$**: nivel continuo equivalente de cada segundo.
2. **$L_{AFmax}$, $L_{ASmax}$, $L_{Cpeak}$**: máximos Fast y Slow y pico C del segundo, y sus máximos desde la lectura anterior del maestro.
3. **$L_{10}$ y $L_{90}$**: niveles estadísticos sobre una ventana deslizante de 300 s, para caracterizar eventos y ruido de fondo.
4. **$L_d$, $L_e$, $L_n$**: promedios energéticos de día (7-19 h), tarde (19-23 h) y noche (23-7 h) del día de evaluación, que va de 07:00 a 07:00 para que la noche quede entera.
5. **$L_{den}$**: índice global día-tarde-noche con penalizaciones de +5 dB y +10 dB.

*Ld/Le/Ln/Lden requieren que el maestro envíe la hora local por I2C.*

### 4.2. Corrección por ruido de fondo
Según UNE 1996-2, si el ruido medido está cerca del ruido de fondo (diferencia < 10 dB), se debe aplicar una corrección:
$$L_{corr} = 10 \cdot \log_{10}(10^{L_{med}/10} - 10^{L_{fondo}/10})$$
Aquí es donde el suelo de ~34 dB del ICS-43434 marca la diferencia frente a los ~58 dB del MAX4466.

## 5. Calibración
El procedimiento de los dos nodos, con instrumentación, constantes, verificación y registro, está en [CALIBRACION.md](CALIBRACION.md).

## 6. Limitación conocida: ausencia de filtro antialiasing (nodo ADC)

El nodo analógico muestrea el MAX4466 directamente con el ADC del ESP32-C3, sin
filtro antialiasing analógico. Todo el contenido por encima de 8 kHz se repliega
dentro de la banda de medida. Para espectros urbanos de tráfico ese contenido es
≈1,3 % de la energía ponderada A, por lo que el efecto es menor, pero con
fuentes ricas en agudos (siseos, impulsos) puede ser apreciable.

La tercera sección del filtro A anterior tenía un cero en Nyquist que enmascaraba
parcialmente ese replegado; la versión ajustada (±0,15 dB hasta 7,9 kHz) ya no lo
hace, de modo que el aliasing queda al descubierto en lugar de oculto. Lo mismo
pasa con la ponderación C, y pesa más en LCpeak porque es un pico. La solución
correcta es física: un filtro RC paso bajo de ~8 kHz a la salida del MAX4466. El
nodo digital no tiene este problema: el ICS-43434 incorpora su propio filtro de
decimación.

## 7. Despliegue exterior: camino hacia Clase 2

Para una estación de campo que se acerque a las tolerancias de Clase 2
(IEC 61672-1) en exteriores:

### 7.1. Hardware
1. **Micrófono MEMS I2S** (el nodo digital, ICS-43434): respuesta más plana,
   salida digital (sin el ruido del ADC del ESP32) y mayor SNR.
2. **Pantalla antiviento obligatoria**: espuma acústica de célula abierta de
   **60 mm de diámetro como mínimo**. Reduce el ruido inducido por el viento
   hasta en 20-30 dB y evita falsos eventos.
3. **Protección contra la intemperie**: micrófono en pértiga, con protección
   contra la lluvia (membrana hidrofóbica) que no atenúe las altas frecuencias.
4. **Envolvente IP65/IP67** para la electrónica, con conectores estancos para
   la alimentación (solar o PoE).

### 7.2. Firmware
Lo que una estación de campo necesita ya está en el nodo: ponderaciones A y C,
Fast y Slow, LAeq, máximos y pico, L10/L90, Ld/Le/Ln/Lden y un autodiagnóstico
que el maestro puede leer (status por segundo, `clip_count`, comprobación del
bias en el nodo analógico y de silencio en el digital, detección de muestreo
detenido y watchdog). La transmisión (MQTT, LoRaWAN, InfluxDB…) es cosa del
maestro, que es quien publica.

### 7.3. Plan de despliegue

| Fase | Acción | Resultado esperado |
| :--- | :--- | :--- |
| **1. Hardware** | Montaje en caja IP65 con pantalla antiviento profesional. | Estabilidad frente al clima. |
| **2. Calibración** | Calibración inicial en laboratorio (94 dB) y comparación con sonómetro. | Nivel trazable. |
| **3. Conectividad** | Integración con el dashboard (Grafana/ThingsBoard). | Visualización en tiempo real. |
| **4. Validación** | Comparación con un sonómetro Clase 1 certificado durante 24 h. | Desviación < ±1,4 dB. |

### 7.4. Mantenimiento preventivo
- Limpieza de la pantalla antiviento cada 3 meses.
- Recalibración anual con calibrador de Clase 1 (y la periódica de
  [CALIBRACION.md](CALIBRACION.md) §9).

## 8. Conclusiones y recomendaciones
1. **Procesado:** ponderaciones A y C, Fast y Slow, y todos los indicadores de
   la normativa están implementados y verificados frente a IEC 61672-1, en
   toda la banda de cada nodo (§3.2).
2. **Legalidad:** el equipo es adecuado para monitorización de Smart City y
   pre-evaluación; sus datos no son legalmente vinculantes para sanciones sin
   un certificado de metrología oficial.
3. **Microfonía:** el nodo digital (ICS-43434) es el recomendado: baja el suelo
   de ~58 a ~34 dB y elimina el ruido del ADC. Su límite es el techo de
   ~106 dB SPL de estas unidades, a tener en cuenta en ubicaciones muy
   ruidosas.
