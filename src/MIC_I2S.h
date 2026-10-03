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

#ifndef MIC_I2S_H
#define MIC_I2S_H

#include <stdint.h>
#include <stddef.h>
#include "DSP_Engine.h"  // SAMPLE_RATE (selects DMA block size below)

/**
 * --- ICS-43434 I2S MEMS microphone capture (XIAO ESP32-S3) ---
 *
 * The ICS-43434 outputs 24-bit samples MSB-aligned inside a 32-bit
 * I2S (Philips) frame. With L/R tied to GND the data is sent on the
 * LEFT channel. Samples are normalized to full scale [-1.0, +1.0].
 *
 * Datasheet reference (TDK InvenSense ICS-43434):
 *   Sensitivity: -26 dBFS @ 94 dB SPL, 1 kHz, on the sine PEAK
 *   AOP: 120 dB SPL / self-noise: ~30 dBA
 * The units in this project measure ~13.8 dB hotter than that sensitivity,
 * which moves their digital full scale down to ~106 dB SPL; see main_i2s.cpp.
 */

// --- Pin mapping (Seeed XIAO ESP32-S3) ---
// D1 = GPIO2, D2 = GPIO3, D3 = GPIO4. I2C slave keeps D4/D5 (GPIO5/6).
#ifndef MIC_I2S_BCLK
#define MIC_I2S_BCLK 2   // SCK  (bit clock)
#endif
#ifndef MIC_I2S_WS
#define MIC_I2S_WS   3   // WS / LRCLK (word select)
#endif
#ifndef MIC_I2S_DIN
#define MIC_I2S_DIN  4   // SD   (data from mic)
#endif

#define MIC_I2S_PORT      I2S_NUM_0
// Frames per i2s_read() block. The legacy I2S driver caps dma_buf_len at 1024,
// so 48 kHz uses 768 (16 ms) rather than a naive 1536 that the driver rejects.
// 16 kHz keeps 512 (32 ms). The ICS-43434 supports up to 51.6 kHz.
#if SAMPLE_RATE == 48000
#define MIC_I2S_READ_LEN  768
#else
#define MIC_I2S_READ_LEN  512
#endif

// --- Acoustic characteristics (ICS-43434) ---
#define MIC_SENSITIVITY_DBFS (-26.0f) // dBFS output at MIC_REF_DB SPL
#define MIC_REF_DB           94.0f    // SPL reference of the sensitivity spec

// #3 clipping detection: |sample| above this fraction of full scale counts as
// a clipped sample. On the units measured that happens from ~106 dB SPL (and
// on strong EMI), not at the datasheet's 120 dB AOP.
#define MIC_CLIP_THRESHOLD   0.99f

/**
 * Install and start the I2S RX driver.
 * @return true on success.
 */
bool MIC_I2S_Init();

/**
 * Blocking read of up to max_samples mono samples, converted to float
 * normalized to full scale [-1, +1]. Peak and clip detection happen per
 * sample in SampleChain, so they land in the second each sample belongs to.
 * @return number of samples written into out.
 */
size_t MIC_I2S_Read(float *out, size_t max_samples);

#endif // MIC_I2S_H
