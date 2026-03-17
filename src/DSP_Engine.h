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

#ifndef DSP_ENGINE_H
#define DSP_ENGINE_H

#include <stdint.h>
#include <math.h>

// --- Configuration ---
#define SAMPLE_RATE 16000          // Reduced sampling to avoid I2C blocking
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)

static_assert(SAMPLE_RATE == 16000, "Biquad coefficients assume 16kHz sampling rate");

#define CALIBRATION_DB 94.0f      // Target dB (Calibrator)
#define CALIBRATION_RMS_MV 166.0f // Measured RMS mV at 94dB
#define REF_VOLTAGE 1100          // ADC Ref (mV)

// --- DSP Structures ---
struct Biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
};

// PeriodStats for Day/Evening/Night accumulations
struct PeriodStats {
    double energySum;
    uint32_t count;
    
    void add(float db) {
        if (db > 10.0f) { // Ignore silence/noise floor
            energySum += pow(10.0, (double)db / 10.0);
            count++;
        }
    }
    
    float getAvg() {
        if (count == 0) return 0.0f;
        return (float)(10.0 * log10(energySum / (double)count));
    }
    
    bool hasData() const { return count > 0; }
    
    void reset() {
        energySum = 0;
        count = 0;
    }
};

// --- Shared Data Types ---
struct SensorData {
    uint32_t noise;           // Current noise level (mV)
    float noiseAvg;           // Average (mV)
    float noiseAvgDb;         // Average (dB)
    float noisePeak;          // Peak (mV)
    float noisePeakDb;        // Peak (dB)
    float noiseMin;           // Minimum (mV)
    float noiseMinDb;         // Minimum (dB)
    float noiseAvgLegal;      // Legal average (mV)
    float noiseAvgLegalDb;    // Legal average (dB)
    float noiseAvgLegalMax;   // Legal max average (mV)
    float noiseAvgLegalMaxDb; // Legal max average (dB)
    uint16_t lowNoiseLevel;   // Dynamic base noise level (used for L90)
    uint32_t cycles;          // Number of cycles completed
    float Ld;                 // Day index
    float Le;                 // Evening index
    float Ln;                 // Night index
    float noiseLden;          // Global day-evening-night index
};

// --- Function Prototypes ---
void DSP_Init();
float DSP_ApplyFilter(float in, Biquad &f);

extern Biquad aWeightingFilters[3];

#endif // DSP_ENGINE_H
