# tools/

## gen_a_weight.py

Genera y verifica los coeficientes de los biquads de ponderación A (IEC 61672-1)
para cada frecuencia de muestreo soportada, por transformada bilineal del
prototipo analógico y normalización a 0 dB @ 1 kHz.

```bash
pip install scipy numpy
python3 tools/gen_a_weight.py
```

Imprime, para 16 kHz y 48 kHz, la respuesta calculada frente a los valores
nominales de la norma en las frecuencias clave y los coeficientes listos para
pegar en `src/DSP_Engine.cpp` (formato del struct `Biquad`: `{b0,b1,b2,a1,a2}`,
convención DF2T donde a1/a2 se restan).

Resultado de referencia (error respecto a IEC 61672-1):
- **16 kHz**: |err| < 0.6 dB hasta 4 kHz (Nyquist 8 kHz).
- **48 kHz**: |err| < 0.6 dB hasta 8 kHz; banda útil hasta ~20 kHz.

Esto responde a la validación de coeficientes que pedía `docs/ESTUDIO_TECNICO.md`
(punto #7 de la revisión externa): los coeficientes ya no son números opacos,
sino reproducibles y contrastados contra la norma.
