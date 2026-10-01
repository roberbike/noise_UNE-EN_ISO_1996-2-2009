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

// L10/L90 sliding-window length in seconds (one LAeq sample per second).
// ISO 1996-2 short-term reference intervals for urban noise are typically
// 5 min; override via build_flags (-D AGG_WINDOW_SEC=600). The window slides
// every second so the master reads the percentiles over the last
// AGG_WINDOW_SEC seconds up to the read, regardless of its polling period.
#ifndef AGG_WINDOW_SEC
#define AGG_WINDOW_SEC 300
#endif
static_assert(AGG_WINDOW_SEC >= 10 && AGG_WINDOW_SEC <= 3600,
              "AGG_WINDOW_SEC must be between 10 and 3600 seconds");

// Convert a linear RMS amplitude (mV for ADC, full-scale for I2S) to dB SPL.
typedef float (*AmplitudeToDb)(float amplitude);

// Stored in the sliding window for an invalid second, so the window still
// advances exactly one slot per wall-clock second instead of stalling on gaps.
// The sentinel ages out of the window like any other sample but is excluded
// from the percentile. No real dB SPL level can come near it.
#define WIN_INVALID_DB (-1000.0f)
#define WIN_IS_VALID(v) ((v) > -500.0f)

struct SecondInput {
    float mean_sq;       // mean of squared A-weighted samples over the second
    float max_fast_sq;   // max of the 125 ms fast EMA of squared samples
    float max_slow_sq;   // max of the 1 s slow EMA of squared samples (LASmax)
    float peak_c;        // max |C-weighted sample| in the second (LCpeak)
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

    // L10/L90 sliding window: circular buffer of the last AGG_WINDOW_SEC
    // one-second LAeq values, one slot per wall-clock second. An invalid
    // second stores WIN_INVALID_DB. head_ is the next write slot; count_ grows
    // to AGG_WINDOW_SEC and then stays full (oldest sample overwritten).
    float win_buffer_[AGG_WINDOW_SEC];
    int win_head_ = 0;
    int win_count_ = 0;

    // Valid (non-sentinel) seconds currently in the window, published in the
    // metadata every second so the master can spot a partial percentile.
    int win_valid_ = 0;

    // Scratch space for nth_element. A per-instance member, not a
    // function-local static: two aggregators would have shared the static one.
    float scratch_[AGG_WINDOW_SEC];

    // Period accumulators
    PeriodStats day_{};
    PeriodStats evening_{};
    PeriodStats night_{};
    int last_mday_ = -1;
};

#endif // NOISE_AGGREGATOR_H
