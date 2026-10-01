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

// #12 Node metadata (firmware version, node type, time-sync, clipping)
#define CMD_GET_METADATA 0x50

// Persistent calibration: master writes a dB offset (int16, hundredths of dB,
// e.g. 55.4 dB target vs 65.4 measured -> -1000) with a physical calibrator;
// the node stores it in NVS so it survives reboots and applies it to every
// level. Payload: 1 cmd byte + 2 bytes int16 little-endian.
#define CMD_SET_CALIB 0x0A

// Accepted range for that offset. A single corrupted byte on the bus must not
// persist an absurd value into NVS, where it would survive reboots and bias
// every reading from then on. Values outside the range are ignored.
#define CALIB_OFFSET_MIN_DB (-30.0f)
#define CALIB_OFFSET_MAX_DB (30.0f)

// Reported by CMD_GET_METADATA. This was still 3.2.1 in the 3.3.0 release, so
// a master asking which firmware a node runs got the wrong answer.
#define FW_VERSION_MAJOR 3
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 1

// Returned by CMD_GET_METADATA. Packed for a stable wire layout.
struct NodeMetadata {
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    uint8_t node_type;        // 0x01 = ADC/MAX4466, 0x02 = I2S/ICS-43434
    uint8_t time_synced;      // 1 once the master has set the clock
    uint16_t clip_count;      // full-scale samples in the last second (I2S)
    int16_t calib_offset;     // NVS calibration offset, hundredths of dB
    // Appended in 3.3.1: how many valid seconds the L10/L90 window currently
    // holds, out of AGG_WINDOW_SEC. Until now a master could not tell a
    // percentile computed over 12 s from one over a full 300 s window, and
    // after a boot or a run of invalid seconds it published the former as if
    // it were the latter. A master reading only the original 9 bytes is
    // unaffected: the field is appended, like SensorData's.
    uint16_t window_fill;
    uint16_t window_size;     // AGG_WINDOW_SEC this firmware was built with
    // Appended in 3.3.1: how much the Lden periods have actually accumulated.
    // Lden is published from the first valid second of a period, so at
    // 07:00:02 a node already reports an "Lden (24h)" built from two seconds
    // of day. window_fill solved exactly this for L10/L90; these do it for
    // Lden. Bit 0 = day has data, bit 1 = evening, bit 2 = night.
    uint8_t lden_periods;
    uint16_t lden_minutes;    // accumulated minutes across populated periods
} __attribute__((packed));
static_assert(sizeof(NodeMetadata) == 16, "metadata wire size changed");

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

// #12 metadata setters (called by the node firmware)
void I2C_Comm_SetNodeType(uint8_t node_type);
void I2C_Comm_SetClipCount(uint16_t clip_count);

// How many valid seconds the L10/L90 sliding window currently holds, and the
// window length this firmware was built with. The aggregator reports both
// every second; they are published in the metadata so the master can tell a
// partial window from a full one. I2C_Comm does not include the aggregator
// header, so the size is passed in rather than read from AGG_WINDOW_SEC here.
void I2C_Comm_SetWindowFill(uint16_t valid_seconds, uint16_t window_size);

// #A7 Feeds this second's impulsive maxima into the hold latches that
// CMD_GET_DATA reports and resets. Called once per valid second by the
// aggregator; the latches keep the maximum until the master reads them, so a
// slow-polling master no longer misses the events between its reads.
void I2C_Comm_AccumulateImpulsive(float lasmax_db, float lcpeak_db);

// How much the Ld/Le/Ln accumulators hold, for the metadata. periods_mask:
// bit 0 day, bit 1 evening, bit 2 night. minutes: total across them.
void I2C_Comm_SetLdenProgress(uint8_t periods_mask, uint16_t minutes);

// #5 time-sync gate: true once the master has set the clock via
// CMD_SET_TIME_LEGACY. The aggregator polls this before computing Ld/Le/Ln.
bool I2C_Comm_TimeSynced();

// Persistent calibration offset in dB, loaded from NVS at init and updated by
// CMD_SET_CALIB. The node firmware reads this and applies it to every level
// (typically by feeding it into the amplitude->dB conversion). Returns 0.0 if
// never calibrated.
float I2C_Comm_GetCalibOffset();

// Applies work deferred by the I2C callback: a pending clock set
// (CMD_SET_TIME_LEGACY) and a pending calibration write to NVS. Must be called
// from task context — the aggregator task calls it once per second. The
// callback only stores values and raises flags; it never writes flash nor
// calls settimeofday(), neither of which is safe in that context.
void I2C_Comm_Service();

#endif // I2C_COMM_H
