# Rama experimental: muestreo a 48 kHz (nodo I2S)

Rama `48khz`. Sube el nodo digital (XIAO ESP32-S3 + ICS-43434) de 16 kHz a
**48 kHz** de muestreo. El nodo analógico (ESP32-C3 + MAX4466) **se queda en
16 kHz** — el cambio es exclusivo del entorno `seeed_xiao_esp32s3`.

## Motivación

- **Margen de banda hasta ~20 kHz** (frente a 8 kHz a 16 kHz), cabecera hacia
  tolerancias de Clase 1 (IEC 61672-1).
- **Mejor rechazo de alias**: a 16 kHz el Nyquist queda justo en 8 kHz, sin
  margen para el filtro anti-alias del micro. A 48 kHz hay holgura.

Para monitorización de ruido urbano puro (energía concentrada por debajo de
8 kHz) la ganancia es marginal; el valor está en la cabecera hacia Clase 1 y en
habilitar análisis espectral por bandas altas si se añade más adelante.

## Qué cambia

| Aspecto | 16 kHz (main) | 48 kHz (esta rama) |
| :--- | :--- | :--- |
| `SAMPLE_RATE` | 16000 | 48000 (build flag en el entorno S3) |
| Coeficientes A | set 16 kHz | set 48 kHz (verificado vs IEC 61672-1) |
| `alpha_fast` (EMA 125 ms) | 1/2000 | 1/6000 (derivado de SAMPLE_RATE) |
| Bloque DMA I2S | 512 frames | 1536 frames (mantiene ~32 ms) |
| RAM DMA extra | — | ~24 KB más (sin problema en los 512 KB del S3) |
| Carga DSP | holgada | 3× el trabajo del filtro; moderada con FPU |

El orden del filtro **no cambia**: sigue siendo 6º orden (3 biquads). Lo que
cambia son los **coeficientes**, recalculados para 48 kHz. Ver
`tools/gen_a_weight.py`.

## Cómo probar

1. `git checkout 48khz`
2. Compilar y flashear el entorno `seeed_xiao_esp32s3`.
3. Abrir el monitor serie: los LAeq deben salir en el mismo rango que a 16 kHz
   (~34 dB de suelo). Si el nivel se desplaza, el sospechoso son los
   coeficientes o `MIC_OFFSET_DB`.
4. Verificar CPU: el nodo debe seguir publicando 1 medida/segundo sin `WARN` de
   muestreo detenido. Si aparecieran, la tarea de audio no está siguiendo el
   ritmo (improbable en el S3, pero es lo que hay que vigilar).
5. Idealmente, contrastar con un tono de 8-16 kHz: a 48 kHz debería medirlo
   correctamente, a 16 kHz no.

## Si funciona

Se puede fundir a `main` manteniendo el nodo C3 en 16 kHz (el build flag ya
aísla el cambio). Si no, la rama queda como referencia y `main` sigue a 16 kHz.
