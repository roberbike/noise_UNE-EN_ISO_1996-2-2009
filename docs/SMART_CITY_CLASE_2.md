# Proyecto: Monitor de Ruido Clase 2 para Smart Cities

## 1. Objetivo
Transformar el prototipo básico en una estación de monitoreo de ruido de campo (Smart City) que cumpla con las tolerancias de la **Clase 2 (IEC 61672-1)** y sea apto para despliegues en exteriores.

## 2. Requisitos de Hardware para Clase 2

### 2.1. Micrófono y Protección
Para cumplir con la Clase 2 en exteriores, el hardware debe actualizarse:
1.  **Cápsula MEMS I2S (Recomendado):** Sustituir el MAX4466 por un sensor **INMP441** o **ICS-43434**.
    - *Razón:* Respuesta en frecuencia más plana, salida digital (evita el ruido del ADC del ESP32) y mayor relación señal/ruido (SNR).
2.  **Pantalla Antiviento (Windshield):** Es OBLIGATORIO el uso de una bola de espuma acústica de célula abierta (mínimo 60mm de diámetro).
    - *Efecto:* Reduce el ruido inducido por el viento en hasta 20-30 dB, evitando falsos positivos en las mediciones.
3.  **Protección contra Intemperie:** El micrófono debe montarse en una pértiga con protección contra lluvia (membrana hidrofóbica) que no atenúe las frecuencias altas.

### 2.2. Envolvente (IP65/IP67)
- Caja estanca para la electrónica.
- Conectores estancos para alimentación (Solar o PoE).

## 3. Especificaciones del Firmware Clase 2

El código actual (V2.0) ya implementa los núcleos de procesamiento necesarios, pero para Smart Cities se añaden:

1.  **Ponderación Frecuencial A:** Ya implementada vía filtros biquad IIR.
2.  **Ponderación Temporal (Fast/Slow):** Ya implementada.
3.  **Cálculo de Percentiles ($L_{10}, L_{50}, L_{90}$):** Necesario para caracterizar el ruido estadístico de la ciudad.
4.  **Transmisión de Datos:** Implementación de protocolos robustos (MQTT o LoRaWAN) para el envío de:
    - $L_{Aeq, 1min}$ (Nivel medio por minuto).
    - $L_{AFmax, 1min}$ (Pico máximo en el minuto).
    - Estado de salud del micrófono (Self-check).

## 4. Implementación del Autochequeo (Self-Monitoring)
Para despliegues de Smart City, es vital saber si el micrófono se ha degradado por la humedad:
```cpp
// Verificación de Bias (Implementado en main.cpp)
if (bias_mv < LOW_LIMIT || bias_mv > HIGH_LIMIT) {
    status = SENSOR_FAULT;
    log_to_cloud("ALERTA: Posible entrada de agua o fallo de cápsula");
}
```

## 5. Plan de Despliegue en Ciudad

| Fase | Acción | Resultado esperado |
| :--- | :--- | :--- |
| **1. Hardware** | Montaje en caja IP65 con Windshield profesional. | Estabilidad frente a clima. |
| **2. Calibración** | Calibración inicial en laboratorio (94dB). | Precisión nominal Clase 2. |
| **3. Conectividad** | Integración con Dashboard (Grafana/ThingsBoard). | Visualización en tiempo real. |
| **4. Validación** | Cruce de datos con sonómetro Clase 1 certificado durante 24h. | Verificación de desviación (< ±1.4 dB). |

## 6. Mantenimiento Preventivo
- Limpieza de la pantalla antiviento cada 3 meses.
- Recalibración anual obligatoria con calibrador de Clase 1.
