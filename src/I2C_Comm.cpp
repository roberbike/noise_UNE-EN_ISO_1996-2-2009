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
#include <Preferences.h>

/*
 * Concurrency. Three contexts touch this module:
 *  - the aggregator task, once per second (Publish, the metadata setters,
 *    Service);
 *  - the Arduino core's I2C slave task, which runs receiveEvent/requestEvent
 *    while the master waits on the bus;
 *  - setup, once (Init).
 * The served struct, its status and the hold latches are read and written
 * together, so they share one spinlock. The metadata fields are independent
 * single values and are only ever read whole.
 */
static portMUX_TYPE cacheMux = portMUX_INITIALIZER_UNLOCKED;

static SensorData cachedSensorData = {};
static uint8_t cachedMicOk = 0;
// 0 until the first second lands, so the status byte reports "not ready" and
// a master that follows the protocol never publishes the boot-time zeros.
static uint8_t data_ready = 0;

// #A7 Impulsive hold latches: the maximum LASmax and LCpeak since the
// master's previous data read. hold_primed is false right after a read, so
// the next second starts a fresh window instead of re-counting the last one.
static float hold_lasmax_db = 0.0f;
static float hold_lcpeak_db = 0.0f;
static bool  hold_primed = false;

// NVS-backed calibration offset (dB). Loaded at init, replaced by
// CMD_SET_CALIB. The callback only sets calib_dirty; Service() writes flash.
static Preferences prefs;
static volatile float calib_offset_db = 0.0f;
static volatile uint8_t calib_dirty = 0;

// #R1 Pending wall clock from CMD_SET_TIME_LEGACY, applied by Service().
static volatile uint32_t pending_epoch = 0;
static volatile uint8_t time_dirty = 0;

static volatile uint8_t meta_node_type = 0x00;
static volatile uint8_t meta_time_synced = 0;
static volatile uint16_t meta_clip_count = 0;
static volatile uint16_t meta_window_fill = 0;
static volatile uint16_t meta_window_size = 0;
static volatile uint8_t meta_lden_periods = 0;
static volatile uint16_t meta_lden_minutes = 0;

static volatile uint8_t i2c_active_command = CMD_GET_STATUS;

// Every comparison with NaN is false and +-inf is out of range, so this also
// rejects a corrupted (non-finite) value without a separate isfinite().
static bool calib_in_range(float v) {
    return v >= CALIB_OFFSET_MIN_DB && v <= CALIB_OFFSET_MAX_DB;
}

// ---------------------------------------------------------------------------
// Aggregator-side API
// ---------------------------------------------------------------------------

void I2C_Comm_Publish(const SensorData &data, uint8_t mic_ok) {
    portENTER_CRITICAL_SAFE(&cacheMux);
    cachedSensorData = data;
    cachedMicOk = mic_ok;
    data_ready = 1;
    // #A7 A valid second also feeds the hold latches, inside the same critical
    // section as the frame it belongs to. Fed separately (as until 3.3.2), a
    // read landing between the two updates got the previous frame with this
    // second's impulse in the latch, consumed the window, and the next read
    // reported the same impulse again as its own last-second value.
    if (mic_ok) {
        if (!hold_primed) {
            hold_lasmax_db = data.noiseLASmaxDb;
            hold_lcpeak_db = data.noiseLCpeakDb;
            hold_primed = true;
        } else {
            if (data.noiseLASmaxDb > hold_lasmax_db) hold_lasmax_db = data.noiseLASmaxDb;
            if (data.noiseLCpeakDb > hold_lcpeak_db) hold_lcpeak_db = data.noiseLCpeakDb;
        }
    }
    portEXIT_CRITICAL_SAFE(&cacheMux);
}

void I2C_Comm_SetNodeType(uint8_t node_type) {
    meta_node_type = node_type;
}

void I2C_Comm_SetClipCount(uint16_t clip_count) {
    meta_clip_count = clip_count;
}

void I2C_Comm_SetLdenProgress(uint8_t periods_mask, uint16_t minutes) {
    meta_lden_periods = periods_mask;
    meta_lden_minutes = minutes;
}

void I2C_Comm_SetWindowFill(uint16_t valid_seconds, uint16_t window_size) {
    meta_window_fill = valid_seconds;
    meta_window_size = window_size;
}

bool I2C_Comm_TimeSynced() {
    return meta_time_synced != 0;
}

float I2C_Comm_GetCalibOffset() {
    return calib_offset_db;
}

void I2C_Comm_Service() {
    if (time_dirty) {
        time_dirty = 0;
        uint32_t ts = pending_epoch;
        // time_t, not long: long is 32-bit on both targets, so (long)ts went
        // negative from 2038-01-19 and the clock landed in 1901, which also
        // stopped Ld/Le/Ln. With IDF 5 (core 3.x) time_t is 64-bit and the
        // master's uint32 epoch is good until 2106; on IDF 4.4 (core 2.0.x)
        // time_t itself is 32-bit, so there the clock ends in 2038 anyway.
        struct timeval tv = {(time_t)ts, 0};
        settimeofday(&tv, NULL);
        meta_time_synced = 1;
        Serial.printf("[TIME] Clock set from master: epoch %lu (local)\n",
                      (unsigned long)ts);
    }

    if (calib_dirty) {
        calib_dirty = 0;
        float v = calib_offset_db;
        prefs.begin("noise", false);
        prefs.putFloat("calib_db", v);
        prefs.end();
        Serial.printf("[CALIB] Offset saved to NVS: %.2f dB\n", v);
    }
}

// ---------------------------------------------------------------------------
// I2C slave callbacks (Arduino core I2C slave task)
// ---------------------------------------------------------------------------

void receiveEvent(int bytes) {
    if (Wire.available() <= 0) {
        return;
    }

    uint8_t cmd = Wire.read();
    if (cmd == CMD_GET_STATUS_LEGACY) {
        cmd = CMD_GET_STATUS;
    }
    i2c_active_command = cmd;

    // Set the clock: 0x09 + uint32 little-endian epoch (LOCAL time — the node
    // applies no timezone, so the day/evening/night bands follow exactly what
    // the master sends). Applied later by Service(), in task context.
    if (cmd == CMD_SET_TIME_LEGACY && bytes == 5) {
        uint32_t timestamp = 0;
        uint8_t *p = (uint8_t *)&timestamp;
        for (int i = 0; i < 4 && Wire.available(); i++) {
            p[i] = Wire.read();
        }
        pending_epoch = timestamp;
        time_dirty = 1;
    }

    // Calibration offset: 0x0A + int16 little-endian, hundredths of dB. It
    // takes effect at once; the flash write is deferred to Service(), because
    // an erase blocks for tens of ms and the master is waiting on the bus.
    if (cmd == CMD_SET_CALIB && bytes == 3) {
        int16_t raw = 0;
        uint8_t *p = (uint8_t *)&raw;
        if (Wire.available()) p[0] = Wire.read();
        if (Wire.available()) p[1] = Wire.read();
        float v = raw / 100.0f;
        if (calib_in_range(v)) {
            calib_offset_db = v;
            calib_dirty = 1;
        }
    }

    while (Wire.available()) {
        Wire.read();
    }
}

void requestEvent() {
    uint8_t cmd = i2c_active_command;

    SensorData snap;
    uint8_t status;
    portENTER_CRITICAL_SAFE(&cacheMux);
    snap = cachedSensorData;
    status = (data_ready && cachedMicOk) ? 1 : 0;
    // #A7 Report the maxima since the previous data read. Before any second
    // has landed in the new window, fall back to the last second's values so
    // the fields never read 0.
    snap.noiseLASmaxHoldDb = hold_primed ? hold_lasmax_db : snap.noiseLASmaxDb;
    snap.noiseLCpeakHoldDb = hold_primed ? hold_lcpeak_db : snap.noiseLCpeakDb;
    // Consume the window only on a read the master can use (status 1). The
    // next second then opens a fresh window: re-arming with the second just
    // reported would put it in two consecutive reads and count every impulse
    // twice.
    if (cmd == CMD_GET_DATA && status == 1) {
        hold_primed = false;
    }
    portEXIT_CRITICAL_SAFE(&cacheMux);

    // #A5 CRC last, so it also covers the hold values. Bitwise over 84 bytes
    // is ~10-20 us; a 256-entry table would only pay off at tens of reads/s.
    snap.reserved = 0;
    snap.crc16 = sensordata_crc16(&snap, SENSORDATA_CRC_LEN);

    switch (cmd) {
        case CMD_GET_STATUS:
            Wire.write(&status, 1);
            break;
        case CMD_GET_DATA:
            Wire.write((uint8_t *)&snap, sizeof(SensorData));
            break;
        case CMD_GET_METADATA: {
            NodeMetadata meta = {
                FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH,
                meta_node_type, meta_time_synced, meta_clip_count,
                (int16_t)lroundf(calib_offset_db * 100.0f),
                meta_window_fill, meta_window_size,
                meta_lden_periods, meta_lden_minutes
            };
            Wire.write((uint8_t *)&meta, sizeof(NodeMetadata));
            break;
        }
        case CMD_IDENTIFY: {
            uint8_t id[5] = {0x01, 0x02, 0x01, 0x01, I2C_ADDR_SLAVE};
            Wire.write(id, 5);
            break;
        }
        // Legacy single-value reads (float or uint32, little-endian).
        case CMD_LEGACY_GET_DB:
            Wire.write((uint8_t *)&snap.noiseAvgDb, 4);
            break;
        case CMD_LEGACY_GET_RAW_MV:
            Wire.write((uint8_t *)&snap.noise, 4);
            break;
        case CMD_LEGACY_GET_LMAX:
            Wire.write((uint8_t *)&snap.noisePeakDb, 4);
            break;
        case CMD_LEGACY_GET_L10:
            Wire.write((uint8_t *)&snap.noiseAvgLegalDb, 4);
            break;
        case CMD_LEGACY_GET_L90: {
            float l90 = (float)snap.lowNoiseLevel;
            Wire.write((uint8_t *)&l90, 4);
            break;
        }
        default:
            Wire.write((uint8_t)0);
            break;
    }
}

void I2C_Comm_Init() {
    // Load the calibration offset, and reject a value out of range: before
    // 3.3.1 any int16/100 (up to +-327 dB) could be stored, and a corrupted
    // value would otherwise be applied to every reading after an upgrade.
    prefs.begin("noise", false);
    float v = prefs.getFloat("calib_db", 0.0f);
    if (!calib_in_range(v)) {
        Serial.printf("[INIT] NVS calibration offset %.2f dB out of range: "
                      "discarded and cleared\n", v);
        prefs.remove("calib_db");
        v = 0.0f;
    }
    prefs.end();
    calib_offset_db = v;
    Serial.printf("[INIT] Calibration offset: %.2f dB (from NVS)\n", v);

    bool pins_ok = Wire.setPins(I2C_SDA, I2C_SCL);

    // Register the callbacks before begin(), so the first transaction already
    // finds them. A slave does not set the bus clock: the master owns it.
    Wire.onReceive(receiveEvent);
    Wire.onRequest(requestEvent);

    bool i2c_ok = pins_ok && Wire.begin((uint8_t)I2C_ADDR_SLAVE);

    if (i2c_ok) {
        Serial.printf("[INIT] I2C slave OK addr=0x%02X SDA=%d SCL=%d\n",
                      I2C_ADDR_SLAVE, I2C_SDA, I2C_SCL);
    } else {
        Serial.printf("[ERR] I2C slave init failed SDA=%d SCL=%d\n",
                      I2C_SDA, I2C_SCL);
    }
}
