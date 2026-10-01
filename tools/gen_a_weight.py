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


# ============================================================
# C-weighting (IEC 61672-1) — for LCpeak (impulsive noise)
# C shares f1 and f4 with A but omits f2, f3 (flatter response).
# 2 zeros at origin, 4 poles (f1 x2, f4 x2). 0 dB @ 1 kHz.
# ============================================================
def design_c(fs):
    z = np.array([0.0, 0.0])
    p = np.array([-2*np.pi*f1, -2*np.pi*f1, -2*np.pi*f4, -2*np.pi*f4])
    k = (2*np.pi*f4)**2
    zd, pd, kd = bilinear_zpk(z, p, k, fs)
    sos = zpk2sos(zd, pd, kd)
    w = np.array([2*np.pi*1000/fs])
    _, h = sosfreqz(sos, worN=w, fs=2*np.pi)
    sos[0, :3] /= np.abs(h[0])
    return sos

def check_c(fs, sos):
    ref = {31.5:-3.0, 63:-0.8, 125:-0.2, 250:0.0, 500:0.0,
           1000:0.0, 2000:-0.2, 4000:-0.8, 8000:-3.0, 16000:-8.5}
    freqs = np.array(sorted(ref.keys()))
    fu = freqs[freqs < fs/2]
    w = 2*np.pi*fu/fs
    _, h = sosfreqz(sos, worN=w, fs=2*np.pi)
    me = 0
    for f, hi in zip(fu, h):
        db = 20*np.log10(np.abs(hi)); e = db-ref[f]; me = max(me, abs(e))
        print(f"    {f:8.0f}  {db:8.2f}  {ref[f]:7.1f}  {e:+.2f}")
    print(f"    max |err| = {me:.2f} dB")

def print_c():
    for fs in [16000, 48000]:
        print(f"=== C-weighting fs = {fs} Hz ===")
        sos = design_c(fs); check_c(fs, sos)
        print(f"  SOS ({sos.shape[0]} biquads):")
        for s in sos:
            b0,b1,b2,a0,a1,a2 = s
            print(f"    {{{b0:.8f}f, {b1:.8f}f, {b2:.8f}f, {a1:.8f}f, {a2:.8f}f, 0, 0}},")
        print()

print_c()


# ============================================================
# Least-squares fit of the third section for fs = 16 kHz (#3.3.1)
# At 16 kHz the 12194 Hz pole of the analog prototype is above Nyquist, so the
# plain bilinear transform collapses it and the response falls off far too
# early (-12 dB at 7 kHz). Sections 1-2 are exact and kept; section 3 is fitted
# over 20 Hz-7.9 kHz. Reproduces the coefficients shipped in DSP_Engine.cpp.
# ============================================================
def fit_third_section_16k():
    from scipy.optimize import minimize
    fs = 16000
    base = [(0.529093,-1.058186,0.529093,-1.983887,0.983952),
            (1.0,-2.0,1.0,-1.705510,0.715988)]
    seed = (1.0, 2.0, 1.0, 0.821564, 0.168742)   # bilinear section (poor fit)
    fev = np.logspace(np.log10(20), np.log10(7900), 200)
    tgt = a_ideal_db(fev) if 'a_ideal_db' in globals() else None
    if tgt is None:
        ra = (f4**2*fev**4)/((fev**2+f1**2)*np.sqrt((fev**2+f2**2)*(fev**2+f3**2))*(fev**2+f4**2))
        tgt = 20*np.log10(ra) + A1000

    def mk(c): return np.array([[b0,b1,b2,1.0,a1,a2] for b0,b1,b2,a1,a2 in c])
    def db(c, f):
        _, h = sosfreqz(mk(c), worN=2*np.pi*np.asarray(f)/fs, fs=2*np.pi)
        return 20*np.log10(np.abs(h)+1e-30)

    def cost(p):
        b0,b1,b2,a1,a2 = p
        if max(abs(np.roots([1,a1,a2]))) >= 0.999:      # keep it stable
            return 1e9
        c = base + [(b0,b1,b2,a1,a2)]
        try: r = db(c, fev) - db(c, [1000.0])[0]        # normalize at 1 kHz
        except Exception: return 1e9
        return 1e9 if not np.all(np.isfinite(r)) else np.sqrt(np.mean((r-tgt)**2))

    best = None
    for s_ in range(40):
        rng = np.random.default_rng(s_)
        g = np.array(seed)*(1+0.35*rng.standard_normal(5)) if s_ else np.array(seed)
        r = minimize(cost, g, method='Nelder-Mead',
                     options={'maxiter':8000,'xatol':1e-9,'fatol':1e-11})
        if best is None or r.fun < best.fun: best = r

    c = base + [tuple(best.x)]
    n = db(c, [1000.0])[0]
    sc = 10**(-n/20)                                     # fold 0 dB @1 kHz into b
    b0,b1,b2,a1,a2 = best.x
    print(f"=== A-weighting, fs = 16000 Hz, fitted 3rd section ===")
    print(f"  RMS error over 20 Hz-7.9 kHz: {best.fun:.3f} dB")
    fr = [63,125,250,500,1000,2000,3150,4000,5000,6300,7000,7900]
    r = db(c, fr) - n
    ra = (np.asarray(fr,float)**4*f4**2)/((np.asarray(fr,float)**2+f1**2)*np.sqrt((np.asarray(fr,float)**2+f2**2)*(np.asarray(fr,float)**2+f3**2))*(np.asarray(fr,float)**2+f4**2))
    ri = 20*np.log10(ra)+A1000
    for k,f in enumerate(fr):
        print(f"    {f:5d} Hz  err {r[k]-ri[k]:+6.2f} dB")
    print("  third section for DSP_Engine.cpp:")
    print("    {%.8ff, %.8ff, %.8ff, %.8ff, %.8ff, 0, 0}"
          % (b0*sc, b1*sc, b2*sc, a1, a2))


def fit_c_high_section_16k():
    """Refit the high-frequency section of the C cascade at 16 kHz.

    C shares the 12194 Hz double pole with A, so at 16 kHz the bilinear
    transform collapses it exactly the same way and the shipped coefficients
    were -5.73 dB off at 6.3 kHz. The 20.6 Hz section is exact and kept; only
    the high section is fitted, over 20 Hz-7.9 kHz, pinned to 0 dB at 1 kHz.
    Reproduces the coefficients committed in DSP_Engine.cpp.
    """
    import numpy as np
    from scipy.optimize import least_squares

    fs = 16000.0
    f1, f4 = 20.598997, 12194.217
    EXACT = (1.0, -2.0, 1.0, -1.98388676, 0.98395167)   # 20.6 Hz double pole

    def c_iec(f):
        f = np.asarray(f, float)
        H = (f4**2)*f**2 / ((f**2+f1**2)*(f**2+f4**2))
        n1 = (f4**2)*1e6 / ((1e6+f1**2)*(1e6+f4**2))
        return 20*np.log10(H/n1)

    def sec(p, f):
        b0, b1, b2, a1, a2 = p
        z = np.exp(2j*np.pi*f/fs)
        return (b0 + b1/z + b2/z**2)/(1 + a1/z + a2/z**2)

    def casc_db(p, f):
        return 20*np.log10(np.abs(sec(p, f) * sec(EXACT, f)))

    fit_f = np.logspace(np.log10(20), np.log10(7900), 400)
    target = c_iec(fit_f)

    def resid(p):
        d = casc_db(p, fit_f) - target
        pin = casc_db(p, np.array([1000.0]))[0] * 30.0      # hard 0 dB at 1 kHz
        r = np.roots([1.0, p[3], p[4]])
        m = float(np.max(np.abs(r)))
        pen = (m - 0.985) * 1e4 if m > 0.985 else 0.0       # keep poles inside
        return np.concatenate([d, [pin], [pen]])

    best = None
    rng = np.random.default_rng(7)
    for _ in range(60):
        p0 = np.array([0.5, 0.5, 0.1, 0.8, 0.15]) + rng.normal(0, 0.6, 5)
        try:
            r = least_squares(resid, p0, method='lm', max_nfev=8000)
        except Exception:
            continue
        e = float(np.max(np.abs(casc_db(r.x, fit_f) - target)))
        if best is None or e < best[0]:
            best = (e, r.x)

    e, p = best
    poles = np.abs(np.roots([1.0, p[3], p[4]]))
    print("C-weighting, high section refit @ 16 kHz")
    print("  max |error| 20 Hz-7.9 kHz = %.3f dB" % e)
    print("  pole radii = %s (%s)" % (np.round(poles, 4),
          "stable" if poles.max() < 1 else "UNSTABLE"))
    print("  high section for DSP_Engine.cpp:")
    print("    {%.8ff, %.8ff, %.8ff, %.8ff, %.8ff, 0, 0}" % tuple(p))


if __name__ == "__main__":
    fit_third_section_16k()
    print()
    fit_c_high_section_16k()
