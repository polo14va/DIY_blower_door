#ifndef BLOWER_TEST_SERVICE_H
#define BLOWER_TEST_SERVICE_H

#include "services/blower_control.h"
#include "services/blower_metrics.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOWER_TEST_MAX_PRESSURE_POINTS 12u
#define BLOWER_TEST_HISTORY_CAPACITY 4u

typedef enum {
  BLOWER_TEST_MODE_PRESSURIZATION = 0,
  BLOWER_TEST_MODE_DEPRESSURIZATION = 1,
  BLOWER_TEST_MODE_BOTH = 2,
} blower_test_mode_t;

typedef enum {
  BLOWER_TEST_DIRECTION_NONE = 0,
  BLOWER_TEST_DIRECTION_PRESSURIZATION = 1,
  BLOWER_TEST_DIRECTION_DEPRESSURIZATION = 2,
} blower_test_direction_t;

typedef enum {
  BLOWER_TEST_STATE_IDLE = 0,
  BLOWER_TEST_STATE_PREPARING,
  BLOWER_TEST_STATE_STABILIZING,
  BLOWER_TEST_STATE_MEASURING,
  BLOWER_TEST_STATE_COMPLETED,
  BLOWER_TEST_STATE_ABORTED,
  BLOWER_TEST_STATE_ERROR,
} blower_test_state_t;

typedef struct {
  float building_volume_m3;
  float floor_area_m2;
  float envelope_area_m2;
  float building_height_m;
  float dimensions_uncertainty_pct;
  float altitude_m;
  float fan_aperture_cm;
  float fan_curve_c;
  float fan_curve_n;
  float target_tolerance_pa;
  uint16_t settle_time_s;
  uint16_t measure_time_s;
  uint8_t reference_pressure_pa;
  uint8_t min_points_required;
  bool enforce_iso_9972_rules;
  uint8_t pressure_points_count;
  float pressure_points_pa[BLOWER_TEST_MAX_PRESSURE_POINTS];
} blower_test_config_t;

typedef struct {
  float target_pressure_pa;
  float avg_pressure_pa;
  float avg_fan_flow_m3h;
  float avg_fan_temperature_c;
  float avg_envelope_temperature_c;
  float avg_pwm_percent;
  uint16_t sample_count;
  bool valid;
} blower_test_point_result_t;

typedef struct {
  float cl_m3h_pan;
  float exponent_n;
  float correlation_r;
  float q_ref_m3h;
  float ach_ref_h1;
  float w_ref_m3h_m2;
  float q_ref_envelope_m3h_m2;
  float eqla10_cm2;
  float eqla10_cm2_per_m2_envelope;
  float lbl_ela4_cm2;
  float lbl_ela4_cm2_per_m2_envelope;
  float uncertainty_pct;
  bool valid;
} blower_test_curve_summary_t;

typedef struct {
  blower_test_direction_t direction;
  uint8_t point_count;
  blower_test_point_result_t points[BLOWER_TEST_MAX_PRESSURE_POINTS];
  blower_test_curve_summary_t summary;
} blower_test_direction_report_t;

typedef struct {
  uint32_t report_id;
  uint32_t completed_tick_ms;
  uint8_t reference_pressure_pa;
  bool has_pressurization;
  bool has_depressurization;
  blower_test_direction_report_t pressurization;
  blower_test_direction_report_t depressurization;
  blower_test_curve_summary_t mean_summary;
} blower_test_report_t;

typedef struct {
  bool active;
  blower_test_state_t state;
  blower_test_mode_t requested_mode;
  blower_test_direction_t current_direction;
  uint8_t current_point_index;
  uint8_t total_points;
  float current_target_pressure_pa;
  float current_measured_pressure_pa;
  float current_measured_flow_m3h;
  uint32_t state_elapsed_ms;
  uint16_t active_sample_count;
  bool report_ready;
  uint32_t latest_report_id;
  float latest_ach_ref_h1;
} blower_test_runtime_status_t;

void blower_test_service_init(void);

void blower_test_service_get_config(blower_test_config_t *out_config);
bool blower_test_service_set_config(const blower_test_config_t *config);
void blower_test_service_reset_config_to_defaults(void);

bool blower_test_service_start(blower_test_mode_t mode);
void blower_test_service_stop(void);

void blower_test_service_update(const blower_metrics_snapshot_t *metrics_snapshot,
                                const blower_control_snapshot_t *control_snapshot,
                                uint32_t now_tick_ms);

void blower_test_service_get_runtime(blower_test_runtime_status_t *out_runtime);
bool blower_test_service_get_latest_report(blower_test_report_t *out_report);
bool blower_test_service_get_report_snapshot(blower_test_report_t *out_report,
                                             bool *out_is_active);

const char *blower_test_mode_name(blower_test_mode_t mode);
const char *blower_test_state_name(blower_test_state_t state);
const char *blower_test_direction_name(blower_test_direction_t direction);

#endif
