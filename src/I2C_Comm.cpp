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

#include "I2C_Comm.h"
#include <sys/time.h>
#include <freertos/queue.h>

QueueHandle_t dataQueue = NULL;
SensorData cachedSensorData = {0};
uint8_t cachedMicOk = 0;

volatile uint8_t i2c_active_command = CMD_GET_STATUS;

static inline void update_i2c_command(uint8_t cmd) {
    i2c_active_command = cmd;
}

static inline uint8_t read_i2c_command() {
    return i2c_active_command;
}

// --- I2C SLAVE EVENT HANDLERS ---
void receiveEvent(int bytes) {
    if (Wire.available() <= 0) {
        return;
    }

    uint8_t cmd = Wire.read();
    if (cmd == CMD_GET_STATUS_LEGACY) {
        cmd = CMD_GET_STATUS;
    }
    update_i2c_command(cmd);

    // Compatibility path: set Unix epoch via 0x09 + 4 bytes.
    if (cmd == CMD_SET_TIME_LEGACY && bytes == 5) {
        uint32_t timestamp = 0;
        uint8_t *p = (uint8_t *)&timestamp;
        for (int i = 0; i < 4 && Wire.available(); i++) {
            p[i] = Wire.read();
        }
        struct timeval tv = {(long)timestamp, 0};
        settimeofday(&tv, NULL);
    }

    while (Wire.available()) {
        Wire.read();
    }
}

void requestEvent() {
    uint8_t cmd = read_i2c_command();
    
    // Drain queue to ensure we have the absolute latest metrics
    I2cPayloadMessage msg;
    while (xQueueReceive(dataQueue, &msg, 0) == pdTRUE) {
        cachedSensorData = msg.data;
        cachedMicOk = msg.mic_ok;
    }

    float laeq = cachedSensorData.noiseAvgDb;
    float lafmax = cachedSensorData.noisePeakDb;
    float l10 = cachedSensorData.noiseAvgLegalDb;
    float l90 = (float)cachedSensorData.lowNoiseLevel;
    uint32_t rms_mv = cachedSensorData.noise;

    switch (cmd) {
        case CMD_GET_STATUS:
            Wire.write(&cachedMicOk, 1);
            break;
        case CMD_GET_DATA:
            Wire.write((uint8_t *)&cachedSensorData, sizeof(SensorData));
            break;
        case CMD_IDENTIFY: {
            uint8_t id[5] = {0x01, 0x02, 0x01, 0x01, I2C_ADDR_SLAVE};
            Wire.write(id, 5);
            break;
        }
        case CMD_LEGACY_GET_DB:
            Wire.write((uint8_t *)&laeq, 4);
            break;
        case CMD_LEGACY_GET_RAW_MV:
            Wire.write((uint8_t *)&rms_mv, 4);
            break;
        case CMD_LEGACY_GET_LMAX:
            Wire.write((uint8_t *)&lafmax, 4);
            break;
        case CMD_LEGACY_GET_L10:
            Wire.write((uint8_t *)&l10, 4);
            break;
        case CMD_LEGACY_GET_L90:
            Wire.write((uint8_t *)&l90, 4);
            break;
        default:
            Wire.write((uint8_t)0);
            break;
    }
}

void I2C_Comm_Init() {
    bool pins_ok = Wire.setPins(I2C_SDA, I2C_SCL);
    bool i2c_ok = pins_ok && Wire.begin((uint8_t)I2C_ADDR_SLAVE);
    Wire.setClock(100000);
    Wire.onReceive(receiveEvent);
    Wire.onRequest(requestEvent);

    if (pins_ok && i2c_ok) {
        Serial.printf("[INIT] I2C Slave OK addr=0x%02X SDA=%d SCL=%d\n", I2C_ADDR_SLAVE, I2C_SDA, I2C_SCL);
    } else {
        Serial.printf("[ERR] I2C Slave init failed SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
    }
}
