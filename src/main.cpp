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

#include "DSP_Engine.h"
#include "I2C_Comm.h"
#include "NoiseAggregator.h"
#include "SampleChain.h"

/**
 * --- ESP32-C3 + MAX4466 NOISE MONITOR (ADC) ---
 * The ADC is polled at SAMPLE_RATE (16 kHz) by a busy loop. SampleChain does
 * the per-sample work and NoiseAggregator the per-second ISO 1996-2 work,
 * exactly as on the I2S node (main_i2s.cpp); only acquisition differs.
 * Orientative indicators per Decree 213/2012 and UNE-EN ISO 1996-2, not a
 * certified sound level meter.
 */

#define ADC_CHANNEL ADC1_CHANNEL_4 // GPIO 4

// One hardware setting, two names: IDF up to 4.4.6 (Arduino core up to
// 2.0.14) only has ADC_ATTEN_DB_11; IDF 4.4.7 (core 2.0.15) renamed it
// ADC_ATTEN_DB_12 and deprecated the old name, and IDF 5 (core 3.x) keeps
// the new one.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 7)
#define ADC_ATTEN ADC_ATTEN_DB_12
#else
#define ADC_ATTEN ADC_ATTEN_DB_11
#endif

#define NODE_TYPE_ADC 0x01         // reported via I2C metadata

// #B4 overload: samples at either end of the 12-bit range count as clipped,
// and more than ADC_MAX_CLIPS_PER_SEC of them invalidate the second (the same
// policy as the I2S node).
#define ADC_CLIP_HIGH 4090
#define ADC_CLIP_LOW  5
#define ADC_MAX_CLIPS_PER_SEC 10

// Task watchdog: if the sampling task stops feeding it, the chip resets instead
// of running mute. 5 s covers the 2 s aggregator timeout with margin.
#define WDT_TIMEOUT_S 5

esp_adc_cal_characteristics_t adc_chars;

// mV per ADC count (calibrated slope, no intercept). An AC amplitude such as
// an RMS must NOT go through esp_adc_cal_raw_to_voltage(): that function maps
// absolute codes to absolute mV and adds the calibration intercept, which
// distorts the dB law at low levels. Computed once in ruido_setup().
float adc_mv_per_count = 1.0f;

NoiseAggregator aggregator;
TaskHandle_t aggregator_task_handle = NULL;
QueueHandle_t secondQueue;

void SerialLog(const char *level, const char *msg) {
    Serial.printf("[%s] %s\n", level, msg);
}

// The MAX4466 output is biased at VCC/2. A bias far from that means the module
// is unpowered, disconnected or shorted.
bool check_microphone_connection(uint32_t bias_mv) {
    return (bias_mv > 800 && bias_mv < 2600);
}

// Amplitude (mV) -> dB SPL for the MAX4466 (injected into the aggregator).
// CALIBRATION_RMS_MV is the compile-time calibration; I2C_Comm_GetCalibOffset()
// adds the per-unit trim stored in NVS (CMD_SET_CALIB), applied without
// reflashing.
static float adc_amp_to_db(float rms_mv) {
    return 20.0f * log10f(rms_mv / CALIBRATION_RMS_MV) + CALIBRATION_DB
           + I2C_Comm_GetCalibOffset();
}

/**
 * Sampling task: polls the ADC at SAMPLE_RATE. On the single-core C3 a busy
 * loop is cheaper than a 16 kHz timer interrupt. It never blocks, so tasks
 * below its priority (Arduino's loopTask, IDLE) do not run once it starts —
 * which is why nothing here may depend on loop(), and why the watchdog below
 * must not watch the IDLE task.
 */
void sampling_task(void *pvParameters) {
    esp_task_wdt_add(NULL); // fed once per completed second

    SampleChain chain;
    SecondAccum second;
    // Half a second of warm-up: the DC tracker is primed from the first sample
    // and the filters settle before anything is accumulated (SampleChain.h).
    chain.begin(2048.0f, SAMPLE_RATE / 2);

    uint32_t period_frac = 0;   // #B1 fractional part of the sample period
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

            int raw = adc1_get_raw(ADC_CHANNEL);
            // #B4 The bias check only looks at the DC average, so it cannot
            // see clipping: a signal pinned at either rail keeps the mean
            // centred. Count samples sitting at the ends of the range.
            bool clipped = (raw >= ADC_CLIP_HIGH || raw <= ADC_CLIP_LOW);

            if (chain.push((float)raw, clipped, second)) {
                xQueueOverwrite(secondQueue, &second);
                esp_task_wdt_reset();
            }

            // #B1 exact average rate: add the integer period and borrow one
            // extra microsecond whenever the accumulated remainder overflows
            // (62/63 us alternating at 16 kHz -> 62.5 us mean = 16000.0 Hz).
            next_sample_time += SAMPLE_PERIOD_US;
            period_frac += SAMPLE_PERIOD_REM;
            if (period_frac >= SAMPLE_RATE) {
                period_frac -= SAMPLE_RATE;
                next_sample_time += 1;
            }
        } else {
            taskYIELD(); // dead time: let equal/higher-priority tasks run
        }
    }
}

/**
 * Aggregator task: woken once per second by the sampling task.
 */
void aggregator_task(void *pvParameters) {
    SecondAccum second;
    SensorData out = {};
    uint8_t mic_ok = 0;

    while (1) {
        if (xQueueReceive(secondQueue, &second, pdMS_TO_TICKS(2000)) == pdTRUE) {
            uint32_t bias_mv = esp_adc_cal_raw_to_voltage((uint32_t)second.dc, &adc_chars);
            bool bias_ok = check_microphone_connection(bias_mv);
            bool not_clipped = (second.clips <= ADC_MAX_CLIPS_PER_SEC);

            SecondInput in = {
                .mean_sq = (float)(second.sum_sq_A / second.samples),
                .max_fast_sq = second.max_fast_sq,
                .max_slow_sq = second.max_slow_sq,
                .peak_c = second.peak_c,
                .samples = second.samples,
                .input_valid = bias_ok && not_clipped
            };

            bool valid = aggregator.process(in, out, mic_ok);
            I2C_Comm_SetClipCount(second.clips > 65535u ? (uint16_t)65535u
                                                        : (uint16_t)second.clips);
            I2C_Comm_Publish(out, mic_ok);
            I2C_Comm_Service(); // deferred clock set + NVS write

            // Logged after publishing, so a slow serial port can never delay
            // the data. The serial drivers wait on a semaphore when their
            // buffer is full, so this task sleeps rather than spins and the
            // sampling task keeps running meanwhile.
            if (valid) {
                Serial.printf("[SMART] LAeq:%.1f | LAFmx:%.1f | LASmx:%.1f | LCpk:%.1f | L10:%.1f | L90:%d | Lden:%.1f | clip:%u | cyc:%u\n",
                              out.noiseAvgDb, out.noisePeakDb, out.noiseLASmaxDb,
                              out.noiseLCpeakDb, out.noiseAvgLegalDb,
                              out.lowNoiseLevel, out.noiseLden,
                              (unsigned)second.clips, (unsigned)out.cycles);
            } else if (!not_clipped) {
                SerialLog("WARN", "Overload: ADC clipping, reading invalidated");
            } else {
                SerialLog("WARN", "Microphone range error/disconnected");
            }
        } else {
            // No second in 2 s: sampling stalled. Report it (status 0, cycles
            // frozen) instead of serving a frozen struct as valid. The task
            // watchdog resets the chip if this persists.
            I2C_Comm_Publish(out, 0);
            I2C_Comm_Service();
            SerialLog("WARN", "No samples for 2 s: sampling task stalled");
        }
    }
}

void ruido_setup() {
    Serial.begin(115200);
    delay(1000);
    SerialLog("INIT", "Smart City Noise Sensor - ESP32-C3 + MAX4466 (ADC)");

    secondQueue = xQueueCreate(1, sizeof(SecondAccum));
    if (secondQueue == NULL) {
        SerialLog("ERR", "Failed to create the FreeRTOS queue");
        while (1) delay(1000);
    }

    DSP_Init();
    I2C_Comm_Init();
    I2C_Comm_SetNodeType(NODE_TYPE_ADC);

    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH_BIT_12, REF_VOLTAGE, &adc_chars);

    adc_mv_per_count = (float)(esp_adc_cal_raw_to_voltage(3000, &adc_chars) -
                               esp_adc_cal_raw_to_voltage(1000, &adc_chars)) / 2000.0f;
    Serial.printf("[INIT] ADC slope: %.4f mV/count\n", adc_mv_per_count);

    // Shared aggregator: ADC amplitude in mV via the calibrated slope; a
    // 0.05 mV floor flags dead-input seconds.
    aggregator.begin(adc_amp_to_db, adc_mv_per_count, 0.05f);

    // #13 Task watchdog. The Arduino core has already initialized it, so a
    // second esp_task_wdt_init() returns ESP_ERR_INVALID_STATE: reconfigure it
    // instead. idle_core_mask = 0 is load-bearing here: the sampling task never
    // blocks, so the IDLE task never runs, and a watchdog that watched it would
    // reset the chip every WDT_TIMEOUT_S.
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

    xTaskCreate(aggregator_task, "DSP_AGG", 8192, NULL, configMAX_PRIORITIES - 5, &aggregator_task_handle);
    // Created last: from here on loopTask does not get the CPU again.
    xTaskCreate(sampling_task, "ADC_SAM", 8192, NULL, 5, NULL);
}

void setup() {
    ruido_setup();
}

void loop() {
    // Never reached on the C3 (see sampling_task). Kept correct anyway: the
    // core may have this task subscribed to the watchdog, and deleting a
    // subscribed task is undefined, so unsubscribe and park instead.
    esp_task_wdt_delete(NULL);
    vTaskDelay(portMAX_DELAY);
}
