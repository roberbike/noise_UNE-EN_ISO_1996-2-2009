/**
 * Firmware de verificación — Nodo XIAO ESP32-S3 + ICS-43434 (I2S)
 * Monitor de ruido (UNE-EN ISO 1996-2, Decreto 213/2012)
 *
 * Ejecuta únicamente la cadena de medida (I2S + ponderación A + RMS) y envía
 * por Serial el nivel RMS (dBFS) y LAeq (dB) cada segundo. No usa I2C.
 *
 * A diferencia del MAX4466, el ICS-43434 tiene sensibilidad especificada de
 * fábrica (-26 dBFS @ 94 dB SPL), por lo que el LAeq mostrado ya debería ser
 * correcto (±1 dB típ. de tolerancia del micrófono) sin calibrador.
 *
 * Uso:
 * 1. Conectar ICS-43434: SCK → GPIO2 (D1), WS → GPIO3 (D2), SD → GPIO4 (D3),
 *    VDD 3.3V, GND, L/R → GND.
 * 2. Flashear este firmware, abrir Monitor Serie a 115200 baud.
 * 3. Verificación con calibrador a 94 dB (1 kHz): acoplar el micrófono y
 *    anotar el LAeq estable.
 * 4. Si difiere de 94.0, el ajuste fino es MIC_OFFSET_DB = 94.0 - LAeq_medido,
 *    a definir en build_flags del firmware principal.
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

// --- Configuración (misma que firmware principal) ---
#define MIC_BCLK 2
#define MIC_WS 3
#define MIC_DIN 4
#define SAMPLE_RATE 16000
#define READ_LEN 256

#define MIC_SENSITIVITY_DBFS (-26.0f) // ICS-43434: -26 dBFS @ 94 dB SPL
#define MIC_REF_DB 94.0f

// --- Filtro A (ponderación A, 16 kHz) ---
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

static int32_t raw[READ_LEN];
static float dc_offset = 0.0f;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[INIT] Verificacion ICS-43434 (I2S) - XIAO ESP32-S3");

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = READ_LEN;
  cfg.use_apll = false; // ESP32-S3 no tiene APLL

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = MIC_BCLK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_DIN;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("[ERR] Fallo al iniciar I2S");
    while (1) delay(1000);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  // Descartar arranque del micro y transitorio de filtros (~500 ms)
  uint32_t t0 = millis();
  size_t br;
  while (millis() - t0 < 500) {
    i2s_read(I2S_NUM_0, raw, sizeof(raw), &br, portMAX_DELAY);
  }
  Serial.println("[INIT] Capturando. LAeq y RMS(dBFS) cada segundo:");
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
    float laeq = MIC_REF_DB + 20.0f * log10f(rms_A) - MIC_SENSITIVITY_DBFS;
    float dbfs = 20.0f * log10f(rms_Z);
    Serial.printf("LAeq: %.1f dB | RMS: %.1f dBFS\n", laeq, dbfs);
  } else {
    Serial.println("[WARN] Silencio absoluto: revisar linea SD y L/R->GND");
  }
}
