#include "services/blower_test_service.h"

#include "FreeRTOS.h"
#include "app/app_config.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "semphr.h"
#include "task.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BLOWER_TEST_STORAGE_MAGIC 0x42544452u /* BTDR */
#define BLOWER_TEST_STORAGE_VERSION 1u
#define BLOWER_TEST_STORAGE_FILL_BYTE 0xffu

#define BLOWER_TEST_FULL_APERTURE_DIAMETER_CM 31.0f
#define BLOWER_TEST_SEA_LEVEL_AIR_DENSITY 1.225f
#define BLOWER_TEST_AIR_GAS_CONSTANT 287.05f
#define BLOWER_TEST_REFERENCE_PRESSURE_PA 101325.0f

#define BLOWER_TEST_MIN_PRESSURE_PA 10.0f
#define BLOWER_TEST_MAX_PRESSURE_PA 100.0f
#define BLOWER_TEST_MIN_TOLERANCE_PA 0.2f
#define BLOWER_TEST_MAX_TOLERANCE_PA 10.0f
#define BLOWER_TEST_MIN_SETTLE_TIME_S 2u
#define BLOWER_TEST_MAX_SETTLE_TIME_S 180u
#define BLOWER_TEST_MIN_MEASURE_TIME_S 2u
#define BLOWER_TEST_MAX_MEASURE_TIME_S 300u
#define BLOWER_TEST_DEFAULT_MIN_POINTS 5u

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t payload_size;
  uint32_t sequence;
  blower_test_config_t config;
  uint8_t history_count;
  uint8_t history_head;
  uint16_t reserved0;
  blower_test_report_t history[BLOWER_TEST_HISTORY_CAPACITY];
  uint32_t crc32;
} blower_test_persistent_blob_t;

typedef struct {
  SemaphoreHandle_t mutex;
  bool initialized;
  bool persistence_available;

  blower_test_config_t config;
  blower_test_runtime_status_t runtime;

  blower_test_report_t active_report;
  blower_test_report_t latest_report;
  bool has_latest_report;
  uint32_t next_report_id;

  blower_test_report_t history[BLOWER_TEST_HISTORY_CAPACITY];
  uint8_t history_count;
  uint8_t history_head;

  uint32_t state_enter_tick_ms;
  uint32_t stable_since_tick_ms;
  uint32_t measure_start_tick_ms;

  float acc_pressure_pa;
  float acc_fan_flow_m3h;
  float acc_fan_temp_c;
  float acc_envelope_temp_c;
  float acc_pwm_percent;
  uint16_t acc_samples;

  blower_test_direction_t direction_sequence[2];
  uint8_t direction_count;
  uint8_t direction_slot;
} blower_test_context_t;

static blower_test_context_t g_context;
static uint8_t g_storage_image_buffer[APP_PERSISTENT_STORAGE_SIZE_BYTES];

_Static_assert(
    sizeof(blower_test_persistent_blob_t) <= APP_PERSISTENT_STORAGE_SIZE_BYTES,
    "Persistent blob is larger than APP_PERSISTENT_STORAGE_SIZE_BYTES");

static float blower_test_absf(float value) {
  return value >= 0.0f ? value : -value;
}

static float blower_test_clampf(float value, float min_value, float max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static uint32_t blower_test_crc32_update(uint32_t crc, const uint8_t *data,
                                         size_t data_len) {
  uint32_t value = crc;
  size_t index = 0u;

  if (data == NULL) {
    return value;
  }

  for (index = 0u; index < data_len; ++index) {
    uint32_t bit = 0u;
    value ^= data[index];
    for (bit = 0u; bit < 8u; ++bit) {
      const uint32_t mask = (uint32_t)-(int32_t)(value & 1u);
      value = (value >> 1u) ^ (0xedb88320u & mask);
    }
  }

  return value;
}

static uint32_t blower_test_crc32_for_blob(
    const blower_test_persistent_blob_t *blob) {
  const size_t payload_size =
      offsetof(blower_test_persistent_blob_t, crc32);
  return blower_test_crc32_update(0xffffffffu, (const uint8_t *)blob,
                                  payload_size) ^
         0xffffffffu;
}

static bool blower_test_storage_layout_is_valid(void) {
  const uint32_t storage_end =
      APP_PERSISTENT_STORAGE_OFFSET_BYTES + APP_PERSISTENT_STORAGE_SIZE_BYTES;

  if (APP_PERSISTENT_STORAGE_SIZE_BYTES == 0u) {
    return false;
  }
  if ((APP_PERSISTENT_STORAGE_OFFSET_BYTES % FLASH_SECTOR_SIZE) != 0u) {
    return false;
  }
  if ((APP_PERSISTENT_STORAGE_SIZE_BYTES % FLASH_SECTOR_SIZE) != 0u) {
    return false;
  }
  if ((APP_PERSISTENT_STORAGE_SIZE_BYTES % FLASH_PAGE_SIZE) != 0u) {
    return false;
  }
  if (APP_PERSISTENT_STORAGE_OFFSET_BYTES >= PICO_FLASH_SIZE_BYTES ||
      storage_end > PICO_FLASH_SIZE_BYTES) {
    return false;
  }

  return true;
}

static bool blower_test_storage_verify(const uint8_t *expected_data,
                                       size_t verify_len) {
  const volatile uint8_t *flash_bytes =
      (const volatile uint8_t *)(XIP_BASE + APP_PERSISTENT_STORAGE_OFFSET_BYTES);
  size_t index = 0u;

  if (expected_data == NULL) {
    return false;
  }

  for (index = 0u; index < verify_len; ++index) {
    if (flash_bytes[index] != expected_data[index]) {
      return false;
    }
  }

  return true;
}

static bool blower_test_storage_program(const blower_test_persistent_blob_t *blob) {
  uint32_t offset = 0u;
  uint32_t irq_state = 0u;

  if (blob == NULL || !blower_test_storage_layout_is_valid()) {
    return false;
  }

  memset(g_storage_image_buffer, BLOWER_TEST_STORAGE_FILL_BYTE,
         sizeof(g_storage_image_buffer));
  memcpy(g_storage_image_buffer, blob, sizeof(*blob));

  irq_state = save_and_disable_interrupts();
  flash_range_erase(APP_PERSISTENT_STORAGE_OFFSET_BYTES,
                    APP_PERSISTENT_STORAGE_SIZE_BYTES);

  for (offset = 0u; offset < APP_PERSISTENT_STORAGE_SIZE_BYTES;
       offset += FLASH_PAGE_SIZE) {
    flash_range_program(APP_PERSISTENT_STORAGE_OFFSET_BYTES + offset,
                        g_storage_image_buffer + offset, FLASH_PAGE_SIZE);
  }
  restore_interrupts(irq_state);

  return blower_test_storage_verify(g_storage_image_buffer,
                                    APP_PERSISTENT_STORAGE_SIZE_BYTES);
}

static bool blower_test_storage_load(blower_test_persistent_blob_t *out_blob) {
  const volatile uint8_t *flash_ptr = NULL;
  blower_test_persistent_blob_t loaded_blob = {0};
  uint32_t expected_crc = 0u;

  if (out_blob == NULL || !blower_test_storage_layout_is_valid()) {
    return false;
  }

  flash_ptr = (const volatile uint8_t *)(XIP_BASE + APP_PERSISTENT_STORAGE_OFFSET_BYTES);
  memcpy(&loaded_blob, (const void *)flash_ptr, sizeof(loaded_blob));

  if (loaded_blob.magic != BLOWER_TEST_STORAGE_MAGIC ||
      loaded_blob.version != BLOWER_TEST_STORAGE_VERSION ||
      loaded_blob.payload_size != sizeof(loaded_blob)) {
    return false;
  }

  expected_crc = blower_test_crc32_for_blob(&loaded_blob);
  if (expected_crc != loaded_blob.crc32) {
    return false;
  }

  *out_blob = loaded_blob;
  return true;
}

static float blower_test_air_density_kg_m3(float altitude_m, float temperature_c) {
  const float clamped_altitude = blower_test_clampf(altitude_m, 0.0f, 6000.0f);
  const float pressure_pa =
      BLOWER_TEST_REFERENCE_PRESSURE_PA *
      powf(1.0f - 2.25577e-5f * clamped_altitude, 5.25588f);
  const float temp_kelvin =
      blower_test_clampf(temperature_c, -40.0f, 80.0f) + 273.15f;

  if (temp_kelvin <= 1.0f) {
    return BLOWER_TEST_SEA_LEVEL_AIR_DENSITY;
  }

  return pressure_pa / (BLOWER_TEST_AIR_GAS_CONSTANT * temp_kelvin);
}

static float blower_test_aperture_scale(float aperture_diameter_cm) {
  const float aperture_cm = blower_test_clampf(aperture_diameter_cm, 5.0f, 60.0f);
  const float aperture_m = aperture_cm / 100.0f;
  const float full_aperture_m = BLOWER_TEST_FULL_APERTURE_DIAMETER_CM / 100.0f;
  const float aperture_area = (float)M_PI * powf(aperture_m * 0.5f, 2.0f);
  const float full_area = (float)M_PI * powf(full_aperture_m * 0.5f, 2.0f);

  if (full_area <= 0.0f) {
    return 1.0f;
  }
  return aperture_area / full_area;
}

static float blower_test_compute_fan_flow_m3h(
    const blower_test_config_t *config, float fan_pressure_pa,
    float envelope_temperature_c) {
  const float dp_abs = blower_test_absf(fan_pressure_pa);
  const float density =
      blower_test_air_density_kg_m3(config->altitude_m, envelope_temperature_c);
  const float density_factor =
      density > 0.0f ? sqrtf(BLOWER_TEST_SEA_LEVEL_AIR_DENSITY / density) : 1.0f;
  const float aperture_scale = blower_test_aperture_scale(config->fan_aperture_cm);

  if (dp_abs <= 0.0f || config->fan_curve_c <= 0.0f || config->fan_curve_n <= 0.0f) {
    return 0.0f;
  }

  return config->fan_curve_c * powf(dp_abs, config->fan_curve_n) * aperture_scale *
         density_factor;
}

static void blower_test_fill_default_config(blower_test_config_t *config) {
  static const float k_default_pressures[] = {65.0f, 58.0f, 50.0f, 42.0f,
                                              34.0f, 26.0f, 18.0f, 10.0f};
  size_t idx = 0u;

  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->building_volume_m3 = 126.7f;
  config->floor_area_m2 = 43.7f;
  config->envelope_area_m2 = 168.0f;
  config->building_height_m = 2.9f;
  config->dimensions_uncertainty_pct = 5.0f;
  config->altitude_m = 650.0f;
  config->fan_aperture_cm = 31.0f;
  config->fan_curve_c = APP_FAN_FLOW_COEFFICIENT_C;
  config->fan_curve_n = APP_FAN_FLOW_EXPONENT_N;
  config->target_tolerance_pa = 2.0f;
  config->settle_time_s = 8u;
  config->measure_time_s = 10u;
  config->reference_pressure_pa = 50u;
  config->min_points_required = BLOWER_TEST_DEFAULT_MIN_POINTS;
  config->enforce_iso_9972_rules = true;
  config->pressure_points_count = (uint8_t)(sizeof(k_default_pressures) /
                                            sizeof(k_default_pressures[0]));

  for (idx = 0u; idx < config->pressure_points_count; ++idx) {
    config->pressure_points_pa[idx] = k_default_pressures[idx];
  }
}

static void blower_test_sort_pressures_desc(float *values, uint8_t count) {
  uint8_t outer = 0u;
  uint8_t inner = 0u;

  if (values == NULL || count < 2u) {
    return;
  }

  for (outer = 0u; outer < count; ++outer) {
    for (inner = 0u; inner + 1u < count; ++inner) {
      if (values[inner] < values[inner + 1u]) {
        const float tmp = values[inner];
        values[inner] = values[inner + 1u];
        values[inner + 1u] = tmp;
      }
    }
  }
}

static bool blower_test_validate_and_normalize_config(blower_test_config_t *config) {
  uint8_t index = 0u;

  if (config == NULL) {
    return false;
  }
  if (!isfinite(config->building_volume_m3) || config->building_volume_m3 <= 1.0f) {
    return false;
  }
  if (!isfinite(config->floor_area_m2) || config->floor_area_m2 <= 1.0f) {
    return false;
  }
  if (!isfinite(config->envelope_area_m2) || config->envelope_area_m2 <= 1.0f) {
    return false;
  }
  if (!isfinite(config->building_height_m) || config->building_height_m <= 0.5f) {
    return false;
  }
  if (!isfinite(config->fan_curve_c) || config->fan_curve_c <= 0.0f) {
    return false;
  }
  if (!isfinite(config->fan_curve_n) || config->fan_curve_n <= 0.0f) {
    return false;
  }
  if (!isfinite(config->target_tolerance_pa)) {
    return false;
  }

  config->target_tolerance_pa = blower_test_clampf(
      config->target_tolerance_pa, BLOWER_TEST_MIN_TOLERANCE_PA,
      BLOWER_TEST_MAX_TOLERANCE_PA);
  config->settle_time_s = (uint16_t)blower_test_clampf(
      (float)config->settle_time_s, (float)BLOWER_TEST_MIN_SETTLE_TIME_S,
      (float)BLOWER_TEST_MAX_SETTLE_TIME_S);
  config->measure_time_s = (uint16_t)blower_test_clampf(
      (float)config->measure_time_s, (float)BLOWER_TEST_MIN_MEASURE_TIME_S,
      (float)BLOWER_TEST_MAX_MEASURE_TIME_S);
  config->fan_aperture_cm =
      blower_test_clampf(config->fan_aperture_cm, 5.0f, 60.0f);
  config->altitude_m = blower_test_clampf(config->altitude_m, 0.0f, 6000.0f);
  config->dimensions_uncertainty_pct =
      blower_test_clampf(config->dimensions_uncertainty_pct, 0.0f, 100.0f);

  if (config->reference_pressure_pa < 10u || config->reference_pressure_pa > 100u) {
    return false;
  }
  if (config->pressure_points_count == 0u ||
      config->pressure_points_count > BLOWER_TEST_MAX_PRESSURE_POINTS) {
    return false;
  }
  if (config->min_points_required == 0u ||
      config->min_points_required > BLOWER_TEST_MAX_PRESSURE_POINTS) {
    config->min_points_required = BLOWER_TEST_DEFAULT_MIN_POINTS;
  }

  for (index = 0u; index < config->pressure_points_count; ++index) {
    const float point = config->pressure_points_pa[index];
    if (!isfinite(point) || point < BLOWER_TEST_MIN_PRESSURE_PA ||
        point > BLOWER_TEST_MAX_PRESSURE_PA) {
      return false;
    }
  }

  blower_test_sort_pressures_desc(config->pressure_points_pa,
                                  config->pressure_points_count);

  if (config->enforce_iso_9972_rules &&
      config->pressure_points_count < config->min_points_required) {
    return false;
  }

  return true;
}

static void blower_test_set_state_locked(blower_test_state_t state,
                                         uint32_t now_tick_ms) {
  g_context.runtime.state = state;
  g_context.state_enter_tick_ms = now_tick_ms;
  g_context.runtime.state_elapsed_ms = 0u;
}

static blower_test_direction_report_t *blower_test_active_direction_report_locked(void) {
  if (g_context.runtime.current_direction ==
      BLOWER_TEST_DIRECTION_PRESSURIZATION) {
    return &g_context.active_report.pressurization;
  }

  if (g_context.runtime.current_direction ==
      BLOWER_TEST_DIRECTION_DEPRESSURIZATION) {
    return &g_context.active_report.depressurization;
  }

  return NULL;
}

static void blower_test_prepare_measurement_locked(uint32_t now_tick_ms) {
  g_context.acc_pressure_pa = 0.0f;
  g_context.acc_fan_flow_m3h = 0.0f;
  g_context.acc_fan_temp_c = 0.0f;
  g_context.acc_envelope_temp_c = 0.0f;
  g_context.acc_pwm_percent = 0.0f;
  g_context.acc_samples = 0u;
  g_context.measure_start_tick_ms = now_tick_ms;
  g_context.runtime.active_sample_count = 0u;
  blower_test_set_state_locked(BLOWER_TEST_STATE_MEASURING, now_tick_ms);
}

static void blower_test_abort_control_locked(void) {
  blower_control_set_mode(BLOWER_CONTROL_MODE_MANUAL_PERCENT);
  blower_control_set_relay_enabled(false);
  blower_control_set_manual_pwm_percent(0u);
}

static bool blower_test_compute_summary_from_direction(
    const blower_test_config_t *config,
    const blower_test_direction_report_t *direction_report,
    blower_test_curve_summary_t *out_summary) {
  float sum_x = 0.0f;
  float sum_y = 0.0f;
  float sum_x2 = 0.0f;
  float sum_y2 = 0.0f;
  float sum_xy = 0.0f;
  float sum_rel_err2 = 0.0f;
  uint8_t valid_count = 0u;
  uint8_t index = 0u;
  float slope_n = 0.0f;
  float intercept = 0.0f;
  float denominator = 0.0f;
  float correlation_denominator = 0.0f;
  float correlation_r = 0.0f;
  float cl = 0.0f;
  float q_ref = 0.0f;
  float q10_m3h = 0.0f;
  float q4_m3h = 0.0f;
  const float rho =
      blower_test_air_density_kg_m3(config->altitude_m, 20.0f);
  blower_test_curve_summary_t summary = {0};

  if (config == NULL || direction_report == NULL || out_summary == NULL) {
    return false;
  }

  for (index = 0u; index < direction_report->point_count; ++index) {
    const blower_test_point_result_t *point = &direction_report->points[index];
    if (!point->valid || point->avg_pressure_pa <= 0.0f ||
        point->avg_fan_flow_m3h <= 0.0f) {
      continue;
    }

    {
      const float x = logf(point->avg_pressure_pa);
      const float y = logf(point->avg_fan_flow_m3h);
      sum_x += x;
      sum_y += y;
      sum_x2 += x * x;
      sum_y2 += y * y;
      sum_xy += x * y;
      valid_count += 1u;
    }
  }

  if (valid_count < 2u ||
      (config->enforce_iso_9972_rules &&
       valid_count < config->min_points_required)) {
    return false;
  }

  denominator = (float)valid_count * sum_x2 - (sum_x * sum_x);
  if (fabsf(denominator) < 1e-6f) {
    return false;
  }

  slope_n = ((float)valid_count * sum_xy - (sum_x * sum_y)) / denominator;
  intercept = (sum_y - slope_n * sum_x) / (float)valid_count;
  cl = expf(intercept);

  correlation_denominator =
      ((float)valid_count * sum_x2 - (sum_x * sum_x)) *
      ((float)valid_count * sum_y2 - (sum_y * sum_y));
  if (correlation_denominator > 1e-9f) {
    correlation_r =
        ((float)valid_count * sum_xy - (sum_x * sum_y)) /
        sqrtf(correlation_denominator);
  }

  for (index = 0u; index < direction_report->point_count; ++index) {
    const blower_test_point_result_t *point = &direction_report->points[index];
    if (!point->valid || point->avg_pressure_pa <= 0.0f ||
        point->avg_fan_flow_m3h <= 0.0f) {
      continue;
    }

    {
      const float q_pred = cl * powf(point->avg_pressure_pa, slope_n);
      if (q_pred > 0.0f) {
        const float rel_err = (point->avg_fan_flow_m3h - q_pred) / q_pred;
        sum_rel_err2 += rel_err * rel_err;
      }
    }
  }

  q_ref = cl * powf((float)config->reference_pressure_pa, slope_n);
  q10_m3h = cl * powf(10.0f, slope_n);
  q4_m3h = cl * powf(4.0f, slope_n);

  summary.cl_m3h_pan = cl;
  summary.exponent_n = slope_n;
  summary.correlation_r = correlation_r;
  summary.q_ref_m3h = q_ref;
  summary.ach_ref_h1 =
      config->building_volume_m3 > 0.0f ? q_ref / config->building_volume_m3 : 0.0f;
  summary.w_ref_m3h_m2 =
      config->floor_area_m2 > 0.0f ? q_ref / config->floor_area_m2 : 0.0f;
  summary.q_ref_envelope_m3h_m2 =
      config->envelope_area_m2 > 0.0f ? q_ref / config->envelope_area_m2 : 0.0f;

  if (rho > 0.0f) {
    const float q10_m3s = q10_m3h / 3600.0f;
    const float q4_m3s = q4_m3h / 3600.0f;
    const float a10_m2 = q10_m3s / sqrtf((2.0f * 10.0f) / rho);
    const float a4_m2 = q4_m3s / sqrtf((2.0f * 4.0f) / rho);
    summary.eqla10_cm2 = a10_m2 * 10000.0f;
    summary.lbl_ela4_cm2 = a4_m2 * 10000.0f;
  }

  summary.eqla10_cm2_per_m2_envelope =
      config->envelope_area_m2 > 0.0f
          ? summary.eqla10_cm2 / config->envelope_area_m2
          : 0.0f;
  summary.lbl_ela4_cm2_per_m2_envelope =
      config->envelope_area_m2 > 0.0f
          ? summary.lbl_ela4_cm2 / config->envelope_area_m2
          : 0.0f;

  summary.uncertainty_pct =
      sqrtf(sum_rel_err2 / (float)valid_count) * 100.0f;
  summary.uncertainty_pct = sqrtf(summary.uncertainty_pct * summary.uncertainty_pct +
                                  config->dimensions_uncertainty_pct *
                                      config->dimensions_uncertainty_pct);
  summary.valid = true;

  *out_summary = summary;
  return true;
}

static void blower_test_compute_mean_summary_locked(void) {
  const bool has_press = g_context.active_report.has_pressurization &&
                         g_context.active_report.pressurization.summary.valid;
  const bool has_depress = g_context.active_report.has_depressurization &&
                           g_context.active_report.depressurization.summary.valid;
  blower_test_curve_summary_t mean = {0};

  if (!has_press && !has_depress) {
    g_context.active_report.mean_summary.valid = false;
    return;
  }

  if (has_press && has_depress) {
    const blower_test_curve_summary_t *press =
        &g_context.active_report.pressurization.summary;
    const blower_test_curve_summary_t *depress =
        &g_context.active_report.depressurization.summary;
    mean.cl_m3h_pan = (press->cl_m3h_pan + depress->cl_m3h_pan) * 0.5f;
    mean.exponent_n = (press->exponent_n + depress->exponent_n) * 0.5f;
    mean.correlation_r = (press->correlation_r + depress->correlation_r) * 0.5f;
    mean.q_ref_m3h = (press->q_ref_m3h + depress->q_ref_m3h) * 0.5f;
    mean.ach_ref_h1 = (press->ach_ref_h1 + depress->ach_ref_h1) * 0.5f;
    mean.w_ref_m3h_m2 = (press->w_ref_m3h_m2 + depress->w_ref_m3h_m2) * 0.5f;
    mean.q_ref_envelope_m3h_m2 =
        (press->q_ref_envelope_m3h_m2 + depress->q_ref_envelope_m3h_m2) * 0.5f;
    mean.eqla10_cm2 = (press->eqla10_cm2 + depress->eqla10_cm2) * 0.5f;
    mean.eqla10_cm2_per_m2_envelope =
        (press->eqla10_cm2_per_m2_envelope +
         depress->eqla10_cm2_per_m2_envelope) *
        0.5f;
    mean.lbl_ela4_cm2 = (press->lbl_ela4_cm2 + depress->lbl_ela4_cm2) * 0.5f;
    mean.lbl_ela4_cm2_per_m2_envelope =
        (press->lbl_ela4_cm2_per_m2_envelope +
         depress->lbl_ela4_cm2_per_m2_envelope) *
        0.5f;
    mean.uncertainty_pct =
        (press->uncertainty_pct + depress->uncertainty_pct) * 0.5f;
    if (mean.q_ref_m3h > 0.0f) {
      const float spread_pct =
          fabsf(press->q_ref_m3h - depress->q_ref_m3h) / mean.q_ref_m3h * 100.0f;
      mean.uncertainty_pct += spread_pct * 0.5f;
    }
    mean.valid = true;
  } else if (has_press) {
    mean = g_context.active_report.pressurization.summary;
  } else {
    mean = g_context.active_report.depressurization.summary;
  }

  g_context.active_report.mean_summary = mean;
}

static void blower_test_history_push_locked(const blower_test_report_t *report) {
  if (report == NULL) {
    return;
  }

  g_context.history[g_context.history_head] = *report;
  g_context.history_head = (uint8_t)((g_context.history_head + 1u) %
                                     BLOWER_TEST_HISTORY_CAPACITY);
  if (g_context.history_count < BLOWER_TEST_HISTORY_CAPACITY) {
    g_context.history_count += 1u;
  }
}

static bool blower_test_persist_locked(void) {
  blower_test_persistent_blob_t blob = {0};

  if (!g_context.persistence_available) {
    return false;
  }

  blob.magic = BLOWER_TEST_STORAGE_MAGIC;
  blob.version = BLOWER_TEST_STORAGE_VERSION;
  blob.payload_size = sizeof(blob);
  blob.sequence = g_context.next_report_id;
  blob.config = g_context.config;
  blob.history_count = g_context.history_count;
  blob.history_head = g_context.history_head;
  memcpy(blob.history, g_context.history, sizeof(blob.history));
  blob.crc32 = blower_test_crc32_for_blob(&blob);
  return blower_test_storage_program(&blob);
}

static void blower_test_load_from_storage_or_defaults_locked(void) {
  blower_test_persistent_blob_t blob = {0};
  blower_test_config_t default_config = {0};
  bool loaded = false;

  blower_test_fill_default_config(&default_config);
  g_context.config = default_config;
  g_context.history_count = 0u;
  g_context.history_head = 0u;
  g_context.has_latest_report = false;
  g_context.next_report_id = 1u;

  if (!g_context.persistence_available) {
    return;
  }

  loaded = blower_test_storage_load(&blob);
  if (!loaded) {
    (void)blower_test_persist_locked();
    return;
  }

  if (blower_test_validate_and_normalize_config(&blob.config)) {
    g_context.config = blob.config;
  }

  g_context.history_count = blob.history_count <= BLOWER_TEST_HISTORY_CAPACITY
                                ? blob.history_count
                                : BLOWER_TEST_HISTORY_CAPACITY;
  g_context.history_head = blob.history_head % BLOWER_TEST_HISTORY_CAPACITY;
  memcpy(g_context.history, blob.history, sizeof(g_context.history));

  if (g_context.history_count > 0u) {
    const uint8_t last_index =
        (uint8_t)((g_context.history_head + BLOWER_TEST_HISTORY_CAPACITY - 1u) %
                  BLOWER_TEST_HISTORY_CAPACITY);
    g_context.latest_report = g_context.history[last_index];
    g_context.has_latest_report = true;
    g_context.next_report_id = g_context.latest_report.report_id + 1u;
  }

  if (blob.sequence >= g_context.next_report_id) {
    g_context.next_report_id = blob.sequence + 1u;
  }
}

static void blower_test_reset_runtime_locked(void) {
  g_context.runtime = (blower_test_runtime_status_t){
      .active = false,
      .state = BLOWER_TEST_STATE_IDLE,
      .requested_mode = BLOWER_TEST_MODE_BOTH,
      .current_direction = BLOWER_TEST_DIRECTION_NONE,
      .current_point_index = 0u,
      .total_points = 0u,
      .current_target_pressure_pa = 0.0f,
      .current_measured_pressure_pa = 0.0f,
      .current_measured_flow_m3h = 0.0f,
      .state_elapsed_ms = 0u,
      .active_sample_count = 0u,
      .report_ready = g_context.has_latest_report,
      .latest_report_id = g_context.has_latest_report
                              ? g_context.latest_report.report_id
                              : 0u,
      .latest_ach_ref_h1 =
          g_context.has_latest_report && g_context.latest_report.mean_summary.valid
              ? g_context.latest_report.mean_summary.ach_ref_h1
              : 0.0f,
  };
  g_context.acc_pressure_pa = 0.0f;
  g_context.acc_fan_flow_m3h = 0.0f;
  g_context.acc_fan_temp_c = 0.0f;
  g_context.acc_envelope_temp_c = 0.0f;
  g_context.acc_pwm_percent = 0.0f;
  g_context.acc_samples = 0u;
  g_context.state_enter_tick_ms = 0u;
  g_context.stable_since_tick_ms = 0u;
  g_context.measure_start_tick_ms = 0u;
  g_context.direction_count = 0u;
  g_context.direction_slot = 0u;
  g_context.direction_sequence[0] = BLOWER_TEST_DIRECTION_NONE;
  g_context.direction_sequence[1] = BLOWER_TEST_DIRECTION_NONE;
}

void blower_test_service_init(void) {
  if (g_context.initialized) {
    return;
  }

  memset(&g_context, 0, sizeof(g_context));
  g_context.mutex = xSemaphoreCreateMutex();
  if (g_context.mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  g_context.persistence_available = blower_test_storage_layout_is_valid();
  blower_test_load_from_storage_or_defaults_locked();
  blower_test_reset_runtime_locked();
  g_context.initialized = true;

  xSemaphoreGive(g_context.mutex);
}

void blower_test_service_get_config(blower_test_config_t *out_config) {
  if (out_config == NULL || g_context.mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  *out_config = g_context.config;
  xSemaphoreGive(g_context.mutex);
}

bool blower_test_service_set_config(const blower_test_config_t *config) {
  blower_test_config_t normalized = {0};
  bool persisted_ok = true;

  if (config == NULL || g_context.mutex == NULL) {
    return false;
  }

  normalized = *config;
  if (!blower_test_validate_and_normalize_config(&normalized)) {
    return false;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (g_context.runtime.active) {
    xSemaphoreGive(g_context.mutex);
    return false;
  }

  g_context.config = normalized;
  if (g_context.persistence_available) {
    persisted_ok = blower_test_persist_locked();
  }

  xSemaphoreGive(g_context.mutex);
  return persisted_ok;
}

void blower_test_service_reset_config_to_defaults(void) {
  blower_test_config_t defaults = {0};

  if (g_context.mutex == NULL) {
    return;
  }

  blower_test_fill_default_config(&defaults);

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (!g_context.runtime.active) {
    g_context.config = defaults;
    if (g_context.persistence_available) {
      (void)blower_test_persist_locked();
    }
  }

  xSemaphoreGive(g_context.mutex);
}

static void blower_test_setup_mode_sequence_locked(blower_test_mode_t mode) {
  g_context.direction_count = 0u;
  g_context.direction_slot = 0u;
  g_context.direction_sequence[0] = BLOWER_TEST_DIRECTION_NONE;
  g_context.direction_sequence[1] = BLOWER_TEST_DIRECTION_NONE;

  if (mode == BLOWER_TEST_MODE_PRESSURIZATION) {
    g_context.direction_sequence[0] = BLOWER_TEST_DIRECTION_PRESSURIZATION;
    g_context.direction_count = 1u;
  } else if (mode == BLOWER_TEST_MODE_DEPRESSURIZATION) {
    g_context.direction_sequence[0] = BLOWER_TEST_DIRECTION_DEPRESSURIZATION;
    g_context.direction_count = 1u;
  } else {
    g_context.direction_sequence[0] = BLOWER_TEST_DIRECTION_PRESSURIZATION;
    g_context.direction_sequence[1] = BLOWER_TEST_DIRECTION_DEPRESSURIZATION;
    g_context.direction_count = 2u;
  }
}

bool blower_test_service_start(blower_test_mode_t mode) {
  bool can_start = false;

  if (g_context.mutex == NULL) {
    return false;
  }

  if (mode != BLOWER_TEST_MODE_PRESSURIZATION &&
      mode != BLOWER_TEST_MODE_DEPRESSURIZATION &&
      mode != BLOWER_TEST_MODE_BOTH) {
    return false;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (g_context.runtime.active || g_context.config.pressure_points_count == 0u ||
      (g_context.config.enforce_iso_9972_rules &&
       g_context.config.pressure_points_count <
           g_context.config.min_points_required)) {
    xSemaphoreGive(g_context.mutex);
    return false;
  }

  memset(&g_context.active_report, 0, sizeof(g_context.active_report));
  g_context.active_report.report_id = g_context.next_report_id++;
  g_context.active_report.reference_pressure_pa = g_context.config.reference_pressure_pa;
  g_context.active_report.completed_tick_ms = 0u;

  blower_test_setup_mode_sequence_locked(mode);

  g_context.runtime.active = true;
  g_context.runtime.requested_mode = mode;
  g_context.runtime.current_direction = g_context.direction_sequence[0];
  g_context.runtime.current_point_index = 0u;
  g_context.runtime.total_points = g_context.config.pressure_points_count;
  g_context.runtime.current_target_pressure_pa =
      g_context.config.pressure_points_pa[0];
  g_context.runtime.current_measured_pressure_pa = 0.0f;
  g_context.runtime.current_measured_flow_m3h = 0.0f;
  g_context.runtime.active_sample_count = 0u;
  g_context.runtime.report_ready = g_context.has_latest_report;
  g_context.runtime.latest_report_id =
      g_context.has_latest_report ? g_context.latest_report.report_id : 0u;
  g_context.runtime.latest_ach_ref_h1 =
      g_context.has_latest_report && g_context.latest_report.mean_summary.valid
          ? g_context.latest_report.mean_summary.ach_ref_h1
          : 0.0f;

  g_context.stable_since_tick_ms = 0u;
  g_context.measure_start_tick_ms = 0u;
  g_context.acc_samples = 0u;
  blower_test_set_state_locked(BLOWER_TEST_STATE_PREPARING, 0u);

  blower_control_set_mode(BLOWER_CONTROL_MODE_AUTO_TEST);
  blower_control_set_relay_enabled(true);

  can_start = true;
  xSemaphoreGive(g_context.mutex);
  return can_start;
}

void blower_test_service_stop(void) {
  const uint32_t now_tick_ms =
      (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;

  if (g_context.mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (g_context.runtime.active) {
    g_context.runtime.active = false;
    blower_test_set_state_locked(BLOWER_TEST_STATE_ABORTED, now_tick_ms);
    blower_test_abort_control_locked();
  }

  xSemaphoreGive(g_context.mutex);
}

static void blower_test_finalize_direction_locked(
    blower_test_direction_report_t *direction_report) {
  if (direction_report == NULL) {
    return;
  }

  (void)blower_test_compute_summary_from_direction(&g_context.config,
                                                   direction_report,
                                                   &direction_report->summary);

  if (direction_report->direction == BLOWER_TEST_DIRECTION_PRESSURIZATION) {
    g_context.active_report.has_pressurization = direction_report->summary.valid;
  } else if (direction_report->direction ==
             BLOWER_TEST_DIRECTION_DEPRESSURIZATION) {
    g_context.active_report.has_depressurization = direction_report->summary.valid;
  }
}

static void blower_test_advance_to_next_target_locked(uint32_t now_tick_ms) {
  blower_test_direction_report_t *direction_report =
      blower_test_active_direction_report_locked();

  if (direction_report == NULL) {
    g_context.runtime.active = false;
    blower_test_set_state_locked(BLOWER_TEST_STATE_ERROR, now_tick_ms);
    blower_test_abort_control_locked();
    return;
  }

  if (g_context.runtime.current_point_index + 1u <
      g_context.config.pressure_points_count) {
    g_context.runtime.current_point_index += 1u;
    g_context.runtime.current_target_pressure_pa =
        g_context.config
            .pressure_points_pa[g_context.runtime.current_point_index];
    g_context.stable_since_tick_ms = 0u;
    blower_test_set_state_locked(BLOWER_TEST_STATE_PREPARING, now_tick_ms);
    return;
  }

  blower_test_finalize_direction_locked(direction_report);

  if (g_context.direction_slot + 1u < g_context.direction_count) {
    g_context.direction_slot += 1u;
    g_context.runtime.current_direction =
        g_context.direction_sequence[g_context.direction_slot];
    g_context.runtime.current_point_index = 0u;
    g_context.runtime.current_target_pressure_pa =
        g_context.config.pressure_points_pa[0];
    g_context.stable_since_tick_ms = 0u;
    g_context.measure_start_tick_ms = 0u;
    g_context.acc_samples = 0u;
    blower_test_set_state_locked(BLOWER_TEST_STATE_PREPARING, now_tick_ms);
    return;
  }

  blower_test_compute_mean_summary_locked();
  g_context.active_report.completed_tick_ms = now_tick_ms;
  g_context.latest_report = g_context.active_report;
  g_context.has_latest_report = true;
  blower_test_history_push_locked(&g_context.latest_report);
  if (g_context.persistence_available) {
    (void)blower_test_persist_locked();
  }

  g_context.runtime.active = false;
  g_context.runtime.report_ready = true;
  g_context.runtime.latest_report_id = g_context.latest_report.report_id;
  g_context.runtime.latest_ach_ref_h1 =
      g_context.latest_report.mean_summary.valid
          ? g_context.latest_report.mean_summary.ach_ref_h1
          : 0.0f;
  blower_test_set_state_locked(BLOWER_TEST_STATE_COMPLETED, now_tick_ms);
  blower_test_abort_control_locked();
}

void blower_test_service_update(const blower_metrics_snapshot_t *metrics_snapshot,
                                const blower_control_snapshot_t *control_snapshot,
                                uint32_t now_tick_ms) {
  float envelope_pressure_pa = 0.0f;
  float fan_flow_m3h = 0.0f;
  bool envelope_valid = false;
  bool fan_valid = false;
  float pwm_percent = 0.0f;

  if (g_context.mutex == NULL || metrics_snapshot == NULL ||
      control_snapshot == NULL) {
    return;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (!g_context.runtime.active) {
    xSemaphoreGive(g_context.mutex);
    return;
  }

  envelope_valid = metrics_snapshot->envelope_sample_valid;
  fan_valid = metrics_snapshot->fan_sample_valid;
  envelope_pressure_pa = blower_test_absf(metrics_snapshot->envelope_pressure_pa);
  fan_flow_m3h = blower_test_compute_fan_flow_m3h(
      &g_context.config, metrics_snapshot->fan_pressure_pa,
      metrics_snapshot->envelope_temperature_c);
  pwm_percent = (float)control_snapshot->output_pwm_percent;

  g_context.runtime.current_measured_pressure_pa = envelope_pressure_pa;
  g_context.runtime.current_measured_flow_m3h = fan_flow_m3h;
  g_context.runtime.state_elapsed_ms = now_tick_ms - g_context.state_enter_tick_ms;

  if (g_context.runtime.state == BLOWER_TEST_STATE_PREPARING) {
    const float target =
        g_context.config
            .pressure_points_pa[g_context.runtime.current_point_index];
    blower_control_set_target_pressure_pa(target);
    g_context.runtime.current_target_pressure_pa = target;
    g_context.stable_since_tick_ms = 0u;
    blower_test_set_state_locked(BLOWER_TEST_STATE_STABILIZING, now_tick_ms);
    xSemaphoreGive(g_context.mutex);
    return;
  }

  if (g_context.runtime.state == BLOWER_TEST_STATE_STABILIZING) {
    if (!envelope_valid) {
      g_context.stable_since_tick_ms = 0u;
      xSemaphoreGive(g_context.mutex);
      return;
    }

    if (fabsf(envelope_pressure_pa - g_context.runtime.current_target_pressure_pa) <=
        g_context.config.target_tolerance_pa) {
      if (g_context.stable_since_tick_ms == 0u) {
        g_context.stable_since_tick_ms = now_tick_ms;
      }

      if ((now_tick_ms - g_context.stable_since_tick_ms) >=
          ((uint32_t)g_context.config.settle_time_s * 1000u)) {
        blower_test_prepare_measurement_locked(now_tick_ms);
      }
    } else {
      g_context.stable_since_tick_ms = 0u;
    }

    xSemaphoreGive(g_context.mutex);
    return;
  }

  if (g_context.runtime.state != BLOWER_TEST_STATE_MEASURING) {
    xSemaphoreGive(g_context.mutex);
    return;
  }

  if (envelope_valid && fan_valid) {
    g_context.acc_pressure_pa += envelope_pressure_pa;
    g_context.acc_fan_flow_m3h += fan_flow_m3h;
    g_context.acc_fan_temp_c += metrics_snapshot->fan_temperature_c;
    g_context.acc_envelope_temp_c += metrics_snapshot->envelope_temperature_c;
    g_context.acc_pwm_percent += pwm_percent;
    g_context.acc_samples += 1u;
    g_context.runtime.active_sample_count = g_context.acc_samples;
  }

  if ((now_tick_ms - g_context.measure_start_tick_ms) <
      ((uint32_t)g_context.config.measure_time_s * 1000u)) {
    xSemaphoreGive(g_context.mutex);
    return;
  }

  {
    blower_test_direction_report_t *direction_report =
        blower_test_active_direction_report_locked();
    blower_test_point_result_t *point = NULL;

    if (direction_report == NULL ||
        g_context.runtime.current_point_index >= BLOWER_TEST_MAX_PRESSURE_POINTS) {
      g_context.runtime.active = false;
      blower_test_set_state_locked(BLOWER_TEST_STATE_ERROR, now_tick_ms);
      blower_test_abort_control_locked();
      xSemaphoreGive(g_context.mutex);
      return;
    }

    if (direction_report->direction == BLOWER_TEST_DIRECTION_NONE) {
      direction_report->direction = g_context.runtime.current_direction;
    }

    if (direction_report->point_count <= g_context.runtime.current_point_index) {
      direction_report->point_count = g_context.runtime.current_point_index + 1u;
    }

    point = &direction_report->points[g_context.runtime.current_point_index];
    point->target_pressure_pa = g_context.runtime.current_target_pressure_pa;
    point->sample_count = g_context.acc_samples;
    point->valid = g_context.acc_samples > 0u;

    if (point->valid) {
      const float sample_count_f = (float)g_context.acc_samples;
      point->avg_pressure_pa = g_context.acc_pressure_pa / sample_count_f;
      point->avg_fan_flow_m3h = g_context.acc_fan_flow_m3h / sample_count_f;
      point->avg_fan_temperature_c = g_context.acc_fan_temp_c / sample_count_f;
      point->avg_envelope_temperature_c =
          g_context.acc_envelope_temp_c / sample_count_f;
      point->avg_pwm_percent = g_context.acc_pwm_percent / sample_count_f;
    } else {
      point->avg_pressure_pa = 0.0f;
      point->avg_fan_flow_m3h = 0.0f;
      point->avg_fan_temperature_c = 0.0f;
      point->avg_envelope_temperature_c = 0.0f;
      point->avg_pwm_percent = 0.0f;
    }
  }

  blower_test_advance_to_next_target_locked(now_tick_ms);
  xSemaphoreGive(g_context.mutex);
}

void blower_test_service_get_runtime(blower_test_runtime_status_t *out_runtime) {
  if (out_runtime == NULL || g_context.mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  *out_runtime = g_context.runtime;
  xSemaphoreGive(g_context.mutex);
}

bool blower_test_service_get_latest_report(blower_test_report_t *out_report) {
  bool has_report = false;

  if (out_report == NULL || g_context.mutex == NULL) {
    return false;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  has_report = g_context.has_latest_report;
  if (has_report) {
    *out_report = g_context.latest_report;
  }

  xSemaphoreGive(g_context.mutex);
  return has_report;
}

bool blower_test_service_get_report_snapshot(blower_test_report_t *out_report,
                                             bool *out_is_active) {
  bool has_report = false;

  if (out_report == NULL || g_context.mutex == NULL) {
    return false;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  if (g_context.runtime.active) {
    *out_report = g_context.active_report;
    has_report = true;
    if (out_is_active != NULL) {
      *out_is_active = true;
    }
  } else if (g_context.has_latest_report) {
    *out_report = g_context.latest_report;
    has_report = true;
    if (out_is_active != NULL) {
      *out_is_active = false;
    }
  }

  xSemaphoreGive(g_context.mutex);
  return has_report;
}

const char *blower_test_mode_name(blower_test_mode_t mode) {
  switch (mode) {
  case BLOWER_TEST_MODE_PRESSURIZATION:
    return "pressurization";
  case BLOWER_TEST_MODE_DEPRESSURIZATION:
    return "depressurization";
  case BLOWER_TEST_MODE_BOTH:
    return "both";
  default:
    return "unknown";
  }
}

const char *blower_test_state_name(blower_test_state_t state) {
  switch (state) {
  case BLOWER_TEST_STATE_IDLE:
    return "idle";
  case BLOWER_TEST_STATE_PREPARING:
    return "preparing";
  case BLOWER_TEST_STATE_STABILIZING:
    return "stabilizing";
  case BLOWER_TEST_STATE_MEASURING:
    return "measuring";
  case BLOWER_TEST_STATE_COMPLETED:
    return "completed";
  case BLOWER_TEST_STATE_ABORTED:
    return "aborted";
  case BLOWER_TEST_STATE_ERROR:
    return "error";
  default:
    return "unknown";
  }
}

const char *blower_test_direction_name(blower_test_direction_t direction) {
  switch (direction) {
  case BLOWER_TEST_DIRECTION_PRESSURIZATION:
    return "pressurization";
  case BLOWER_TEST_DIRECTION_DEPRESSURIZATION:
    return "depressurization";
  default:
    return "none";
  }
}
