# Muestreo a 48 kHz (nodo I2S)

> **Estado: configuración por defecto.** El entorno `seeed_xiao_esp32s3`
> muestrea a 48 kHz desde la 3.2.1 (`-D SAMPLE_RATE=48000` en
> `platformio.ini`). Este documento se conserva como registro de la decisión y
> de cómo verificarla.

El nodo digital (XIAO ESP32-S3 + ICS-43434) muestrea a **48 kHz**. El nodo
analógico (ESP32-C3 + MAX4466) **se queda en 16 kHz**: el cambio es exclusivo
del entorno `seeed_xiao_esp32s3`.

## Motivación

- **Banda.** A 16 kHz, Nyquist queda en 8 kHz; a 48 kHz, en 24 kHz. Desde la
  3.3.3 las ponderaciones A y C a 48 kHz siguen la curva de IEC 61672-1 dentro
  de **±0,08 dB de 20 Hz a 20 kHz**. Hasta la 3.3.2 eran la transformada
  bilineal directa, sin precompensación, y solo eran exactas hasta 8 kHz
  (−1,2 dB a 10 kHz, −6,4 dB a 16 kHz): la banda estaba, pero mal ponderada.
- **Aliasing.** El ICS-43434 decima con su propio filtro antes de entregar las
  muestras; a 48 kHz queda holgura de sobra.

Para ruido urbano de tráfico la ganancia en LAeq es marginal (su energía
ponderada A está muy por debajo de 8 kHz). Donde se nota es en fuentes ricas en
agudos y en LCpeak, que es un pico y lo mueve un solo impulso con contenido
alto.

## Qué cambia

| Aspecto | 16 kHz (nodo ADC) | 48 kHz (nodo I2S) |
| :--- | :--- | :--- |
| `SAMPLE_RATE` | 16000 (por defecto) | 48000 (build flag del entorno S3) |
| Coeficientes A y C | set de 16 kHz | set de 48 kHz |
| Envolvente Fast (125 ms) | α = 1/2000 | α = 1/6000 |
| Envolvente Slow (1 s) | α = 1/16000 | α = 1/48000 |
| Bloque DMA I2S | 512 frames (32 ms) | 768 frames (16 ms) |
| RAM DMA | — | ~6 KB más (sin problema en el S3) |
| BCLK | 1,024 MHz | 3,072 MHz |
| Carga DSP | — | El triple de muestras; moderada con FPU |

Las constantes de las envolventes salen de `SAMPLE_RATE` en
`src/SampleChain.h`. El orden de los filtros **no cambia** (A: 3 biquads, C: 2);
cambian los **coeficientes**, uno por cada rate, en `src/DSP_Engine.cpp`. En los
dos rates las secciones bajas son la transformada bilineal exacta y la sección
alta está ajustada por mínimos cuadrados; `tools/gen_a_weight.py` reproduce
todos los ajustes.

## Cómo probar

1. Compilar y flashear el entorno `seeed_xiao_esp32s3`.
2. Abrir el monitor serie: los LAeq deben salir en el rango habitual (suelo de
   ~34 dB en una sala tranquila). Si el nivel se desplaza, los sospechosos son
   los coeficientes o `MIC_OFFSET_DB`.
3. Vigilar la CPU: el nodo debe seguir publicando una medida por segundo sin
   `WARN` de muestreo detenido. Si aparecieran, la tarea de audio no sigue el
   ritmo (improbable en el S3, pero es lo que hay que vigilar).
4. Contrastar con un tono de **8-16 kHz** frente a un sonómetro: a 48 kHz el
   nodo lo mide con su ponderación correcta; a 16 kHz **no puede**, porque
   Nyquist está en 8 kHz y ese tono entra como alias, no como el tono que es.

## Resultado

Fundido en `main`: el nodo C3 se queda en 16 kHz y el S3 va a 48 kHz, aislados
por el build flag de cada entorno. El bloque DMA quedó en 768 frames y no en los
1536 que planteaba la tabla original, porque el driver I2S heredado limita
`dma_buf_len` a 1024 y rechaza valores mayores (ver `src/MIC_I2S.h`).
