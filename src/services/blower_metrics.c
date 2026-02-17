#include "services/blower_metrics.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <string.h>

typedef struct {
  SemaphoreHandle_t mutex;
  blower_metrics_models_t models;
  blower_metrics_snapshot_t snapshot;
  float fan_pressure_offset_pa;
  float envelope_pressure_offset_pa;
  float last_fan_pressure_raw_pa;
  float last_envelope_pressure_raw_pa;
  bool has_last_fan_pressure_raw;
  bool has_last_envelope_pressure_raw;
  bool is_initialized;
} blower_metrics_service_context_t;

static blower_metrics_service_context_t g_service_context;

static float blower_absf(float value) {
  return value >= 0.0f ? value : -value;
}

float blower_linear_fan_speed_model(float fan_pressure_pa, const void *context) {
  const blower_linear_fan_speed_model_config_t *config =
      (const blower_linear_fan_speed_model_config_t *)context;
  float gain = 1.0f;

  if (config != NULL && config->pascal_to_speed_gain > 0.0f) {
    gain = config->pascal_to_speed_gain;
  }

  return blower_absf(fan_pressure_pa) * gain;
}

float blower_linear_air_leakage_model(float fan_speed_units,
                                      float envelope_pressure_pa,
                                      const void *context) {
  const blower_linear_air_leakage_model_config_t *config =
      (const blower_linear_air_leakage_model_config_t *)context;
  float gain = 1.0f;

  if (config != NULL && config->leakage_gain > 0.0f) {
    gain = config->leakage_gain;
  }

  return fan_speed_units * blower_absf(envelope_pressure_pa) * gain;
}

static void blower_metrics_service_apply_default_models(
    blower_metrics_models_t *models) {
  models->fan_speed_model = blower_linear_fan_speed_model;
  models->fan_speed_model_context = NULL;
  models->air_leakage_model = blower_linear_air_leakage_model;
  models->air_leakage_model_context = NULL;
}

void blower_metrics_service_initialize(const blower_metrics_models_t *models) {
  if (g_service_context.mutex == NULL) {
    g_service_context.mutex = xSemaphoreCreateMutex();
  }

  if (g_service_context.mutex == NULL) {
    return;
  }

  if (models != NULL && models->fan_speed_model != NULL &&
      models->air_leakage_model != NULL) {
    g_service_context.models = *models;
  } else {
    blower_metrics_service_apply_default_models(&g_service_context.models);
  }

  xSemaphoreTake(g_service_context.mutex, portMAX_DELAY);
  memset(&g_service_context.snapshot, 0, sizeof(g_service_context.snapshot));
  g_service_context.fan_pressure_offset_pa = 0.0f;
  g_service_context.envelope_pressure_offset_pa = 0.0f;
  g_service_context.last_fan_pressure_raw_pa = 0.0f;
  g_service_context.last_envelope_pressure_raw_pa = 0.0f;
  g_service_context.has_last_fan_pressure_raw = false;
  g_service_context.has_last_envelope_pressure_raw = false;
  g_service_context.is_initialized = true;
  xSemaphoreGive(g_service_context.mutex);
}

void blower_metrics_service_update(const adp910_sample_t *fan_sample,
                                   bool fan_sample_valid,
                                   const adp910_sample_t *envelope_sample,
                                   bool envelope_sample_valid) {
  blower_metrics_snapshot_t *snapshot = &g_service_context.snapshot;

  if (!g_service_context.is_initialized) {
    blower_metrics_service_initialize(NULL);
  }

  if (!g_service_context.is_initialized || g_service_context.mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(g_service_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (fan_sample_valid && fan_sample != NULL) {
    g_service_context.last_fan_pressure_raw_pa = fan_sample->corrected_pressure_pa;
    g_service_context.has_last_fan_pressure_raw = true;
    snapshot->fan_pressure_pa = g_service_context.last_fan_pressure_raw_pa -
                                g_service_context.fan_pressure_offset_pa;
    snapshot->fan_temperature_c = fan_sample->temperature_c;
    snapshot->fan_sample_valid = true;
  } else {
    snapshot->fan_sample_valid = false;
  }

  if (envelope_sample_valid && envelope_sample != NULL) {
    g_service_context.last_envelope_pressure_raw_pa =
        envelope_sample->corrected_pressure_pa;
    g_service_context.has_last_envelope_pressure_raw = true;
    snapshot->envelope_pressure_pa =
        g_service_context.last_envelope_pressure_raw_pa -
        g_service_context.envelope_pressure_offset_pa;
    snapshot->envelope_temperature_c = envelope_sample->temperature_c;
    snapshot->envelope_sample_valid = true;
  } else {
    snapshot->envelope_sample_valid = false;
  }

  snapshot->fan_speed_units = g_service_context.models.fan_speed_model(
      snapshot->fan_pressure_pa, g_service_context.models.fan_speed_model_context);

  snapshot->estimated_air_leakage_units = g_service_context.models.air_leakage_model(
      snapshot->fan_speed_units, snapshot->envelope_pressure_pa,
      g_service_context.models.air_leakage_model_context);

  snapshot->update_sequence += 1u;
  snapshot->last_update_tick = (uint32_t)xTaskGetTickCount();

  xSemaphoreGive(g_service_context.mutex);
}

bool blower_metrics_service_get_snapshot(blower_metrics_snapshot_t *out_snapshot) {
  bool is_valid = false;

  if (out_snapshot == NULL || g_service_context.mutex == NULL ||
      !g_service_context.is_initialized) {
    return false;
  }

  if (xSemaphoreTake(g_service_context.mutex, portMAX_DELAY) == pdTRUE) {
    *out_snapshot = g_service_context.snapshot;
    is_valid = true;
    xSemaphoreGive(g_service_context.mutex);
  }

  return is_valid;
}

bool blower_metrics_service_capture_zero_offsets(void) {
  bool captured = false;
  blower_metrics_snapshot_t *snapshot = &g_service_context.snapshot;

  if (g_service_context.mutex == NULL || !g_service_context.is_initialized) {
    return false;
  }

  if (xSemaphoreTake(g_service_context.mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (snapshot->fan_sample_valid && g_service_context.has_last_fan_pressure_raw) {
    g_service_context.fan_pressure_offset_pa = g_service_context.last_fan_pressure_raw_pa;
    snapshot->fan_pressure_pa = 0.0f;
    captured = true;
  }

  if (snapshot->envelope_sample_valid &&
      g_service_context.has_last_envelope_pressure_raw) {
    g_service_context.envelope_pressure_offset_pa =
        g_service_context.last_envelope_pressure_raw_pa;
    snapshot->envelope_pressure_pa = 0.0f;
    captured = true;
  }

  if (captured) {
    snapshot->fan_speed_units = g_service_context.models.fan_speed_model(
        snapshot->fan_pressure_pa, g_service_context.models.fan_speed_model_context);
    snapshot->estimated_air_leakage_units =
        g_service_context.models.air_leakage_model(
            snapshot->fan_speed_units, snapshot->envelope_pressure_pa,
            g_service_context.models.air_leakage_model_context);
    snapshot->update_sequence += 1u;
    snapshot->last_update_tick = (uint32_t)xTaskGetTickCount();
  }

  xSemaphoreGive(g_service_context.mutex);
  return captured;
}
