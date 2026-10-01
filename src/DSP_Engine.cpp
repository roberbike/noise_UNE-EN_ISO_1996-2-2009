/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "DSP_Engine.h"

// --- A-Weighting Filter: cascade of 3 biquads (6th order, IEC 61672-1) ---
// Coefficients are computed by bilinear transform of the analog A-weighting
// prototype and normalized to 0 dB @ 1 kHz. One set per supported sample rate
// (see tools/gen_a_weight.py to regenerate/verify). Struct layout is
// {b0,b1,b2,a1,a2} with the DF2T convention of DSP_ApplyFilter (a1,a2 are
// subtracted).
#if SAMPLE_RATE == 48000
// 48 kHz. Verified vs IEC 61672-1 nominal values: |err| < 0.6 dB up to 8 kHz.
Biquad aWeightingFilters[3] = {
    {0.23418304f, 0.46836609f, 0.23418304f, -0.22455846f, 0.01260663f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.89387049f, 0.89515977f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.99461446f, 0.99462171f, 0, 0}
};
// C-weighting @ 48 kHz. Verified vs IEC 61672-1: |err| < 0.6 dB up to 8 kHz.
Biquad cWeightingFilters[2] = {
    {0.19789071f, 0.39578141f, 0.19789071f, -0.22455846f, 0.01260663f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.99461446f, 0.99462171f, 0, 0}
};
#else
// 16 kHz. The 12194 Hz pole of the analog A prototype sits above Nyquist, so a
// plain bilinear transform collapses it and the response falls off far too
// early: -1.5 dB at 5 kHz, -5.7 at 6.3 kHz, -12.2 at 7 kHz against the IEC
// 61672-1 curve. The first two sections (poles at 20.6/107.7/737.9 Hz) are
// exact and kept; the third is fitted by least squares over 20 Hz-7.9 kHz,
// which brings the whole band within +-0.15 dB. Poles |z| = 0.25 and 0.30,
// comfortably stable. Normalized to 0 dB at 1 kHz, so the existing
// CALIBRATION_RMS_MV (measured with a 1 kHz calibrator) stays valid.
//
// Trade-off: the old third section happened to have a zero at Nyquist, which
// masked aliasing from 8-9 kHz (the ADC has no analog anti-alias filter). The
// fitted one does not. For urban/traffic spectra only ~1.3 % of A-weighted
// energy lies above 8 kHz, so the net effect is a clear gain; for hiss-heavy
// sources it is less clear-cut. Fit an RC low-pass (~8 kHz) at the MAX4466
// output to remove the issue at the source, or build with
// -D A_WEIGHT_LEGACY_16K to restore the previous coefficients.
#ifdef A_WEIGHT_LEGACY_16K
Biquad aWeightingFilters[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {1.000000f, 2.000000f, 1.000000f, 0.821564f, 0.168742f, 0, 0}
};
#else
Biquad aWeightingFilters[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {-0.36654287f, 1.61495515f, 0.72597221f, 0.05375630f, -0.07418460f, 0, 0}
};
#endif
// C-weighting @ 16 kHz. Verified vs IEC 61672-1: |err| < 0.6 dB up to 4 kHz.
Biquad cWeightingFilters[2] = {
    {0.49718768f, 0.99437536f, 0.49718768f, 0.82156382f, 0.16874178f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.98388676f, 0.98395167f, 0, 0}
};
#endif

void DSP_Init() {
    // Basic init if needed
}

float DSP_ApplyFilter(float in, Biquad &f) {
    float out = in * f.b0 + f.z1;
    f.z1 = in * f.b1 - f.a1 * out + f.z2;
    f.z2 = in * f.b2 - f.a2 * out;
    return out;
}
