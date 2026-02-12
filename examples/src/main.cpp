#include <Arduino.h>
#include <Wire.h>

/**
 *  Master Device: Wemos Lolin S2 Mini (ESP32-S2)
 *  Function: Requests full SensorData from ESP32-C3 slave over I2C.
 *  Slave Address: 0x08
 *  Compatibility: CanAirIO SensorLib
 */

#define SLAVE_ADDR 0x08
#define REQUEST_INTERVAL_MS 1000

// I2C Pins for ESP32-S2 Mini
#define I2C_SDA 8
#define I2C_SCL 9

// Protocol Commands (Sync with src/main.cpp and SensorLib)
#define CMD_GET_STATUS 0x00
#define CMD_GET_DATA 0x01
#define CMD_GET_DB 0x10
#define CMD_GET_RAW_MV 0x30

// Full Data Structure (Must match firmware)
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

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- Wemos S2 Mini Master - SensorLib Compat Test ---");

  // Initialize I2C with specific pins
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.printf("I2C Initialized (SDA=%d, SCL=%d). Polling Slave 0x%02X...\n",
                I2C_SDA, I2C_SCL, SLAVE_ADDR);
}

void loop() {
  static unsigned long lastRequest = 0;

  if (millis() - lastRequest >= REQUEST_INTERVAL_MS) {
    lastRequest = millis();

    // 1. Check Status
    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write(CMD_GET_STATUS);
    uint8_t error = Wire.endTransmission();

    if (error != 0) {
      Serial.printf("I2C Connection Error: %d\n", error);
      return;
    }

    delay(10);

    Wire.requestFrom((uint16_t)SLAVE_ADDR, (uint8_t)1);
    if (Wire.available()) {
      uint8_t status = Wire.read();

      // 2. Request Full Data (SensorLib mode)
      Wire.beginTransmission(SLAVE_ADDR);
      Wire.write(CMD_GET_DATA);
      Wire.endTransmission();

      delay(20); // Give slave time to prepare struct

      size_t sizeToRead = sizeof(SensorData);
      Wire.requestFrom((uint16_t)SLAVE_ADDR, (size_t)sizeToRead);

      if (Wire.available() == sizeToRead) {
        SensorData data;
        uint8_t *p = (uint8_t *)&data;
        for (size_t i = 0; i < sizeToRead; i++) {
          p[i] = Wire.read();
        }

        // Display Results
        Serial.println("--- Sensor Data ---");
        Serial.printf("Status: %s\n", (status == 1 ? "MIC OK" : "MIC ERROR"));
        Serial.printf("LAeq (1s): %.2f dB\n", data.noiseAvgDb);
        Serial.printf("Lmax (1s): %.2f dB\n", data.noisePeakDb);
        Serial.printf("L10 (Legal): %.2f dB\n", data.noiseAvgLegalDb);
        Serial.printf("L90 (Backg): %u\n", data.lowNoiseLevel);
        Serial.printf("Lden (24h): %.2f dB\n", data.noiseLden);
        Serial.printf("Raw Voltage: %u mV\n", data.noise);
        Serial.printf("Cycles: %u\n", data.cycles);
        Serial.println("-------------------");

      } else {
        Serial.printf("Error: Incomplete Data. Expected %d, got %d\n",
                      sizeToRead, Wire.available());
        // Flush buffer
        while (Wire.available())
          Wire.read();
      }
    } else {
      Serial.println("Error: Slave Not Responding to Status Request");
    }
  }
}
