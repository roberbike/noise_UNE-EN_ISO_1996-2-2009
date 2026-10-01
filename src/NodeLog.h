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

#ifndef NODE_LOG_H
#define NODE_LOG_H

#include <Arduino.h>

/*
 * Deferred serial logging (#R3).
 *
 * The problem: the aggregator task runs at a high priority (so that a
 * completed second is processed promptly) and used to call Serial.printf()
 * directly. At 115200 baud the per-second status line is ~110 characters,
 * which is ~9.5 ms of blocking UART writes. On the single-core C3 that
 * preempted the sampling task for those 9.5 ms every second: the samples were
 * not lost, because the loop catches up from next_sample_time, but they
 * arrived in a burst at the end. A burst is not uniform sampling, and with no
 * analog anti-alias filter in front of the ADC that is a real measurement
 * error, not just jitter.
 *
 * The fix: the aggregator only formats the line into a queue (no I/O, no
 * blocking) and the Arduino loop task — priority 1, below the sampling task —
 * does the actual writing. The UART still blocks, but now in a task the
 * sampling task can preempt, so the sampling cadence stays uniform.
 *
 * Logging must never delay a measurement, so a full queue DROPS the line
 * rather than waiting for room.
 */

#define NODE_LOG_LINE_LEN 160
#define NODE_LOG_DEPTH    4

// Creates the queue. Call once from setup(), before the tasks start.
// Returns false if the queue could not be allocated, in which case
// NodeLog_Printf() falls back to printing directly.
bool NodeLog_Init();

// Formats and enqueues one line. Safe to call from any task; never blocks.
// Returns false if the line was dropped because the queue was full.
bool NodeLog_Printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Convenience wrapper matching the old SerialLog(level, msg) signature.
void NodeLog_Msg(const char *level, const char *msg);

// Blocks until a line is available and writes it. Called from loop().
void NodeLog_Pump();

#endif // NODE_LOG_H
