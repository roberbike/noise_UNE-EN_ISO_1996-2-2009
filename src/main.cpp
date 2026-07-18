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
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "sys/time.h"
#include "time.h"

#include "DSP_Engine.h"
#include "I2C_Comm.h"

/**
 * --- ESP32-C3 PROFESSIONAL NOISE MONITOR ---
 * RTOS Polling Architecture (Senior Level)
 * Compliant with (orientative) requirements of Decree 213/2012 & UNE-ISO 1996-2.
 */

#define ADC_CHANNEL ADC1_CHANNEL_4 // GPIO 4

// Stats
PeriodStats statsDay = {0.0f, 0};
PeriodStats statsEvening = {0.0f, 0};
PeriodStats statsNight = {0.0f, 0};

#define STAT_SAMPLES 20
float stat_buffer[STAT_SAMPLES];
int stat_idx = 0;

esp_adc_cal_characteristics_t adc_chars;
float dc_offset = 2048.0;

// mV per ADC count (calibrated slope, no intercept). An AC amplitude such as
// an RMS must NOT go through esp_adc_cal_raw_to_voltage(): that function maps
// absolute codes to absolute mV and adds the calibration intercept, which
// distorts the dB law at low levels. Computed once in ruido_setup().
float adc_mv_per_count = 1.0f;

// Task synchronization
TaskHandle_t aggregator_task_handle = NULL;

// Safe double-buffer for passing aggregated 1-second data to aggregator task
struct RawSecondData {
    float max_fast_sq;
    double sum_sq_A;
    uint32_t samples_count;
};
QueueHandle_t timerToTaskQueue;

void SerialLog(const char *level, const char *msg) {
    Serial.printf("[%s] %s\n", level, msg);
}

bool check_microphone_connection(uint32_t bias_mv) {
    return (bias_mv > 800 && bias_mv < 2600); // 3.3V bias check
}

/**
 * High Priority Sampling Task (Polling)
 * Senior Programmer Note: On Single-Core C3, polling at 16kHz is MORE efficient 
 * than esp_timer because it eliminates 16,000 task context switches per second.
 */
void sampling_task(void *pvParameters) {
    int samples_count = 0;
    double sum_sq_A = 0.0;
    float fast_ema_sq = 0.0f;
    float slow_ema_sq = 0.0f;
    float max_fast_sq = 0.0f;
    float max_slow_sq = 0.0f;

    const float alpha_fast = 0.000500f; // Fast = 125ms
    const float alpha_slow = 0.000062f; // Slow = 1s

    uint32_t next_sample_time = micros();

    while (1) {
        uint32_t now = micros();
        // Wrap-safe elapsed check: a direct `now >= next_sample_time` breaks
        // at the micros() rollover (every ~71.6 min). If a blocking event
        // straddled the rollover, sampling froze for up to ~71 min and the
        // cached struct was served unchanged (flat line in dashboards).
        int32_t behind = (int32_t)(now - next_sample_time);
        if (behind >= 0) {
            // If pathologically late (>100 ms blocked), resync instead of
            // burst-sampling to catch up with a compressed, invalid second.
            if (behind > 100000) {
                next_sample_time = now;
            }

            uint32_t raw = adc1_get_raw(ADC_CHANNEL);

            dc_offset = (dc_offset * 0.9999f) + ((float)raw * 0.0001f);
            float signal = (float)raw - dc_offset;

            float filtered = signal;
            for (int i = 0; i < 3; i++) {
                filtered = DSP_ApplyFilter(filtered, aWeightingFilters[i]);
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
                    .samples_count = (uint32_t)samples_count
                };
                xQueueOverwrite(timerToTaskQueue, &secData);

                sum_sq_A = 0.0;
                max_fast_sq = 0.0f;
                max_slow_sq = 0.0f;
                samples_count = 0;
            }

            next_sample_time += SAMPLE_PERIOD_US;
        } else {
            // Dead time between samples (~62us window).
            // Release the CPU so I2C slave callbacks can be serviced.
            taskYIELD();
        }
    }
}

/**
 * Aggregator Task (Runs once per second, woken by Queue)
 */
void aggregator_task(void *pvParameters) {
    RawSecondData secData;
    SensorData localSensorData = {0};
    I2cPayloadMessage i2cMsg;

    while (1) {
        if (xQueueReceive(timerToTaskQueue, &secData, pdMS_TO_TICKS(2000)) == pdTRUE) {
            
            float mean_sq_A = (float)(secData.sum_sq_A / secData.samples_count);
            // Full float chain: no integer truncation. Near the noise floor the
            // A-weighted RMS is only a few mV; truncating to integer counts and
            // integer mV quantized LAeq into ~2.5 dB steps (flat lines at the
            // floor, e.g. a constant 58.7 dB).
            float voltage_rms_A = sqrtf(mean_sq_A) * adc_mv_per_count;
            float voltage_fast_max = sqrtf(secData.max_fast_sq) * adc_mv_per_count;

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

            // 0.05 mV floor avoids log10(0) and flags dead-input seconds
            // (mic supply glitch, stuck ADC) as invalid.
            bool valid_sample = (voltage_rms_A > 0.05f && CALIBRATION_RMS_MV > 0.0f);
            if (valid_sample) {
                laeq_local = 20.0f * log10f(voltage_rms_A / CALIBRATION_RMS_MV) + CALIBRATION_DB;
                lafmax_local = 20.0f * log10f(voltage_fast_max / CALIBRATION_RMS_MV) + CALIBRATION_DB;

                if (stat_idx < STAT_SAMPLES) {
                    stat_buffer[stat_idx++] = laeq_local;
                }

                // Percentiles over a FULL 20 s window. Resetting stat_idx every
                // second (previous behavior) meant the buffer never held more
                // than 1 sample, so L10/L90 were just the last LAeq.
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

            uint32_t bias_mv = esp_adc_cal_raw_to_voltage((uint32_t)dc_offset, &adc_chars);
            bool mic_ok_local = check_microphone_connection(bias_mv) && valid_sample;

            // Build output struct. On an invalid second every field keeps its
            // last valid value; only cycles advances and mic_ok reports the
            // fault. Masters must gate publishing on the status byte.
            if (valid_sample) {
                localSensorData.noise = (uint32_t)lroundf(voltage_rms_A);
                localSensorData.noiseAvg = voltage_rms_A;
                localSensorData.noiseAvgDb = laeq_local;
                localSensorData.noisePeak = voltage_fast_max;
                localSensorData.noisePeakDb = lafmax_local;
                localSensorData.noiseMin = voltage_rms_A;
                localSensorData.noiseMinDb = laeq_local;
                localSensorData.noiseAvgLegal = l10_local;
                localSensorData.noiseAvgLegalDb = l10_local;
                localSensorData.noiseAvgLegalMax = voltage_fast_max;
                localSensorData.noiseAvgLegalMaxDb = lafmax_local;
                localSensorData.lowNoiseLevel = (l90_local > 0.0f) ? (uint16_t)l90_local : 0;
                localSensorData.Ld = ld_local;
                localSensorData.Le = le_local;
                localSensorData.Ln = ln_local;
                localSensorData.noiseLden = lden_local;
            }
            localSensorData.cycles++;

            if (mic_ok_local) {
                Serial.printf("[SMART] LAeq:%.1f | LAFmx:%.1f | L10:%.1f | L90:%.1f | RMS:%.2fmV | Lden:%.1f\n",
                              laeq_local, lafmax_local, l10_local, l90_local, voltage_rms_A, lden_local);
            } else {
                SerialLog("WARN", "Microphone range error/disconnected");
            }

            i2cMsg.data = localSensorData;
            i2cMsg.mic_ok = mic_ok_local ? 1 : 0;
            xQueueOverwrite(dataQueue, &i2cMsg);
            I2C_Comm_Sync();
        } else {
            // No second completed in 2 s: the sampling task is stalled.
            // Surface the fault (status 0, frozen cycles) instead of silently
            // serving a frozen struct forever, which draws a flat line at an
            // arbitrary level in dashboards. Masters gate on the status byte.
            SerialLog("WARN", "No samples for 2 s: sampling task stalled");
            i2cMsg.data = localSensorData;
            i2cMsg.mic_ok = 0;
            xQueueOverwrite(dataQueue, &i2cMsg);
            I2C_Comm_Sync();
        }
    }
}

void ruido_setup() {
    Serial.begin(115200);
    delay(1000);
    SerialLog("INIT", "Smart City Noise Sensor (Class 1 Architecture)");

    dataQueue = xQueueCreate(1, sizeof(I2cPayloadMessage));
    timerToTaskQueue = xQueueCreate(1, sizeof(RawSecondData));

    if (dataQueue == NULL || timerToTaskQueue == NULL) {
        SerialLog("ERR", "Failed to create FreeRTOS Queues");
        while (1) delay(1000);
    }

    DSP_Init();
    I2C_Comm_Init();

#if defined(ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S2)
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_13, REF_VOLTAGE, &adc_chars);
    dc_offset = 4096.0f;
#else
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_12);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, REF_VOLTAGE, &adc_chars);
    dc_offset = 2048.0f;
#endif

    // Calibrated slope in mV/count (differential, intercept removed):
    // valid for converting AC amplitudes such as the A-weighted RMS.
    adc_mv_per_count = (float)(esp_adc_cal_raw_to_voltage(3000, &adc_chars) -
                               esp_adc_cal_raw_to_voltage(1000, &adc_chars)) / 2000.0f;
    Serial.printf("[INIT] ADC slope: %.4f mV/count\n", adc_mv_per_count);

    // Launch Aggregator Task
    xTaskCreate(aggregator_task, "DSP_AGG", 8192, NULL, configMAX_PRIORITIES - 5, &aggregator_task_handle); 

    // Launch Sampling Task
    // Note: Priority MUST be lower than the I2C interrupt handler priority.
    // On ESP32-C3, I2C slave callbacks are ISR-based, but high RTOS task priority
    // can still delay their proper execution via interrupt latency.
    xTaskCreate(sampling_task, "ADC_SAM", 8192, NULL, 5, NULL);
}

void setup() { 
    ruido_setup(); 
}

void loop() { 
    vTaskDelete(NULL); 
}
