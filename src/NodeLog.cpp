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

#include "NodeLog.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdarg.h>

static QueueHandle_t logQueue = NULL;

bool NodeLog_Init() {
    if (logQueue != NULL) return true;
    logQueue = xQueueCreate(NODE_LOG_DEPTH, NODE_LOG_LINE_LEN);
    return logQueue != NULL;
}

bool NodeLog_Printf(const char *fmt, ...) {
    char line[NODE_LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (logQueue == NULL) {
        // Before NodeLog_Init(), or if the queue could not be allocated:
        // print directly. Only setup-time messages take this path.
        Serial.print(line);
        return true;
    }
    // Zero timeout: a full queue drops the line. A measurement must never
    // wait on a log message.
    return xQueueSend(logQueue, line, 0) == pdTRUE;
}

void NodeLog_Msg(const char *level, const char *msg) {
    NodeLog_Printf("[%s] %s\n", level, msg);
}

void NodeLog_Pump() {
    char line[NODE_LOG_LINE_LEN];
    if (logQueue == NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    if (xQueueReceive(logQueue, line, portMAX_DELAY) == pdTRUE) {
        Serial.print(line);
    }
}
