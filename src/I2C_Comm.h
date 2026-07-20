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

#ifndef I2C_COMM_H
#define I2C_COMM_H

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "DSP_Engine.h"

// --- I2C Configuration ---
#define I2C_ADDR_SLAVE 0x08 

// Default slave pins per target (overridable via build_flags)
#ifndef I2C_SDA
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define I2C_SDA 5   // XIAO ESP32-S3: D4
#define I2C_SCL 6   // XIAO ESP32-S3: D5
#else
#define I2C_SDA 8   // ESP32-C3 (lolin_c3_mini)
#define I2C_SCL 10
#endif
#endif

// Protocol Commands
#define CMD_GET_STATUS 0x20
#define CMD_GET_STATUS_LEGACY 0x00
#define CMD_GET_DATA 0x01
#define CMD_IDENTIFY 0x09
// Compatibility alias: legacy masters may send timestamp using 0x09 + 4 bytes.
#define CMD_SET_TIME_LEGACY CMD_IDENTIFY

// Mapping for secondary/Legacy commands
#define CMD_LEGACY_GET_DB 0x10
#define CMD_LEGACY_GET_L10 0x60
#define CMD_LEGACY_GET_L90 0x70
#define CMD_LEGACY_GET_RAW_MV 0x30
#define CMD_LEGACY_GET_LMAX 0x40

// --- Secure Queue Payload ---
// Packed struct to guarantee memory size alignment across FreeRTOS Queues
struct I2cPayloadMessage {
    SensorData data;
    uint8_t mic_ok;
} __attribute__((packed));

// --- Globals managed by the DSP task ---
// Used to snapshot values when passing data to the I2C requests
// Now powered by FreeRTOS Queue to prevent thread locking
extern QueueHandle_t dataQueue;
extern SensorData cachedSensorData;
extern uint8_t cachedMicOk;

// Synchronization
void I2C_Comm_Sync();

// Initialization
void I2C_Comm_Init();

#endif // I2C_COMM_H
