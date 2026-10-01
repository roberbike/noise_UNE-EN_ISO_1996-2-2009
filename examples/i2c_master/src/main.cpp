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
#include <Wire.h>

/**
 * Master device: XIAO ESP32-S3 / Lolin S2 Mini
 * Function: requests the full SensorData from the ESP32-C3 slave over I2C.
 * Slave address: 0x08
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

// Protocol commands (keep in sync with the slave firmware)
#define CMD_GET_STATUS 0x20
#define CMD_GET_STATUS_LEGACY 0x00
#define CMD_GET_DATA 0x01
#define CMD_GET_METADATA 0x50
#define CMD_SET_CALIB 0x0A
#define CMD_SET_TIME  0x09   // epoch (uint32 LE) — see setNodeTime()

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
  // Appended in 3.3.1. In 3.3.0 these two sat BEFORE lowNoiseLevel, which
  // shifted everything after it by 8 bytes relative to the 3.2.x layout that
  // CanAirIO and other existing masters still use. Keep them last.
  float noiseLASmaxDb;
  float noiseLCpeakDb;
};

// Reference helper: push a persistent calibration offset (dB) to the node.
// The node stores it in NVS (survives reboots) and applies it to every level.
// Typical use: with a physical calibrator emitting 94.0 dB, if the node reads
// 96.5 dB, send calibrateNode(-2.5). Call once, not in the polling loop.
void calibrateNode(float offset_db) {
  int16_t raw = (int16_t)lroundf(offset_db * 100.0f); // hundredths of dB
  Wire.beginTransmission(SLAVE_ADDR);
  Wire.write(CMD_SET_CALIB);
  Wire.write((uint8_t)(raw & 0xFF));
  Wire.write((uint8_t)((raw >> 8) & 0xFF));
  Wire.endTransmission();
}

// #B10 Push the wall clock to the node. Without this the node never sets
// time_synced, so Ld/Le/Ln/Lden stay at 0 forever — with the stock example
// that was exactly what happened.
//
// IMPORTANT: send LOCAL epoch, not UTC. The node applies no timezone of its
// own, so the day (07-19 h), evening (19-23 h) and night (23-07 h) bands are
// read straight off whatever you send. In Spain that means UTC + 1 h in
// winter and UTC + 2 h in summer; get it wrong and every period index is
// shifted by an hour. Call it once after the node answers, and again after
// any DST change or clock resync.
void setNodeTime(uint32_t local_epoch) {
  Wire.beginTransmission(SLAVE_ADDR);
  Wire.write(CMD_SET_TIME);
  Wire.write((uint8_t)(local_epoch & 0xFF));
  Wire.write((uint8_t)((local_epoch >> 8) & 0xFF));
  Wire.write((uint8_t)((local_epoch >> 16) & 0xFF));
  Wire.write((uint8_t)((local_epoch >> 24) & 0xFF));
  Wire.endTransmission();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- Master - Sensor Compat Test ---");
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(100); // 100 ms hardware timeout to prevent master-side lockups
  Serial.printf("I2C Initialized (SDA=%d, SCL=%d). Polling Slave 0x%02X...\n",
                I2C_SDA, I2C_SCL, SLAVE_ADDR);

  // #B10 Set the node clock. Replace with your real local epoch (NTP + your
  // timezone offset, or an RTC already holding local time). The node needs
  // this before it can produce Ld/Le/Ln/Lden; metadata byte 4 reports whether
  // it took effect (time_synced).
  // setNodeTime(local_epoch_from_your_time_source());
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

      // --- Triple freshness validation (see docs/COMUNICACION.md) ---
      // Complete read alone is NOT enough: the ESP32 I2C slave HAL may pad a
      // short reply to full length. Require status==1 AND cycles advancing;
      // otherwise the node is stalled/booting and the struct is stale.
      static uint32_t last_cycles = 0;
      static bool have_last = false;
      bool fresh = (data.cycles != last_cycles) || !have_last;
      bool publishable = (status == 1) && fresh;
      last_cycles = data.cycles;
      have_last = true;

      Serial.println("--- Sensor Data ---");
      Serial.printf("Status: %s | %s\n",
                    (status == 1 ? "MIC OK" : "MIC ERROR"),
                    (publishable ? "PUBLISH" : "SKIP (stale/not ready)"));
      Serial.printf("LAeq (1s): %.2f dB\n", data.noiseAvgDb);
      Serial.printf("LAFmax (1s): %.2f dB\n", data.noisePeakDb);
      Serial.printf("LASmax (1s): %.2f dB\n", data.noiseLASmaxDb);
      Serial.printf("LCpeak: %.2f dB\n", data.noiseLCpeakDb);
      Serial.printf("L10 (Legal): %.2f dB\n", data.noiseAvgLegalDb);
      Serial.printf("L90 (Backg): %u\n", data.lowNoiseLevel);
      Serial.printf("Lden (24h): %.2f dB\n", data.noiseLden);
      Serial.printf("Raw: %u\n", data.noise);
      Serial.printf("Cycles: %u\n", data.cycles);
      Serial.println("-------------------");

      // Only forward to the cloud/InfluxDB when publishable. A stalled node
      // (frozen cycles) or a not-ready node (status 0) is skipped, never
      // republished — this is what prevents the flat lines in Grafana.
      if (publishable) {
        // publishToCloud(data);   // integrate here
      }

      // #12: optional metadata read (fw version, node type, time-sync, clips,
      // NVS calibration offset). Metadata is 9 bytes as of the calibration
      // feature; older nodes returned 7.
      Wire.beginTransmission(SLAVE_ADDR);
      Wire.write(CMD_GET_METADATA);
      if (Wire.endTransmission() == 0) {
        delay(5);
        if (Wire.requestFrom((uint16_t)SLAVE_ADDR, (size_t)9) == 9) {
          uint8_t m[9];
          for (int i = 0; i < 9; i++) m[i] = Wire.read();
          uint16_t clips = (uint16_t)m[5] | ((uint16_t)m[6] << 8);
          int16_t calib = (int16_t)((uint16_t)m[7] | ((uint16_t)m[8] << 8));
          Serial.printf("Meta: fw %u.%u.%u | node=%s | time_synced=%u | clips=%u | calib=%.2f dB\n",
                        m[0], m[1], m[2],
                        (m[3] == 0x02 ? "I2S" : (m[3] == 0x01 ? "ADC" : "?")),
                        m[4], clips, calib / 100.0f);
        }
      }
    } else {
      Serial.printf("Error: Incomplete Data. Expected %u, got %d\n",
                    (unsigned int)sizeToRead, Wire.available());
      while (Wire.available()) {
        Wire.read();
      }
    }
  }
}

