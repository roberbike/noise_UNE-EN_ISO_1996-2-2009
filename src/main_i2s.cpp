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

#include "DSP_Engine.h"
#include "I2C_Comm.h"
#include "MIC_I2S.h"
#include "NoiseAggregator.h"
#include "SampleChain.h"

/**
 * --- XIAO ESP32-S3 + ICS-43434 NOISE MONITOR (I2S) ---
 * The I2S DMA engine paces acquisition at SAMPLE_RATE (48 kHz in this build).
 * SampleChain does the per-sample work and NoiseAggregator the per-second
 * ISO 1996-2 work, exactly as on the ADC node (main.cpp).
 *
 * LEVEL CONVERSION. The datasheet sensitivity (-26 dBFS at 94 dB SPL) is
 * specified on the PEAK of a sine and this code measures an RMS, hence
 * MIC_PEAK_TO_RMS_DB. On top of that, the units in this project read ~13.8 dB
 * hotter than the datasheet, which MIC_OFFSET_DB (platformio.ini) corrects;
 * the value was verified against a 94 dB calibrator and a reference sound
 * level meter. The whole derivation, and how to re-check a unit, is in
 * docs/CALIBRACION.md. Per-unit adjustments go through CMD_SET_CALIB.
 *
 * MEASUREMENT CEILING. Those extra 13.8 dB of sensitivity cost the same
 * amount of headroom: digital full scale arrives at ~106 dB SPL, not at the
 * part's nominal 120 dB AOP. Verified with a calibrator at 94 and 104 dB,
 * where the chain is linear within 0.16 dB, and at 114 dB, where it sits
 * ~8 dB past full scale and the output degrades instead of clipping cleanly.
 * Ample for urban ambient levels, but a loud impulse's LCpeak can exceed it;
 * MIC_MAX_CLIPS_PER_SEC then invalidates the second.
 */

#ifndef MIC_OFFSET_DB
#define MIC_OFFSET_DB 0.0f
#endif

// Sensitivity is specified on the sine peak; we measure RMS (20*log10(sqrt 2)).
// Cross-check: the datasheet AOP (120 dB SPL) and sensitivity (-26 dBFS)
// differ by exactly 26 dB, which only holds if full scale is reached by the
// PEAK of a 120 dB sine.
#define MIC_PEAK_TO_RMS_DB 3.0103f

#define NODE_TYPE_I2S 0x02         // reported via I2C metadata
#define WDT_TIMEOUT_S 5

// Dead-microphone gates. 1e-5 of full scale is -100 dBFS: ~9 dB SPL with the
// calibrated conversion (23 dB with the datasheet one), below any real room
// and below the microphone's own self-noise, so a connected ICS-43434 never
// reads that low. A dead SD line reads exact zeros (or -1 LSB, -138 dBFS).
//  - MIC_MIN_PEAK_FS: the raw input peak (before DC removal and weighting)
//    must exceed it for the second to count as input at all.
//  - MIC_MIN_RMS_FS: the A-weighted RMS must exceed it (aggregator gate).
#define MIC_MIN_PEAK_FS 1e-5f
#define MIC_MIN_RMS_FS  1e-5f

// #3 more clipped samples than this in one second invalidate it.
#define MIC_MAX_CLIPS_PER_SEC 10

NoiseAggregator aggregator;
TaskHandle_t aggregator_task_handle = NULL;
QueueHandle_t secondQueue;

void SerialLog(const char *level, const char *msg) {
    Serial.printf("[%s] %s\n", level, msg);
}

// Full-scale RMS -> dB SPL for the ICS-43434 (injected into the aggregator):
//   L = 94 + 20*log10(rms_fs) + 26 + 3.0103 + MIC_OFFSET_DB + NVS offset
static float i2s_fs_to_db(float rms_fs) {
    return MIC_REF_DB + 20.0f * log10f(rms_fs) - MIC_SENSITIVITY_DBFS
           + MIC_PEAK_TO_RMS_DB
           + MIC_OFFSET_DB + I2C_Comm_GetCalibOffset();
}

/**
 * Sampling task: blocks on MIC_I2S_Read(), so the DMA engine paces it and no
 * timer is needed.
 */
void sampling_task(void *pvParameters) {
    esp_task_wdt_add(NULL); // fed once per completed second

    static float block[MIC_I2S_READ_LEN];
    SampleChain chain;
    SecondAccum second;
    // Half a second of warm-up, through the filters (SampleChain.h). It also
    // covers the microphone's own power-up.
    chain.begin(0.0f, SAMPLE_RATE / 2);

    while (1) {
        size_t n = MIC_I2S_Read(block, MIC_I2S_READ_LEN);
        for (size_t i = 0; i < n; i++) {
            bool clipped = fabsf(block[i]) > MIC_CLIP_THRESHOLD;
            if (chain.push(block[i], clipped, second)) {
                xQueueOverwrite(secondQueue, &second);
                esp_task_wdt_reset();
            }
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
            bool alive = (second.max_abs > MIC_MIN_PEAK_FS);
            bool not_clipped = (second.clips <= MIC_MAX_CLIPS_PER_SEC);

            SecondInput in = {
                .mean_sq = (float)(second.sum_sq_A / second.samples),
                .max_fast_sq = second.max_fast_sq,
                .max_slow_sq = second.max_slow_sq,
                .peak_c = second.peak_c,
                .samples = second.samples,
                .input_valid = alive && not_clipped
            };

            bool valid = aggregator.process(in, out, mic_ok);
            // A fully clipped second at 48 kHz is 48000, inside uint16_t;
            // saturate anyway in case the rate ever rises.
            I2C_Comm_SetClipCount(second.clips > 65535u ? (uint16_t)65535u
                                                        : (uint16_t)second.clips);
            I2C_Comm_Publish(out, mic_ok);
            I2C_Comm_Service(); // deferred clock set + NVS write

            // Logged after publishing, so a slow serial port can never delay
            // the data. RMS in dBFS is the one figure that depends on neither
            // the trim nor the sensitivity constant — it is the microphone's
            // raw output — so field calibration is a single reading of it.
            if (valid) {
                float rms_dbfs = (out.noise > 0)
                    ? 20.0f * log10f((float)out.noise / 1e6f) : -120.0f;
                Serial.printf("[ICS43434] LAeq:%.1f | LAFmx:%.1f | LASmx:%.1f | LCpk:%.1f | L10:%.1f | L90:%d | Lden:%.1f | RMS:%.1f dBFS | clip:%u | cyc:%u\n",
                              out.noiseAvgDb, out.noisePeakDb, out.noiseLASmaxDb,
                              out.noiseLCpeakDb, out.noiseAvgLegalDb,
                              out.lowNoiseLevel, out.noiseLden, rms_dbfs,
                              (unsigned)second.clips, (unsigned)out.cycles);
            } else if (!not_clipped) {
                SerialLog("WARN", "Clipping detected: reading invalidated");
            } else {
                SerialLog("WARN", "I2S microphone silent/disconnected (check SD line and L/R->GND)");
            }
        } else {
            // No second in 2 s: report it (status 0, cycles frozen) instead of
            // serving a frozen struct as valid. The watchdog resets the chip
            // if this persists.
            I2C_Comm_Publish(out, 0);
            I2C_Comm_Service();
            SerialLog("WARN", "No samples for 2 s: I2S sampling stalled");
        }
    }
}

void ruido_setup() {
    Serial.begin(115200);
    delay(1000);
    SerialLog("INIT", "Smart City Noise Sensor - XIAO ESP32-S3 + ICS-43434 (I2S)");

    secondQueue = xQueueCreate(1, sizeof(SecondAccum));
    if (secondQueue == NULL) {
        SerialLog("ERR", "Failed to create the FreeRTOS queue");
        while (1) delay(1000);
    }

    DSP_Init();
    I2C_Comm_Init();
    I2C_Comm_SetNodeType(NODE_TYPE_I2S);

    if (!MIC_I2S_Init()) {
        SerialLog("ERR", "I2S driver install failed");
        while (1) delay(1000);
    }
    Serial.printf("[INIT] I2S mic OK BCLK=%d WS=%d SD=%d @ %d Hz\n",
                  MIC_I2S_BCLK, MIC_I2S_WS, MIC_I2S_DIN, SAMPLE_RATE);

    // Shared aggregator: I2S samples are already full scale (amp scale 1.0);
    // the integer `noise` field is stored in µFS (int_scale 1e6) so it does
    // not round to 0.
    aggregator.begin(i2s_fs_to_db, 1.0f, MIC_MIN_RMS_FS, 1e6f);

    // #13 Task watchdog: already initialized by the Arduino core, so
    // reconfigure it (see main.cpp). Only the sampling task subscribes.
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

    // Aggregator on core 0, sampling pinned to core 1: the acoustic chain
    // never competes with WiFi/BLE/I2C work on core 0.
    xTaskCreatePinnedToCore(aggregator_task, "DSP_AGG", 8192, NULL,
                            configMAX_PRIORITIES - 5, &aggregator_task_handle, 0);
    xTaskCreatePinnedToCore(sampling_task, "I2S_SAM", 8192, NULL,
                            configMAX_PRIORITIES - 3, NULL, 1);
}

void setup() {
    ruido_setup();
}

void loop() {
    // The core may have this task subscribed to the watchdog, and deleting a
    // subscribed task is undefined: unsubscribe and park it instead.
    esp_task_wdt_delete(NULL);
    vTaskDelay(portMAX_DELAY);
}
