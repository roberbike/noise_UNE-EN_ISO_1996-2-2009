# Calibración Automática ICS-43434 (I2S)

Este ejemplo realiza una **calibración completa y automática** del nodo digital (XIAO ESP32-S3 + ICS-43434) usando un calibrador a 94 dB. El proceso es 100% automático: no requiere intervención del usuario más allá de acoplar el calibrador.

## Qué hace

1. **Resetea el offset en NVM** — borra cualquier calibración previa
2. **Realiza 5 lecturas** de 1 segundo para verificar estabilidad
3. **Determina si el micrófono es válido o falso** basándose en:
   - Sensibilidad dentro de rango (-26 ± 3 dBFS @ 94 dB SPL)
   - Factor de cresta entre 2.8 y 3.2 dB (tono limpio sin distorsión)
   - Sin recortes (clips = 0)
   - Lecturas estables (variación < 0.5 dB)
4. **Calcula el offset óptimo** como `offset = 94.0 - LAeq_medido`
5. **Guarda el offset en NVM** para que el firmware principal lo use
6. **Verifica la calibración** aplicando el offset y comparando con el calibrador

## Uso

### Requisitos

- XIAO ESP32-S3 con ICS-43434 conectado:
  - SCK → GPIO2 (D1)
  - WS → GPIO3 (D2)
  - SD → GPIO4 (D3)
  - VDD → 3.3V
  - GND → GND
  - L/R → GND
- Calibrador acústico de 94.0 dB / 1 kHz (Clase 1 o 2, IEC 60942)

### Pasos

1. Abrir esta carpeta como proyecto en PlatformIO
2. Compilar y flashear
3. **Acoplar el calibrador al micrófono** (antes de abrir el Monitor Serie)
4. Abrir el Monitor Serie a 115200 baudios
5. El sketch realizará todo automáticamente

### Resultado

Si el micrófono es válido, verás:

```
========================================
  CALIBRACIÓN COMPLETADA CON ÉXITO
========================================

  Offset guardado en NVM: -13.71 dB
  Sensibilidad de la unidad: -26.15 dBFS @ 94 dB SPL

El firmware principal ahora usará este offset
automáticamente al arrancar.
```

Si el micrófono es falso o defectuoso, verás:

```
========================================
  RESULTADO: MICRÓFONO NO VÁLIDO
========================================

El micrófono no cumple los criterios de validez.
Posibles causas:
  - Micrófono falso o defectuoso
  - Calibrador no acoplado correctamente
  - Ruido de fondo excesivo
  - Cables I2S mal conectados

No se ha guardado ningún offset en NVM.
```

## Ventajas sobre la calibración manual

| Aspecto | Calibración manual (`calibration_i2s`) | Calibración automática (este ejemplo) |
|---------|---------------------------------------|--------------------------------------|
| Reset NVM | Requiere recompilar con `-D RESET_NVS_CALIB` | Automático al arrancar |
| Validez del micrófono | El usuario compara manualmente | Determinada automáticamente |
| Cálculo del offset | El usuario calcula y envía por I2C | Calculado y guardado automáticamente |
| Verificación | El usuario verifica manualmente | Verificación automática al final |
| Intervención | Mínima (solo acoplar calibrador) | Nula (solo acoplar calibrador) |

## Configuración

Puedes ajustar los criterios de validez editando estas constantes en `src/main.cpp`:

```cpp
#define STABILITY_TEST_COUNT 5        // Número de lecturas para estabilidad
#define MAX_VARIATION_DB 0.5f         // Variación máxima entre lecturas
#define SENS_MIN_DBFS (-29.0f)        // Sensibilidad mínima válida
#define SENS_MAX_DBFS (-23.0f)        // Sensibilidad máxima válida
#define CREST_MIN_DB 2.8f             // Factor de cresta mínimo
#define CREST_MAX_DB 3.2f             // Factor de cresta máximo
#define MAX_OFFSET_DB 5.0f            // Offset máximo permitido
```

## Cómo funciona técnicamente

### Cadena de medida

El sketch replica exactamente la cadena de medida del firmware principal:

```
L = 94 + 20·log10(rms_fs) + 26 + 3.0103 + MIC_OFFSET_DB + NVS_offset
```

Donde:
- `94` es el nivel de referencia del calibrador
- `26` es la sensibilidad del datasheet (-26 dBFS @ 94 dB SPL)
- `3.0103` es la corrección pico→RMS (20·log10(√2))
- `MIC_OFFSET_DB` es el trim de compilación (0 en este sketch)
- `NVS_offset` es el offset por unidad (0 tras el reset)

### Cálculo del offset

El offset se calcula como:

```
offset = CALIBRATOR_DB - LAeq_medido
```

Donde `LAeq_medido` es el promedio de 5 lecturas consecutivas sin ningún trim aplicado.

### NVM (Non-Volatile Storage)

El offset se guarda en la NVM del ESP32 usando la librería `Preferences` bajo la clave `calib_db`. Este mismo mecanismo usa el firmware principal (`CMD_SET_CALIB`), por lo que el offset es compatible y persistente entre reflasheos.

## Resolución de problemas

### "Sensibilidad fuera de rango"

El micrófono puede ser falso o defectuoso. Verifica:
- Que el calibrador esté bien acoplado
- Que el calibrador esté encendido y a 94 dB
- Que los cables I2S estén bien conectados

### "Factor de cresta anormal"

El tono del calibrador puede estar distorsionado. Verifica:
- Que el calibrador no esté sobrecargado
- Que no haya ruido de fondo excesivo
- Que el micrófono esté bien sellado al calibrador

### "Lecturas inestables"

Las lecturas varían demasiado entre sí. Verifica:
- Que el ambiente sea silencioso
- Que el calibrador esté estable
- Que no haya vibraciones mecánicas
