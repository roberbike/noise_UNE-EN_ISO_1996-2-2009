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
#include <stddef.h>   // offsetof, for the layout guards below

/**
 * Reference I2C master for the noise nodes: ESP32-C3 + MAX4466 or
 * XIAO ESP32-S3 + ICS-43434, which speak the same protocol. Runs on any
 * ESP32 board; the pins below cover the XIAO ESP32-S3 and the Lolin S2 Mini.
 * Slave address 0x08.
 *
 * It shows what a real integration needs, in order:
 *  - the read sequence (status, data, status again) and when a frame may be
 *    published — see loop();
 *  - the frame checks: length, CRC and an advancing `cycles`;
 *  - the metadata, whose real length depends on the node's version;
 *  - setting the node clock (LOCAL epoch) and correcting its calibration.
 */

#define SLAVE_ADDR 0x08
// One poll per second, the node's aggregation period. The ...HoldDb fields
// make a slower period safe for impulses, but LAeq is the level of the last
// second only: a slower master samples it instead of averaging it.
#define REQUEST_INTERVAL_MS 1000

#if defined(CONFIG_IDF_TARGET_ESP32S2)
#define I2C_SDA 8      // Lolin S2 Mini
#define I2C_SCL 9
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define I2C_SDA 5      // XIAO ESP32-S3: D4
#define I2C_SCL 6      // XIAO ESP32-S3: D5
#else
#define I2C_SDA SDA    // any other board: the default I2C pins of its variant
#define I2C_SCL SCL
#endif

// Protocol commands (keep in sync with src/I2C_Comm.h)
#define CMD_GET_STATUS        0x20
#define CMD_GET_STATUS_LEGACY 0x00
#define CMD_GET_DATA          0x01
#define CMD_SET_TIME          0x09   // + uint32 LE local epoch, see setNodeTime()
#define CMD_SET_CALIB         0x0A   // + int16 LE, hundredths of dB
#define CMD_GET_METADATA      0x50

#define CALIB_LIMIT_DB 30.0f         // the node ignores offsets beyond +-30 dB

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
  // Appended in 3.3.1: maxima since the previous data read, reset by it.
  // Use these, not the per-second fields above, for impulsive noise: the
  // per-second ones only describe the last second before the read.
  float noiseLASmaxHoldDb;
  float noiseLCpeakHoldDb;
  uint16_t crc16;      // CRC-16/CCITT-FALSE over the 84 bytes before it
  uint16_t reserved;
};

// Offset guards. THIS is what was missing when 3.3.0 moved two fields: both
// sides still compiled, the read still returned the expected byte count, and
// the values were silently wrong. If these ever fail, your struct has drifted
// from the node's — fix the struct, do not change the numbers.
static_assert(offsetof(SensorData, noiseAvgDb)        ==  8, "layout: noiseAvgDb moved");
static_assert(offsetof(SensorData, lowNoiseLevel)     == 44, "layout: lowNoiseLevel moved");
static_assert(offsetof(SensorData, cycles)            == 48, "layout: cycles moved");
static_assert(offsetof(SensorData, noiseLden)         == 64, "layout: noiseLden moved");
static_assert(offsetof(SensorData, noiseLASmaxDb)     == 68, "layout: noiseLASmaxDb moved");
static_assert(offsetof(SensorData, noiseLCpeakDb)     == 72, "layout: noiseLCpeakDb moved");
static_assert(offsetof(SensorData, noiseLASmaxHoldDb) == 76, "layout: noiseLASmaxHoldDb moved");
static_assert(offsetof(SensorData, noiseLCpeakHoldDb) == 80, "layout: noiseLCpeakHoldDb moved");
static_assert(offsetof(SensorData, crc16)             == 84, "layout: crc16 moved");
static_assert(sizeof(SensorData) == 88, "layout: unexpected SensorData size");

// Node metadata (CMD_GET_METADATA), decoded.
struct NodeMeta {
  uint8_t fw_major, fw_minor, fw_patch;
  uint8_t node_type;       // 0x01 ADC/MAX4466, 0x02 I2S/ICS-43434
  uint8_t time_synced;     // 1 once a master has set the clock
  uint16_t clip_count;     // clipped samples in the last second
  int length;              // bytes the node really sent (from its version)
  // Valid only when length == 16 (node 3.3.1 or later):
  int16_t calib_offset;    // hundredths of dB
  uint16_t window_fill;    // valid seconds in the L10/L90 window...
  uint16_t window_size;    // ...out of this many
  uint8_t lden_periods;    // bit 0 day, bit 1 evening, bit 2 night
  uint16_t lden_minutes;   // minutes accumulated across those periods
};

// Same polynomial and seed the node uses (CRC-16/CCITT-FALSE).
static uint16_t frameCrc16(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// ---------------------------------------------------------------------------
// Bus helpers
// ---------------------------------------------------------------------------

// Sends a one-byte command. Returns the Wire error code (0 = ACK).
static uint8_t sendCommand(uint8_t cmd) {
  Wire.beginTransmission(SLAVE_ADDR);
  Wire.write(cmd);
  return Wire.endTransmission();
}

// Reads up to `len` bytes after a command. Returns how many arrived and never
// leaves bytes behind for the next transaction.
static size_t readReply(uint8_t *buf, size_t len) {
  Wire.requestFrom((uint8_t)SLAVE_ADDR, len, true);
  size_t n = 0;
  while (Wire.available() && n < len) buf[n++] = Wire.read();
  while (Wire.available()) Wire.read();
  return n;
}

// Status byte: 1 when the node's last second was valid, 0 while it boots,
// after an invalid second (clipping, microphone fault) or with sampling
// stalled.
static bool readStatus(uint8_t &status) {
  uint8_t err = sendCommand(CMD_GET_STATUS);
  if (err != 0) err = sendCommand(CMD_GET_STATUS_LEGACY);   // older nodes
  if (err != 0) {
    Serial.printf("I2C error %u on the status command (wiring, power, common GND?)\n",
                  (unsigned)err);
    return false;
  }
  delay(10);
  if (readReply(&status, 1) != 1) {
    Serial.println("Error: no reply to the status request");
    return false;
  }
  return true;
}

static size_t readFrame(SensorData &data) {
  if (sendCommand(CMD_GET_DATA) != 0) return 0;
  delay(20);
  return readReply((uint8_t *)&data, sizeof(SensorData));
}

// The metadata frame has only ever grown at the end: 7 bytes, 9 in 3.3.0
// (calibration offset), 16 from 3.3.1. Ask for the longest and trust only what
// the node's version says it sent: an ESP32 master pads a short reply to the
// requested length, so the byte count proves nothing. And 3.3.0 reported
// itself as 3.2.1, so for any node older than 3.3.1 only the first 7 bytes
// are certain.
static bool readMetadata(NodeMeta &m) {
  if (sendCommand(CMD_GET_METADATA) != 0) return false;
  delay(5);
  uint8_t b[16] = {0};
  if (readReply(b, sizeof(b)) < 7) return false;
  // A node without CMD_GET_METADATA answers a single 0 (no release is 0.x).
  if (b[0] == 0) return false;
  uint32_t ver = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
  m.fw_major = b[0];
  m.fw_minor = b[1];
  m.fw_patch = b[2];
  m.node_type = b[3];
  m.time_synced = b[4];
  m.clip_count = (uint16_t)b[5] | ((uint16_t)b[6] << 8);
  m.length = (ver >= 0x030301) ? 16 : 7;
  m.calib_offset = (int16_t)((uint16_t)b[7] | ((uint16_t)b[8] << 8));
  m.window_fill = (uint16_t)b[9] | ((uint16_t)b[10] << 8);
  m.window_size = (uint16_t)b[11] | ((uint16_t)b[12] << 8);
  m.lden_periods = b[13];
  m.lden_minutes = (uint16_t)b[14] | ((uint16_t)b[15] << 8);
  return true;
}

static void printMetadata(const NodeMeta &m) {
  Serial.printf("Meta: fw %u.%u.%u | node=%s | time_synced=%u | clips=%u",
                (unsigned)m.fw_major, (unsigned)m.fw_minor, (unsigned)m.fw_patch,
                (m.node_type == 0x02 ? "I2S" : (m.node_type == 0x01 ? "ADC" : "?")),
                (unsigned)m.time_synced, (unsigned)m.clip_count);
  if (m.length < 16) {
    Serial.println(" | (node older than 3.3.1: no further fields)");
    return;
  }
  Serial.printf(" | calib=%.2f dB\n", m.calib_offset / 100.0f);
  // A percentile over a partial window is a different statistic: treat
  // L10/L90 as provisional until the window has filled.
  Serial.printf("      L10/L90 window: %u/%u s%s\n", (unsigned)m.window_fill,
                (unsigned)m.window_size,
                (m.window_size && m.window_fill < m.window_size) ? "  (PARTIAL)" : "");
  // Same caution for Lden: it is published from the first valid second, so
  // check what it rests on before storing it as a 24 h index.
  Serial.printf("      Lden rests on: %s%s%s %u min%s\n",
                (m.lden_periods & 0x01) ? "D" : "-", (m.lden_periods & 0x02) ? "E" : "-",
                (m.lden_periods & 0x04) ? "N" : "-", (unsigned)m.lden_minutes,
                (m.lden_periods != 0x07) ? "  (INCOMPLETE DAY)" : "");
}

// ---------------------------------------------------------------------------
// Node configuration
// ---------------------------------------------------------------------------

// #B10 Pushes the wall clock to the node. Without it the node never sets
// time_synced and Ld/Le/Ln/Lden stay at 0 forever.
//
// IMPORTANT: send LOCAL epoch, not UTC. The node applies no timezone of its
// own, so the day (07-19 h), evening (19-23 h) and night (23-07 h) bands are
// read straight off whatever you send. In Spain that means UTC + 1 h in
// winter and UTC + 2 h in summer; get it wrong and every period index is
// shifted by an hour. Call it once the node answers, and again after any DST
// change or clock resync.
bool setNodeTime(uint32_t local_epoch) {
  Wire.beginTransmission(SLAVE_ADDR);
  Wire.write(CMD_SET_TIME);
  Wire.write((uint8_t)(local_epoch & 0xFF));
  Wire.write((uint8_t)((local_epoch >> 8) & 0xFF));
  Wire.write((uint8_t)((local_epoch >> 16) & 0xFF));
  Wire.write((uint8_t)((local_epoch >> 24) & 0xFF));
  uint8_t err = Wire.endTransmission();
  if (err != 0) Serial.printf("setNodeTime: I2C error %u\n", (unsigned)err);
  return err == 0;
}

// Sets the node's calibration offset to an ABSOLUTE value in dB. It replaces
// the stored offset (it is not added to it) and the node keeps it in NVS,
// where it survives reboots and reflashing. Usually you want calibrateNode().
bool setNodeCalibration(float offset_db) {
  if (!(offset_db >= -CALIB_LIMIT_DB && offset_db <= CALIB_LIMIT_DB)) {
    Serial.printf("setNodeCalibration: %.2f dB is outside +-%.0f dB, not sent\n",
                  offset_db, CALIB_LIMIT_DB);
    return false;
  }
  int16_t raw = (int16_t)lroundf(offset_db * 100.0f);   // hundredths of dB
  Wire.beginTransmission(SLAVE_ADDR);
  Wire.write(CMD_SET_CALIB);
  Wire.write((uint8_t)(raw & 0xFF));
  Wire.write((uint8_t)((raw >> 8) & 0xFF));
  uint8_t err = Wire.endTransmission();
  if (err != 0) Serial.printf("setNodeCalibration: I2C error %u\n", (unsigned)err);
  return err == 0;
}

// Corrects a node against a reference: a calibrator on its microphone, or a
// reference sound level meter beside it. Every level the node reports already
// includes its stored offset, so the new offset is the current one plus the
// error, new = current + (reference - measured); sending only the difference
// would discard the previous correction. Call it once, never from loop().
// Example: calibrator at 94.0 dB, node reads 94.6 -> calibrateNode(94.0, 94.6).
bool calibrateNode(float reference_db, float measured_db) {
  NodeMeta m;
  if (!readMetadata(m) || m.length < 16) {
    Serial.println("calibrateNode: the current offset cannot be read (node firmware "
                   "older than 3.3.1); compute it yourself and use setNodeCalibration()");
    return false;
  }
  float current = m.calib_offset / 100.0f;
  float target = current + (reference_db - measured_db);
  Serial.printf("Calibration: current %.2f dB, error %+.2f dB -> new %.2f dB\n",
                current, reference_db - measured_db, target);
  if (!setNodeCalibration(target)) return false;
  delay(20);
  if (readMetadata(m) && m.length == 16) {
    Serial.printf("Node now reports %.2f dB\n", m.calib_offset / 100.0f);
  }
  return true;
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- Noise node I2C master ---");
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(100); // ms; keeps a hung bus from locking the master
  Serial.printf("I2C initialized (SDA=%d, SCL=%d). Polling slave 0x%02X...\n",
                (int)I2C_SDA, (int)I2C_SCL, SLAVE_ADDR);

  // REQUIRED for Ld/Le/Ln/Lden: the node has no clock of its own and computes
  // none of them until a master sets one. Left commented because only you
  // know your time source (NTP plus your timezone offset, or an RTC holding
  // local time) — but leave it commented and expect Lden = 0 and
  // time_synced = 0 forever. LOCAL epoch, not UTC.
  // setNodeTime(local_epoch_from_your_time_source());
}

void loop() {
  static uint32_t lastRequest = 0;
  static uint32_t last_cycles = 0;
  static bool have_last = false;

  if (millis() - lastRequest < REQUEST_INTERVAL_MS) return;
  lastRequest = millis();

  // 1. Status first. On 0 do not even read the frame: there is nothing new
  //    to publish, and skipping loses nothing, because the node only restarts
  //    the ...HoldDb window on a read it serves with status 1.
  uint8_t status = 0;
  if (!readStatus(status)) return;
  if (status != 1) {
    Serial.println("Status 0: node booting, last second invalid or sampling stalled - frame not read");
    NodeMeta meta;   // clip_count tells clipping apart from the rest
    if (readMetadata(meta)) printMetadata(meta);
    return;
  }

  // 2. The frame.
  SensorData data;
  size_t got = readFrame(data);
  if (got != sizeof(SensorData)) {
    Serial.printf("Error: incomplete frame, expected %u bytes, got %u\n",
                  (unsigned)sizeof(SensorData), (unsigned)got);
    return;
  }

  // 3. Status again. A node second can end between steps 1 and 2; if that
  //    second was invalid, the frame was served with status 0 and carries
  //    the previous second's values, so it must not be published. This
  //    second read is what catches it. (A second that ends between steps 2
  //    and 3 costs a good frame instead — see docs/COMUNICACION.md.)
  uint8_t status_after = 0;
  bool after_ok = readStatus(status_after);

  // 4. Checks, CRC first: a frame corrupted in transit can still carry an
  //    advancing `cycles`, so the other checks alone would pass it.
  uint16_t crc_calc = frameCrc16(&data, offsetof(SensorData, crc16));
  bool crc_ok = (crc_calc == data.crc16);
  // `cycles` must CHANGE, not grow: it restarts from 1 when the node reboots.
  // Equal means the same second as the previous read (two polls inside one
  // node second, or a node whose sampling has stalled).
  bool fresh = !have_last || data.cycles != last_cycles;
  if (crc_ok) {   // only a frame that passed its CRC moves the reference
    last_cycles = data.cycles;
    have_last = true;
  }

  const char *skip = nullptr;
  if (!crc_ok) skip = "CRC mismatch";
  else if (!after_ok || status_after != 1) skip = "status fell to 0 during the read";
  else if (!fresh) skip = "same second as the previous read";

  Serial.println("--- Sensor Data ---");
  if (!crc_ok) {
    Serial.printf("CRC mismatch: frame 0x%04X, computed 0x%04X\n",
                  (unsigned)data.crc16, (unsigned)crc_calc);
  }
  Serial.printf("%s%s\n", skip ? "SKIP: " : "PUBLISH", skip ? skip : "");
  Serial.printf("LAeq (1s): %.2f dB\n", data.noiseAvgDb);
  Serial.printf("LAFmax (1s): %.2f dB\n", data.noisePeakDb);
  Serial.printf("LASmax (1s): %.2f dB | since last read: %.2f dB\n",
                data.noiseLASmaxDb, data.noiseLASmaxHoldDb);
  Serial.printf("LCpeak (1s): %.2f dB | since last read: %.2f dB\n",
                data.noiseLCpeakDb, data.noiseLCpeakHoldDb);
  Serial.printf("L10: %.2f dB | L90: %u dB\n", data.noiseAvgLegalDb,
                (unsigned)data.lowNoiseLevel);
  Serial.printf("Ld: %.2f | Le: %.2f | Ln: %.2f | Lden: %.2f dB\n",
                data.Ld, data.Le, data.Ln, data.noiseLden);
  Serial.printf("Raw: %lu | Cycles: %lu\n", (unsigned long)data.noise,
                (unsigned long)data.cycles);

  if (!skip) {
    // publishToCloud(data);   // integrate here. Skipped frames are never
    // republished: that is what keeps flat lines out of the dashboard.
  }

  // Optional: node metadata (consumes nothing on the node).
  NodeMeta meta;
  if (readMetadata(meta)) printMetadata(meta);
  Serial.println("-------------------");
}
