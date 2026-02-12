# Estudio Técnico y de Conformidad: Proyecto Monitor de Ruido (ESP32-C3 + MAX4466)

## 1. Introducción
El presente documento analiza la viabilidad técnica y el cumplimiento legal del sistema de monitorización de ruido basado en el microcontrolador ESP32-C3 y el preamplificador de micrófono MAX4466, de acuerdo con el **Decreto 213/2012** (Pais Vasco) o equivalentes (como el Decreto 266/2004 en C. Valenciana) y la norma **UNE-ISO 1996-2:2009**.

## 2. Análisis del Hardware y Capacidad de Medida

### 2.1. Sensor de Micrófono (MAX4466)
El módulo MAX4466 utiliza una cápsula de electreto conectada a un preamplificador con ganancia ajustable.
- **Respuesta en Frecuencia:** Según el datasheet, el amplificador tiene un GBW de 600kHz, lo que permite cubrir el rango audible (20Hz-20kHz) sin problemas. Sin embargo, la cápsula de electreto estándar suele tener una respuesta no lineal, con caídas significativas en frecuencias bajas (<100Hz) y altas (>10kHz).
- **Relación Señal/Ruido (SNR):** El MAX4466 tiene un PSRR excelente (112dB), lo que minimiza el ruido de la fuente de alimentación. No obstante, el ruido térmico y el ruido de fondo del ADC del ESP32 limitan el rango dinámico inferior (ruido de piso).

### 2.2. Digitalización (ESP32-C3 ADC)
- **Resolución:** 12 bits (4096 niveles). Teóricamente permite ~72dB de rango dinámico. En la práctica, debido a la no linealidad del ADC del ESP32 y el ruido electrónico, el rango dinámico efectivo es de aproximadamente **50-60 dB**.
- **Frecuencia de Muestreo:** Configurada ahora a **22.05 kHz**. Esto permite cubrir todo el espectro audible legalmente requerido (incluyendo la banda de 8kHz) cumpliendo con el Teorema de Nyquist.

## 3. Conformidad con el Decreto y Normativa Legal

### 3.1. Clases de Precisión (IEC 61672-1)
El Decreto y la norma UNE 1996-2 exigen el uso de instrumentos de **Clase 1** (para mediciones de precisión/legales) o **Clase 2** (para mediciones de campo generales).
- **Estado Actual:** El sistema propuesto carece de certificación de clase. Por sus características de ADC y cápsula, se clasificaría como un instrumento de monitorización (Tipo 3/Informático), no apto para denuncias legales u informes oficiales vinculantes.
- **Uso Legal:** Puede emplearse para **monitorización preventiva y mapeo de ruido**, pero no para certificar superaciones de límites legales en un juicio sin una verificación metrológica del Estado.

### 3.2. Ponderación Frecuencial (A-Weighting)
La normativa exige que los niveles se expresen en $dB(A)$. 
- **Estado Actual:** **Implementado.** Se utiliza un filtro digital IIR de 6º orden que aplica la curva de ponderación A de forma precisa a los 22.05kHz de muestreo.

### 3.3. Ponderación Temporal (Fast/Slow)
Las medidas de $L_{max}$ y $L_{min}$ requieren constantes de tiempo normalizadas:
- **Fast (F):** 125 ms.
- **Slow (S):** 1 s.
- **Estado Actual:** **Implementado.** El motor DSP calcula simultáneamente las envolventes temporales Fast ($L_{AFmax}$) y Slow ($L_{ASmax}$) mediante algoritmos de suavizado exponencial (EMA) según IEC 61672.

## 4. Cálculos y Medidas Legales Sugeridas

### 4.1. Parámetros de Ruido Ambiental (Decreto 213/2012)
El sistema ahora calcula y reporta los indicadores exigidos por la ley:
1.  **$L_{Aeq, 1s}$**: Nivel continuo equivalente cada segundo.
2.  **$L_{d}, L_{e}, L_{n}$**: Índices promedio para Día, Tarde y Noche.
3.  **$L_{den}$**: Índice global Día-Tarde-Noche con penalizaciones de +5dB y +10dB.
*Nota: Requiere sincronización horaria vía I2C desde el Maestro.*
4.  **$L_{10}$ y $L_{90}$**: Niveles estadísticos para caracterizar el ruido de fondo y eventos intrusivos.

### 4.2. Corrección por Ruido de Fondo
Según UNE 1996-2, si el ruido medido está cerca del ruido de fondo (diferencia < 10dB), se debe aplicar una corrección:
$$L_{corr} = 10 \cdot \log_{10}(10^{L_{med}/10} - 10^{L_{fondo}/10})$$

## 5. Método de Calibración Profesional

Para garantizar la fiabilidad, se propone el siguiente método utilizando un **sonómetro patrón** y un **generador de ruido/calibrador acústico**.

### Instrumentos Necesarios:
- Calibrador acústico de Clase 1 o 2 (genera 94.0 dB o 114.0 dB a 1 kHz).
- Sonómetro de referencia certificado.

### Pasos del Procedimiento:
1.  **Preparación:** Colocar la cápsula del micrófono dentro de la cavidad del calibrador. Asegurar un sellado hermético.
2.  **Referencia:** Encender el calibrador a 94.0 dB (1 kHz).
3.  **Ajuste de Ganancia Hardware:** Girar el potenciómetro del MAX4466 hasta que la señal en el ADC no sature pero tenga una amplitud significativa (idealmente pico-a-pico de 2V).
4.  **Ajuste Software (Cero):**
    - Leer el valor RMS en mV que reporta el ESP32.
    - Introducir este valor en la constante `CALIBRATION_RMS_MV` del código.
    - Verificar que el valor `LAeq` reportado coincida con los 94.0 dB del calibrador.
5.  **Verificación de Linealidad:** Cambiar el calibrador a 114.0 dB. El sistema debería marcar 114.0 dB ($\pm 0.5$ dB). Si no es así, existe un problema de linealidad en el ADC o saturación en el previo.
6.  **Registro:** Documentar la fecha, temperatura y humedad, ya que afectan a la sensibilidad de la cápsula de electreto.

## 6. Conclusiones y Recomendaciones
1.  **Software:** Es crítico implementar la **ponderación A** y aumentar el **muestreo a 22kHz**.
2.  **Legalidad:** El equipo es excelente para **"Smart City Monitoring"** y pre-evaluación, pero los datos no son legalmente vinculantes para sanciones sin un certificado de metrología estatal.
3.  **Microfonía:** Para mayor precisión profesional, se recomienda sustituir la cápsula de electreto de 50 céntimos por una cápsula de medición MEMS (como la INMP441 o ICS-43434) que tiene respuesta plana y salida digital I2S, eliminando el ruido del ADC interno del ESP32.
