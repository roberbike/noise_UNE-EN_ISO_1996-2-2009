/**
 * Calibración automática I2S — XIAO ESP32-S3 + ICS-43434
 * Noise monitor (UNE-EN ISO 1996-2, Decree 213/2012)
 *
 * Este sketch realiza una calibración completa y automática del nodo digital:
 *   1. Resetea el offset guardado en NVM
 *   2. Mide con un calibrador a 94 dB
 *   3. Determina si el micrófono es válido o falso
 *   4. Calcula y guarda el offset óptimo en NVM
 *   5. Verifica la calibración
 *
 * El proceso es 100% automático: no requiere intervención del usuario.
 *
 * Uso:
 *   1. Conectar el ICS-43434: SCK -> GPIO2 (D1), WS -> GPIO3 (D2),
 *      SD -> GPIO4 (D3), VDD 3.3V, GND, L/R -> GND.
 *   2. Compilar y flashear este firmware.
 *   3. Acoplar un calibrador de 94.0 dB / 1 kHz al micrófono.
 *   4. Abrir el Monitor Serie a 115200 baudios.
 *   5. El sketch hará todo automáticamente y mostrará el resultado.
 *
 * Criterios de validez del micrófono:
 *   - Sensibilidad dentro de rango: -26 ± 3 dBFS @ 94 dB SPL
 *   - Factor de cresta entre 2.8 y 3.2 dB (tono limpio)
 *   - Sin recortes (clips = 0)
 *   - RMS estable (variación < 0.5 dB entre lecturas)
 *
 * Si el micrófono es válido, se calcula el offset como:
 *   offset = 94.0 - LAeq_medido
 * y se guarda en NVM para que el firmware principal lo use.
 *
 * Si el micrófono es falso, no se guarda ningún offset y se reporta el error.
 */

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
#include <math.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include "driver/i2s.h"

// --- Configuración (debe coincidir con el firmware principal) ---
#define MIC_BCLK 2
#define MIC_WS 3
#define MIC_DIN 4

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 48000
#endif
#if SAMPLE_RATE != 16000 && SAMPLE_RATE != 48000
#error "SAMPLE_RATE must be 16000 or 48000"
#endif

#define READ_LEN 256

#define MIC_SENSITIVITY_DBFS (-26.0f) // ICS-43434 datasheet, on the sine PEAK
#define MIC_REF_DB 94.0f
#define MIC_CLIP_THRESHOLD 0.99f
#define MIC_PEAK_TO_RMS_DB 3.0103f

// --- Configuración de la calibración automática ---
#ifndef CALIBRATOR_DB
#define CALIBRATOR_DB 94.0f
#endif

// Número de lecturas para probar estabilidad
#define STABILITY_TEST_COUNT 5
// Variación máxima permitida entre lecturas (dB)
#define MAX_VARIATION_DB 0.5f
// Rango de sensibilidad válido (dBFS @ 94 dB SPL)
#define SENS_MIN_DBFS (-29.0f)  // -26 - 3
#define SENS_MAX_DBFS (-23.0f)  // -26 + 3
// Rango de factor de cresta válido
#define CREST_MIN_DB 2.8f
#define CREST_MAX_DB 3.2f
// Offset máximo permitido (dB)
#define MAX_OFFSET_DB 5.0f

// --- Coeficientes de ponderación A (mismos que src/DSP_Engine.cpp) ---
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
};

#if SAMPLE_RATE == 48000
static Biquad aWeightingFilters[3] = {
    {0.59481685f, 0.02328688f, -0.07069832f, -0.65174475f, 0.11228949f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.89387049f, 0.89515977f, 0, 0},
    {1.00000000f, -2.00000000f, 1.00000000f, -1.99461446f, 0.99462171f, 0, 0}};
#else
static Biquad aWeightingFilters[3] = {
    {0.529093f, -1.058186f, 0.529093f, -1.983887f, 0.983952f, 0, 0},
    {1.000000f, -2.000000f, 1.000000f, -1.705510f, 0.715988f, 0, 0},
    {-0.36654287f, 1.61495515f, 0.72597221f, 0.05375630f, -0.07418460f, 0, 0}};
#endif

static float applyFilter(float in, Biquad &f) {
  float out = in * f.b0 + f.z1;
  f.z1 = in * f.b1 - f.a1 * out + f.z2;
  f.z2 = in * f.b2 - f.a2 * out;
  return out;
}

static int32_t raw[READ_LEN];

// Estado de la cadena, mantenido entre segundos como en el firmware
static float dc_offset = 0.0f;
static bool dc_primed = false;

static float removeDc(float s) {
  if (!dc_primed) {
    dc_offset = s;
    dc_primed = true;
  }
  dc_offset = (dc_offset * 0.9999f) + (s * 0.0001f);
  return s - dc_offset;
}

// Lee un bloque DMA, convertido a fondo de escala. Devuelve el número de muestras.
static size_t readBlock(float *out) {
  size_t br = 0;
  if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &br, portMAX_DELAY) != ESP_OK) return 0;
  size_t n = br / sizeof(int32_t);
  for (size_t i = 0; i < n; i++) out[i] = (float)(raw[i] >> 8) / 8388608.0f;
  return n;
}

// Estructura para almacenar los resultados de una lectura de 1 segundo
struct SecondResult {
  float laeq;           // LAeq sin trim (dB)
  float dbfs_A;         // RMS ponderado A (dBFS)
  float dbfs_Z;         // RMS sin ponderar (dBFS)
  float crest;          // Factor de cresta (dB)
  float peak;           // Pico máximo (FS)
  uint32_t clips;       // Número de muestras recortadas
  float sens;           // Sensibilidad de la unidad (dBFS @ 94 dB SPL)
};

// Realiza una lectura de 1 segundo y devuelve los resultados
static bool readSecond(SecondResult &result) {
  static float block[READ_LEN];
  double sum_sq_A = 0.0;
  double sum_sq_Z = 0.0;
  float peak = 0.0f;
  uint32_t clips = 0;
  uint32_t count = 0;

  while (count < SAMPLE_RATE) {
    size_t n = readBlock(block);
    for (size_t i = 0; i < n; i++) {
      if (fabsf(block[i]) > MIC_CLIP_THRESHOLD) clips++;
      float s = removeDc(block[i]);
      sum_sq_Z += (double)(s * s);
      if (fabsf(s) > peak) peak = fabsf(s);
      float a = s;
      for (int k = 0; k < 3; k++) a = applyFilter(a, aWeightingFilters[k]);
      sum_sq_A += (double)(a * a);
      count++;
    }
  }

  if (count == 0) return false;

  float rms_A = sqrtf((float)(sum_sq_A / count));
  float rms_Z = sqrtf((float)(sum_sq_Z / count));
  if (rms_A <= 0.0f || rms_Z <= 0.0f) return false;

  result.dbfs_A = 20.0f * log10f(rms_A);
  result.dbfs_Z = 20.0f * log10f(rms_Z);
  result.crest = 20.0f * log10f(peak / rms_Z);
  result.peak = peak;
  result.clips = clips;
  // LAeq sin trim (igual que el firmware principal pero sin MIC_OFFSET_DB)
  result.laeq = MIC_REF_DB + result.dbfs_A - MIC_SENSITIVITY_DBFS + MIC_PEAK_TO_RMS_DB;
  // Sensibilidad de esta unidad referida a 94 dB SPL
  result.sens = result.dbfs_A + MIC_PEAK_TO_RMS_DB - ((float)CALIBRATOR_DB - 94.0f);

  return true;
}

// Resetea el offset en NVM
static void resetNvmOffset() {
  Preferences prefs;
  if (prefs.begin("noise", false)) {
    prefs.remove("calib_db");
    prefs.end();
    Serial.println("[INIT] Offset NVM reseteado");
  }
}

// Lee el offset actual de NVM
static float readNvmOffset() {
  Preferences prefs;
  float v = 0.0f;
  if (prefs.begin("noise", true)) {
    if (prefs.isKey("calib_db")) v = prefs.getFloat("calib_db", 0.0f);
    prefs.end();
  }
  return v;
}

// Guarda el offset en NVM
static bool saveNvmOffset(float offset) {
  Preferences prefs;
  if (prefs.begin("noise", false)) {
    prefs.putFloat("calib_db", offset);
    prefs.end();
    return true;
  }
  return false;
}

// Determina si el micrófono es válido basándose en los resultados de las lecturas
static bool isMicValid(const SecondResult results[], int count, float &avgLaeq, float &avgSens) {
  if (count < STABILITY_TEST_COUNT) return false;

  // Calcular promedios
  float sumLaeq = 0.0f, sumSens = 0.0f;
  for (int i = 0; i < count; i++) {
    sumLaeq += results[i].laeq;
    sumSens += results[i].sens;
  }
  avgLaeq = sumLaeq / count;
  avgSens = sumSens / count;

  // Verificar estabilidad (variación entre lecturas)
  float minLaeq = results[0].laeq, maxLaeq = results[0].laeq;
  for (int i = 1; i < count; i++) {
    if (results[i].laeq < minLaeq) minLaeq = results[i].laeq;
    if (results[i].laeq > maxLaeq) maxLaeq = results[i].laeq;
  }
  float variation = maxLaeq - minLaeq;
  Serial.printf("[CHECK] Variación entre lecturas: %.2f dB (máx permitido: %.2f dB)\n",
                variation, MAX_VARIATION_DB);
  if (variation > MAX_VARIATION_DB) {
    Serial.println("[FAIL] Lecturas inestables: posible ruido de fondo o acoplamiento deficiente");
    return false;
  }

  // Verificar sensibilidad dentro de rango
  Serial.printf("[CHECK] Sensibilidad: %.2f dBFS (rango válido: %.1f a %.1f)\n",
                avgSens, SENS_MIN_DBFS, SENS_MAX_DBFS);
  if (avgSens < SENS_MIN_DBFS || avgSens > SENS_MAX_DBFS) {
    Serial.println("[FAIL] Sensibilidad fuera de rango: micrófono falso o defectuoso");
    return false;
  }

  // Verificar factor de cresta (tono limpio)
  float avgCrest = 0.0f;
  for (int i = 0; i < count; i++) avgCrest += results[i].crest;
  avgCrest /= count;
  Serial.printf("[CHECK] Factor de cresta: %.2f dB (rango válido: %.1f a %.1f)\n",
                avgCrest, CREST_MIN_DB, CREST_MAX_DB);
  if (avgCrest < CREST_MIN_DB || avgCrest > CREST_MAX_DB) {
    Serial.println("[FAIL] Factor de cresta anormal: tono distorsionado o con fugas");
    return false;
  }

  // Verificar que no hay recortes
  uint32_t totalClips = 0;
  for (int i = 0; i < count; i++) totalClips += results[i].clips;
  Serial.printf("[CHECK] Recortes totales: %u\n", totalClips);
  if (totalClips > 0) {
    Serial.println("[FAIL] Hay recortes: nivel demasiado alto para esta unidad");
    return false;
  }

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("========================================");
  Serial.println("  CALIBRACIÓN AUTOMÁTICA ICS-43434 (I2S)");
  Serial.println("  XIAO ESP32-S3 @ " + String(SAMPLE_RATE) + " Hz");
  Serial.println("========================================");
  Serial.println();

  // Mostrar offset actual y resetearlo
  float currentOffset = readNvmOffset();
  Serial.printf("[INIT] Offset NVM actual: %.2f dB\n", currentOffset);
  resetNvmOffset();
  Serial.println();

  // Configurar I2S
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = READ_LEN;
  cfg.use_apll = false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = MIC_BCLK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_DIN;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("[ERR] Error al instalar el driver I2S");
    while (1) delay(1000);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.printf("[INIT] Conversión: SPL = %.1f + 20*log10(rms) + %.1f + %.4f (sin trim)\n",
                MIC_REF_DB, -MIC_SENSITIVITY_DBFS, MIC_PEAK_TO_RMS_DB);
  Serial.printf("[INIT] Calibrador asumido a %.1f dB\n", (float)CALIBRATOR_DB);
  Serial.println();

  // Warm-up: medio segundo a través de la cadena sin medir
  Serial.println("[INIT] Calentando cadena de medida...");
  static float warm[READ_LEN];
  uint32_t done = 0;
  while (done < SAMPLE_RATE / 2) {
    size_t n = readBlock(warm);
    for (size_t i = 0; i < n; i++) {
      float s = removeDc(warm[i]);
      for (int k = 0; k < 3; k++) s = applyFilter(s, aWeightingFilters[k]);
    }
    done += n;
  }
  Serial.println("[INIT] Cadena lista");
  Serial.println();

  // Fase 1: Realizar múltiples lecturas para verificar estabilidad
  Serial.println("========================================");
  Serial.println("  FASE 1: LECTURAS DE ESTABILIDAD");
  Serial.println("========================================");
  Serial.printf("Realizando %d lecturas de 1 segundo...\n\n", STABILITY_TEST_COUNT);

  SecondResult results[STABILITY_TEST_COUNT];
  for (int i = 0; i < STABILITY_TEST_COUNT; i++) {
    Serial.printf("[LECTURA %d/%d]\n", i + 1, STABILITY_TEST_COUNT);
    if (!readSecond(results[i])) {
      Serial.println("[ERR] Error al leer del micrófono");
      while (1) delay(1000);
    }
    Serial.printf("  LAeq: %.2f dB | dBFS(A): %.2f | dBFS(Z): %.2f | crest: %.2f dB | peak: %.2f FS | clips: %u\n",
                  results[i].laeq, results[i].dbfs_A, results[i].dbfs_Z,
                  results[i].crest, results[i].peak, results[i].clips);
    Serial.println();
    delay(500); // Pequeña pausa entre lecturas
  }

  // Fase 2: Evaluar validez del micrófono
  Serial.println("========================================");
  Serial.println("  FASE 2: EVALUACIÓN DEL MICRÓFONO");
  Serial.println("========================================");

  float avgLaeq = 0.0f, avgSens = 0.0f;
  bool valid = isMicValid(results, STABILITY_TEST_COUNT, avgLaeq, avgSens);

  Serial.println();
  if (!valid) {
    Serial.println("========================================");
    Serial.println("  RESULTADO: MICRÓFONO NO VÁLIDO");
    Serial.println("========================================");
    Serial.println();
    Serial.println("El micrófono no cumple los criterios de validez.");
    Serial.println("Posibles causas:");
    Serial.println("  - Micrófono falso o defectuoso");
    Serial.println("  - Calibrador no acoplado correctamente");
    Serial.println("  - Ruido de fondo excesivo");
    Serial.println("  - Cables I2S mal conectados");
    Serial.println();
    Serial.println("No se ha guardado ningún offset en NVM.");
    Serial.println("El firmware principal usará el trim de compilación.");
    while (1) delay(1000);
  }

  Serial.println("[OK] Micrófono válido");
  Serial.println();

  // Fase 3: Calcular y guardar el offset
  Serial.println("========================================");
  Serial.println("  FASE 3: CÁLCULO DEL OFFSET");
  Serial.println("========================================");

  float offset = (float)CALIBRATOR_DB - avgLaeq;
  Serial.printf("  LAeq medio medido: %.2f dB\n", avgLaeq);
  Serial.printf("  Nivel del calibrador: %.1f dB\n", (float)CALIBRATOR_DB);
  Serial.printf("  Offset calculado: %.2f dB\n", offset);

  // Verificar que el offset está dentro de rango razonable
  if (fabsf(offset) > MAX_OFFSET_DB) {
    Serial.println();
    Serial.println("========================================");
    Serial.println("  RESULTADO: OFFSET EXCESIVO");
    Serial.println("========================================");
    Serial.printf("El offset calculado (%.2f dB) excede el máximo permitido (%.1f dB).\n",
                  offset, MAX_OFFSET_DB);
    Serial.println("Esto indica un problema grave con el micrófono o el calibrador.");
    Serial.println("No se ha guardado ningún offset en NVM.");
    while (1) delay(1000);
  }

  // Guardar offset en NVM
  if (!saveNvmOffset(offset)) {
    Serial.println("[ERR] Error al guardar el offset en NVM");
    while (1) delay(1000);
  }
  Serial.println("[OK] Offset guardado en NVM");
  Serial.println();

  // Fase 4: Verificación final
  Serial.println("========================================");
  Serial.println("  FASE 4: VERIFICACIÓN FINAL");
  Serial.println("========================================");
  Serial.println("Verificando que el offset funciona correctamente...");
  Serial.println();

  // Realizar una nueva lectura con el offset aplicado
  SecondResult finalResult;
  if (!readSecond(finalResult)) {
    Serial.println("[ERR] Error al leer del micrófono");
    while (1) delay(1000);
  }

  // Aplicar el offset manualmente para la verificación
  float finalLaeq = finalResult.laeq + offset;
  Serial.printf("  LAeq medido: %.2f dB\n", finalResult.laeq);
  Serial.printf("  Offset aplicado: %.2f dB\n", offset);
  Serial.printf("  LAeq corregido: %.2f dB\n", finalLaeq);
  Serial.printf("  Diferencia con calibrador: %+.2f dB\n", finalLaeq - (float)CALIBRATOR_DB);
  Serial.println();

  // Verificar que la corrección es correcta
  float error = fabsf(finalLaeq - (float)CALIBRATOR_DB);
  if (error > 0.5f) {
    Serial.println("[WARN] La corrección no es perfecta (error > 0.5 dB)");
    Serial.println("Esto puede deberse a variaciones en el acoplamiento del calibrador.");
  } else {
    Serial.println("[OK] Corrección verificada");
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("  CALIBRACIÓN COMPLETADA CON ÉXITO");
  Serial.println("========================================");
  Serial.println();
  Serial.printf("  Offset guardado en NVM: %.2f dB\n", offset);
  Serial.printf("  Sensibilidad de la unidad: %.2f dBFS @ 94 dB SPL\n", avgSens);
  Serial.println();
  Serial.println("El firmware principal ahora usará este offset");
  Serial.println("automáticamente al arrancar.");
  Serial.println();
  Serial.println("Puedes flashear el firmware principal (src/main_i2s.cpp)");
  Serial.println("y el nodo estará calibrado y listo para usar.");
}

void loop() {
  // No hacer nada: la calibración se realiza una vez en setup()
  delay(1000);
}
