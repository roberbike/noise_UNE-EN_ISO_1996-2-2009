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

/**
 * --- ICS-43434 I2S MEMS microphone capture (XIAO ESP32-S3) ---
 *
 * The ICS-43434 outputs 24-bit samples MSB-aligned inside a 32-bit
 * I2S (Philips) frame. With L/R tied to GND the data is sent on the
 * LEFT channel. Samples are normalized to full scale [-1.0, +1.0].
 *
 * Datasheet reference (TDK InvenSense ICS-43434):
 *   Sensitivity: -26 dBFS @ 94 dB SPL, 1 kHz
 *   SNR: 64 dBA / Noise floor: ~30 dBA
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
#define MIC_I2S_READ_LEN  512   // frames per i2s_read() block (32 ms @ 16 kHz)

// --- Acoustic characteristics (ICS-43434) ---
#define MIC_SENSITIVITY_DBFS (-26.0f) // dBFS output at MIC_REF_DB SPL
#define MIC_REF_DB           94.0f    // SPL reference of the sensitivity spec

// #3 clipping detection: |sample| above this fraction of full scale counts as
// a clip. The ICS-43434 saturates near ±1.0 FS at ~120 dB SPL / on strong EMI.
#define MIC_CLIP_THRESHOLD   0.99f

/**
 * Install and start the I2S RX driver.
 * @return true on success.
 */
bool MIC_I2S_Init();

/**
 * Blocking read of up to max_samples mono samples.
 * Samples are converted to float normalized to full scale [-1, +1].
 * @return number of samples written into out.
 */
size_t MIC_I2S_Read(float *out, size_t max_samples);

/**
 * Peak absolute amplitude (full-scale units) seen in the last read.
 * A physically connected ICS-43434 always shows its own noise floor
 * (~1e-5 FS); a stuck-at-zero data line reads as silence below that.
 */
float MIC_I2S_LastPeak();

/**
 * Number of samples that hit full scale (|s| > MIC_CLIP_THRESHOLD) in the
 * last read. Accumulated by the caller across a 1 s window to gate validity.
 */
uint32_t MIC_I2S_LastClipCount();

#endif // MIC_I2S_H
