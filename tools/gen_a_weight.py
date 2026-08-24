import numpy as np
from scipy.signal import bilinear_zpk, zpk2sos, sosfreqz

# --- A-weighting analog prototype (IEC 61672-1) ---
# Standard pole frequencies (Hz)
f1, f2, f3, f4 = 20.598997, 107.65265, 737.86223, 12194.217
A1000 = 1.9997  # gain normalization so that A(1kHz) = 0 dB

def a_weight_analog_zpk():
    # Zeros: 4 at s=0 (two double). Poles at -2*pi*f (f1 x2, f2, f3, f4 x2)
    z = [0, 0, 0, 0]
    p = [-2*np.pi*f1, -2*np.pi*f1,
         -2*np.pi*f4, -2*np.pi*f4,
         -2*np.pi*f2,
         -2*np.pi*f3]
    # gain so that |H(j2pi*1000)| = 1
    k = (2*np.pi*f4)**2 * 10**(A1000/20)  # approx; will renormalize numerically
    return np.array(z), np.array(p), k

def design(fs):
    z, p, k = a_weight_analog_zpk()
    # Bilinear transform (matched at low freq); prewarp not critical for A-weight
    zd, pd, kd = bilinear_zpk(z, p, k, fs)
    sos = zpk2sos(zd, pd, kd)
    # Renormalize to exactly 0 dB @ 1 kHz
    w = np.array([2*np.pi*1000/fs])
    _, h = sosfreqz(sos, worN=w, fs=2*np.pi)
    sos = sos.copy()
    sos[0, :3] /= np.abs(h[0])
    return sos

def check(fs, sos):
    # IEC 61672-1 nominal A-weighting values (dB) at key frequencies
    ref = {
        31.5: -39.4, 63: -26.2, 125: -16.1, 250: -8.6, 500: -3.2,
        1000: 0.0, 2000: 1.2, 4000: 1.0, 8000: -1.1, 16000: -6.6
    }
    freqs = np.array(sorted(ref.keys()))
    freqs_use = freqs[freqs < fs/2]
    w = 2*np.pi*freqs_use/fs
    _, h = sosfreqz(sos, worN=w, fs=2*np.pi)
    print(f"    freq(Hz)  calc(dB)  ref(dB)  err")
    maxerr = 0
    for f, hi in zip(freqs_use, h):
        db = 20*np.log10(np.abs(hi))
        err = db - ref[f]
        maxerr = max(maxerr, abs(err))
        print(f"    {f:8.0f}  {db:8.2f}  {ref[f]:7.1f}  {err:+.2f}")
    print(f"    max |err| = {maxerr:.2f} dB")
    return maxerr

for fs in [16000, 48000]:
    print(f"=== fs = {fs} Hz ===")
    sos = design(fs)
    check(fs, sos)
    print(f"  SOS ({sos.shape[0]} biquads):")
    for i, s in enumerate(sos):
        b0,b1,b2,a0,a1,a2 = s
        # Normalize so a0=1 (scipy already does), print in the C struct layout:
        # our struct stores {b0,b1,b2,a1,a2} with the DF2T convention used in code.
        print(f"    {{{b0:.8f}f, {b1:.8f}f, {b2:.8f}f, {a1:.8f}f, {a2:.8f}f, 0, 0}},")
    print()
