#ifndef BLOWER_CONTROL_H
#define BLOWER_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BLOWER_CONTROL_MODE_MANUAL_PERCENT = 0,
  BLOWER_CONTROL_MODE_SEMI_AUTO_TARGET = 1,
  BLOWER_CONTROL_MODE_AUTO_TEST = 2,
} blower_control_mode_t;

typedef struct {
  uint8_t manual_pwm_percent;
  uint8_t output_pwm_percent;
  blower_control_mode_t mode;
  bool auto_hold_enabled;
  bool relay_enabled;
  float target_pressure_pa;
  float pd_kp;
  float pd_kd;
  float pd_deadband_pa;
  float pd_max_step_percent;
  bool line_sync;
  float line_frequency_hz;
} blower_control_snapshot_t;

void blower_control_initialize(void);
void blower_control_set_manual_pwm_percent(uint8_t pwm_percent);
void blower_control_set_mode(blower_control_mode_t mode);
void blower_control_set_auto_hold_enabled(bool enabled);
void blower_control_set_relay_enabled(bool enabled);
void blower_control_set_target_pressure_pa(float target_pressure_pa);

uint8_t blower_control_step(float envelope_pressure_pa, bool measurement_valid,
                            uint32_t now_tick_ms);
void blower_control_update_line_feedback(bool line_sync, float line_frequency_hz);
void blower_control_get_snapshot(blower_control_snapshot_t *out_snapshot);

#endif
