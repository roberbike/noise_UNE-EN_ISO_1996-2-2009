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
#include <stddef.h>   // offsetof (wire-format guards below)

// --- Configuration ---
// SAMPLE_RATE is overridable via build_flags (-D SAMPLE_RATE=48000).
// Supported: 16000 (default, ADC node) and 48000 (I2S node, wider band /
// Class-1 headroom). Each rate has its own A-weighting coefficient set,
// selected at compile time in DSP_Engine.cpp.
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 16000
#endif
// Integer sample period and its remainder. 1000000/16000 is 62.5 us, so plain
// integer division yields 62 and the real rate drifts to 16129 Hz (+0.8 %).
// The sampling loop adds SAMPLE_PERIOD_REM each sample and borrows one extra
// microsecond when it overflows, so the long-run average is exactly 1/fs.
#define SAMPLE_PERIOD_US  (1000000 / SAMPLE_RATE)
#define SAMPLE_PERIOD_REM (1000000 % SAMPLE_RATE)

static_assert(SAMPLE_RATE == 16000 || SAMPLE_RATE == 48000,
              "A-weighting coefficients are only provided for 16 kHz and 48 kHz");

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
    float energySum;
    uint32_t count;
    
    void add(float db) {
        // Every valid second counts. Discarding quiet seconds (the old
        // db > 10 gate) removed real low levels from the energy average and
        // biased Ld/Le/Ln upwards — exactly the periods a night index must
        // capture. Validity is decided upstream by the aggregator.
        energySum += powf(10.0f, db / 10.0f);
        count++;
    }
    
    float getAvg() {
        if (count == 0 || energySum <= 0.0f) return 0.0f;
        return 10.0f * log10f(energySum / (float)count);
    }
    
    bool hasData() const { return count > 0; }
    
    void reset() {
        energySum = 0.0f;
        count = 0;
    }
};

// --- Shared Data Types ---
struct SensorData {
    uint32_t noise;           // Current noise level (mV)
    float noiseAvg;           // Average (mV)
    float noiseAvgDb;         // Average (dB)
    // NOTE (#B6): "Peak" is a misnomer — this is the Fast-weighted maximum
    // (LAFmax), an RMS envelope, not an instantaneous peak. The true peak is
    // noiseLCpeakDb (C-weighted, no time weighting). Name kept for the layout.
    float noisePeak;          // LAFmax, linear units (mV ADC / uFS I2S)
    float noisePeakDb;        // LAFmax in dB (NOT a peak; see noiseLCpeakDb)
    // NOTE (#B6): these are NOT a minimum. They are written with the same
    // value as noiseAvg/noiseAvgDb every second and are kept only so the wire
    // layout stays frozen. Treat as deprecated; do not use for an Lmin.
    float noiseMin;           // deprecated: duplicate of noiseAvg
    float noiseMinDb;         // deprecated: duplicate of noiseAvgDb
    // NOTE (#B3): despite the historical name/unit, BOTH of these carry L10
    // in dB — not millivolts. The layout is frozen (masters read it as a byte
    // block), so the fields keep their position and the comment is the truth.
    float noiseAvgLegal;      // L10 in dB  (NOT mV, legacy name)
    float noiseAvgLegalDb;    // L10 in dB
    float noiseAvgLegalMax;   // LAFmax in linear units (mV ADC / uFS I2S)
    float noiseAvgLegalMaxDb; // LAFmax in dB
    uint16_t lowNoiseLevel;   // Dynamic base noise level (used for L90)
    uint32_t cycles;          // Number of cycles completed
    float Ld;                 // Day index
    float Le;                 // Evening index
    float Ln;                 // Night index
    float noiseLden;          // Global day-evening-night index
    // --- appended in 3.3.1; older masters simply don't read this far ---
    // These two were added MID-STRUCT in 3.3.0, which shifted every field from
    // lowNoiseLevel onwards by 8 bytes and silently fed masters built against
    // the 3.2.x definition the wrong values (their "Lden" was really Le).
    // Appending keeps every pre-existing offset, so a master that requests the
    // old 68-byte size gets exactly the layout it expects.
    float noiseLASmaxDb;      // Max with Slow (1 s) time weighting, dB(A)
    float noiseLCpeakDb;      // Absolute C-weighted peak, dB(C) — impulsive
};

// Wire-format guards. Masters read SensorData as a raw byte block, so each
// offset is part of the protocol: a new field must be APPENDED, never
// inserted. These break the build instead of silently corrupting what a
// master reads.
static_assert(offsetof(SensorData, noiseAvgDb) == 8, "wire format: noiseAvgDb moved");
static_assert(offsetof(SensorData, lowNoiseLevel) == 44, "wire format: lowNoiseLevel moved");
static_assert(offsetof(SensorData, cycles) == 48, "wire format: cycles moved");
static_assert(offsetof(SensorData, noiseLden) == 64, "wire format: noiseLden moved");

// --- Function Prototypes ---
void DSP_Init();
float DSP_ApplyFilter(float in, Biquad &f);

extern Biquad aWeightingFilters[3];

// C-weighting cascade (2 biquads, 4th order) for LCpeak — impulsive noise
// per IEC 61672-1. Flatter than A; used only for the absolute peak detector.
extern Biquad cWeightingFilters[2];

#endif // DSP_ENGINE_H
