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
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "sys/time.h"
#include "time.h"

#include "DSP_Engine.h"
#include "I2C_Comm.h"

/**
 * --- ESP32-C3 PROFESSIONAL NOISE MONITOR ---
 * Compliant with (orientative) requirements of Decree 213/2012 & UNE-ISO
 * 1996-2.
 */

#define ADC_CHANNEL ADC1_CHANNEL_4 // GPIO 4

// Shared variables for I2C Communication defined in I2C_Comm.h
volatile float g_LAeq_1s = 0.0f;
volatile float g_LAFmax_1s = 0.0f;
volatile float g_LASmax_1s = 0.0f;
volatile float g_L10_1s = 0.0f;
volatile float g_L90_1s = 0.0f;
volatile uint32_t g_last_rms_mv = 0;
volatile bool g_mic_connected = false;

SensorData globalSensorData = {
    0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f
};

// Operating System Primitives replacing portMUX_TYPE
SemaphoreHandle_t dataMutex;

// Stats
PeriodStats statsDay = {0, 0};
PeriodStats statsEvening = {0, 0};
PeriodStats statsNight = {0, 0};

#define STAT_SAMPLES 20
float stat_buffer[STAT_SAMPLES];
int stat_idx = 0;

esp_adc_cal_characteristics_t adc_chars;

void SerialLog(const char *level, const char *msg) {
    Serial.printf("[%s] %s\n", level, msg);
}

bool check_microphone_connection(uint32_t bias_mv) {
    return (bias_mv > 800 && bias_mv < 2600); // 3.3V bias check
}

static inline SensorData snapshot_sensor_data() {
    SensorData copy;
    if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
        memcpy(&copy, (const void *)&globalSensorData, sizeof(SensorData));
        xSemaphoreGive(dataMutex);
    }
    return copy;
}

/**
 * Main DSP Task
 */
void adc_task(void *pvParameters) {
#if defined(ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S2)
    adc1_config_width(ADC_WIDTH_BIT_13);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_13, REF_VOLTAGE, &adc_chars);
#else
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_12);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, REF_VOLTAGE, &adc_chars);
#endif

    double sum_sq_A = 0.0;
    float fast_ema_sq = 0.0f;
    float slow_ema_sq = 0.0f;
    float max_fast_sq = 0.0f;
    float max_slow_sq = 0.0f;

    const float alpha_fast = 0.000500f; // Fast = 125ms
    const float alpha_slow = 0.000062f; // Slow = 1s

    int samples_count = 0;
    int sub_sample_trigger = SAMPLE_RATE / STAT_SAMPLES;
    if (sub_sample_trigger < 1) sub_sample_trigger = 1;

#if defined(ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S2)
    float dc_offset = 4096.0f;
#else
    float dc_offset = 2048.0f;
#endif

    SerialLog("SYS", "Smart City DSP Engine V3.0 RTOS (Clase 2 Support)");

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xSamplePeriodTicks = pdMS_TO_TICKS(1000) / SAMPLE_RATE; 
    // Usually FreeRTOS configTICK_RATE_HZ is 1000. 
    // Note: For 16kHz, pure FreeRTOS task delay is too slow (1 tick = 1ms).
    // Using vTaskDelayUntil might not hit 16kHz accurately if configTICK_RATE_HZ == 1000.
    // For true high-frequency polling, hardware timers or I2S DMA is required.
    // As an intermediate step to lower CPU usage drastically from pure while(micros()),
    // we use a mixed approach: a tight yield loop bounded by a yield logic to allow OS breathing room.
    // Proper DMA is highly recommended for Production (Clase 1).

    unsigned long next_sample_time = micros();

    while (true) {
        unsigned long now = micros();
        if ((long)(now - next_sample_time) >= 0) {
            next_sample_time += SAMPLE_PERIOD_US;

            uint32_t raw = adc1_get_raw(ADC_CHANNEL);

            // DC Filter (Bias removal)
            dc_offset = (dc_offset * 0.9999f) + ((float)raw * 0.0001f);
            float signal = (float)raw - dc_offset;

            // 1. Apply A-Weighting Filter
            float filtered = signal;
            for (int i = 0; i < 3; i++) {
                filtered = DSP_ApplyFilter(filtered, aWeightingFilters[i]);
            }

            // 2. Integration for Leq
            float sq = filtered * filtered;
            sum_sq_A += sq;

            // 3. Time Weighting (Exponential Moving Average)
            fast_ema_sq = (sq * alpha_fast) + (fast_ema_sq * (1.0f - alpha_fast));
            slow_ema_sq = (sq * alpha_slow) + (slow_ema_sq * (1.0f - alpha_slow));

            if (fast_ema_sq > max_fast_sq) max_fast_sq = fast_ema_sq;
            if (slow_ema_sq > max_slow_sq) max_slow_sq = slow_ema_sq;

            if (samples_count % sub_sample_trigger == 0 && stat_idx < STAT_SAMPLES) {
                float current_db = 20.0f * log10(sqrtf(fast_ema_sq + 0.00001f) / (CALIBRATION_RMS_MV / 10.0f)) + CALIBRATION_DB;
                stat_buffer[stat_idx++] = current_db;
            }

            samples_count++;

            // Process once per second
            if (samples_count >= SAMPLE_RATE) {
                float mean_sq_A = (float)(sum_sq_A / samples_count);
                uint32_t voltage_rms_A = esp_adc_cal_raw_to_voltage((uint32_t)sqrtf(mean_sq_A), &adc_chars);
                uint32_t voltage_fast_max = esp_adc_cal_raw_to_voltage((uint32_t)sqrtf(max_fast_sq), &adc_chars);
                uint32_t voltage_slow_max = esp_adc_cal_raw_to_voltage((uint32_t)sqrtf(max_slow_sq), &adc_chars);

                float laeq_local = 0.0f;
                float lafmax_local = 0.0f;
                float lasmax_local = 0.0f;
                float l10_local = 0.0f;
                float l90_local = 0.0f;

                SensorData prev_data = snapshot_sensor_data();
                float ld_local = prev_data.Ld;
                float le_local = prev_data.Le;
                float ln_local = prev_data.Ln;
                float lden_local = prev_data.noiseLden;

                bool valid_sample = (voltage_rms_A > 0 && CALIBRATION_RMS_MV > 0.0f);
                if (valid_sample) {
                    laeq_local = 20.0f * log10((float)voltage_rms_A / CALIBRATION_RMS_MV) + CALIBRATION_DB;
                    lafmax_local = 20.0f * log10((float)voltage_fast_max / CALIBRATION_RMS_MV) + CALIBRATION_DB;
                    lasmax_local = 20.0f * log10((float)voltage_slow_max / CALIBRATION_RMS_MV) + CALIBRATION_DB;

                    if (stat_idx > 0) {
                        for (int i = 0; i < stat_idx - 1; i++) {
                            for (int j = i + 1; j < stat_idx; j++) {
                                if (stat_buffer[i] < stat_buffer[j]) {
                                    float t = stat_buffer[i];
                                    stat_buffer[i] = stat_buffer[j];
                                    stat_buffer[j] = t;
                                }
                            }
                        }
                        l10_local = stat_buffer[STAT_SAMPLES / 10];
                        l90_local = stat_buffer[STAT_SAMPLES * 9 / 10];
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

                        if (statsDay.hasData()) ld_local = statsDay.getAvg();
                        if (statsEvening.hasData()) le_local = statsEvening.getAvg();
                        if (statsNight.hasData()) ln_local = statsNight.getAvg();

                        double l_d = (double)ld_local;
                        double l_e = (double)le_local;
                        double l_n = (double)ln_local;
                        if (l_d > 0 || l_e > 0 || l_n > 0) {
                            double lden_energy = (12.0 * pow(10.0, l_d / 10.0) +
                                                  4.0 * pow(10.0, (l_e + 5.0) / 10.0) +
                                                  8.0 * pow(10.0, (l_n + 10.0) / 10.0)) / 24.0;
                            lden_local = (float)(10.0 * log10(lden_energy));
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

                // Update global data safely via Mutex (Replacing portMUX)
                if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
                    g_last_rms_mv = voltage_rms_A;
                    g_LAeq_1s = laeq_local;
                    g_LAFmax_1s = lafmax_local;
                    g_LASmax_1s = lasmax_local;
                    g_L10_1s = l10_local;
                    g_L90_1s = l90_local;
                    g_mic_connected = mic_ok_local;

                    globalSensorData.noise = valid_sample ? voltage_rms_A : 0;
                    globalSensorData.noiseAvg = valid_sample ? (float)voltage_rms_A : 0.0f;
                    globalSensorData.noiseAvgDb = laeq_local;
                    globalSensorData.noisePeak = valid_sample ? (float)voltage_fast_max : 0.0f;
                    globalSensorData.noisePeakDb = lafmax_local;
                    globalSensorData.noiseMin = valid_sample ? (float)voltage_rms_A : 0.0f;
                    globalSensorData.noiseMinDb = laeq_local;
                    globalSensorData.noiseAvgLegal = l10_local;
                    globalSensorData.noiseAvgLegalDb = l10_local;
                    globalSensorData.noiseAvgLegalMax = valid_sample ? (float)voltage_fast_max : 0.0f;
                    globalSensorData.noiseAvgLegalMaxDb = lafmax_local;
                    globalSensorData.lowNoiseLevel = (l90_local > 0.0f) ? (uint16_t)l90_local : 0;
                    globalSensorData.Ld = ld_local;
                    globalSensorData.Le = le_local;
                    globalSensorData.Ln = ln_local;
                    globalSensorData.noiseLden = lden_local;
                    globalSensorData.cycles++;
                    xSemaphoreGive(dataMutex);
                }

                if (mic_ok_local) {
                    Serial.printf("[SMART] LAeq:%.1f | LAFmx:%.1f | L10:%.1f | L90:%.1f | RMS:%dmV\n",
                                  laeq_local, lafmax_local, l10_local, l90_local, voltage_rms_A);
                } else {
                    SerialLog("WARN", "Microphone range error (check wiring)");
                }

                sum_sq_A = 0;
                max_fast_sq = 0;
                max_slow_sq = 0;
                samples_count = 0;
                stat_idx = 0;
                
                vTaskDelay(pdMS_TO_TICKS(1)); // Brief yield to the OS after intensive calc
                next_sample_time = micros();
            }
        } else {
            // Very short yield to let idle task run if we are way ahead
            if (next_sample_time - now > 2000) {
               vTaskDelay(pdMS_TO_TICKS(1));
            } else {
               taskYIELD(); 
            }
        }
    }
}

void ruido_setup() {
    Serial.begin(115200);
    delay(1000);
    SerialLog("INIT", "Smart City Noise Sensor (Clase 2 Ready) - RTOS Enhanced");

    dataMutex = xSemaphoreCreateMutex();
    if (dataMutex == NULL) {
        SerialLog("ERR", "Failed to create FreeRTOS Mutex");
        while (1) delay(1000);
    }

    DSP_Init();
    I2C_Comm_Init();

    xTaskCreate(adc_task, "ADC_DSP", 8192, NULL, configMAX_PRIORITIES - 1, NULL); // Highest possible tracking
}

void setup() { 
    ruido_setup(); 
}

void loop() { 
    vTaskDelete(NULL); // Free loop task, Everything handled in RTOS task 
}
