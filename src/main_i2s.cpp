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
#include "sys/time.h"
#include "time.h"

#include "DSP_Engine.h"
#include "I2C_Comm.h"
#include "MIC_I2S.h"

/**
 * --- XIAO ESP32-S3 + ICS-43434 PROFESSIONAL NOISE MONITOR ---
 * I2S digital MEMS microphone node. Same RTOS architecture, DSP chain
 * (16 kHz A-weighting), ISO 1996-2 indicators and I2C slave protocol
 * as the ESP32-C3 + MAX4466 node, so existing masters work unchanged.
 *
 * Key difference vs. the analog node: level is computed directly from
 * dBFS using the microphone sensitivity spec (-26 dBFS @ 94 dB SPL),
 * so no acoustic calibrator is strictly required. MIC_OFFSET_DB allows
 * an optional fine trim against a Class 1/2 reference meter.
 *
 * Compliant with (orientative) requirements of Decree 213/2012 & UNE-ISO 1996-2.
 */

// Optional field trim (dB) applied on top of the datasheet sensitivity.
#ifndef MIC_OFFSET_DB
#define MIC_OFFSET_DB 0.0f
#endif

// Settle time: microphone power-up + IIR filter transient.
#define WARMUP_MS 500

// Any live ICS-43434 shows its own noise floor (~1e-5 FS peak).
// Below this the data line is stuck/disconnected.
#define MIC_MIN_PEAK_FS 1e-6f

// Stats
PeriodStats statsDay = {0.0f, 0};
PeriodStats statsEvening = {0.0f, 0};
PeriodStats statsNight = {0.0f, 0};

#define STAT_SAMPLES 20
float stat_buffer[STAT_SAMPLES];
int stat_idx = 0;

// Task synchronization
TaskHandle_t aggregator_task_handle = NULL;

// Safe double-buffer for passing aggregated 1-second data to aggregator task
struct RawSecondData {
    float max_fast_sq;
    double sum_sq_A;
    uint32_t samples_count;
    float max_abs_fs;      // raw peak (FS) for mic-alive detection
};
QueueHandle_t timerToTaskQueue;

void SerialLog(const char *level, const char *msg) {
    Serial.printf("[%s] %s\n", level, msg);
}

/**
 * High Priority Sampling Task (I2S DMA)
 * Blocks on i2s_read(); the DMA engine paces the loop at exactly
 * SAMPLE_RATE, so no polling/timers are needed and the I2C slave
 * callbacks are serviced during the blocking wait.
 */
void sampling_task(void *pvParameters) {
    static float block[MIC_I2S_READ_LEN];

    int samples_count = 0;
    double sum_sq_A = 0.0;
    float fast_ema_sq = 0.0f;
    float slow_ema_sq = 0.0f;
    float max_fast_sq = 0.0f;
    float max_slow_sq = 0.0f;
    float max_abs_fs = 0.0f;

    // Same time constants as the C3 node (per-sample EMA @ 16 kHz)
    const float alpha_fast = 0.000500f; // Fast = 125ms
    const float alpha_slow = 0.000062f; // Slow = 1s

    float dc_offset = 0.0f;

    // Discard mic power-up + filter transient
    uint32_t t0 = millis();
    while (millis() - t0 < WARMUP_MS) {
        MIC_I2S_Read(block, MIC_I2S_READ_LEN);
    }

    while (1) {
        size_t n = MIC_I2S_Read(block, MIC_I2S_READ_LEN);

        if (MIC_I2S_LastPeak() > max_abs_fs) {
            max_abs_fs = MIC_I2S_LastPeak();
        }

        for (size_t i = 0; i < n; i++) {
            // Residual DC tracking (digital mics carry a small DC offset)
            dc_offset = (dc_offset * 0.9999f) + (block[i] * 0.0001f);
            float signal = block[i] - dc_offset;

            float filtered = signal;
            for (int k = 0; k < 3; k++) {
                filtered = DSP_ApplyFilter(filtered, aWeightingFilters[k]);
            }

            float sq = filtered * filtered;
            sum_sq_A += (double)sq;

            fast_ema_sq = (sq * alpha_fast) + (fast_ema_sq * (1.0f - alpha_fast));
            slow_ema_sq = (sq * alpha_slow) + (slow_ema_sq * (1.0f - alpha_slow));

            if (fast_ema_sq > max_fast_sq) max_fast_sq = fast_ema_sq;
            if (slow_ema_sq > max_slow_sq) max_slow_sq = slow_ema_sq;

            samples_count++;

            if (samples_count >= SAMPLE_RATE) {
                RawSecondData secData = {
                    .max_fast_sq = max_fast_sq,
                    .sum_sq_A = sum_sq_A,
                    .samples_count = (uint32_t)samples_count,
                    .max_abs_fs = max_abs_fs
                };
                xQueueOverwrite(timerToTaskQueue, &secData);

                sum_sq_A = 0.0;
                max_fast_sq = 0.0f;
                max_slow_sq = 0.0f;
                max_abs_fs = 0.0f;
                samples_count = 0;
            }
        }
    }
}

/**
 * dBFS -> dB SPL using the ICS-43434 sensitivity spec.
 * At 94 dB SPL / 1 kHz the mic outputs -26 dBFS, therefore:
 *   L = MIC_REF_DB + 20*log10(rms_fs) - MIC_SENSITIVITY_DBFS + MIC_OFFSET_DB
 */
static inline float fs_to_spl(float rms_fs) {
    return MIC_REF_DB + 20.0f * log10f(rms_fs) - MIC_SENSITIVITY_DBFS + MIC_OFFSET_DB;
}

/**
 * Aggregator Task (Runs once per second, woken by Queue)
 * Identical ISO 1996-2 logic to the C3 node: LAeq/LAFmax per second,
 * L10/L90 over a rolling 20 s window, Ld/Le/Ln accumulation by period
 * and Lden when device time is available.
 */
void aggregator_task(void *pvParameters) {
    RawSecondData secData;
    SensorData localSensorData = {0};
    I2cPayloadMessage i2cMsg;

    while (1) {
        if (xQueueReceive(timerToTaskQueue, &secData, portMAX_DELAY) == pdTRUE) {

            float mean_sq_A = (float)(secData.sum_sq_A / secData.samples_count);
            float rms_fs = sqrtf(mean_sq_A);
            float peak_fast_fs = sqrtf(secData.max_fast_sq);

            // Legacy mV fields carry micro-FS units on this node (digital mic
            // has no analog voltage). Documented in README; dB fields are the
            // primary output.
            uint32_t rms_ufs = (uint32_t)(rms_fs * 1e6f);
            uint32_t peak_ufs = (uint32_t)(peak_fast_fs * 1e6f);

            // Defaults: hold last valid values (never publish 0 dB on an
            // invalid second; a zero sample poisons Grafana/averages).
            float laeq_local = localSensorData.noiseAvgDb;
            float lafmax_local = localSensorData.noisePeakDb;
            float l10_local = localSensorData.noiseAvgLegalDb;
            float l90_local = (float)localSensorData.lowNoiseLevel;
            float lden_local = localSensorData.noiseLden;
            // Preserve last known period values as default (ISO 1996-2: keep last valid)
            float ld_local = localSensorData.Ld;
            float le_local = localSensorData.Le;
            float ln_local = localSensorData.Ln;

            bool mic_ok_local = (secData.max_abs_fs > MIC_MIN_PEAK_FS);
            bool valid_sample = mic_ok_local && (rms_fs > 0.0f);

            if (valid_sample) {
                laeq_local = fs_to_spl(rms_fs);
                lafmax_local = fs_to_spl(peak_fast_fs);

                if (stat_idx < STAT_SAMPLES) {
                    stat_buffer[stat_idx++] = laeq_local;
                }

                // Percentiles over a FULL 20 s window (see main.cpp fix).
                if (stat_idx >= STAT_SAMPLES) {
                    float temp_buf[STAT_SAMPLES];
                    memcpy(temp_buf, stat_buffer, STAT_SAMPLES * sizeof(float));

                    for (int k = 0; k < STAT_SAMPLES - 1; k++) {
                        for (int j = k + 1; j < STAT_SAMPLES; j++) {
                            if (temp_buf[k] < temp_buf[j]) {
                                float t = temp_buf[k];
                                temp_buf[k] = temp_buf[j];
                                temp_buf[j] = t;
                            }
                        }
                    }
                    l10_local = temp_buf[STAT_SAMPLES / 10];
                    l90_local = temp_buf[STAT_SAMPLES * 9 / 10];
                    stat_idx = 0; // start next 20 s block
                }

                struct tm timeinfo;
                if (getLocalTime(&timeinfo, 0)) {
                    int h = timeinfo.tm_hour;
                    if (h >= 7 && h < 19) {
                        statsDay.add(laeq_local);
                    } else if (h >= 19 && h < 23) {
                        statsEvening.add(laeq_local);
                    } else {
                        statsNight.add(laeq_local);
                    }

                    ld_local = statsDay.hasData() ? statsDay.getAvg() : localSensorData.Ld;
                    le_local = statsEvening.hasData() ? statsEvening.getAvg() : localSensorData.Le;
                    ln_local = statsNight.hasData() ? statsNight.getAvg() : localSensorData.Ln;

                    if (ld_local > 0 || le_local > 0 || ln_local > 0) {
                        float lden_energy = (12.0f * powf(10.0f, ld_local / 10.0f) +
                                             4.0f * powf(10.0f, (le_local + 5.0f) / 10.0f) +
                                             8.0f * powf(10.0f, (ln_local + 10.0f) / 10.0f)) / 24.0f;
                        lden_local = 10.0f * log10f(lden_energy);
                    }

                    static int last_day = -1;
                    if (last_day != -1 && last_day != timeinfo.tm_mday) {
                        statsDay.reset();
                        statsEvening.reset();
                        statsNight.reset();
                    }
                    last_day = timeinfo.tm_mday;
                }
            }

            // Build output struct (same layout as C3 node). On an invalid
            // second every field keeps its last valid value; only cycles
            // advances and mic_ok reports the fault.
            if (valid_sample) {
                localSensorData.noise = rms_ufs;
                localSensorData.noiseAvg = (float)rms_ufs;
                localSensorData.noiseAvgDb = laeq_local;
                localSensorData.noisePeak = (float)peak_ufs;
                localSensorData.noisePeakDb = lafmax_local;
                localSensorData.noiseMin = (float)rms_ufs;
                localSensorData.noiseMinDb = laeq_local;
                localSensorData.noiseAvgLegal = l10_local;
                localSensorData.noiseAvgLegalDb = l10_local;
                localSensorData.noiseAvgLegalMax = (float)peak_ufs;
                localSensorData.noiseAvgLegalMaxDb = lafmax_local;
                localSensorData.lowNoiseLevel = (l90_local > 0.0f) ? (uint16_t)l90_local : 0;
                localSensorData.Ld = ld_local;
                localSensorData.Le = le_local;
                localSensorData.Ln = ln_local;
                localSensorData.noiseLden = lden_local;
            }
            localSensorData.cycles++;

            if (mic_ok_local) {
                Serial.printf("[ICS43434] LAeq:%.1f | LAFmx:%.1f | L10:%.1f | L90:%.1f | RMS:%luuFS | Lden:%.1f\n",
                              laeq_local, lafmax_local, l10_local, l90_local,
                              (unsigned long)rms_ufs, lden_local);
            } else {
                SerialLog("WARN", "I2S microphone silent/disconnected (check SD line and L/R->GND)");
            }

            i2cMsg.data = localSensorData;
            i2cMsg.mic_ok = mic_ok_local ? 1 : 0;
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

    if (!MIC_I2S_Init()) {
        SerialLog("ERR", "I2S driver install failed");
        while (1) delay(1000);
    }
    Serial.printf("[INIT] I2S mic OK BCLK=%d WS=%d SD=%d @ %d Hz\n",
                  MIC_I2S_BCLK, MIC_I2S_WS, MIC_I2S_DIN, SAMPLE_RATE);

    // Launch Aggregator Task
    xTaskCreatePinnedToCore(aggregator_task, "DSP_AGG", 8192, NULL,
                            configMAX_PRIORITIES - 5, &aggregator_task_handle, 0);

    // Launch Sampling Task pinned to core 1: the S3 is dual-core, so the
    // acoustic chain never competes with WiFi/BLE/I2C running on core 0.
    xTaskCreatePinnedToCore(sampling_task, "I2S_SAM", 8192, NULL,
                            configMAX_PRIORITIES - 3, NULL, 1);
}

void setup() {
    ruido_setup();
}

void loop() {
    vTaskDelete(NULL);
}
