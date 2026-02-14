#include <Arduino.h>
#include <Wire.h>

/**
 * Master Device: XIAO ESP32-S3 / Lolin S2 Mini
 * Function: Requests full SensorData from ESP32-C3 slave over I2C.
 * Slave Address: 0x08
 */

#define SLAVE_ADDR 0x08
#define REQUEST_INTERVAL_MS 5000

#if defined(CONFIG_IDF_TARGET_ESP32S2)
#define I2C_SDA 8
#define I2C_SCL 9
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define I2C_SDA 5
#define I2C_SCL 6
#else
#define I2C_SDA 5
#define I2C_SCL 6
#endif

// Protocol commands (sync with slave firmware)
#define CMD_GET_STATUS 0x20
#define CMD_GET_STATUS_LEGACY 0x00
#define CMD_GET_DATA 0x01

struct SensorData {
  uint32_t noise;
  float noiseAvg;
  float noiseAvgDb;
  float noisePeak;
  float noisePeakDb;
  float noiseMin;
  float noiseMinDb;
  float noiseAvgLegal;
  float noiseAvgLegalDb;
  float noiseAvgLegalMax;
  float noiseAvgLegalMaxDb;
  uint16_t lowNoiseLevel;
  uint32_t cycles;
  float Ld;
  float Le;
  float Ln;
  float noiseLden;
};

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- Master - Sensor Compat Test ---");
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("I2C Initialized (SDA=%d, SCL=%d). Polling Slave 0x%02X...\n",
                I2C_SDA, I2C_SCL, SLAVE_ADDR);
}

void loop() {
  static unsigned long lastRequest = 0;

  if (millis() - lastRequest >= REQUEST_INTERVAL_MS) {
    lastRequest = millis();

    uint8_t error = 0;
    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write(CMD_GET_STATUS);
    error = Wire.endTransmission();
    if (error != 0) {
      // Backward compatibility with legacy slave firmware.
      Wire.beginTransmission(SLAVE_ADDR);
      Wire.write(CMD_GET_STATUS_LEGACY);
      error = Wire.endTransmission();
    }
    if (error != 0) {
      Serial.printf("I2C Connection Error: %d\n", error);
      return;
    }

    delay(10);
    int received = Wire.requestFrom((uint16_t)SLAVE_ADDR, (uint8_t)1);
    if (received != 1 || !Wire.available()) {
      Serial.println("Error: Slave Not Responding to Status Request");
      return;
    }

    uint8_t status = Wire.read();

    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write(CMD_GET_DATA);
    if (Wire.endTransmission() != 0) {
      Serial.println("Error: CMD_GET_DATA transmission failed");
      return;
    }

    delay(20);
    const size_t sizeToRead = sizeof(SensorData);
    received = Wire.requestFrom((uint16_t)SLAVE_ADDR, (size_t)sizeToRead);

    if (received == (int)sizeToRead && Wire.available() == (int)sizeToRead) {
      SensorData data;
      uint8_t *p = (uint8_t *)&data;
      for (size_t i = 0; i < sizeToRead; i++) {
        p[i] = Wire.read();
      }

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
      Serial.printf("Error: Incomplete Data. Expected %u, got %d\n",
                    (unsigned int)sizeToRead, Wire.available());
      while (Wire.available()) {
        Wire.read();
      }
    }
  }
}

