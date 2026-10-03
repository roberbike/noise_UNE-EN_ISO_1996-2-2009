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
    win_valid_ = 0;
    last_eval_day_ = -1;
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

    // --- Sliding window advances every second, valid or not ---
    // Previously the buffer was only written inside the validity gate, so a
    // run of invalid seconds froze the window and "the last 300 s" could span
    // far more than 300 s of wall clock. One slot per second keeps the window
    // a real time window; invalid seconds hold a sentinel that ages out
    // normally and is skipped when the percentiles are selected.
    float laeq = valid ? to_db_(rms_amp) : WIN_INVALID_DB;
    win_buffer_[win_head_] = laeq;
    win_head_ = (win_head_ + 1) % AGG_WINDOW_SEC;
    if (win_count_ < AGG_WINDOW_SEC) win_count_++;

    if (valid) {
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
        // The #A7 hold latches are fed from these two when the node firmware
        // publishes the second (I2C_Comm_Publish), atomically with the frame.

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

        // --- L10/L90 over the sliding window (inserted above) ---
        // Recompute the percentiles over the whole window every second, so the
        // master always reads the percentiles for the last AGG_WINDOW_SEC
        // seconds up to its read, with no per-block "staircase". nth_element
        // is O(N), so this stays cheap even for 300-600 s windows.
        {
            // Compact the window into the scratch buffer, dropping the
            // sentinels left by invalid seconds.
            int n = 0;
            for (int i = 0; i < win_count_; i++) {
                if (WIN_IS_VALID(win_buffer_[i])) scratch_[n++] = win_buffer_[i];
            }
            // How full the window is, for the master: a percentile over 12 s
            // is not the same statistic as one over 300 s, and until now there
            // was no way to tell the two apart from the bus.
            win_valid_ = n;

            if (n > 0) {
                // L10 = level exceeded 10% of the time = 90th percentile by
                // value. L90 = exceeded 90% of the time = 10th percentile.
                int idx_l10 = (int)(n * 0.90f);
                int idx_l90 = (int)(n * 0.10f);
                if (idx_l10 >= n) idx_l10 = n - 1;

                std::nth_element(scratch_, scratch_ + idx_l90, scratch_ + n);
                float l90 = scratch_[idx_l90];
                std::nth_element(scratch_, scratch_ + idx_l10, scratch_ + n);
                float l10 = scratch_[idx_l10];

                last_.noiseAvgLegal = l10;
                last_.noiseAvgLegalDb = l10;
                // L90 stored as uint16_t; guard against a spurious sub-zero.
                last_.lowNoiseLevel = (l90 > 0.0f) ? (uint16_t)(l90 + 0.5f) : 0;
            }
        }

        // --- Period indicators (only with a synced clock, #5) ---
        if (I2C_Comm_TimeSynced()) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0)) {
                // The evaluation day runs 07:00 -> 07:00. The night period is
                // 23:00-07:00, a single 8 h interval across midnight; rolling
                // the day over at midnight (as this did until 3.3.2) split it,
                // so Ln mixed the small hours of one night with the first hour
                // of the next. Keying the day on local time shifted back 7 h
                // makes the rollover happen as the day period starts.
                //
                // #B8 The rollover runs BEFORE accumulating, and clears the
                // published indices: they belong to the current evaluation
                // day, and a period with no data yet reports 0 and stays out
                // of Lden (see below). Read them just before 07:00 to get the
                // complete day.
                time_t shifted = mktime(&timeinfo) - 7 * 3600;
                struct tm eval;
                localtime_r(&shifted, &eval);
                int eval_day = eval.tm_year * 1000 + eval.tm_yday;
                if (last_eval_day_ != -1 && last_eval_day_ != eval_day) {
                    day_.reset(); evening_.reset(); night_.reset();
                    last_.Ld = 0.0f; last_.Le = 0.0f; last_.Ln = 0.0f;
                    last_.noiseLden = 0.0f;
                }
                last_eval_day_ = eval_day;

                int h = timeinfo.tm_hour;
                if (h >= 7 && h < 19)       day_.add(laeq);
                else if (h >= 19 && h < 23) evening_.add(laeq);
                else                        night_.add(laeq);

                if (day_.hasData())     last_.Ld = day_.getAvg();
                if (evening_.hasData()) last_.Le = evening_.getAvg();
                if (night_.hasData())   last_.Ln = night_.getAvg();

                // Lden over the periods that ACTUALLY have data, weighted by
                // their hours. The naive 24 h formula counted a period with no
                // samples yet as 0 dB — energy 1 — which dragged the result
                // down hard: with only Ld = 55 it returned 52.0 dB instead of
                // 55.0, so every node published a depressed Lden until the
                // night period had filled. Once all three periods are
                // populated the divisor is 24 again and the value is
                // identical to the standard definition.
                float num = 0.0f, hours = 0.0f;
                if (day_.hasData()) {
                    num += 12.0f * powf(10.0f, last_.Ld / 10.0f);
                    hours += 12.0f;
                }
                if (evening_.hasData()) {
                    num += 4.0f * powf(10.0f, (last_.Le + 5.0f) / 10.0f);
                    hours += 4.0f;
                }
                if (night_.hasData()) {
                    num += 8.0f * powf(10.0f, (last_.Ln + 10.0f) / 10.0f);
                    hours += 8.0f;
                }
                last_.noiseLden = (hours > 0.0f) ? 10.0f * log10f(num / hours)
                                                 : 0.0f;

                // Tell the master how little (or much) this Lden rests on.
                uint8_t mask = (uint8_t)((day_.hasData() ? 0x01 : 0) |
                                         (evening_.hasData() ? 0x02 : 0) |
                                         (night_.hasData() ? 0x04 : 0));
                uint32_t secs = day_.count + evening_.count + night_.count;
                uint32_t mins = secs / 60u;
                I2C_Comm_SetLdenProgress(mask, (uint16_t)(mins > 65535u ? 65535u : mins));
            }
        }
    }
    // Invalid second: last_ keeps its previous values (hold-last-valid).

    // Reported every second, valid or not: during a run of invalid seconds the
    // count really does fall as the sentinels age in, and freezing the last
    // computed value would hide exactly that.
    if (!valid) {
        int n = 0;
        for (int i = 0; i < win_count_; i++) {
            if (WIN_IS_VALID(win_buffer_[i])) n++;
        }
        win_valid_ = n;
    }
    I2C_Comm_SetWindowFill((uint16_t)win_valid_, (uint16_t)AGG_WINDOW_SEC);

    last_.cycles++;
    out = last_;
    mic_ok = valid ? 1 : 0;
    return valid;
}
