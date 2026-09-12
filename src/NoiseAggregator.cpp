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

#include <Arduino.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include "time.h"
#include "NoiseAggregator.h"
#include "I2C_Comm.h"

void NoiseAggregator::begin(AmplitudeToDb to_db, float amp_scale, float min_amp,
                            float int_scale) {
    to_db_ = to_db;
    amp_scale_ = amp_scale;
    min_amp_ = min_amp;
    int_scale_ = int_scale;
    win_head_ = 0;
    win_count_ = 0;
    last_mday_ = -1;
    memset(&last_, 0, sizeof(last_));
    day_.reset();
    evening_.reset();
    night_.reset();
}

bool NoiseAggregator::process(const SecondInput &in, SensorData &out, uint8_t &mic_ok) {
    // --- Validity gate (#3 clipping, #4 realistic floor) ---
    // A second is valid only if the raw input was alive and not clipped and
    // the resulting amplitude is above the disconnection threshold.
    float rms_amp = (in.samples > 0) ? sqrtf(in.mean_sq) * amp_scale_ : 0.0f;
    float fast_amp = sqrtf(in.max_fast_sq) * amp_scale_;

    bool valid = in.input_valid && (rms_amp > min_amp_);

    if (valid) {
        float laeq = to_db_(rms_amp);
        float lafmax = to_db_(fast_amp);

        // LASmax: max level with Slow (1 s) time weighting. Same amplitude->dB
        // path as LAeq/LAFmax; the slow envelope is an RMS-like amplitude.
        float slow_amp = sqrtf(in.max_slow_sq) * amp_scale_;
        float lasmax = (slow_amp > 0.0f) ? to_db_(slow_amp) : laeq;

        // LCpeak: absolute C-weighted instantaneous peak (no time weighting).
        // The peak is a bare amplitude, not an RMS; to_db_ expects the same
        // amplitude scale as the RMS path, so peak_c (C-weighted |sample| max)
        // goes straight through it. This is the impulsive-noise indicator.
        float lcpeak = (in.peak_c > 0.0f) ? to_db_(in.peak_c * amp_scale_) : laeq;

        last_.noiseAvgDb = laeq;
        last_.noisePeakDb = lafmax;
        last_.noiseMinDb = laeq;
        last_.noiseLASmaxDb = lasmax;
        last_.noiseLCpeakDb = lcpeak;

        // Linear-amplitude fields, scaled to per-node integer-friendly units:
        // mV for ADC (int_scale=1), µFS for I2S (int_scale=1e6). Without the
        // scale the I2S full-scale RMS (~1e-4) rounds the uint32 `noise` to 0.
        float rms_scaled = rms_amp * int_scale_;
        float fast_scaled = fast_amp * int_scale_;
        last_.noise = (uint32_t)lroundf(rms_scaled);
        last_.noiseAvg = rms_scaled;
        last_.noisePeak = fast_scaled;
        last_.noiseMin = rms_scaled;
        last_.noiseAvgLegalMax = fast_scaled;
        last_.noiseAvgLegalMaxDb = lafmax;

        // --- L10/L90 over a sliding window of the last AGG_WINDOW_SEC s ---
        // Insert this second's LAeq into the circular buffer, then recompute
        // the percentiles over the whole window every second. The master thus
        // always reads the percentiles for the last AGG_WINDOW_SEC seconds up
        // to its read, with no per-block "staircase". nth_element is O(N), so
        // this stays cheap even for 300-600 s windows.
        win_buffer_[win_head_] = laeq;
        win_head_ = (win_head_ + 1) % AGG_WINDOW_SEC;
        if (win_count_ < AGG_WINDOW_SEC) win_count_++;

        {
            // Copy the valid part of the window and select the percentile ranks.
            static float tmp[AGG_WINDOW_SEC];
            memcpy(tmp, win_buffer_, win_count_ * sizeof(float));

            // L10 = level exceeded 10% of the time = 90th percentile by value.
            // L90 = level exceeded 90% of the time = 10th percentile by value.
            int idx_l10 = (int)(win_count_ * 0.90f);
            int idx_l90 = (int)(win_count_ * 0.10f);
            if (idx_l10 >= win_count_) idx_l10 = win_count_ - 1;

            std::nth_element(tmp, tmp + idx_l90, tmp + win_count_);
            float l90 = tmp[idx_l90];
            std::nth_element(tmp, tmp + idx_l10, tmp + win_count_);
            float l10 = tmp[idx_l10];

            last_.noiseAvgLegal = l10;
            last_.noiseAvgLegalDb = l10;
            // L90 stored as uint16_t; guard the cast against a spurious sub-zero.
            last_.lowNoiseLevel = (l90 > 0.0f) ? (uint16_t)(l90 + 0.5f) : 0;
        }

        // --- Period indicators (only with a synced clock, #5) ---
        if (I2C_Comm_TimeSynced()) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0)) {
                int h = timeinfo.tm_hour;
                if (h >= 7 && h < 19)       day_.add(laeq);
                else if (h >= 19 && h < 23) evening_.add(laeq);
                else                        night_.add(laeq);

                if (day_.hasData())     last_.Ld = day_.getAvg();
                if (evening_.hasData()) last_.Le = evening_.getAvg();
                if (night_.hasData())   last_.Ln = night_.getAvg();

                if (last_.Ld > 0 || last_.Le > 0 || last_.Ln > 0) {
                    float e = (12.0f * powf(10.0f, last_.Ld / 10.0f) +
                                4.0f * powf(10.0f, (last_.Le + 5.0f) / 10.0f) +
                                8.0f * powf(10.0f, (last_.Ln + 10.0f) / 10.0f)) / 24.0f;
                    last_.noiseLden = 10.0f * log10f(e);
                }

                if (last_mday_ != -1 && last_mday_ != timeinfo.tm_mday) {
                    day_.reset(); evening_.reset(); night_.reset();
                }
                last_mday_ = timeinfo.tm_mday;
            }
        }
    }
    // Invalid second: last_ keeps its previous values (hold-last-valid).

    last_.cycles++;
    out = last_;
    mic_ok = valid ? 1 : 0;
    return valid;
}
