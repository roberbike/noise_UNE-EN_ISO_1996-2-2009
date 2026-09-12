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
// 16 kHz (default). Nyquist at 8 kHz; matches the original coefficient set.
Biquad aWeightingFilters[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {1.000000f, 2.000000f, 1.000000f, 0.821564f, 0.168742f, 0, 0}
};
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
