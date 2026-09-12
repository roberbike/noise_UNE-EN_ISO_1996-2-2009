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
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include "sys/time.h"
#include "time.h"

#include "DSP_Engine.h"
#include "I2C_Comm.h"
#include "MIC_I2S.h"
#include "NoiseAggregator.h"

/**
 * --- XIAO ESP32-S3 + ICS-43434 NOISE MONITOR (I2S) ---
 * I2S digital MEMS front-end; the per-second acoustic math lives in the shared
 * NoiseAggregator (same code path as the ADC node in main.cpp). Level comes
 * from dBFS via the mic sensitivity spec (-26 dBFS @ 94 dB SPL); MIC_OFFSET_DB
 * allows an optional field trim against a reference meter.
 */

#ifndef MIC_OFFSET_DB
#define MIC_OFFSET_DB 0.0f
#endif

#define NODE_TYPE_I2S 0x02         // reported via I2C metadata
#define WARMUP_MS 500              // mic power-up + IIR transient
#define WDT_TIMEOUT_S 5

// #4 realistic disconnection threshold. The ICS-43434's own noise floor sits
// around 1e-5 FS (~30 dBA). The previous 1e-6 FS (~0 dB SPL) never triggered.
// 1e-5 FS detects a dead SD line without false positives on the real floor.
#define MIC_MIN_RMS_FS 1e-5f

// #3 max clips tolerated per second before invalidating the reading.
#define MIC_MAX_CLIPS_PER_SEC 10

NoiseAggregator aggregator;
TaskHandle_t aggregator_task_handle = NULL;

struct RawSecondData {
    float max_fast_sq;
    float max_slow_sq;     // LASmax
    float peak_c;          // LCpeak (C-weighted absolute peak)
    double sum_sq_A;
    uint32_t samples_count;
    float max_abs_fs;      // raw peak (FS) for mic-alive detection
    uint32_t clip_count;   // #3 samples at full scale this second
};
QueueHandle_t timerToTaskQueue;

void SerialLog(const char *level, const char *msg) {
    Serial.printf("[%s] %s\n", level, msg);
}

// Full-scale RMS -> dB SPL for the ICS-43434 (injected into aggregator).
//   L = 94 + 20*log10(rms_fs) - (-26) + offset
static float i2s_fs_to_db(float rms_fs) {
    // MIC_OFFSET_DB is the compile-time trim; I2C_Comm_GetCalibOffset() adds
    // the NVS field-calibration offset (CMD_SET_CALIB), applied without
    // reflashing.
    return MIC_REF_DB + 20.0f * log10f(rms_fs) - MIC_SENSITIVITY_DBFS
           + MIC_OFFSET_DB + I2C_Comm_GetCalibOffset();
}

/**
 * Sampling Task (I2S DMA). Blocks on MIC_I2S_Read(); the DMA engine paces the
 * loop at SAMPLE_RATE, so no timers are needed.
 */
void sampling_task(void *pvParameters) {
    esp_task_wdt_add(NULL); // #13 watchdog

    static float block[MIC_I2S_READ_LEN];

    int samples_count = 0;
    double sum_sq_A = 0.0;
    float fast_ema_sq = 0.0f;
    float max_fast_sq = 0.0f;
    float slow_ema_sq = 0.0f;
    float max_slow_sq = 0.0f;
    float peak_c = 0.0f;
    float max_abs_fs = 0.0f;
    uint32_t clip_acc = 0;

    // Fast time weighting = 125 ms. alpha = 1/(0.125 s * fs), so it tracks the
    // sample rate automatically (1/2000 at 16 kHz, 1/6000 at 48 kHz).
    const float alpha_fast = 1.0f / (0.125f * SAMPLE_RATE);
    // Slow time weighting = 1 s (LASmax).
    const float alpha_slow = 1.0f / (1.0f * SAMPLE_RATE);

    float dc_offset = 0.0f;

    // Discard mic power-up + filter transient
    uint32_t t0 = millis();
    while (millis() - t0 < WARMUP_MS) {
        MIC_I2S_Read(block, MIC_I2S_READ_LEN);
    }

    while (1) {
        size_t n = MIC_I2S_Read(block, MIC_I2S_READ_LEN);

        if (MIC_I2S_LastPeak() > max_abs_fs) max_abs_fs = MIC_I2S_LastPeak();
        clip_acc += MIC_I2S_LastClipCount();

        for (size_t i = 0; i < n; i++) {
            dc_offset = (dc_offset * 0.9999f) + (block[i] * 0.0001f);
            float signal = block[i] - dc_offset;

            float filtered = signal;
            for (int k = 0; k < 3; k++) {
                filtered = DSP_ApplyFilter(filtered, aWeightingFilters[k]);
            }

            float sq = filtered * filtered;
            sum_sq_A += (double)sq;

            fast_ema_sq = (sq * alpha_fast) + (fast_ema_sq * (1.0f - alpha_fast));
            if (fast_ema_sq > max_fast_sq) max_fast_sq = fast_ema_sq;

            // LASmax: slow (1 s) envelope of the A-weighted squared signal.
            slow_ema_sq = (sq * alpha_slow) + (slow_ema_sq * (1.0f - alpha_slow));
            if (slow_ema_sq > max_slow_sq) max_slow_sq = slow_ema_sq;

            // LCpeak: absolute peak of the C-weighted signal (no time weighting).
            float c_filt = signal;
            for (int k = 0; k < 2; k++) {
                c_filt = DSP_ApplyFilter(c_filt, cWeightingFilters[k]);
            }
            float c_abs = fabsf(c_filt);
            if (c_abs > peak_c) peak_c = c_abs;

            samples_count++;

            if (samples_count >= SAMPLE_RATE) {
                RawSecondData secData = {
                    .max_fast_sq = max_fast_sq,
                    .max_slow_sq = max_slow_sq,
                    .peak_c = peak_c,
                    .sum_sq_A = sum_sq_A,
                    .samples_count = (uint32_t)samples_count,
                    .max_abs_fs = max_abs_fs,
                    .clip_count = clip_acc
                };
                xQueueOverwrite(timerToTaskQueue, &secData);
                esp_task_wdt_reset();

                sum_sq_A = 0.0;
                max_fast_sq = 0.0f;
                max_slow_sq = 0.0f;
                peak_c = 0.0f;
                max_abs_fs = 0.0f;
                clip_acc = 0;
                samples_count = 0;
            }
        }
    }
}

/**
 * Aggregator Task. All ISO 1996-2 math delegated to the shared NoiseAggregator.
 */
void aggregator_task(void *pvParameters) {
    RawSecondData secData;
    SensorData out = {0};
    uint8_t mic_ok = 0;
    I2cPayloadMessage i2cMsg;

    while (1) {
        if (xQueueReceive(timerToTaskQueue, &secData, pdMS_TO_TICKS(2000)) == pdTRUE) {
            // Input validity (I2S-specific): mic alive AND not clipping.
            bool alive = (secData.max_abs_fs > MIC_MIN_RMS_FS);
            bool not_clipped = (secData.clip_count <= MIC_MAX_CLIPS_PER_SEC);
            bool input_ok = alive && not_clipped;

            SecondInput in = {
                .mean_sq = (float)(secData.sum_sq_A / secData.samples_count),
                .max_fast_sq = secData.max_fast_sq,
                .max_slow_sq = secData.max_slow_sq,
                .peak_c = secData.peak_c,
                .samples = secData.samples_count,
                .input_valid = input_ok,
                .clip_count = secData.clip_count
            };

            bool valid = aggregator.process(in, out, mic_ok);
            // uint32_t -> uint16_t: at 16 kHz a second never reaches 65535
            // clips, but the explicit cast silences -Wconversion.
            I2C_Comm_SetClipCount((uint16_t)secData.clip_count); // #12 metadata

            if (valid) {
                Serial.printf("[ICS43434] LAeq:%.1f | LAFmx:%.1f | LASmx:%.1f | LCpk:%.1f | L10:%.1f | L90:%d | Lden:%.1f | clip:%u | cyc:%u\n",
                              out.noiseAvgDb, out.noisePeakDb, out.noiseLASmaxDb,
                              out.noiseLCpeakDb, out.noiseAvgLegalDb,
                              out.lowNoiseLevel, out.noiseLden,
                              (unsigned)secData.clip_count, (unsigned)out.cycles);
            } else if (!not_clipped) {
                SerialLog("WARN", "Clipping detected: reading invalidated");
            } else {
                SerialLog("WARN", "I2S microphone silent/disconnected (check SD line and L/R->GND)");
            }

            i2cMsg.data = out;
            i2cMsg.mic_ok = mic_ok;
            xQueueOverwrite(dataQueue, &i2cMsg);
            I2C_Comm_Sync();
        } else {
            SerialLog("WARN", "No samples for 2 s: I2S sampling stalled");
            i2cMsg.data = out;
            i2cMsg.mic_ok = 0;
            xQueueOverwrite(dataQueue, &i2cMsg);
            I2C_Comm_Sync();
        }
    }
}

void ruido_setup() {
    Serial.begin(115200);
    delay(1000);
    SerialLog("INIT", "Smart City Noise Sensor - XIAO ESP32-S3 + ICS-43434 (I2S)");

    dataQueue = xQueueCreate(1, sizeof(I2cPayloadMessage));
    timerToTaskQueue = xQueueCreate(1, sizeof(RawSecondData));

    if (dataQueue == NULL || timerToTaskQueue == NULL) {
        SerialLog("ERR", "Failed to create FreeRTOS Queues");
        while (1) delay(1000);
    }

    DSP_Init();
    I2C_Comm_Init();
    I2C_Comm_SetNodeType(NODE_TYPE_I2S); // #12 metadata

    if (!MIC_I2S_Init()) {
        SerialLog("ERR", "I2S driver install failed");
        while (1) delay(1000);
    }
    Serial.printf("[INIT] I2S mic OK BCLK=%d WS=%d SD=%d @ %d Hz\n",
                  MIC_I2S_BCLK, MIC_I2S_WS, MIC_I2S_DIN, SAMPLE_RATE);

    // Shared aggregator: I2S samples are already full-scale (scale = 1.0);
    // dBFS->SPL via sensitivity; 1e-5 FS floor flags a dead SD line.
    // Shared aggregator: I2S samples are already full-scale (amp scale = 1.0);
    // dBFS->SPL via sensitivity; 1e-5 FS floor flags a dead SD line; the
    // integer `noise` field is stored in µFS (int_scale = 1e6) so it doesn't
    // round to 0.
    aggregator.begin(i2s_fs_to_db, 1.0f, MIC_MIN_RMS_FS, 1e6f);

    // #13 task watchdog. Arduino-ESP32 already inits the TWDT for loop();
    // reconfigure instead of re-init (see main.cpp). Sampling task subscribes
    // via esp_task_wdt_add(NULL).
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    if (esp_task_wdt_reconfigure(&wdt_cfg) == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_init(&wdt_cfg);
    }
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif

    // Aggregator on core 0, sampling pinned to core 1 (S3 is dual-core): the
    // acoustic chain never competes with WiFi/BLE/I2C on core 0.
    xTaskCreatePinnedToCore(aggregator_task, "DSP_AGG", 8192, NULL,
                            configMAX_PRIORITIES - 5, &aggregator_task_handle, 0);
    xTaskCreatePinnedToCore(sampling_task, "I2S_SAM", 8192, NULL,
                            configMAX_PRIORITIES - 3, NULL, 1);
}

void setup() {
    ruido_setup();
}

void loop() {
    vTaskDelete(NULL);
}
