/**
 * Calibration firmware — XIAO ESP32-S3 + ICS-43434 node (I2S)
 * Noise monitor (UNE-EN ISO 1996-2, Decree 213/2012)
 *
 * Runs only the measurement chain (I2S + A-weighting + RMS) and prints every
 * second the untrimmed LAeq, the A-weighted and unweighted RMS in dBFS, the
 * crest factor and peak of the signal, this unit's sensitivity, and the
 * MIC_OFFSET_DB that would make it read the calibrator. It does not use I2C.
 *
 * It mirrors the main firmware's conversion EXACTLY: same sample rate, same
 * A-weighting coefficients, same peak-to-RMS term, same DC tracker. If the two
 * ever diverge, the trim derived here is wrong by the difference. Earlier
 * versions of this example got three of those wrong at once (16 kHz against
 * the node's 48 kHz, the superseded A coefficients, no peak-to-RMS term),
 * which is how a measured 106.5 dB turned into a trim 3 dB off.
 *
 * It applies NO trim of its own, by design: its job is to show the untrimmed
 * level so the trim can be derived from it.
 *
 * Usage (details in README.md and docs/CALIBRACION.md):
 * 1. Connect the ICS-43434: SCK -> GPIO2 (D1), WS -> GPIO3 (D2),
 *    SD -> GPIO4 (D3), VDD 3.3V, GND, L/R -> GND.
 * 2. Flash this firmware and open the Serial Monitor at 115200 baud.
 * 3. Couple a 94.0 dB / 1 kHz calibrator and let the reading settle.
 * 4. Check `crest` first: with a clean tone it reads 3.0 dB. Anything else
 *    means the tone is distorted (too loud for the unit) or the coupling lets
 *    room noise in, and nothing derived from that line is usable.
 * 5. Compare the printed MIC_OFFSET_DB with the firmware's (platformio.ini).
 *    With the calibrator coupled, a unit like the reference one prints
 *    between -13.8 and -14.3: a calibrator built for 1/2" capsules, on a
 *    MEMS port in a small cavity, delivers up to ~0.5 dB more than in free
 *    field. Off by more than ~1 dB, correct THIS unit with CMD_SET_CALIB
 *    (calibrateNode() in examples/i2c_master) rather than changing the
 *    shared build flag.
 * 6. For fine adjustment, compare in free field against a reference sound
 *    level meter, the reference -13.71 was validated against.
 *
 * Note on the two RMS figures: dBFS(A) is what the main firmware's log line
 * reports; dBFS(Z) is unweighted and includes low-frequency rumble that
 * A-weighting removes, so it reads higher in a real room. With a calibrator
 * tone (1 kHz, where A-weighting is 0 dB) the two agree.
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
#include <freertos/FreeRTOS.h>
#include "driver/i2s.h"

// --- Configuration (must match the main firmware) ---
#define MIC_BCLK 2
#define MIC_WS 3
#define MIC_DIN 4

// The S3 environment of the main firmware builds with -D SAMPLE_RATE=48000.
// Keep this in step with it: the A-weighting coefficients below are selected
// by this value, and measuring at a different rate changes both the weighting
// and the aliasing.
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 48000
#endif
#if SAMPLE_RATE != 16000 && SAMPLE_RATE != 48000
#error "SAMPLE_RATE must be 16000 or 48000 (coefficient sets available)"
#endif

#define READ_LEN 256

#define MIC_SENSITIVITY_DBFS (-26.0f) // ICS-43434 datasheet, on the sine PEAK
#define MIC_REF_DB 94.0f
#define MIC_CLIP_THRESHOLD 0.99f      // same as the firmware's clip counter

// Peak-to-RMS correction: the datasheet gives the sensitivity on the PEAK of
// a sine and this measures an RMS; for a sine they differ by 20*log10(sqrt 2).
// The firmware applies the same term (MIC_PEAK_TO_RMS_DB).
#define MIC_PEAK_TO_RMS_DB 3.0103f

// Level the calibrator is SET TO. Rebuild with -D CALIBRATOR_DB=104.0 for a
// linearity check at 104 dB. Printed on every line so a figure can never be
// read against the wrong reference.
#ifndef CALIBRATOR_DB
#define CALIBRATOR_DB 94.0f
#endif

// The trim the main firmware is built with (platformio.ini). Only used to
// show how far this unit is from it.
#ifndef FIRMWARE_MIC_OFFSET_DB
#define FIRMWARE_MIC_OFFSET_DB (-13.71f)
#endif

// --- A-weighting filter, same coefficients as src/DSP_Engine.cpp ---
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
};

#if SAMPLE_RATE == 48000 && defined(A_WEIGHT_LEGACY_48K)
static Biquad aWeightingFilters[3] = {
    {0.23418304f, 0.46836609f, 0.23418304f, -0.22455846f, 0.01260663f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.89387049f, 0.89515977f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.99461446f, 0.99462171f, 0, 0}};
#elif SAMPLE_RATE == 48000
static Biquad aWeightingFilters[3] = {
    {0.59481685f, 0.02328688f, -0.07069832f, -0.65174475f, 0.11228949f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.89387049f, 0.89515977f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.99461446f, 0.99462171f, 0, 0}};
#elif defined(A_WEIGHT_LEGACY_16K)
static Biquad aWeightingFilters[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {1.000000f, 2.000000f, 1.000000f, 0.821564f, 0.168742f, 0, 0}};
#else
static Biquad aWeightingFilters[3] = {
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

static int32_t raw[READ_LEN];

// Chain state, kept across seconds as in the firmware.
static float dc_offset = 0.0f;
static bool dc_primed = false;

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
                v != 0.0f ? "  <- the production firmware adds this on top of MIC_OFFSET_DB" : "");
}

// Reads one DMA block, converted to full scale. Returns the sample count.
static size_t readBlock(float *out) {
  size_t br = 0;
  if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &br, portMAX_DELAY) != ESP_OK) return 0;
  size_t n = br / sizeof(int32_t);
  for (size_t i = 0; i < n; i++) out[i] = (float)(raw[i] >> 8) / 8388608.0f;
  return n;
}

// DC tracker of the firmware, primed from the first sample.
static float removeDc(float s) {
  if (!dc_primed) {
    dc_offset = s;
    dc_primed = true;
  }
  dc_offset = (dc_offset * 0.9999f) + (s * 0.0001f);
  return s - dc_offset;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("[INIT] Calibration ICS-43434 (I2S) - XIAO ESP32-S3 @ %d Hz\n",
                SAMPLE_RATE);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = READ_LEN;
  cfg.use_apll = false; // the ESP32-S3 has no APLL

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = MIC_BCLK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_DIN;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("[ERR] I2S driver install failed");
    while (1) delay(1000);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.printf("[INIT] Conversion: SPL = %.1f + 20*log10(rms) + %.1f + %.4f (no trim)\n",
                MIC_REF_DB, -MIC_SENSITIVITY_DBFS, MIC_PEAK_TO_RMS_DB);
  Serial.printf("[INIT] Calibrator assumed at %.1f dB. Rebuild with "
                "-D CALIBRATOR_DB=<level> if yours is set differently.\n",
                (float)CALIBRATOR_DB);
  if ((float)CALIBRATOR_DB > 104.5f) {
    Serial.println("[WARN] The units of this project reach digital full scale near "
                   "106 dB SPL: a calibrator above ~104 dB overloads them (watch crest).");
  }
  Serial.printf("[INIT] sens is this unit's sensitivity referred to 94 dB SPL; "
                "datasheet says %.1f dBFS.\n", MIC_SENSITIVITY_DBFS);
  showNvsOffset();

  // Half a second through the chain without measuring: microphone start-up,
  // DC tracker and filters settle before the first reading (as the firmware).
  static float warm[READ_LEN];
  uint32_t done = 0;
  while (done < SAMPLE_RATE / 2) {
    size_t n = readBlock(warm);
    for (size_t i = 0; i < n; i++) {
      float s = removeDc(warm[i]);
      for (int k = 0; k < 3; k++) s = applyFilter(s, aWeightingFilters[k]);
    }
    done += n;
  }
  Serial.println("[INIT] Capturing. One line per second:");
}

void loop() {
  static float block[READ_LEN];
  double sum_sq_A = 0.0;
  double sum_sq_Z = 0.0;
  float peak = 0.0f;       // max |sample| after DC removal (the AC peak)
  uint32_t clips = 0;
  uint32_t count = 0;

  while (count < SAMPLE_RATE) {
    size_t n = readBlock(block);
    for (size_t i = 0; i < n; i++) {
      if (fabsf(block[i]) > MIC_CLIP_THRESHOLD) clips++;
      float s = removeDc(block[i]);
      sum_sq_Z += (double)(s * s);
      if (fabsf(s) > peak) peak = fabsf(s);
      float a = s;
      for (int k = 0; k < 3; k++) a = applyFilter(a, aWeightingFilters[k]);
      sum_sq_A += (double)(a * a);
      count++;
    }
  }

  float rms_A = sqrtf((float)(sum_sq_A / count));
  float rms_Z = sqrtf((float)(sum_sq_Z / count));
  if (rms_A <= 0.0f || rms_Z <= 0.0f) {
    Serial.println("[WARN] Absolute silence: check the SD line and L/R->GND");
    return;
  }

  float dbfs_A = 20.0f * log10f(rms_A);
  float dbfs_Z = 20.0f * log10f(rms_Z);
  // Crest factor of the unweighted signal: 3.01 dB for a clean sine. A
  // calibrator line that reads otherwise is distorted or leaking room noise.
  float crest = 20.0f * log10f(peak / rms_Z);
  // Identical to the main firmware's i2s_fs_to_db(), minus the trims.
  float laeq = MIC_REF_DB + dbfs_A - MIC_SENSITIVITY_DBFS + MIC_PEAK_TO_RMS_DB;
  // The firmware's MIC_OFFSET_DB that would make this unit read the
  // calibrator. Only meaningful while the calibrator is coupled.
  float offset = (float)CALIBRATOR_DB - laeq;
  // This unit's sensitivity referred to 94 dB SPL, on the sine peak, directly
  // comparable with the datasheet's -26 dBFS. For a linear chain it comes out
  // the same at 94 and at 104 dB.
  float sens = dbfs_A + MIC_PEAK_TO_RMS_DB - ((float)CALIBRATOR_DB - 94.0f);

  Serial.printf("LAeq: %.2f dB | dBFS(A): %.2f | dBFS(Z): %.2f | crest: %.2f dB | "
                "peak: %.2f FS | sens: %.2f dBFS | @%.0fdB: MIC_OFFSET_DB=%.2f "
                "(firmware %.2f, diff %+.2f)\n",
                laeq, dbfs_A, dbfs_Z, crest, peak, sens, (float)CALIBRATOR_DB,
                offset, (float)FIRMWARE_MIC_OFFSET_DB,
                offset - (float)FIRMWARE_MIC_OFFSET_DB);
  if (clips > 0) {
    Serial.printf("[WARN] %lu samples at full scale: overload, this line is not valid\n",
                  (unsigned long)clips);
  }
}
