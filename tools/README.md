# tools/

## gen_a_weight.py

Genera y verifica los coeficientes de los biquads de ponderación A y C
(IEC 61672-1) para cada frecuencia de muestreo soportada, y reproduce los
ajustes por mínimos cuadrados que usa `src/DSP_Engine.cpp`.

```bash
pip install scipy numpy
python3 tools/gen_a_weight.py
```

Qué imprime:

1. Para 16 y 48 kHz, la transformada bilineal directa del prototipo analógico
   (A: 3 biquads; C: 2 biquads), normalizada a 0 dB a 1 kHz, con su error
   frente a los valores nominales de la norma.
2. Los ajustes que sustituyen a la sección alta de esas cascadas:
   - **A a 16 kHz** (`fit_third_section_16k`): el polo de 12194 Hz queda por
     encima de Nyquist y la bilineal se hunde (−12 dB a 7 kHz). Ajustada:
     ±0,15 dB hasta 7,9 kHz.
   - **C a 16 kHz** (`fit_c_high_section_16k`): mismo problema (−5,7 dB a
     6,3 kHz). Ajustada: ±0,05 dB hasta 7,9 kHz.
   - **A y C a 48 kHz** (`fit_high_sections_48k`): la bilineal sin
     precompensar solo era exacta hasta 8 kHz (−6,4 dB a 16 kHz). Ajustadas:
     ±0,08 dB de 20 Hz a 20 kHz, con los ceros dentro del círculo unidad (fase
     mínima, como la curva analógica: importa en LCpeak).

Los coeficientes salen en el formato del struct `Biquad`, `{b0,b1,b2,a1,a2}`,
con la convención DF2T de `DSP_ApplyFilter` (a1 y a2 se restan). Los ajustes
parten de semillas aleatorias fijas; según la versión de SciPy pueden diferir
del código en los últimos decimales, sin efecto en el error.

Las secciones bajas son exactas en todos los casos y se conservan. Todas las
cascadas valen 0 dB a 1 kHz, así que ningún ajuste afecta a la calibración.

---

## English version

# tools/

## gen_a_weight.py

Generates and verifies the A- and C-weighting biquad coefficients (IEC 61672-1) for each supported sample rate, and reproduces the least-squares fits used in `src/DSP_Engine.cpp`.

```bash
pip install scipy numpy
python3 tools/gen_a_weight.py
```

What it prints:

1. For 16 and 48 kHz, the plain bilinear transform of the analog prototype (A: 3 biquads; C: 2 biquads), normalized to 0 dB at 1 kHz, with its error against the standard's nominal values.
2. The fits that replace the high section of those cascades:
   - **A at 16 kHz** (`fit_third_section_16k`): the 12194 Hz pole sits above Nyquist and the bilinear response collapses (−12 dB at 7 kHz). Fitted: ±0.15 dB up to 7.9 kHz.
   - **C at 16 kHz** (`fit_c_high_section_16k`): same problem (−5.7 dB at 6.3 kHz). Fitted: ±0.05 dB up to 7.9 kHz.
   - **A and C at 48 kHz** (`fit_high_sections_48k`): the bilinear transform without prewarping was only accurate up to 8 kHz (−6.4 dB at 16 kHz). Fitted: ±0.08 dB from 20 Hz to 20 kHz, with the zeros inside the unit circle (minimum phase, like the analog curve: it matters for LCpeak).

Coefficients come out in the `Biquad` struct format, `{b0,b1,b2,a1,a2}`, with the DF2T convention of `DSP_ApplyFilter` (a1 and a2 are subtracted). The fits start from fixed random seeds; depending on the SciPy version they may differ from the code in the last decimals, with no effect on the error.

The low sections are exact in every case and are kept. Every cascade is 0 dB at 1 kHz, so no fit affects the calibration.
