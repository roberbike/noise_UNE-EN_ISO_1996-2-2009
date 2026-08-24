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
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "sys/time.h"
#include "time.h"

#include "DSP_Engine.h"
#include "I2C_Comm.h"
#include "NoiseAggregator.h"

/**
 * --- ESP32-C3 PROFESSIONAL NOISE MONITOR ---
 * RTOS Polling Architecture. ADC (MAX4466) front-end; the per-second acoustic
 * math lives in the shared NoiseAggregator (see main_i2s.cpp for the I2S node).
 * Compliant with (orientative) requirements of Decree 213/2012 & UNE-ISO 1996-2.
 */

#define ADC_CHANNEL ADC1_CHANNEL_4 // GPIO 4

#define NODE_TYPE_ADC 0x01         // reported via I2C metadata

// Task/sampling watchdog: if a task stops feeding it, the chip resets instead
// of running mute. 5 s covers the 2 s aggregator timeout with margin.
#define WDT_TIMEOUT_S 5

esp_adc_cal_characteristics_t adc_chars;
float dc_offset = 2048.0;

// mV per ADC count (calibrated slope, no intercept). An AC amplitude such as
// an RMS must NOT go through esp_adc_cal_raw_to_voltage(): that function maps
// absolute codes to absolute mV and adds the calibration intercept, which
// distorts the dB law at low levels. Computed once in ruido_setup().
float adc_mv_per_count = 1.0f;

NoiseAggregator aggregator;
TaskHandle_t aggregator_task_handle = NULL;

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

// Amplitude(mV) -> dB SPL for the ADC/MAX4466 path (injected into aggregator).
static float adc_amp_to_db(float rms_mv) {
    return 20.0f * log10f(rms_mv / CALIBRATION_RMS_MV) + CALIBRATION_DB;
}

/**
 * High Priority Sampling Task (Polling)
 * On single-core C3, polling at 16 kHz is more efficient than esp_timer:
 * it eliminates 16,000 context switches per second.
 */
void sampling_task(void *pvParameters) {
    esp_task_wdt_add(NULL); // #13 watchdog: this task must keep sampling

    int samples_count = 0;
    double sum_sq_A = 0.0;
    float fast_ema_sq = 0.0f;
    float max_fast_sq = 0.0f;

    // Fast time weighting = 125 ms. alpha = 1/(0.125 s * fs).
    const float alpha_fast = 1.0f / (0.125f * SAMPLE_RATE);

    uint32_t next_sample_time = micros();

    while (1) {
        uint32_t now = micros();
        // Wrap-safe elapsed check (a direct `now >= next` breaks at the
        // micros() rollover every ~71.6 min, freezing sampling).
        int32_t behind = (int32_t)(now - next_sample_time);
        if (behind >= 0) {
            if (behind > 100000) {
                next_sample_time = now; // resync instead of burst-sampling
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
            if (fast_ema_sq > max_fast_sq) max_fast_sq = fast_ema_sq;

            samples_count++;

            if (samples_count >= SAMPLE_RATE) {
                RawSecondData secData = {
                    .max_fast_sq = max_fast_sq,
                    .sum_sq_A = sum_sq_A,
                    .samples_count = (uint32_t)samples_count
                };
                xQueueOverwrite(timerToTaskQueue, &secData);
                esp_task_wdt_reset(); // fed once per completed second

                sum_sq_A = 0.0;
                max_fast_sq = 0.0f;
                samples_count = 0;
            }

            next_sample_time += SAMPLE_PERIOD_US;
        } else {
            taskYIELD(); // dead time: let I2C slave callbacks run
        }
    }
}

/**
 * Aggregator Task (once per second, woken by queue).
 * All ISO 1996-2 math is delegated to the shared NoiseAggregator.
 */
void aggregator_task(void *pvParameters) {
    RawSecondData secData;
    SensorData out = {0};
    uint8_t mic_ok = 0;
    I2cPayloadMessage i2cMsg;

    while (1) {
        if (xQueueReceive(timerToTaskQueue, &secData, pdMS_TO_TICKS(2000)) == pdTRUE) {
            // Bias/connection check (ADC-specific) folds into input_valid.
            uint32_t bias_mv = esp_adc_cal_raw_to_voltage((uint32_t)dc_offset, &adc_chars);
            bool input_ok = check_microphone_connection(bias_mv);

            SecondInput in = {
                .mean_sq = (float)(secData.sum_sq_A / secData.samples_count),
                .max_fast_sq = secData.max_fast_sq,
                .samples = secData.samples_count,
                .input_valid = input_ok,
                .clip_count = 0 // ADC path: clipping handled by bias range
            };

            bool valid = aggregator.process(in, out, mic_ok);

            if (valid) {
                Serial.printf("[SMART] LAeq:%.1f | LAFmx:%.1f | L10:%.1f | L90:%d | RMS:%.2fmV | Lden:%.1f | cyc:%u\n",
                              out.noiseAvgDb, out.noisePeakDb, out.noiseAvgLegalDb,
                              out.lowNoiseLevel, out.noiseAvg, out.noiseLden,
                              (unsigned)out.cycles);
            } else {
                SerialLog("WARN", "Microphone range error/disconnected");
            }

            i2cMsg.data = out;
            i2cMsg.mic_ok = mic_ok;
            xQueueOverwrite(dataQueue, &i2cMsg);
            I2C_Comm_Sync();
        } else {
            // No second in 2 s: sampling stalled. Surface it (status 0,
            // cycles frozen) instead of serving a frozen struct. The task
            // watchdog will reset the chip if this persists.
            SerialLog("WARN", "No samples for 2 s: sampling task stalled");
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
    SerialLog("INIT", "Smart City Noise Sensor - ESP32-C3 + MAX4466 (ADC)");

    dataQueue = xQueueCreate(1, sizeof(I2cPayloadMessage));
    timerToTaskQueue = xQueueCreate(1, sizeof(RawSecondData));

    if (dataQueue == NULL || timerToTaskQueue == NULL) {
        SerialLog("ERR", "Failed to create FreeRTOS Queues");
        while (1) delay(1000);
    }

    DSP_Init();
    I2C_Comm_Init();
    I2C_Comm_SetNodeType(NODE_TYPE_ADC); // #12 metadata

#if defined(ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S2)
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_13, REF_VOLTAGE, &adc_chars);
    dc_offset = 4096.0f;
#else
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_12);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, REF_VOLTAGE, &adc_chars);
    dc_offset = 2048.0f;
#endif

    adc_mv_per_count = (float)(esp_adc_cal_raw_to_voltage(3000, &adc_chars) -
                               esp_adc_cal_raw_to_voltage(1000, &adc_chars)) / 2000.0f;
    Serial.printf("[INIT] ADC slope: %.4f mV/count\n", adc_mv_per_count);

    // Shared aggregator: ADC amplitude is mV; scale by the calibrated slope;
    // 0.05 mV floor flags dead-input seconds.
    aggregator.begin(adc_amp_to_db, adc_mv_per_count, 0.05f);

    // #13 task watchdog. Arduino-ESP32 already initializes the TWDT for the
    // loop() task, so calling esp_task_wdt_init() again returns
    // ESP_ERR_INVALID_STATE (and can disturb the loop task). Reconfigure the
    // existing TWDT instead; the sampling task subscribes via
    // esp_task_wdt_add(NULL) and feeds it once per completed second.
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    // reconfigure if already running, otherwise init (bare-IDF builds).
    if (esp_task_wdt_reconfigure(&wdt_cfg) == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_init(&wdt_cfg);
    }
#else
    // On 4.x, re-init with a longer timeout is tolerated (idempotent enough).
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif

    xTaskCreate(aggregator_task, "DSP_AGG", 8192, NULL, configMAX_PRIORITIES - 5, &aggregator_task_handle);
    xTaskCreate(sampling_task, "ADC_SAM", 8192, NULL, 5, NULL);
}

void setup() {
    ruido_setup();
}

void loop() {
    vTaskDelete(NULL);
}
