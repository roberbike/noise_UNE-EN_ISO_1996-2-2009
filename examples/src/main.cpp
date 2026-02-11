#include <Arduino.h>
#include <Wire.h>

/**
 *  Master Device: Seeed Studio XIAO ESP32-S3
 *  Function: Requests noise level (LAeq,1s) from ESP32-C3 slave over I2C.
 *  Slave Address: 0x42
 */

#define SLAVE_ADDR 0x42
#define REQUEST_INTERVAL_MS 1000

// Protocol Commands
#define CMD_GET_DB      0x10
#define CMD_GET_STATUS  0x20
#define CMD_GET_RAW_MV  0x30

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("--- Xiao ESP32-S3 Master Noise Reader ---");
    Wire.begin(); 
    Serial.printf("I2C Initialized. Polling Slave 0x%02X...\n", SLAVE_ADDR);
}

void loop() {
    static unsigned long lastRequest = 0;
    
    if (millis() - lastRequest >= REQUEST_INTERVAL_MS) {
        lastRequest = millis();

        // 1. Check Status First
        Wire.beginTransmission(SLAVE_ADDR);
        Wire.write(CMD_GET_STATUS);
        Wire.endTransmission();
        
        delay(5); // Process time

        Wire.requestFrom(SLAVE_ADDR, 1);
        if (Wire.available()) {
             uint8_t status = Wire.read();
             if (status == 1) {
                 // 2. If OK, read dB
                 Wire.beginTransmission(SLAVE_ADDR);
                 Wire.write(CMD_GET_DB);
                 Wire.endTransmission();
                 
                 delay(5);
                 
                 Wire.requestFrom(SLAVE_ADDR, 4);
                 if (Wire.available() == 4) {
                     uint8_t buffer[4];
                     for(int i=0; i<4; i++) buffer[i] = Wire.read();
                     float noiseLevel;
                     memcpy(&noiseLevel, buffer, 4);
                     Serial.printf("Status: OK | Trovato: %.2f dB\n", noiseLevel);
                 }
             } else {
                 Serial.println("Status: ERROR (Check Mic Connection)");
             }
        } else {
             Serial.println("Error: Slave Not Responding");
        }
    }
}
