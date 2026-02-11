#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

/**
 * --- ESP32-C3 NOISE MONITOR (I2C SLAVE) ---
 *
 * Target: ESP32-C3 (Lolin/SuperMini)
 * Sensor: MAX4466 Electret Microphone on GPIO 4
 * Protocol: I2C Slave at 0x42
 */

// --- CONFIGURATION ---
#define I2C_SLAVE_ADDR 0x42
#define I2C_SDA 8
#define I2C_SCL 10

#define ADC_CHANNEL ADC1_CHANNEL_4 // GPIO 4
#define SAMPLE_RATE 10000          // 10kHz sampling
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)

// Calibration constants
#define CALIBRATION_DB 94.0f      // dB produced by the acoustic calibrator
#define CALIBRATION_RMS_MV 155.0f // Vrms measured while calibrator is applied
#define REF_VOLTAGE 1100           // Internal ADC reference (mV)

// Protocol Commands
#define CMD_GET_DB 0x10     // Returns 4-byte Float (LAeq,1s)
#define CMD_GET_STATUS 0x20 // Returns 1-byte Status (0=ERR, 1=OK)
#define CMD_GET_RAW_MV 0x30 // Returns 4-byte Uint32 (Last RMS mV)
#define CMD_GET_LMAX 0x40   // Returns 4-byte Float (Lmax,1s)

// --- GLOBAL VARIABLES ---
volatile float LAeq_1s = 0.0f;
volatile float Lmax_1s = 0.0f;
volatile uint32_t last_rms_mv = 0;
volatile bool mic_connected = false;
volatile uint8_t i2c_active_command = CMD_GET_DB;

esp_adc_cal_characteristics_t adc_chars;
TaskHandle_t inputTaskHandle = NULL;

// --- LOGGING ---
void SerialLog(const char *level, const char *msg) {
  Serial.printf("[%s] %s\n", level, msg);
}

/**
 * Simple diagnostic to check if the microphone bias voltage is correct.
 * MAX4466 should be centered around VCC/2.
 */
bool check_microphone_connection(uint32_t bias_mv) {
  return (bias_mv > 1000 && bias_mv < 2400); // Valid range for 3.3V supply
}

/**
 * ADC Processing Task
 * Captures samples and calculates RMS + Lmax every second.
 */
void adc_task(void *pvParameters) {
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_12);

  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12,
                           REF_VOLTAGE, &adc_chars);

  unsigned long next_sample_time = micros();
  double sum_squares = 0.0;
  float current_max_sample = 0.0f;
  int samples_count = 0;
  float dc_offset = 0.0f;
  bool first_run = true;

  SerialLog("SYS", "ADC Engine started at 10kHz");

  while (true) {
    if (micros() >= next_sample_time) {
      next_sample_time += SAMPLE_PERIOD_US;

      uint32_t raw = adc1_get_raw(ADC_CHANNEL);

      // Recursive DC offset calculation (Bias removal)
      if (first_run) {
        dc_offset = (float)raw;
        first_run = false;
      } else {
        dc_offset = (dc_offset * 0.999f) + ((float)raw * 0.001f);
      }

      // Connectivity check on every cycle start
      if (samples_count == 0) {
        uint32_t bias_mv =
            esp_adc_cal_raw_to_voltage((uint32_t)dc_offset, &adc_chars);
        mic_connected = check_microphone_connection(bias_mv);
      }

      float sample = fabs((float)raw - dc_offset);

      // Track peak for Lmax
      if (sample > current_max_sample)
        current_max_sample = sample;

      sum_squares += (sample * sample);
      samples_count++;

      // Once per second: Process results
      if (samples_count >= SAMPLE_RATE) {
        float mean_sq = (float)(sum_squares / samples_count);
        float rms_raw = sqrtf(mean_sq);
        uint32_t voltage_rms =
            esp_adc_cal_raw_to_voltage((uint32_t)rms_raw, &adc_chars);
        uint32_t voltage_max = esp_adc_cal_raw_to_voltage(
            (uint32_t)current_max_sample, &adc_chars);

        last_rms_mv = voltage_rms;

        if (voltage_rms > 0 && CALIBRATION_RMS_MV > 0.0f) {
          LAeq_1s = 20.0f * log10((float)voltage_rms / CALIBRATION_RMS_MV) +
                    CALIBRATION_DB;
        } else {
          LAeq_1s = 0.0f;
        }

        if (voltage_max > 0 && CALIBRATION_RMS_MV > 0.0f) {
          Lmax_1s = 20.0f * log10((float)voltage_max / CALIBRATION_RMS_MV) +
                    CALIBRATION_DB;
        } else {
          Lmax_1s = 0.0f;
        }

        if (mic_connected) {
          Serial.printf("[DATA] LAeq: %.2f | Lmax: %.2f | RMS: %d mV\n",
                        LAeq_1s, Lmax_1s, voltage_rms);
        } else {
          SerialLog("ERROR", "Microphone Disconnected (Bias Invalid)!");
        }

        sum_squares = 0.0;
        current_max_sample = 0.0f;
        samples_count = 0;

        vTaskDelay(pdMS_TO_TICKS(1)); // Allow other tasks (like I2C) to run
        next_sample_time = micros();  // Sync clock
      }
    }
    yield(); // Non-blocking cooperation
  }
}

// --- I2C INTERRUPT HANDLERS ---

void receiveEvent(int bytes) {
  if (Wire.available() > 0) {
    uint8_t cmd = Wire.read();
    if (cmd == CMD_GET_DB || cmd == CMD_GET_STATUS || cmd == CMD_GET_RAW_MV ||
        cmd == CMD_GET_LMAX) {
      i2c_active_command = cmd;
    }
    while (Wire.available())
      Wire.read(); // Flush
  }
}

void requestEvent() {
  if (i2c_active_command == CMD_GET_STATUS) {
    uint8_t status = mic_connected ? 1 : 0;
    Wire.write(&status, 1);
  } else if (i2c_active_command == CMD_GET_RAW_MV) {
    Wire.write((uint8_t *)&last_rms_mv, 4);
  } else if (i2c_active_command == CMD_GET_LMAX) {
    Wire.write((uint8_t *)&Lmax_1s, 4);
  } else {
    Wire.write((uint8_t *)&LAeq_1s, 4);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  SerialLog("INIT", "ESP32-C3 Noise Meter starting...");

  // I2C Config (Join as slave 0x42)
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  if (Wire.begin(I2C_SLAVE_ADDR, I2C_SDA, I2C_SCL)) {
    Wire.setClock(100000); // 100kHz standard
    Serial.printf("[INIT] I2C Slave at 0x%02X (SDA:%d, SCL:%d)\n",
                  I2C_SLAVE_ADDR, I2C_SDA, I2C_SCL);
  } else {
    SerialLog("ERROR", "Failed to start I2C Slave");
  }

  // Launch DSP task on Core 0
  xTaskCreate(adc_task, "ADC_DSP", 4096, NULL, 5, &inputTaskHandle);
}

void loop() {
  // Empty loop - Logic is in Tasks and Interrupts
  delay(5000);
}
