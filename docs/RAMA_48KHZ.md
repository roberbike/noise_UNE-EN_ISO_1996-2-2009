# Muestreo a 48 kHz (nodo I2S)

> **Estado: ya no es experimental.** 48 kHz es la configuracion por defecto
> del entorno `seeed_xiao_esp32s3` desde la 3.3.0 (`-D SAMPLE_RATE=48000` en
> `platformio.ini`). Este documento se conserva como registro de la decision
> y de como verificarla.

Sube el nodo digital (XIAO ESP32-S3 + ICS-43434) de 16 kHz a
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
| Bloque DMA I2S | 512 frames (32 ms) | 768 frames (16 ms) |
| RAM DMA extra | — | ~6 KB más (sin problema en los 512 KB del S3) |
| Carga DSP | holgada | 3× el trabajo del filtro; moderada con FPU |

El orden del filtro **no cambia**: sigue siendo 6º orden (3 biquads). Lo que
cambia son los **coeficientes**, recalculados para 48 kHz. Ver
`tools/gen_a_weight.py`.

## Cómo probar

1. Compilar y flashear el entorno `seeed_xiao_esp32s3` (ya trae 48 kHz).
3. Abrir el monitor serie: los LAeq deben salir en el mismo rango que a 16 kHz
   (~34 dB de suelo). Si el nivel se desplaza, el sospechoso son los
   coeficientes o `MIC_OFFSET_DB`.
4. Verificar CPU: el nodo debe seguir publicando 1 medida/segundo sin `WARN` de
   muestreo detenido. Si aparecieran, la tarea de audio no está siguiendo el
   ritmo (improbable en el S3, pero es lo que hay que vigilar).
5. Contrastar con un tono de **8-16 kHz**: a 48 kHz el nodo lo mide; a
   16 kHz **no puede**, porque Nyquist esta en 8 kHz y ese tono entra como
   alias en la banda audible, no como el tono que es. Es una comprobacion
   de que 48 kHz aporta banda, no de exactitud a 16 kHz.

## Resultado

Fundido en `main`: el nodo C3 se queda en 16 kHz y el S3 va a 48 kHz, aislados
por el build flag de cada entorno. El bloque DMA quedo en 768 frames y no en
los 1536 que planteaba la tabla original, porque el driver I2S heredado limita
`dma_buf_len` a 1024 y rechaza valores mayores (ver `src/MIC_I2S.h`).
