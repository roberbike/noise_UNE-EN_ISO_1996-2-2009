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
#include "time.h"
#include "NoiseAggregator.h"
#include "I2C_Comm.h"

void NoiseAggregator::begin(AmplitudeToDb to_db, float amp_scale, float min_amp) {
    to_db_ = to_db;
    amp_scale_ = amp_scale;
    min_amp_ = min_amp;
    stat_idx_ = 0;
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

        last_.noiseAvgDb = laeq;
        last_.noisePeakDb = lafmax;
        last_.noiseMinDb = laeq;

        // Linear-amplitude fields (mV for ADC, µFS for I2S)
        last_.noise = (uint32_t)lroundf(rms_amp);
        last_.noiseAvg = rms_amp;
        last_.noisePeak = fast_amp;
        last_.noiseMin = rms_amp;
        last_.noiseAvgLegalMax = fast_amp;
        last_.noiseAvgLegalMaxDb = lafmax;

        // --- L10/L90 over a full AGG_STAT_SAMPLES-second block ---
        if (stat_idx_ < AGG_STAT_SAMPLES) {
            stat_buffer_[stat_idx_++] = laeq;
        }
        if (stat_idx_ >= AGG_STAT_SAMPLES) {
            float tmp[AGG_STAT_SAMPLES];
            memcpy(tmp, stat_buffer_, sizeof(tmp));
            // Descending selection sort (small fixed N)
            for (int k = 0; k < AGG_STAT_SAMPLES - 1; k++) {
                for (int j = k + 1; j < AGG_STAT_SAMPLES; j++) {
                    if (tmp[k] < tmp[j]) {
                        float t = tmp[k]; tmp[k] = tmp[j]; tmp[j] = t;
                    }
                }
            }
            float l10 = tmp[AGG_STAT_SAMPLES / 10];
            float l90 = tmp[AGG_STAT_SAMPLES * 9 / 10];
            last_.noiseAvgLegal = l10;
            last_.noiseAvgLegalDb = l10;
            // L90 stored as uint16_t; clamp to valid range (SPL is never
            // negative, but guard the cast against a spurious sub-zero value).
            last_.lowNoiseLevel = (l90 > 0.0f) ? (uint16_t)(l90 + 0.5f) : 0;
            stat_idx_ = 0;
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
