/**
 * Calibration firmware — Noise monitor (UNE-EN ISO 1996-2, Decree 213/2012)
 *
 * Runs only the measurement chain (ADC + A-weighting + RMS) and prints the RMS
 * value (mV) and LAeq (dB) every second over Serial. It does not use I2C.
 *
 * Usage:
 * 1. Connect MAX4466 OUT → GPIO 4, VCC 3.3V, GND.
 * 2. Adjust the MAX4466 potentiometer according to docs/CALIBRACION.md.
 * 3. Flash this firmware, open the Serial Monitor at 115200 baud.
 * 4. With a 94 dB calibrator (1 kHz), couple the microphone and note the stable RMS (mV).
 * 5. That value becomes CALIBRATION_RMS_MV in src/main.cpp of the main firmware.
 */

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

#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <Arduino.h>
#include <math.h>

// --- Configuration (same as the main firmware) ---
#define ADC_CHANNEL ADC1_CHANNEL_4  // GPIO 4 — MAX4466 output
#define SAMPLE_RATE 16000
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)
#define REF_VOLTAGE 1100

// Calibration constants. With the 94 dB calibrator, set CALIBRATION_RMS_MV to
// the stable RMS value (mV) that is displayed; this makes LAeq read about 94 dB.
#define CALIBRATION_DB 94.0f
#define CALIBRATION_RMS_MV 166.0f  // Replace with the measured value from step 4

// --- A-weighting filter (16 kHz) ---
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
};

static Biquad aWeightingFilters[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {1.000000f, 2.000000f, 1.000000f, 0.821564f, 0.168742f, 0, 0}};

static float applyFilter(float in, Biquad &f) {
  float out = in * f.b0 + f.z1;
  f.z1 = in * f.b1 - f.a1 * out + f.z2;
  f.z2 = in * f.b2 - f.a2 * out;
  return out;
}

static esp_adc_cal_characteristics_t adc_chars;

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  CALIBRATION - Noise monitor");
  Serial.println("  ISO 1996-2 / Decree 213/2012");
  Serial.println("========================================");
  Serial.println();
  Serial.println("Input: GPIO 4 (MAX4466 OUT)");
  Serial.println("Output: RMS (mV) and LAeq (dB) every 1 s");
  Serial.println();
  Serial.println("Steps:");
  Serial.println("  1. 94 dB calibrator @ 1 kHz, microphone coupled.");
  Serial.println("  2. Record the stable RMS (mV) value.");
  Serial.println("  3. Copy that value to CALIBRATION_RMS_MV in src/main.cpp");
  Serial.println("     of the main firmware.");
  Serial.println();
  Serial.println("----------------------------------------");

#if defined(ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S2)
  adc1_config_width(ADC_WIDTH_BIT_13);
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_13,
                           REF_VOLTAGE, &adc_chars);
#else
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_12);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12,
                           REF_VOLTAGE, &adc_chars);
#endif
}

void loop() {
  double sum_sq_A = 0.0;
  int samples_count = 0;

#if defined(ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S2)
  float dc_offset = 4096.0f;
#else
  float dc_offset = 2048.0f;
#endif

  uint32_t next_sample = micros();
  const uint32_t start = next_sample;

    while ((int32_t)(micros() - start) < 1000000L) {  // 1 s, wrap-safe
      if ((int32_t)(micros() - next_sample) >= 0) {
        next_sample += SAMPLE_PERIOD_US;

        uint32_t raw = adc1_get_raw(ADC_CHANNEL);
        dc_offset = (dc_offset * 0.9999f) + ((float)raw * 0.0001f);
        float signal = (float)raw - dc_offset;

        float filtered = signal;
        for (int i = 0; i < 3; i++) {
          filtered = applyFilter(filtered, aWeightingFilters[i]);
        }
        sum_sq_A += (double)(filtered * filtered);
        samples_count++;
      } else {
          int32_t remaining = (int32_t)(next_sample - micros());
          if (remaining > 2000) {
              vTaskDelay(pdMS_TO_TICKS(1));
          } else {
              taskYIELD();
          }
      }
    }

  if (samples_count > 0) {
    float mean_sq = (float)(sum_sq_A / (double)samples_count);
    // Slope-only float conversion (same as the fixed main firmware): no
    // integer truncation and no calibration intercept on an AC amplitude.
    static float mv_per_count = 0.0f;
    if (mv_per_count == 0.0f) {
      mv_per_count = (float)(esp_adc_cal_raw_to_voltage(3000, &adc_chars) -
                             esp_adc_cal_raw_to_voltage(1000, &adc_chars)) / 2000.0f;
      Serial.printf("[INIT] ADC slope: %.4f mV/count\n", mv_per_count);
    }
    float voltage_rms_mv = sqrtf(mean_sq) * mv_per_count;

    float laeq = 0.0f;
    if (voltage_rms_mv > 0.05f && CALIBRATION_RMS_MV > 0.0f) {
      laeq = 20.0f * log10(voltage_rms_mv / CALIBRATION_RMS_MV) +
             CALIBRATION_DB;
    }

    Serial.printf("RMS: %.2f mV  |  LAeq: %.1f dB(A)  (ref %.1f dB @ %.1f mV)\n",
                  voltage_rms_mv, laeq, CALIBRATION_DB,
                  CALIBRATION_RMS_MV);
  }
}
