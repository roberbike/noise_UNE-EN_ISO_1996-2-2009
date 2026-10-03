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
#include <freertos/FreeRTOS.h>
#include "driver/i2s.h"
#include "DSP_Engine.h"
#include "MIC_I2S.h"

// DMA buffer of 32-bit frames shared by MIC_I2S_Read()
static int32_t i2s_raw[MIC_I2S_READ_LEN];

bool MIC_I2S_Init() {
    // Field-by-field assignment (instead of designated initializers): the
    // layout of i2s_config_t differs between IDF 4.4 (Arduino core 2.0.x)
    // and IDF 5 (core 3.x), and this compiles on both.
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = SAMPLE_RATE;                    // 48 kHz on the S3 build
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;  // 24-bit data in 32-bit frame
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // ICS-43434 L/R pin -> GND
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 6;
    cfg.dma_buf_len = MIC_I2S_READ_LEN;
    cfg.use_apll = false;                             // ESP32-S3 has no APLL
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 0;

    esp_err_t inst = i2s_driver_install(MIC_I2S_PORT, &cfg, 0, NULL);
    if (inst != ESP_OK) {
        // Surface the concrete reason (e.g. ESP_ERR_INVALID_ARG when
        // dma_buf_len exceeds the legacy driver's 1024 cap) instead of a bare
        // false, so a bad sample-rate/buffer combo is diagnosable from serial.
        Serial.printf("[ERR] i2s_driver_install failed: 0x%x (rate=%d, dma_buf_len=%d)\n",
                      inst, SAMPLE_RATE, MIC_I2S_READ_LEN);
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = MIC_I2S_BCLK;
    pins.ws_io_num = MIC_I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = MIC_I2S_DIN;

    if (i2s_set_pin(MIC_I2S_PORT, &pins) != ESP_OK) {
        i2s_driver_uninstall(MIC_I2S_PORT);
        return false;
    }

    i2s_zero_dma_buffer(MIC_I2S_PORT);
    return true;
}

size_t MIC_I2S_Read(float *out, size_t max_samples) {
    size_t frames = (max_samples < MIC_I2S_READ_LEN) ? max_samples : MIC_I2S_READ_LEN;
    size_t bytes_read = 0;

    esp_err_t err = i2s_read(MIC_I2S_PORT, (void *)i2s_raw,
                             frames * sizeof(int32_t), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK || bytes_read == 0) {
        return 0;
    }

    size_t n = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        // 24-bit signed sample MSB-aligned in the 32-bit slot.
        // Arithmetic shift preserves the sign; normalize by 2^23.
        int32_t s24 = i2s_raw[i] >> 8;
        out[i] = (float)s24 / 8388608.0f;
    }
    return n;
}
