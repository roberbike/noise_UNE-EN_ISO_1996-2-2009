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

#ifndef NOISE_AGGREGATOR_H
#define NOISE_AGGREGATOR_H

#include <stdint.h>
#include "DSP_Engine.h"

/**
 * --- Common noise aggregator (ISO 1996-2) ---
 *
 * The per-second acoustic math is identical on both nodes; only the raw
 * acquisition differs (ADC polling vs I2S DMA) and the conversion from a
 * linear amplitude to dB SPL. This class owns everything that is shared:
 * LAeq/LAFmax, L10/L90 percentiles, Ld/Le/Ln period accumulation, Lden,
 * clipping gate, time-sync gate and the "hold last valid" policy — so a fix
 * lands in one place instead of two (previously duplicated in main.cpp and
 * main_i2s.cpp).
 *
 * Each platform's sampling_task calls process() once per second with the
 * second's aggregated energy. The amplitude->dB conversion is injected as a
 * function pointer (ADC: slope in mV + calibration; I2S: dBFS + sensitivity).
 */

// Number of 1-second LAeq values per L10/L90 percentile block.
#define AGG_STAT_SAMPLES 20

// Convert a linear RMS amplitude (mV for ADC, full-scale for I2S) to dB SPL.
typedef float (*AmplitudeToDb)(float amplitude);

struct SecondInput {
    float mean_sq;       // mean of squared A-weighted samples over the second
    float max_fast_sq;   // max of the 125 ms fast EMA of squared samples
    uint32_t samples;    // sample count actually accumulated this second
    bool input_valid;    // false if the raw input was dead/clipped this second
    uint32_t clip_count; // samples that hit full scale this second (I2S)
};

class NoiseAggregator {
public:
    // to_db: amplitude->dB SPL conversion for this platform.
    // amp_scale: multiplies sqrt(mean_sq) before to_db (ADC mV/count slope;
    //            1.0 for I2S, where samples are already full-scale).
    // min_amp: amplitude below which the second is treated as invalid input
    //          (disconnected mic / silence below the real noise floor).
    // int_scale: multiplies the amplitude before storing the integer `noise`
    //            field, so it lands in sensible units per node (1.0 = mV for
    //            ADC; 1e6 = µFS for I2S, whose FS amplitude is ~1e-4 and would
    //            otherwise round to 0).
    void begin(AmplitudeToDb to_db, float amp_scale, float min_amp,
               float int_scale = 1.0f);

    // Called from the platform sampling_task once per completed second.
    // Fills out with the current SensorData snapshot and mic_ok flag.
    // Returns true when the second was acoustically valid.
    // Period indicators (Ld/Le/Ln/Lden) are only computed once the master has
    // set the clock (I2C_Comm_TimeSynced()), avoiding accumulation into 1970.
    bool process(const SecondInput &in, SensorData &out, uint8_t &mic_ok);

private:
    AmplitudeToDb to_db_ = nullptr;
    float amp_scale_ = 1.0f;
    float min_amp_ = 0.0f;
    float int_scale_ = 1.0f;

    // Rolling snapshot: invalid seconds keep the last valid values.
    SensorData last_{};

    // L10/L90 block buffer
    float stat_buffer_[AGG_STAT_SAMPLES];
    int stat_idx_ = 0;

    // Period accumulators
    PeriodStats day_{};
    PeriodStats evening_{};
    PeriodStats night_{};
    int last_mday_ = -1;
};

#endif // NOISE_AGGREGATOR_H
