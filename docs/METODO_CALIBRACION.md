# Método de Calibración: Monitor de Ruido ESP32
Este documento describe el procedimiento paso a paso para calibrar el monitor de ruido utilizando un sonómetro de referencia y un generador de ruido acústico.

## 1. Fundamento Teórico
La calibración consiste en establecer la relación entre el voltaje RMS medido por el ADC y el Nivel de Presión Sonora (SPL) en decibelios. Dado que la ganancia del MAX4466 es variable (potenciómetro) y la sensibilidad de la cápsula varía, es necesario un punto de ajuste.

La fórmula implementada es:
$$L_{Aeq} = 20 \cdot \log_{10}\left(\frac{V_{rms}}{V_{ref\_rms}}\right) + L_{ref\_db}$$

Donde:
- $V_{rms}$: Voltaje RMS actual medido.
- $V_{ref\_rms}$: Voltaje RMS medido durante la calibración (Constante `CALIBRATION_RMS_MV`).
- $L_{ref\_db}$: Nivel del calibrador (usualmente 94.0 dB).

## 2. Equipamiento Requerido
- **Calibrador Acústico:** Dispositivo que genera un tono puro (usualmente 1 kHz) a un nivel conocido (94 dB o 114 dB).
- **Destornillador de precisión:** Para ajustar el potenciómetro de ganancia.
- **Monitor Serie:** Para visualizar los valores en tiempo real.

## 3. Procedimiento de Calibración

### Paso 1: Ajuste de Rango Dinámico (Hardware)
1. Conecte el micrófono al sistema y abra el Monitor Serie.
2. Inserte el micrófono en el calibrador acústico.
3. Encienda el calibrador a 94 dB.
4. Ajuste el potenciómetro en la parte trasera del MAX4466 hasta que el valor de **RMS (mV)** mostrado en el Monitor Serie esté entre **150 mV y 300 mV**.
   - *Nota:* No exceda los 500 mV para evitar saturación en picos de ruido más altos.

### Paso 2: Determinación del Punto de Referencia (Software)
1. Con el calibrador emitiendo 94 dB estables, observe el valor de **RMS (mV)** en el terminal.
2. Anote este valor (ejemplo: `155 mV`).
3. Abra el archivo `src/main.cpp`.
4. Localice la línea:
   ```cpp
   #define CALIBRATION_RMS_MV 155.0f
   ```
5. Sustituya `155.0f` por el valor que ha anotado.
6. Si su calibrador emite a un nivel distinto de 94 dB, ajuste también:
   ```cpp
   #define CALIBRATION_DB 94.0f
   ```

### Paso 3: Verificación
1. Compile y suba el código con el nuevo valor de calibración.
2. Vuelva a colocar el calibrador a 94 dB.
3. El terminal ahora debería mostrar `LAeq: 94.0` (±0.2 dB).
4. (Opcional) Cambie el calibrador a 114 dB. El terminal debería mostrar `LAeq: 114.0`.

## 4. Calibración Multiequipo (Field Calibration)
Si no dispone de un calibrador acústico, puede realizar una calibración aproximada:
1. Coloque un sonómetro profesional al lado de su micrófono.
2. Exponga ambos a un ruido constante (ruido blanco o tráfico estable).
3. Ajuste `CALIBRATION_RMS_MV` en el código hasta que el valor de `LAeq` en su monitor coincida con el del sonómetro profesional.

## 5. Mantenimiento de la Calibración
- Se recomienda recalibrar cada **6 meses**.
- Si el equipo se mueve o se cambia la carcasa del micrófono, se debe recalibrar obligatoriamente.
