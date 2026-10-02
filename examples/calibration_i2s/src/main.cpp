/**
 * Calibration firmware — XIAO ESP32-S3 + ICS-43434 node (I2S)
 * Noise monitor (UNE-EN ISO 1996-2, Decree 213/2012)
 *
 * Runs only the measurement chain (I2S + A-weighting + RMS) and prints, every
 * second, the A-weighted and unweighted RMS in dBFS, the LAeq, and the
 * MIC_OFFSET_DB value the main firmware would need. It does not use I2C.
 *
 * IMPORTANT: this sketch mirrors the main firmware's conversion EXACTLY —
 * same sample rate, same A-weighting coefficients, same peak-to-RMS term. If
 * the two ever diverge, the trim derived here is wrong by the difference.
 * Earlier versions of this example got all three wrong at once (16 kHz against
 * the node's 48 kHz, the superseded A coefficients, and no peak-to-RMS term),
 * which is how a measured 106.5 dB turned into a trim 3 dB off.
 *
 * It applies NO trim of its own, by design: its job is to show the untrimmed
 * level so the trim can be derived from it.
 *
 * Usage:
 * 1. Connect the ICS-43434: SCK → GPIO2 (D1), WS → GPIO3 (D2), SD → GPIO4 (D3),
 *    VDD 3.3V, GND, L/R → GND.
 * 2. Flash this firmware and open the Serial Monitor at 115200 baud.
 * 3. Couple a 94.0 dB / 1 kHz calibrator to the microphone port and let the
 *    reading settle for a few seconds.
 * 4. Check `sens` first. It is this unit's sensitivity referred to 94 dB SPL
 *    and must come out the same whichever calibrator level you use. Measure at
 *    94 and at 114 (rebuilding with -D CALIBRATOR_DB=114.0 for the latter): if
 *    the two disagree, the coupling is not delivering the level the calibrator
 *    is set to, and nothing derived from it is usable. Only if they agree,
 *    copy the printed MIC_OFFSET_DB into the main firmware's build_flags.
 *    Do NOT compute it by hand as "94 - LAeq": that only holds if this sketch
 *    and the firmware share the same conversion, which is what the printed
 *    value already accounts for.
 * 5. Verify in free field against a reference sound level meter. A calibrator
 *    designed for a 1/2" capsule, coupled to a MEMS port in a small cavity,
 *    delivers more SPL than nominal, so step 4 alone can over-correct.
 *
 * Note on the two RMS figures: dBFS(A) is what the main firmware's log line
 * reports; dBFS(Z) is unweighted and includes low-frequency rumble that
 * A-weighting removes, so it reads higher in a real room. Use dBFS(A) when
 * comparing against the firmware.
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

// Peak-to-RMS correction. The datasheet specifies sensitivity on the PEAK of a
// sine (94 dB SPL "peaks at -26 dB below full scale") and what we feed in is an
// RMS amplitude; for a sine the two differ by 20*log10(sqrt(2)). The AOP
// confirms the peak reading: 120 dB SPL and -26 dBFS differ by exactly 26 dB,
// which only works if full scale is reached by the peak of a 120 dB sine.
// The main firmware applies this as MIC_PEAK_TO_RMS_DB; without it here, every
// level this sketch printed was 3.01 dB below what the firmware would publish.
#define MIC_PEAK_TO_RMS_DB 3.0103f

// Reference level the calibrator is SET TO. Standard acoustic calibrators
// offer 94 and 114 dB; pass -D CALIBRATOR_DB=114.0 when using the high
// setting, or the suggested trim comes out 20 dB wrong. It is printed on every
// line so the figure can never be read against the wrong reference.
#ifndef CALIBRATOR_DB
#define CALIBRATOR_DB 94.0f
#endif

// --- A-weighting filter, same coefficients as src/DSP_Engine.cpp ---
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
};

#if SAMPLE_RATE == 48000
// 48 kHz. Verified vs IEC 61672-1: |err| < 0.6 dB up to 8 kHz.
static Biquad aWeightingFilters[3] = {
    {0.23418304f, 0.46836609f, 0.23418304f, -0.22455846f, 0.01260663f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.89387049f, 0.89515977f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.99461446f, 0.99462171f, 0, 0}};
#else
// 16 kHz, third section refitted by least squares (|err| < 0.15 dB to 7.9 kHz).
// The previous set here kept the old third section, whose response collapsed
// above 5 kHz — fine at 1 kHz for a calibrator, wrong for anything broadband.
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
static float dc_offset = 0.0f;

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

  // Discard microphone startup and filter transient (~500 ms)
  uint32_t t0 = millis();
  size_t br;
  while (millis() - t0 < 500) {
    i2s_read(I2S_NUM_0, raw, sizeof(raw), &br, portMAX_DELAY);
  }
  Serial.println("[INIT] Capturing. One line per second:");
  Serial.printf("[INIT] Conversion: SPL = %.1f + 20*log10(rms) + %.1f + %.4f\n",
                MIC_REF_DB, -MIC_SENSITIVITY_DBFS, MIC_PEAK_TO_RMS_DB);
  Serial.printf("[INIT] Calibrator assumed at %.1f dB. Rebuild with "
                "-D CALIBRATOR_DB=<level> if yours is set differently.\n",
                (float)CALIBRATOR_DB);
  Serial.printf("[INIT] sens is this unit's sensitivity referred to 94 dB SPL; "
                "datasheet says %.1f dBFS.\n", MIC_SENSITIVITY_DBFS);
}

void loop() {
  double sum_sq_A = 0.0;
  double sum_sq_Z = 0.0;
  uint32_t count = 0;

  while (count < SAMPLE_RATE) {
    size_t br = 0;
    if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &br, portMAX_DELAY) != ESP_OK) continue;
    size_t n = br / sizeof(int32_t);

    for (size_t i = 0; i < n; i++) {
      float s = (float)(raw[i] >> 8) / 8388608.0f;
      dc_offset = (dc_offset * 0.9999f) + (s * 0.0001f);
      s -= dc_offset;

      sum_sq_Z += (double)(s * s);

      float f = s;
      for (int k = 0; k < 3; k++) f = applyFilter(f, aWeightingFilters[k]);
      sum_sq_A += (double)(f * f);
      count++;
    }
  }

  float rms_A = sqrtf((float)(sum_sq_A / count));
  float rms_Z = sqrtf((float)(sum_sq_Z / count));

  if (rms_A > 0.0f) {
    float dbfs_A = 20.0f * log10f(rms_A);
    float dbfs_Z = 20.0f * log10f(rms_Z);
    // Identical to the main firmware's i2s_fs_to_db(), minus the trims.
    float laeq = MIC_REF_DB + dbfs_A - MIC_SENSITIVITY_DBFS + MIC_PEAK_TO_RMS_DB;
    // What the firmware's build_flags would need, for the calibrator level
    // this build was told about. Only meaningful while it is actually coupled.
    float offset = (float)CALIBRATOR_DB - laeq;
    // This unit's sensitivity referred to 94 dB SPL, on the sine peak, so it
    // is directly comparable with the datasheet's -26 dBFS. Being referred to
    // a fixed level makes it LEVEL-INDEPENDENT for a linear chain: measure at
    // 94 and at 114 and it must come out the same. If it does not, the
    // coupling is not delivering the level the calibrator is set to, and no
    // trim derived from it is trustworthy -- go to a free-field comparison
    // against a reference meter instead.
    float sens = dbfs_A + MIC_PEAK_TO_RMS_DB - ((float)CALIBRATOR_DB - 94.0f);
    Serial.printf("LAeq: %.2f dB | dBFS(A): %.2f | dBFS(Z): %.2f | "
                  "sens: %.2f dBFS | @%.0fdB: MIC_OFFSET_DB=%.2f\n",
                  laeq, dbfs_A, dbfs_Z, sens, (float)CALIBRATOR_DB, offset);
  } else {
    Serial.println("[WARN] Absolute silence: check the SD line and L/R->GND");
  }
}