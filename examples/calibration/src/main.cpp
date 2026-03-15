/**
 * Firmware de calibración — Monitor de ruido (UNE-EN ISO 1996-2, Decreto 213/2012)
 *
 * Ejecuta únicamente la cadena de medida (ADC + ponderación A + RMS) y envía
 * por Serial el valor RMS (mV) y LAeq (dB) cada segundo. No usa I2C.
 *
 * Uso:
 * 1. Conectar MAX4466 OUT → GPIO 4, VCC 3.3V, GND.
 * 2. Ajustar potenciómetro del MAX4466 según docs/CALIBRACION.md.
 * 3. Flashear este firmware, abrir Monitor Serie a 115200 baud.
 * 4. Con calibrador a 94 dB (1 kHz), acoplar el micrófono y anotar RMS (mV) estable.
 * 5. Ese valor es CALIBRATION_RMS_MV en src/main.cpp del firmware principal.
 */

#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <Arduino.h>
#include <math.h>

// --- Configuración (misma que firmware principal) ---
#define ADC_CHANNEL ADC1_CHANNEL_4  // GPIO 4 — salida MAX4466
#define SAMPLE_RATE 16000
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)
#define REF_VOLTAGE 1100

// Constantes de calibración. Con el calibrador a 94 dB, ajustar CALIBRATION_RMS_MV
// al valor de RMS (mV) que se muestre estable; así LAeq mostrará ~94 dB.
#define CALIBRATION_DB 94.0f
#define CALIBRATION_RMS_MV 166.0f  // Sustituir por el valor medido en el paso 4

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

static esp_adc_cal_characteristics_t adc_chars;

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  CALIBRACION - Monitor de ruido");
  Serial.println("  ISO 1996-2 / Decreto 213/2012");
  Serial.println("========================================");
  Serial.println();
  Serial.println("Entrada: GPIO 4 (MAX4466 OUT)");
  Serial.println("Salida: RMS (mV) y LAeq (dB) cada 1 s");
  Serial.println();
  Serial.println("Pasos:");
  Serial.println("  1. Calibrador 94 dB @ 1 kHz, micrófono acoplado.");
  Serial.println("  2. Anotar el valor estable de RMS (mV).");
  Serial.println("  3. Copiar ese valor a CALIBRATION_RMS_MV en src/main.cpp");
  Serial.println("     del firmware principal.");
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

  unsigned long next_sample = micros();
  const unsigned long deadline = next_sample + 1000000UL;  // 1 s

  while (micros() < deadline) {
    if (micros() >= next_sample) {
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
    }
    yield();
  }

  if (samples_count > 0) {
    float mean_sq = (float)(sum_sq_A / (double)samples_count);
    uint32_t voltage_rms_mv =
        esp_adc_cal_raw_to_voltage((uint32_t)sqrtf(mean_sq), &adc_chars);

    float laeq = 0.0f;
    if (voltage_rms_mv > 0 && CALIBRATION_RMS_MV > 0.0f) {
      laeq = 20.0f * log10((float)voltage_rms_mv / CALIBRATION_RMS_MV) +
             CALIBRATION_DB;
    }

    Serial.printf("RMS: %lu mV  |  LAeq: %.1f dB(A)  (ref %.1f dB @ %.1f mV)\n",
                  (unsigned long)voltage_rms_mv, laeq, CALIBRATION_DB,
                  CALIBRATION_RMS_MV);
  }
}
