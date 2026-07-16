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

// Guards cachedSensorData/cachedMicOk: I2C_Comm_Sync (task context) copies the
// struct while requestEvent (slave HAL context) reads it. Without the lock a
// request landing mid-copy delivers a torn struct to the master.
// portENTER/EXIT_CRITICAL_SAFE work from both task and ISR context.
static portMUX_TYPE cacheMux = portMUX_INITIALIZER_UNLOCKED;

// 0 until the first aggregation lands. The status byte reports 0 (not ready)
// so protocol-following masters never publish the boot-time zeroed struct.
static volatile uint8_t data_ready = 0;

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

void I2C_Comm_Sync() {
    // Drain queue to ensure we have the absolute latest metrics
    // This is called from a task context, not from the I2C callback
    I2cPayloadMessage msg;
    bool updated = false;
    while (xQueueReceive(dataQueue, &msg, 0) == pdTRUE) {
        updated = true;
    }
    if (updated) {
        portENTER_CRITICAL_SAFE(&cacheMux);
        cachedSensorData = msg.data;
        cachedMicOk = msg.mic_ok;
        data_ready = 1;
        portEXIT_CRITICAL_SAFE(&cacheMux);
    }
}

void requestEvent() {
    uint8_t cmd = read_i2c_command();

    // Atomic snapshot: never serve the struct while Sync is copying into it.
    SensorData snap;
    uint8_t status;
    portENTER_CRITICAL_SAFE(&cacheMux);
    snap = cachedSensorData;
    status = (data_ready && cachedMicOk) ? 1 : 0;
    portEXIT_CRITICAL_SAFE(&cacheMux);

    float laeq = snap.noiseAvgDb;
    float lafmax = snap.noisePeakDb;
    float l10 = snap.noiseAvgLegalDb;
    float l90 = (float)snap.lowNoiseLevel;
    uint32_t rms_mv = snap.noise;

    switch (cmd) {
        case CMD_GET_STATUS:
            Wire.write(&status, 1);
            break;
        case CMD_GET_DATA:
            Wire.write((uint8_t *)&snap, sizeof(SensorData));
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
    
    // Senior Programmer Note: Register callbacks BEFORE begin() to ensure 
    // the hardware is ready to handle the very first transaction.
    Wire.onReceive(receiveEvent);
    Wire.onRequest(requestEvent);

    bool i2c_ok = pins_ok && Wire.begin((uint8_t)I2C_ADDR_SLAVE);
    
    // Slave should NOT set the bus clock; it's controlled by the Master.
    // Wire.setClock(100000); 

    if (pins_ok && i2c_ok) {
        Serial.printf("[INIT] I2C Slave OK addr=0x%02X SDA=%d SCL=%d\n", I2C_ADDR_SLAVE, I2C_SDA, I2C_SCL);
    } else {
        Serial.printf("[ERR] I2C Slave init failed SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
    }
}
