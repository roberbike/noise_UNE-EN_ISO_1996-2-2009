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
#include "esp_timer.h"
#include "esp_adc_cal.h"
#include "sys/time.h"
#include "time.h"

#include "DSP_Engine.h"
#include "I2C_Comm.h"

/**
 * --- ESP32-C3 PROFESSIONAL NOISE MONITOR ---
 * RTOS Timer Driven, FreeRTOS Queue Thread-Safe Architecture
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

// Timer and Task synchronization
esp_timer_handle_t adc_timer_handle;
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
 * HW Timer Callback (Runs at strict 16kHz / 62us)
 * Avoids 100% CPU lock from micros() while polling ADC perfectly.
 */
static void IRAM_ATTR adc_timer_callback(void* arg) {
    static int samples_count = 0;
    static double sum_sq_A = 0.0;
    static float fast_ema_sq = 0.0f;
    static float slow_ema_sq = 0.0f;
    static float max_fast_sq = 0.0f;
    static float max_slow_sq = 0.0f;

    const float alpha_fast = 0.000500f; // Fast = 125ms
    const float alpha_slow = 0.000062f; // Slow = 1s

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

    // Once per second, offload the heavy summary to the main RTOS task
    if (samples_count >= SAMPLE_RATE) {
        RawSecondData secData = {
            .max_fast_sq = max_fast_sq,
            .sum_sq_A = sum_sq_A,
            .samples_count = (uint32_t)samples_count
        };
        
        // BUG FIX: esp_timer callback runs from an RTOS task natively, NOT from an HW ISR.
        // Therefore, we must use standard xQueueOverwrite, avoiding illegal ..FromISR() calls.
        xQueueOverwrite(timerToTaskQueue, &secData);

        // Reset accumulators
        sum_sq_A = 0.0;
        max_fast_sq = 0.0f;
        max_slow_sq = 0.0f;
        samples_count = 0;
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
        // Blocks efficiently waiting for 1 second of data
        if (xQueueReceive(timerToTaskQueue, &secData, portMAX_DELAY) == pdTRUE) {
            
            float mean_sq_A = (float)(secData.sum_sq_A / secData.samples_count);
            uint32_t voltage_rms_A = esp_adc_cal_raw_to_voltage((uint32_t)sqrtf(mean_sq_A), &adc_chars);
            uint32_t voltage_fast_max = esp_adc_cal_raw_to_voltage((uint32_t)sqrtf(secData.max_fast_sq), &adc_chars);

            float laeq_local = 0.0f;
            float lafmax_local = 0.0f;
            float l10_local = 0.0f;
            float l90_local = 0.0f;
            float lden_local = localSensorData.noiseLden;

            bool valid_sample = (voltage_rms_A > 0 && CALIBRATION_RMS_MV > 0.0f);
            if (valid_sample) {
                laeq_local = 20.0f * log10f((float)voltage_rms_A / CALIBRATION_RMS_MV) + CALIBRATION_DB;
                lafmax_local = 20.0f * log10f((float)voltage_fast_max / CALIBRATION_RMS_MV) + CALIBRATION_DB;

                if (stat_idx < STAT_SAMPLES) {
                    stat_buffer[stat_idx++] = laeq_local;
                }

                if (stat_idx > 0) {
                    float temp_buf[STAT_SAMPLES];
                    memcpy(temp_buf, stat_buffer, stat_idx * sizeof(float));
                    
                    for (int k = 0; k < stat_idx - 1; k++) {
                        for (int j = k + 1; j < stat_idx; j++) {
                            if (temp_buf[k] < temp_buf[j]) {
                                float t = temp_buf[k];
                                temp_buf[k] = temp_buf[j];
                                temp_buf[j] = t;
                            }
                        }
                    }
                    l10_local = temp_buf[stat_idx / 10];
                    l90_local = temp_buf[stat_idx * 9 / 10];
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

                    float ld_local = statsDay.hasData() ? statsDay.getAvg() : localSensorData.Ld;
                    float le_local = statsEvening.hasData() ? statsEvening.getAvg() : localSensorData.Le;
                    float ln_local = statsNight.hasData() ? statsNight.getAvg() : localSensorData.Ln;

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
            bool mic_ok_local = check_microphone_connection(bias_mv);

            // Build output struct
            localSensorData.noise = valid_sample ? voltage_rms_A : 0;
            localSensorData.noiseAvg = valid_sample ? (float)voltage_rms_A : 0.0f;
            localSensorData.noiseAvgDb = laeq_local;
            localSensorData.noisePeak = valid_sample ? (float)voltage_fast_max : 0.0f;
            localSensorData.noisePeakDb = lafmax_local;
            localSensorData.noiseMin = valid_sample ? (float)voltage_rms_A : 0.0f;
            localSensorData.noiseMinDb = laeq_local;
            localSensorData.noiseAvgLegal = l10_local;
            localSensorData.noiseAvgLegalDb = l10_local;
            localSensorData.noiseAvgLegalMax = valid_sample ? (float)voltage_fast_max : 0.0f;
            localSensorData.noiseAvgLegalMaxDb = lafmax_local;
            localSensorData.lowNoiseLevel = (l90_local > 0.0f) ? (uint16_t)l90_local : 0;
            localSensorData.Ld = statsDay.getAvg();
            localSensorData.Le = statsEvening.getAvg();
            localSensorData.Ln = statsNight.getAvg();
            localSensorData.noiseLden = lden_local;
            localSensorData.cycles++;

            if (mic_ok_local) {
                Serial.printf("[SMART] LAeq:%.1f | LAFmx:%.1f | L10:%.1f | L90:%.1f | RMS:%dmV | Lden:%.1f\n",
                              laeq_local, lafmax_local, l10_local, l90_local, voltage_rms_A, lden_local);
            } else {
                SerialLog("WARN", "Microphone range error/disconnected");
            }

            // POST TO QUEUE (Overwrites previous unread item. Non-blocking thread-safe mechanism)
            i2cMsg.data = localSensorData;
            i2cMsg.mic_ok = mic_ok_local ? 1 : 0;
            xQueueOverwrite(dataQueue, &i2cMsg);
        }
    }
}

void ruido_setup() {
    Serial.begin(115200);
    delay(1000);
    SerialLog("INIT", "Smart City Noise Sensor (Class 1 HW Timer Driven)");

    // Define queues with explicit structural sizes
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

    // Launch Aggregator Task
    xTaskCreate(aggregator_task, "DSP_AGG", 8192, NULL, configMAX_PRIORITIES - 2, &aggregator_task_handle); 

    // Configure and start highest priority hardware timer for sampling
    const esp_timer_create_args_t adc_timer_args = {
        .callback = &adc_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "adc_sample_timer",
        .skip_unhandled_events = true
    };
    
    esp_timer_create(&adc_timer_args, &adc_timer_handle);
    esp_timer_start_periodic(adc_timer_handle, SAMPLE_PERIOD_US);
}

void setup() { 
    ruido_setup(); 
}

void loop() { 
    vTaskDelete(NULL); 
}
