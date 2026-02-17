#include "services/blower_control.h"

#include "app/app_config.h"
#include "hardware/sync.h"
#include <math.h>

typedef struct {
  bool initialized;
  uint8_t manual_pwm_percent;
  uint8_t output_pwm_percent;
  blower_control_mode_t mode;
  bool auto_hold_enabled;
  bool relay_enabled;
  float target_pressure_pa;
  float pd_kp;
  float pid_ki;
  float pd_kd;
  float pd_deadband_pa;
  float pd_max_step_percent;
  float pd_max_step_down_percent;
  float integral_error_pa_s;
  float gain_scale;
  float last_error_pa;
  uint32_t last_tick_ms;
  bool has_last_error;
  float filtered_pressure_pa;
  bool has_filtered_pressure;
  bool learning_active;
  uint32_t learning_start_tick_ms;
  uint16_t learning_stable_cycles;
  float learned_feedforward_pwm;
  bool has_learned_feedforward_pwm;
  bool startup_boost_active;
  uint32_t startup_boost_start_tick_ms;
  bool line_sync;
  float line_frequency_hz;
} blower_control_state_t;

static blower_control_state_t g_state;

static float blower_control_clampf(float value, float min_value,
                                   float max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static float blower_control_lerpf(float from, float to, float ratio) {
  return from + (to - from) * blower_control_clampf(ratio, 0.0f, 1.0f);
}

static void blower_control_reset_pd_terms(blower_control_state_t *state) {
  state->integral_error_pa_s = 0.0f;
  state->last_error_pa = 0.0f;
  state->last_tick_ms = 0u;
  state->has_last_error = false;
}

static void blower_control_reset_pd_state(blower_control_state_t *state) {
  const float gain_scale_min =
      blower_control_clampf(APP_CONTROL_GAIN_SCALE_MIN, 0.05f, 1.0f);

  blower_control_reset_pd_terms(state);
  state->filtered_pressure_pa = 0.0f;
  state->has_filtered_pressure = false;
  state->gain_scale = gain_scale_min;
  state->learning_active = true;
  state->learning_start_tick_ms = 0u;
  state->learning_stable_cycles = 0u;
  state->learned_feedforward_pwm = (float)state->output_pwm_percent;
  state->has_learned_feedforward_pwm = false;
}

static float blower_control_filter_pressure(blower_control_state_t *state,
                                            float measured_pressure_pa) {
  const float alpha =
      blower_control_clampf(APP_CONTROL_MEASUREMENT_FILTER_ALPHA, 0.01f, 1.0f);

  if (!state->has_filtered_pressure) {
    state->filtered_pressure_pa = measured_pressure_pa;
    state->has_filtered_pressure = true;
  } else {
    state->filtered_pressure_pa +=
        alpha * (measured_pressure_pa - state->filtered_pressure_pa);
  }

  return state->filtered_pressure_pa;
}

static float blower_control_compute_step_scale(float error_pa) {
  const float far_error_pa = APP_CONTROL_STEP_FAR_ERROR_PA > 0.1f
                                 ? APP_CONTROL_STEP_FAR_ERROR_PA
                                 : 0.1f;
  const float near_ratio = blower_control_clampf(
      APP_CONTROL_STEP_NEAR_TARGET_RATIO, 0.05f, 1.0f);
  const float normalized_error = fabsf(error_pa) / far_error_pa;

  return blower_control_lerpf(near_ratio, 1.0f, normalized_error);
}

static void blower_control_update_learning_state(blower_control_state_t *state,
                                                 float error_pa,
                                                 float derivative_pa_per_s,
                                                 uint32_t now_tick_ms) {
  const float settle_band =
      blower_control_clampf(APP_CONTROL_LEARNING_SETTLE_BAND_PA, 0.5f, 10.0f);
  const float max_settle_derivative = blower_control_clampf(
      APP_CONTROL_LEARNING_MAX_DERIVATIVE_PA_PER_S, 0.5f, 20.0f);
  const float gain_scale_min =
      blower_control_clampf(APP_CONTROL_GAIN_SCALE_MIN, 0.05f, 1.0f);
  const float gain_scale_max =
      blower_control_clampf(APP_CONTROL_GAIN_SCALE_MAX, gain_scale_min, 2.0f);
  const float gain_growth = blower_control_clampf(
      APP_CONTROL_GAIN_SCALE_GROWTH, 0.0001f, 0.05f);
  const bool in_settle_zone =
      fabsf(error_pa) <= settle_band &&
      fabsf(derivative_pa_per_s) <= max_settle_derivative;

  if (state->learning_start_tick_ms == 0u) {
    state->learning_start_tick_ms = now_tick_ms;
  }

  if (state->learning_active) {
    if (in_settle_zone) {
      if (state->learning_stable_cycles < 65535u) {
        state->learning_stable_cycles += 1u;
      }
      if (!state->has_learned_feedforward_pwm) {
        state->learned_feedforward_pwm = (float)state->output_pwm_percent;
        state->has_learned_feedforward_pwm = true;
      } else {
        const float ff_alpha = blower_control_clampf(
            APP_CONTROL_LEARNING_FEEDFORWARD_ALPHA, 0.01f, 0.5f);
        state->learned_feedforward_pwm +=
            ff_alpha *
            ((float)state->output_pwm_percent - state->learned_feedforward_pwm);
      }
      state->gain_scale += gain_growth * 2.0f;
    } else {
      state->learning_stable_cycles = 0u;
      state->gain_scale += gain_growth;
    }

    if ((now_tick_ms - state->learning_start_tick_ms) >=
            APP_CONTROL_LEARNING_WINDOW_MS ||
        state->learning_stable_cycles >= APP_CONTROL_LEARNING_STABLE_CYCLES) {
      state->learning_active = false;
    }
  } else {
    state->gain_scale += gain_growth;
  }

  state->gain_scale =
      blower_control_clampf(state->gain_scale, gain_scale_min, gain_scale_max);
}

static void blower_control_initialize_defaults(blower_control_state_t *state) {
  *state = (blower_control_state_t){
      .initialized = true,
      .manual_pwm_percent = 0u,
      .output_pwm_percent = 0u,
      .mode = BLOWER_CONTROL_MODE_MANUAL_PERCENT,
      .auto_hold_enabled = false,
      .relay_enabled = false,
      .target_pressure_pa = APP_CONTROL_TARGET_PRESSURE_PA,
      .pd_kp = APP_CONTROL_PD_KP,
      .pid_ki = APP_CONTROL_PID_KI,
      .pd_kd = APP_CONTROL_PD_KD,
      .pd_deadband_pa = APP_CONTROL_PD_DEADBAND_PA,
      .pd_max_step_percent = APP_CONTROL_MAX_STEP_UP_PERCENT,
      .pd_max_step_down_percent = APP_CONTROL_MAX_STEP_DOWN_PERCENT,
      .integral_error_pa_s = 0.0f,
      .gain_scale = APP_CONTROL_GAIN_SCALE_MIN,
      .last_error_pa = 0.0f,
      .last_tick_ms = 0u,
      .has_last_error = false,
      .filtered_pressure_pa = 0.0f,
      .has_filtered_pressure = false,
      .learning_active = true,
      .learning_start_tick_ms = 0u,
      .learning_stable_cycles = 0u,
      .learned_feedforward_pwm = 0.0f,
      .has_learned_feedforward_pwm = false,
      .startup_boost_active = true,
      .startup_boost_start_tick_ms = 0u,
      .line_sync = false,
      .line_frequency_hz = 0.0f,
  };
}

static void blower_control_ensure_initialized_locked(void) {
  if (!g_state.initialized) {
    blower_control_initialize_defaults(&g_state);
  }
}

static void blower_control_apply_mode_locked(blower_control_state_t *state,
                                             blower_control_mode_t mode) {
  const bool auto_hold_enabled = mode != BLOWER_CONTROL_MODE_MANUAL_PERCENT;
  const bool mode_changed = state->mode != mode;
  const bool auto_hold_changed = state->auto_hold_enabled != auto_hold_enabled;

  if (!mode_changed && !auto_hold_changed) {
    return;
  }

  state->mode = mode;
  state->auto_hold_enabled = auto_hold_enabled;
  blower_control_reset_pd_state(state);
  state->startup_boost_active = auto_hold_enabled;
  state->startup_boost_start_tick_ms = 0u;

  if (auto_hold_enabled) {
    state->output_pwm_percent = state->manual_pwm_percent;
    state->learned_feedforward_pwm = (float)state->output_pwm_percent;
    state->has_learned_feedforward_pwm = false;
  } else if (state->relay_enabled) {
    state->output_pwm_percent = state->manual_pwm_percent;
  }
}

void blower_control_initialize(void) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_initialize_defaults(&g_state);
  restore_interrupts(irq_state);
}

void blower_control_set_manual_pwm_percent(uint8_t pwm_percent) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_ensure_initialized_locked();
  g_state.manual_pwm_percent = pwm_percent <= 100u ? pwm_percent : 100u;
  if (!g_state.auto_hold_enabled && g_state.relay_enabled) {
    g_state.output_pwm_percent = g_state.manual_pwm_percent;
  }
  restore_interrupts(irq_state);
}

void blower_control_set_mode(blower_control_mode_t mode) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_ensure_initialized_locked();

  if (mode != BLOWER_CONTROL_MODE_MANUAL_PERCENT &&
      mode != BLOWER_CONTROL_MODE_SEMI_AUTO_TARGET &&
      mode != BLOWER_CONTROL_MODE_AUTO_TEST) {
    restore_interrupts(irq_state);
    return;
  }

  blower_control_apply_mode_locked(&g_state, mode);
  restore_interrupts(irq_state);
}

void blower_control_set_auto_hold_enabled(bool enabled) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_mode_t mode = BLOWER_CONTROL_MODE_MANUAL_PERCENT;
  blower_control_ensure_initialized_locked();

  if (enabled) {
    mode = g_state.mode == BLOWER_CONTROL_MODE_AUTO_TEST
               ? BLOWER_CONTROL_MODE_AUTO_TEST
               : BLOWER_CONTROL_MODE_SEMI_AUTO_TARGET;
  }

  blower_control_apply_mode_locked(&g_state, mode);
  restore_interrupts(irq_state);
}

void blower_control_set_relay_enabled(bool enabled) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_ensure_initialized_locked();

  g_state.relay_enabled = enabled;
  if (!enabled) {
    g_state.output_pwm_percent = 0u;
    blower_control_reset_pd_state(&g_state);
    g_state.startup_boost_active = true;
    g_state.startup_boost_start_tick_ms = 0u;
  } else if (!g_state.auto_hold_enabled) {
    g_state.output_pwm_percent = g_state.manual_pwm_percent;
  } else {
    g_state.startup_boost_active = true;
    g_state.startup_boost_start_tick_ms = 0u;
    g_state.learned_feedforward_pwm = (float)g_state.output_pwm_percent;
    g_state.has_learned_feedforward_pwm = false;
  }

  restore_interrupts(irq_state);
}

void blower_control_set_target_pressure_pa(float target_pressure_pa) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_ensure_initialized_locked();

  if (!isnan(target_pressure_pa) && target_pressure_pa >= 0.0f &&
      target_pressure_pa <= 200.0f) {
    g_state.target_pressure_pa = target_pressure_pa;
    blower_control_reset_pd_state(&g_state);
  }

  restore_interrupts(irq_state);
}

uint8_t blower_control_step(float envelope_pressure_pa, bool measurement_valid,
                            uint32_t now_tick_ms) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_state_t *state = &g_state;
  float next_output = 0.0f;

  blower_control_ensure_initialized_locked();

  if (!state->relay_enabled) {
    state->output_pwm_percent = 0u;
    blower_control_reset_pd_state(state);
    restore_interrupts(irq_state);
    return 0u;
  }

  if (!state->auto_hold_enabled || !measurement_valid) {
    state->output_pwm_percent = state->manual_pwm_percent;
    blower_control_reset_pd_state(state);
    state->startup_boost_active = true;
    state->startup_boost_start_tick_ms = 0u;
    restore_interrupts(irq_state);
    return state->output_pwm_percent;
  }

  {
    const float filtered_pressure_pa =
        blower_control_filter_pressure(state, envelope_pressure_pa);
    const float measured_abs_pressure = fabsf(filtered_pressure_pa);
    const bool target_reached =
        measured_abs_pressure >=
        (state->target_pressure_pa * APP_CONTROL_STARTUP_TARGET_RATIO);
    const bool startup_overshoot_reached =
        measured_abs_pressure >=
        (state->target_pressure_pa * APP_CONTROL_STARTUP_MAX_OVERSHOOT_RATIO);
    float error_pa = state->target_pressure_pa - measured_abs_pressure;
    float derivative_pa_per_s = 0.0f;
    float dt_s = (float)APP_CONTROL_LOOP_PERIOD_MS / 1000.0f;
    float control_base_pwm = 0.0f;

    if (state->startup_boost_start_tick_ms == 0u) {
      state->startup_boost_start_tick_ms = now_tick_ms;
    }

    if (state->startup_boost_active) {
      const uint32_t boost_elapsed_ms =
          now_tick_ms - state->startup_boost_start_tick_ms;
      const bool min_hold_elapsed =
          boost_elapsed_ms >= APP_CONTROL_STARTUP_MIN_HOLD_MS;
      const bool max_hold_elapsed =
          (now_tick_ms - state->startup_boost_start_tick_ms) >=
          APP_CONTROL_STARTUP_FULL_POWER_HOLD_MS;
      state->output_pwm_percent = 100u;

      if ((target_reached && min_hold_elapsed) || startup_overshoot_reached ||
          max_hold_elapsed) {
        state->startup_boost_active = false;
        blower_control_reset_pd_terms(state);
        state->learning_active = true;
        state->learning_start_tick_ms = now_tick_ms;
        state->learning_stable_cycles = 0u;
      } else {
        restore_interrupts(irq_state);
        return state->output_pwm_percent;
      }
    }

    if (fabsf(error_pa) < state->pd_deadband_pa) {
      error_pa = 0.0f;
    }

    if (state->has_last_error && now_tick_ms > state->last_tick_ms) {
      dt_s = (float)(now_tick_ms - state->last_tick_ms) / 1000.0f;
      if (dt_s > 0.0001f) {
        derivative_pa_per_s = (error_pa - state->last_error_pa) / dt_s;
      }
    }

    derivative_pa_per_s = blower_control_clampf(
        derivative_pa_per_s, -APP_CONTROL_DERIVATIVE_CLAMP_PA_PER_S,
        APP_CONTROL_DERIVATIVE_CLAMP_PA_PER_S);

    if (state->has_last_error &&
        (error_pa * state->last_error_pa) < 0.0f &&
        fabsf(error_pa) > state->pd_deadband_pa) {
      const float decay = blower_control_clampf(
          APP_CONTROL_INTEGRAL_DECAY_ON_SIGN_FLIP, 0.1f, 1.0f);
      const float gain_scale_min =
          blower_control_clampf(APP_CONTROL_GAIN_SCALE_MIN, 0.05f, 1.0f);
      const float gain_scale_shrink = blower_control_clampf(
          APP_CONTROL_GAIN_SCALE_SHRINK, 0.0001f, 0.2f);

      state->integral_error_pa_s *= decay;
      state->gain_scale -= gain_scale_shrink;
      if (state->gain_scale < gain_scale_min) {
        state->gain_scale = gain_scale_min;
      }
      state->learning_stable_cycles = 0u;
    }

    if (error_pa == 0.0f) {
      state->integral_error_pa_s *= 0.98f;
    } else {
      const float integral_limit =
          blower_control_clampf(APP_CONTROL_INTEGRAL_LIMIT_PA_S, 5.0f, 500.0f);
      state->integral_error_pa_s = blower_control_clampf(
          state->integral_error_pa_s + (error_pa * dt_s), -integral_limit,
          integral_limit);
    }

    blower_control_update_learning_state(state, error_pa, derivative_pa_per_s,
                                         now_tick_ms);

    control_base_pwm = state->has_learned_feedforward_pwm
                           ? state->learned_feedforward_pwm
                           : (float)state->output_pwm_percent;

    {
      const float step_scale = blower_control_compute_step_scale(error_pa);
      float max_step_up = state->pd_max_step_percent * step_scale;
      float max_step_down = state->pd_max_step_down_percent * step_scale;
      const float kp_eff = state->pd_kp * state->gain_scale;
      const float ki_eff = state->pid_ki * state->gain_scale;
      const float kd_eff = state->pd_kd * state->gain_scale;

      if (state->learning_active) {
        max_step_up = fminf(max_step_up, APP_CONTROL_LEARNING_STEP_UP_PERCENT);
        max_step_down =
            fminf(max_step_down, APP_CONTROL_LEARNING_STEP_DOWN_PERCENT);
      }

      next_output = control_base_pwm + (kp_eff * error_pa) +
                    (ki_eff * state->integral_error_pa_s) +
                    (kd_eff * derivative_pa_per_s);

      if (next_output > (float)state->output_pwm_percent + max_step_up) {
        next_output = (float)state->output_pwm_percent + max_step_up;
      } else if (next_output <
                 (float)state->output_pwm_percent - max_step_down) {
        next_output = (float)state->output_pwm_percent - max_step_down;
      }
    }

    next_output = blower_control_clampf(next_output, 0.0f, 100.0f);
    state->output_pwm_percent = (uint8_t)(next_output + 0.5f);
    state->last_error_pa = error_pa;
    state->last_tick_ms = now_tick_ms;
    state->has_last_error = true;
  }

  restore_interrupts(irq_state);
  return g_state.output_pwm_percent;
}

void blower_control_update_line_feedback(bool line_sync, float line_frequency_hz) {
  uint32_t irq_state = save_and_disable_interrupts();
  blower_control_ensure_initialized_locked();

  g_state.line_sync = line_sync;
  g_state.line_frequency_hz = line_frequency_hz >= 0.0f ? line_frequency_hz : 0.0f;

  restore_interrupts(irq_state);
}

void blower_control_get_snapshot(blower_control_snapshot_t *out_snapshot) {
  uint32_t irq_state = 0u;

  if (out_snapshot == NULL) {
    return;
  }

  irq_state = save_and_disable_interrupts();
  blower_control_ensure_initialized_locked();

  *out_snapshot = (blower_control_snapshot_t){
      .manual_pwm_percent = g_state.manual_pwm_percent,
      .output_pwm_percent = g_state.output_pwm_percent,
      .mode = g_state.mode,
      .auto_hold_enabled = g_state.auto_hold_enabled,
      .relay_enabled = g_state.relay_enabled,
      .target_pressure_pa = g_state.target_pressure_pa,
      .pd_kp = g_state.pd_kp,
      .pd_kd = g_state.pd_kd,
      .pd_deadband_pa = g_state.pd_deadband_pa,
      .pd_max_step_percent = g_state.pd_max_step_percent,
      .line_sync = g_state.line_sync,
      .line_frequency_hz = g_state.line_frequency_hz,
  };

  restore_interrupts(irq_state);
}
