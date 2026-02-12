#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "sys/time.h"
#include "time.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

/**
 * --- ESP32-C3 PROFESSIONAL NOISE MONITOR ---
 * Compliant with (orientative) requirements of Decree 213/2012 & UNE-ISO
 * 1996-2.
 */

// --- CONFIGURATION ---
#define I2C_ADDR_SLAVE 0x08 // Default for SensorLib
#define I2C_SDA 8
#define I2C_SCL 10

#define ADC_CHANNEL ADC1_CHANNEL_4 // GPIO 4
#define SAMPLE_RATE 22050          // High quality sampling
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)

// Calibration constants
#define CALIBRATION_DB 52.5f      // Target dB (Calibrator)
#define CALIBRATION_RMS_MV 168.0f // Measured RMS mV at 94dB
#define REF_VOLTAGE 1100          // ADC Ref (mV)

// --- DSP STRUCTURES ---

struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
};

// A-Weighting Filter (Cascaded Biquads for 22050 Hz)
Biquad aWeightingFilters[3] = {
    {0.5367f, -1.0734f, 0.5367f, -1.1892f, 0.3165f, 0, 0},
    {1.0000f, 2.0000f, 1.0000f, -1.5434f, 0.5891f, 0, 0},
    {1.0000f, 0.0000f, -1.0000f, -1.9772f, 0.9773f, 0, 0}};

float applyFilter(float in, Biquad &f) {
  float out = in * f.b0 + f.z1;
  f.z1 = in * f.b1 - f.a1 * out + f.z2;
  f.z2 = in * f.b2 - f.a2 * out;
  return out;
}

// Protocol Commands (Sync with SensorLib/NoiseSlave.h)
#define CMD_GET_STATUS 0x00
#define CMD_GET_DATA 0x01
#define CMD_RESET 0x08
#define CMD_IDENTIFY 0x09

// Mapping for secondary/Legacy commands
#define CMD_LEGACY_GET_DB 0x10
#define CMD_LEGACY_GET_L10 0x60
#define CMD_LEGACY_GET_L90 0x70

// SensorData struct according to SensorLib
struct SensorData {
  uint32_t noise;           // Current noise level (mV)
  float noiseAvg;           // Average (mV)
  float noiseAvgDb;         // Average (dB)
  float noisePeak;          // Peak (mV)
  float noisePeakDb;        // Peak (dB)
  float noiseMin;           // Minimum (mV)
  float noiseMinDb;         // Minimum (dB)
  float noiseAvgLegal;      // Legal average (mV)
  float noiseAvgLegalDb;    // Legal average (dB)
  float noiseAvgLegalMax;   // Legal max average (mV)
  float noiseAvgLegalMaxDb; // Legal max average (dB)
  uint16_t lowNoiseLevel;   // Dynamic base noise level (used for L90)
  uint32_t cycles;          // Number of cycles completed
  float Ld;                 // Day index
  float Le;                 // Evening index
  float Ln;                 // Night index
  float noiseLden;          // Global day-evening-night index
};

volatile SensorData globalSensorData = {0,    0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0,
                                        0,    0.0f, 0.0f, 0.0f, 0.0f};

// --- GLOBAL VARIABLES ---
volatile float LAeq_1s = 0.0f;
volatile float LAFmax_1s = 0.0f;
volatile float LASmax_1s = 0.0f;
volatile float L10_1s = 0.0f;
volatile float L90_1s = 0.0f;
volatile uint32_t last_rms_mv = 0;
volatile bool mic_connected = false;
volatile uint8_t i2c_active_command = CMD_GET_STATUS;

// --- NOISE PERIOD STATISTICS (DECRETO 213/2012) ---
struct PeriodStats {
  double energySum;
  uint32_t count;
  void add(float db) {
    if (db > 10.0f) { // Ignore silence/noise floor
      energySum += pow(10.0, (double)db / 10.0);
      count++;
    }
  }
  float getAvg() {
    if (count == 0)
      return 0.0f;
    return (float)(10.0 * log10(energySum / (double)count));
  }
  void reset() {
    energySum = 0;
    count = 0;
  }
};

PeriodStats statsDay = {0, 0};
PeriodStats statsEvening = {0, 0};
PeriodStats statsNight = {0, 0};

#define STAT_SAMPLES 20
float stat_buffer[STAT_SAMPLES];
int stat_idx = 0;

esp_adc_cal_characteristics_t adc_chars;

// --- LOGGING ---
void SerialLog(const char *level, const char *msg) {
  Serial.printf("[%s] %s\n", level, msg);
}

bool check_microphone_connection(uint32_t bias_mv) {
  return (bias_mv > 800 && bias_mv < 2600); // 3.3V bias check
}

/**
 * Main DSP Task
 */
void adc_task(void *pvParameters) {
// Configuración específica para ESP32-S2 (13 bits ADC)
#if defined(ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S2)
  // ADC_WIDTH_BIT_13 es el nativo del S2
  adc1_config_width(ADC_WIDTH_BIT_13);
  // Atenuación 11dB para rango completo (hasta ~2.6V o 3.3V según ref)
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_13,
                           REF_VOLTAGE, &adc_chars);
#else
  // Configuración estándar para ESP32 / C3 (12 bits ADC)
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_12);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12,
                           REF_VOLTAGE, &adc_chars);
#endif

  unsigned long next_sample_time = micros();

  double sum_sq_A = 0.0;
  float fast_ema_sq = 0.0f;
  float slow_ema_sq = 0.0f;
  float max_fast_sq = 0.0f;
  float max_slow_sq = 0.0f;

  // EMA Alpha coefficients for Time Weighting (at 22050 Hz)
  // Alpha = 1 - exp(-1 / (sample_rate * time_constant))
  const float alpha_fast = 0.000362f; // Fast = 125ms
  const float alpha_slow = 0.000045f; // Slow = 1s

  int samples_count = 0;
  int sub_sample_trigger = SAMPLE_RATE / STAT_SAMPLES;
  float dc_offset = 2048.0f; // Approx midway

  SerialLog("SYS", "Smart City DSP Engine V2.1 (Clase 2 Support)");

  while (true) {
    if (micros() >= next_sample_time) {
      next_sample_time += SAMPLE_PERIOD_US;

      uint32_t raw = adc1_get_raw(ADC_CHANNEL);

      // DC Filter (Bias removal)
      dc_offset = (dc_offset * 0.9999f) + ((float)raw * 0.0001f);
      float signal = (float)raw - dc_offset;

      // 1. Apply A-Weighting Filter
      float filtered = signal;
      for (int i = 0; i < 3; i++) {
        filtered = applyFilter(filtered, aWeightingFilters[i]);
      }

      // 2. Integration for Leq
      float sq = filtered * filtered;
      sum_sq_A += sq;

      // 3. Time Weighting (Exponential Moving Average)
      fast_ema_sq = (sq * alpha_fast) + (fast_ema_sq * (1.0f - alpha_fast));
      slow_ema_sq = (sq * alpha_slow) + (slow_ema_sq * (1.0f - alpha_slow));

      if (fast_ema_sq > max_fast_sq)
        max_fast_sq = fast_ema_sq;
      if (slow_ema_sq > max_slow_sq)
        max_slow_sq = slow_ema_sq;

      // Capture statistical sub-samples for L10/L90
      if (samples_count % sub_sample_trigger == 0 && stat_idx < STAT_SAMPLES) {
        // Rough estimate of current dB using fast EMA for statistical buffer
        float current_db = 20.0f * log10(sqrtf(fast_ema_sq + 0.00001f) /
                                         (CALIBRATION_RMS_MV / 10.0f)) +
                           CALIBRATION_DB;
        stat_buffer[stat_idx++] = current_db;
      }

      samples_count++;

      // Process once per second
      if (samples_count >= SAMPLE_RATE) {
        float mean_sq_A = (float)(sum_sq_A / samples_count);
        uint32_t voltage_rms_A =
            esp_adc_cal_raw_to_voltage((uint32_t)sqrtf(mean_sq_A), &adc_chars);
        uint32_t voltage_fast_max = esp_adc_cal_raw_to_voltage(
            (uint32_t)sqrtf(max_fast_sq), &adc_chars);
        uint32_t voltage_slow_max = esp_adc_cal_raw_to_voltage(
            (uint32_t)sqrtf(max_slow_sq), &adc_chars);

        last_rms_mv = voltage_rms_A;

        if (voltage_rms_A > 0 && CALIBRATION_RMS_MV > 0.0f) {
          LAeq_1s = 20.0f * log10((float)voltage_rms_A / CALIBRATION_RMS_MV) +
                    CALIBRATION_DB;
          LAFmax_1s =
              20.0f * log10((float)voltage_fast_max / CALIBRATION_RMS_MV) +
              CALIBRATION_DB;
          LASmax_1s =
              20.0f * log10((float)voltage_slow_max / CALIBRATION_RMS_MV) +
              CALIBRATION_DB;

          // Process L10/L90
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
            L10_1s = stat_buffer[STAT_SAMPLES / 10];
            L90_1s = stat_buffer[STAT_SAMPLES * 9 / 10];
          }

          // Fill SensorLib Structure
          globalSensorData.noise = voltage_rms_A;
          globalSensorData.noiseAvg = (float)voltage_rms_A;
          globalSensorData.noiseAvgDb = LAeq_1s; // Main metric
          globalSensorData.noisePeak = (float)voltage_fast_max;
          globalSensorData.noisePeakDb = LAFmax_1s;
          globalSensorData.noiseAvgLegal =
              L10_1s; // Mapping L10 to legal average
          globalSensorData.noiseAvgLegalDb = L10_1s;
          globalSensorData.lowNoiseLevel = (uint16_t)L90_1s;
          globalSensorData.cycles++;

          // --- LONG TERM PERIODS (DECRETO 213/2012) ---
          struct tm timeinfo;
          if (getLocalTime(&timeinfo)) {
            int h = timeinfo.tm_hour;
            if (h >= 7 && h < 19)
              statsDay.add(LAeq_1s);
            else if (h >= 19 && h < 23)
              statsEvening.add(LAeq_1s);
            else
              statsNight.add(LAeq_1s);

            globalSensorData.Ld = statsDay.getAvg();
            globalSensorData.Le = statsEvening.getAvg();
            globalSensorData.Ln = statsNight.getAvg();

            // Lden calculation
            double l_d = (double)globalSensorData.Ld;
            double l_e = (double)globalSensorData.Le;
            double l_n = (double)globalSensorData.Ln;

            if (l_d > 0 || l_e > 0 || l_n > 0) {
              double lden_energy = (12.0 * pow(10.0, l_d / 10.0) +
                                    4.0 * pow(10.0, (l_e + 5.0) / 10.0) +
                                    8.0 * pow(10.0, (l_n + 10.0) / 10.0)) /
                                   24.0;
              globalSensorData.noiseLden = (float)(10.0 * log10(lden_energy));
            }

            // Daily reset at midnight
            static int last_day = -1;
            if (last_day != -1 && last_day != timeinfo.tm_mday) {
              statsDay.reset();
              statsEvening.reset();
              statsNight.reset();
            }
            last_day = timeinfo.tm_mday;
          }

        } else {
          LAeq_1s = LAFmax_1s = LASmax_1s = L10_1s = L90_1s = 0.0f;
        }

        // Connectivity check
        uint32_t bias_mv =
            esp_adc_cal_raw_to_voltage((uint32_t)dc_offset, &adc_chars);
        mic_connected = check_microphone_connection(bias_mv);

        if (mic_connected) {
          Serial.printf("[SMART] LAeq:%.1f | LAFmx:%.1f | L10:%.1f | L90:%.1f "
                        "| RMS:%dmV\n",
                        LAeq_1s, LAFmax_1s, L10_1s, L90_1s, voltage_rms_A);
        } else {
          SerialLog("WARN", "Microphone range error (check wiring)");
        }

        // Reset counters
        sum_sq_A = 0;
        max_fast_sq = 0;
        max_slow_sq = 0;
        samples_count = 0;
        stat_idx = 0;

        vTaskDelay(pdMS_TO_TICKS(1));
        next_sample_time = micros();
      }
    }
    yield();
  }
}

// --- I2C SLAVE ---
void receiveEvent(int bytes) {
  if (Wire.available() > 0) {
    uint8_t cmd = Wire.read();
    i2c_active_command = cmd;

    // CMD_SET_TIME (0x09) support: expects 4-byte Unix Timestamp
    if (cmd == CMD_IDENTIFY && bytes == 5) {
      uint32_t timestamp = 0;
      uint8_t *p = (uint8_t *)&timestamp;
      for (int i = 0; i < 4 && Wire.available(); i++)
        p[i] = Wire.read();

      struct timeval tv = {(long)timestamp, 0};
      settimeofday(&tv, NULL);
      Serial.printf("[I2C] Time synced to %lu\n", timestamp);
    }

    while (Wire.available())
      Wire.read(); // Flush
  }
}

void requestEvent() {
  switch (i2c_active_command) {
  case CMD_GET_STATUS: {
    uint8_t s = mic_connected ? 1 : 0;
    Wire.write(&s, 1);
  } break;
  case CMD_GET_DATA:
    Wire.write((uint8_t *)&globalSensorData, sizeof(SensorData));
    break;
  case CMD_IDENTIFY: {
    uint8_t id[5] = {0x01, 0x02, 0x01, 0x01, I2C_ADDR_SLAVE};
    Wire.write(id, 5);
  } break;
  case CMD_LEGACY_GET_DB:
    Wire.write((uint8_t *)&LAeq_1s, 4);
    break;
  case 0x30: // Legacy RAW MV
    Wire.write((uint8_t *)&last_rms_mv, 4);
    break;
  case 0x40: // Legacy Lmax
    Wire.write((uint8_t *)&LAFmax_1s, 4);
    break;
  case CMD_LEGACY_GET_L10:
    Wire.write((uint8_t *)&L10_1s, 4);
    break;
  case CMD_LEGACY_GET_L90:
    Wire.write((uint8_t *)&L90_1s, 4);
    break;
  default:
    Wire.write(0);
    break;
  }
}

// ... (código existente)

// Renombrando para evitar conflictos si se usa como librería
// Renombrando para evitar conflictos si se usa como librería

void ruido_setup() {
  Serial.begin(115200);
  delay(1000);
  SerialLog("INIT", "Smart City Noise Sensor (Clase 2 Ready)");

  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  // Initialize I2C as Slave at default address 0x08
  Wire.begin((uint8_t)I2C_ADDR_SLAVE, I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  // Create ADC/DSP task on a dedicated core (ESP32-C3 has one core, but
  // FreeRTOS handles scheduling)
  xTaskCreate(adc_task, "ADC_DSP", 8192, NULL, 5, NULL);
}

void ruido_loop() { delay(10000); }

// Si se compila como firmware independiente, se puede descomentar esto o usar
// flags
void setup() { ruido_setup(); }
void loop() { ruido_loop(); }
