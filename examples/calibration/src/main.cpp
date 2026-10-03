/**
 * Calibration firmware — ESP32-C3 + MAX4466 node (ADC)
 * Noise monitor (UNE-EN ISO 1996-2, Decree 213/2012)
 *
 * Runs only the measurement chain of the main firmware (ADC at 16 kHz, DC
 * removal, A-weighting, RMS) and prints every second the RMS voltage at the
 * ADC input, the LAeq it gives with the current constants, the MAX4466 bias
 * and the number of samples at the ADC rails. It does not use I2C.
 *
 * It mirrors src/main.cpp and src/DSP_Engine.cpp: same sample timing, same
 * DC tracker, same A-weighting coefficients, same mV-per-count conversion. If
 * they ever diverge, the CALIBRATION_RMS_MV measured here is off by the
 * difference.
 *
 * Usage (details in README.md and docs/CALIBRACION.md):
 * 1. MAX4466 OUT -> GPIO 4, VCC -> 3.3 V, GND -> GND.
 * 2. Flash this firmware, open the Serial Monitor at 115200 baud.
 * 3. Couple a 94 dB / 1 kHz calibrator to the microphone and turn the
 *    MAX4466 gain trimmer until RMS reads 100-400 mV with clip = 0.
 * 4. Note the stable RMS: that is CALIBRATION_RMS_MV. Set it in the main
 *    firmware's platformio.ini (-D CALIBRATION_RMS_MV=<value>) and reflash.
 * 5. The production firmware also adds the per-unit offset kept in this
 *    chip's NVS (CMD_SET_CALIB). This sketch prints it at boot. A new
 *    CALIBRATION_RMS_MV makes any old offset meaningless: clear it, either by
 *    building this sketch with -D RESET_NVS_CALIB or by sending CMD_SET_CALIB
 *    with 0 from the master.
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

#include <Arduino.h>
#include <math.h>
#include <Preferences.h>
#include <esp_idf_version.h>
#include "driver/adc.h"
#include "esp_adc_cal.h"

// --- Configuration (same as the main firmware) ---
#define ADC_CHANNEL ADC1_CHANNEL_4  // GPIO 4 — MAX4466 output
// One hardware setting, two names: IDF up to 4.4.6 (Arduino core up to
// 2.0.14) only has ADC_ATTEN_DB_11; IDF 4.4.7 (core 2.0.15) renamed it
// ADC_ATTEN_DB_12 and deprecated the old name, and IDF 5 (core 3.x) keeps
// the new one.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 7)
#define ADC_ATTEN ADC_ATTEN_DB_12
#else
#define ADC_ATTEN ADC_ATTEN_DB_11
#endif
#define REF_VOLTAGE 1100

#define SAMPLE_RATE 16000
// 1000000/16000 = 62.5 us: add the integer part every sample and borrow one
// extra microsecond whenever the remainder overflows, exactly as the firmware
// does, so the average rate is 16000.0 Hz and not the 16129 Hz of a bare 62.
#define SAMPLE_PERIOD_US  (1000000 / SAMPLE_RATE)
#define SAMPLE_PERIOD_REM (1000000 % SAMPLE_RATE)

// Same rails as the firmware's overload counter.
#define ADC_CLIP_HIGH 4090
#define ADC_CLIP_LOW  5

// The constants in use, only to show the LAeq they give. Same defaults and
// the same build_flags as the main firmware (src/DSP_Engine.h).
#ifndef CALIBRATION_DB
#define CALIBRATION_DB 94.0f
#endif
#ifndef CALIBRATION_RMS_MV
#define CALIBRATION_RMS_MV 166.0f
#endif

// --- A-weighting @ 16 kHz: the coefficients of src/DSP_Engine.cpp ---
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
};

#ifdef A_WEIGHT_LEGACY_16K
static Biquad aWeighting[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {1.000000f, 2.000000f, 1.000000f, 0.821564f, 0.168742f, 0, 0}};
#else
static Biquad aWeighting[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {-0.36654287f, 1.61495515f, 0.72597221f, 0.05375630f, -0.07418460f, 0, 0}};
#endif

static float applyFilter(float in, Biquad &f) {
  float out = in * f.b0 + f.z1;
  f.z1 = in * f.b1 - f.a1 * out + f.z2;
  f.z2 = in * f.b2 - f.a2 * out;
  return out;
}

static esp_adc_cal_characteristics_t adc_chars;
static float mv_per_count = 1.0f;

// Chain state, kept across seconds as in the firmware.
static float dc_offset = 0.0f;
static bool dc_primed = false;
static uint32_t next_sample = 0;
static uint32_t period_frac = 0;

static void showNvsOffset() {
  Preferences prefs;
#ifdef RESET_NVS_CALIB
  if (prefs.begin("noise", false)) {
    prefs.remove("calib_db");
    prefs.end();
    Serial.println("[INIT] NVS calibration offset cleared (RESET_NVS_CALIB)");
  }
#endif
  float v = 0.0f;
  if (prefs.begin("noise", true)) {   // read-only: creates nothing
    if (prefs.isKey("calib_db")) v = prefs.getFloat("calib_db", 0.0f);
    prefs.end();
  }
  Serial.printf("[INIT] NVS calibration offset on this chip: %.2f dB%s\n", v,
                v != 0.0f ? "  <- the production firmware adds this to every level;"
                            " clear it after setting a new CALIBRATION_RMS_MV"
                          : "");
}

// Waits for the next sample instant and returns one raw ADC reading.
static int nextRaw() {
  while (true) {
    int32_t behind = (int32_t)(micros() - next_sample);   // wrap-safe
    if (behind >= 0) {
      if (behind > 100000) next_sample = micros();         // resync, no burst
      next_sample += SAMPLE_PERIOD_US;
      period_frac += SAMPLE_PERIOD_REM;
      if (period_frac >= SAMPLE_RATE) {
        period_frac -= SAMPLE_RATE;
        next_sample += 1;
      }
      return adc1_get_raw(ADC_CHANNEL);
    }
    taskYIELD();
  }
}

// One sample through the firmware's chain: DC tracker, then A-weighting.
static float processSample(int raw) {
  if (!dc_primed) {   // prime from the first sample, as the firmware does
    dc_offset = (float)raw;
    dc_primed = true;
  }
  dc_offset = (dc_offset * 0.9999f) + ((float)raw * 0.0001f);
  float a = (float)raw - dc_offset;
  for (int k = 0; k < 3; k++) a = applyFilter(a, aWeighting[k]);
  return a;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  CALIBRATION - MAX4466 node (ADC)");
  Serial.println("  ISO 1996-2 / Decree 213/2012");
  Serial.println("========================================");
  Serial.println("Input: GPIO 4 (MAX4466 OUT). One line per second.");
  Serial.println("1. 94 dB / 1 kHz calibrator coupled to the microphone.");
  Serial.println("2. Trimmer until RMS is 100-400 mV with clip = 0.");
  Serial.println("3. Stable RMS -> -D CALIBRATION_RMS_MV=<value> in the main");
  Serial.println("   firmware's platformio.ini.");
  Serial.println("----------------------------------------");

  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH_BIT_12, REF_VOLTAGE, &adc_chars);
  // Slope only (no intercept), as the firmware: an AC amplitude must not go
  // through esp_adc_cal_raw_to_voltage(), which adds the calibration offset.
  mv_per_count = (float)(esp_adc_cal_raw_to_voltage(3000, &adc_chars) -
                         esp_adc_cal_raw_to_voltage(1000, &adc_chars)) / 2000.0f;
  Serial.printf("[INIT] ADC slope: %.4f mV/count\n", mv_per_count);
  // Cast: a build flag such as -D CALIBRATION_RMS_MV=166 is an int.
  Serial.printf("[INIT] LAeq shown with CALIBRATION_DB=%.1f, CALIBRATION_RMS_MV=%.1f\n",
                (double)CALIBRATION_DB, (double)CALIBRATION_RMS_MV);
  showNvsOffset();

  // Half a second through the chain without measuring, so the DC tracker and
  // the filters have settled before the first reading (the firmware does the
  // same).
  next_sample = micros();
  for (int i = 0; i < SAMPLE_RATE / 2; i++) processSample(nextRaw());
}

void loop() {
  double sum_sq_A = 0.0;
  uint32_t clips = 0;

  for (int i = 0; i < SAMPLE_RATE; i++) {
    int raw = nextRaw();
    if (raw >= ADC_CLIP_HIGH || raw <= ADC_CLIP_LOW) clips++;
    float a = processSample(raw);
    sum_sq_A += (double)(a * a);
  }

  float rms_mv = sqrtf((float)(sum_sq_A / SAMPLE_RATE)) * mv_per_count;
  uint32_t bias_mv = esp_adc_cal_raw_to_voltage((uint32_t)dc_offset, &adc_chars);
  float laeq = (rms_mv > 0.05f)
      ? 20.0f * log10f(rms_mv / CALIBRATION_RMS_MV) + CALIBRATION_DB : 0.0f;

  Serial.printf("RMS: %.2f mV | LAeq: %.1f dB(A) | bias: %lu mV | clip: %lu\n",
                rms_mv, laeq, (unsigned long)bias_mv, (unsigned long)clips);
  if (clips > 0) {
    Serial.println("[WARN] Samples at the ADC rails: the signal is clipping. Lower the "
                   "MAX4466 gain (or the calibrator level); this reading is not valid.");
  }
  if (bias_mv <= 800 || bias_mv >= 2600) {
    Serial.println("[WARN] Bias out of range: MAX4466 unpowered, disconnected or shorted.");
  }
}
