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

#ifndef SAMPLE_CHAIN_H
#define SAMPLE_CHAIN_H

#include <stdint.h>
#include <math.h>
#include "DSP_Engine.h"

/*
 * Per-sample acoustic chain, shared by both nodes.
 *
 * Each node only supplies samples: the ADC node feeds raw counts from its
 * polling loop, the I2S node feeds full-scale floats from its DMA blocks.
 * Everything that happens to a sample from there on — DC removal, A and C
 * weighting, the Fast and Slow time weightings, the peak detector, clip
 * counting and the one-second accumulation — lives here once, so the two
 * nodes cannot drift apart. (They used to carry two copies of this loop.)
 *
 * Two things this does that the old copies did not:
 *
 *  - WARM-UP. The DC tracker is primed with the first sample and the first
 *    `warmup_samples` run through the filters without being accumulated, so
 *    the filter start-up transient never reaches a published second. Without
 *    it, the ADC node's tracker started at 2048 counts while the MAX4466 sits
 *    near 2700, and that step went through the C filter as a false ~100 dB
 *    LCpeak in the first second after every boot or watchdog reset. The two
 *    time-weighting envelopes are then seeded with the mean square of the
 *    last quarter of the warm-up, so LAFmax and LASmax are right from the
 *    first second instead of rising from zero.
 *
 *  - CLIPS ARE COUNTED PER SAMPLE, in the second the sample belongs to. The
 *    I2S node used to add a whole DMA block's clip count before processing
 *    the block, and at 48 kHz a 768-frame block straddles every other second
 *    boundary — so clipping was charged to the previous, clean second and the
 *    second that really clipped was published as valid.
 */

// What one completed second hands from the sampling task to the aggregator.
struct SecondAccum {
    double   sum_sq_A;     // sum of squared A-weighted samples
    float    max_fast_sq;  // max of the 125 ms Fast envelope of the A signal
    float    max_slow_sq;  // max of the 1 s Slow envelope (LASmax)
    float    peak_c;       // max |C-weighted sample| (LCpeak)
    float    max_abs;      // max |input sample| before DC removal
    float    dc;           // DC estimate at the end of the second
    uint32_t samples;      // samples accumulated
    uint32_t clips;        // samples the caller flagged as clipped
};

class SampleChain {
public:
    // initial_dc: DC estimate to start from when there is no warm-up.
    // warmup_samples: samples to run through the filters before accumulating;
    //                 when non-zero the DC tracker is primed from the first
    //                 sample instead of initial_dc.
    void begin(float initial_dc, uint32_t warmup_samples) {
        dc_ = initial_dc;
        warmup_left_ = warmup_samples;
        seed_from_ = warmup_samples / 4;   // seed from the last quarter
        primed_ = (warmup_samples == 0);
        seed_sum_ = 0.0;
        seed_n_ = 0;
        fast_ema_sq_ = 0.0f;
        slow_ema_sq_ = 0.0f;
        reset_second();
    }

    // Feeds one sample. `clipped` is decided by the caller, because each node
    // knows its own rails. Returns true when a second has just completed, and
    // then `out` holds it.
    bool push(float x, bool clipped, SecondAccum &out) {
        if (!primed_) {
            dc_ = x;
            primed_ = true;
        }
        dc_ = (dc_ * 0.9999f) + (x * 0.0001f);
        float signal = x - dc_;

        float a = signal;
        for (int k = 0; k < 3; k++) a = DSP_ApplyFilter(a, aWeightingFilters[k]);
        float c = signal;
        for (int k = 0; k < 2; k++) c = DSP_ApplyFilter(c, cWeightingFilters[k]);
        float sq = a * a;

        if (warmup_left_ > 0) {
            if (warmup_left_ <= seed_from_) {
                seed_sum_ += (double)sq;
                seed_n_++;
            }
            if (--warmup_left_ == 0 && seed_n_ > 0) {
                fast_ema_sq_ = (float)(seed_sum_ / seed_n_);
                slow_ema_sq_ = fast_ema_sq_;
            }
            return false;
        }

        acc_.sum_sq_A += (double)sq;

        fast_ema_sq_ = (sq * kAlphaFast) + (fast_ema_sq_ * (1.0f - kAlphaFast));
        if (fast_ema_sq_ > acc_.max_fast_sq) acc_.max_fast_sq = fast_ema_sq_;

        slow_ema_sq_ = (sq * kAlphaSlow) + (slow_ema_sq_ * (1.0f - kAlphaSlow));
        if (slow_ema_sq_ > acc_.max_slow_sq) acc_.max_slow_sq = slow_ema_sq_;

        float c_abs = fabsf(c);
        if (c_abs > acc_.peak_c) acc_.peak_c = c_abs;

        float x_abs = fabsf(x);
        if (x_abs > acc_.max_abs) acc_.max_abs = x_abs;

        if (clipped) acc_.clips++;

        if (++acc_.samples >= (uint32_t)SAMPLE_RATE) {
            acc_.dc = dc_;
            out = acc_;
            reset_second();
            return true;
        }
        return false;
    }

    float dc() const { return dc_; }

private:
    // Fast = 125 ms, Slow = 1 s: alpha = 1 / (tau * fs), so both follow the
    // sample rate automatically.
    static constexpr float kAlphaFast = 1.0f / (0.125f * SAMPLE_RATE);
    static constexpr float kAlphaSlow = 1.0f / (1.0f * SAMPLE_RATE);

    void reset_second() {
        acc_.sum_sq_A = 0.0;
        acc_.max_fast_sq = 0.0f;
        acc_.max_slow_sq = 0.0f;
        acc_.peak_c = 0.0f;
        acc_.max_abs = 0.0f;
        acc_.dc = dc_;
        acc_.samples = 0;
        acc_.clips = 0;
    }

    SecondAccum acc_{};
    float dc_ = 0.0f;
    float fast_ema_sq_ = 0.0f;
    float slow_ema_sq_ = 0.0f;
    uint32_t warmup_left_ = 0;
    uint32_t seed_from_ = 0;
    double seed_sum_ = 0.0;
    uint32_t seed_n_ = 0;
    bool primed_ = true;
};

#endif // SAMPLE_CHAIN_H
