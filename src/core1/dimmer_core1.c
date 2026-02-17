#include "core1_dimmer.h"

#include "app_config.h"
#include "shared_state.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"

// Use a dedicated timer instance and alarm on Core1 to avoid contention with Core0 time services.
// RP2350 has two timer instances (timer0_hw and timer1_hw).
#define DIMMER_TIMER timer1_hw
#define DIMMER_ALARM_NUM 0u

static volatile uint32_t s_last_zc_us = 0;

static inline uint32_t dimmer_time_us_32(void) { return DIMMER_TIMER->timerawl; }

static inline void dimmer_gate_on(void) {
  sio_hw->gpio_set = 1u << DIMMER_GATE_GPIO;
}

static inline void dimmer_gate_off(void) {
  sio_hw->gpio_clr = 1u << DIMMER_GATE_GPIO;
}

static void __time_critical_func(dimmer_busy_wait_us)(uint32_t us) {
  uint32_t start = dimmer_time_us_32();
  while ((uint32_t)(dimmer_time_us_32() - start) < us) {
    __asm volatile("nop");
  }
}

static inline void __time_critical_func(dimmer_fire_gate_pulse)(void) {
  dimmer_gate_on();
  dimmer_busy_wait_us(DIMMER_GATE_PULSE_US);
  dimmer_gate_off();
}

static void __time_critical_func(dimmer_alarm_irq_handler)(void) {
  // Clear IRQ (write-1-to-clear)
  DIMMER_TIMER->intr = 1u << DIMMER_ALARM_NUM;

  // Fire gate pulse (keep this ISR minimal and deterministic)
  dimmer_fire_gate_pulse();
}

static void __time_critical_func(dimmer_zc_irq_handler)(void) {
  const uint32_t event_mask = GPIO_IRQ_EDGE_RISE;
  if ((gpio_get_irq_event_mask(DIMMER_ZC_GPIO) & event_mask) == 0u) {
    return;
  }
  gpio_acknowledge_irq(DIMMER_ZC_GPIO, event_mask);

  const uint32_t now = dimmer_time_us_32();
  if ((uint32_t)(now - s_last_zc_us) < DIMMER_ZC_DEBOUNCE_US) {
    return;
  }
  s_last_zc_us = now;

  const uint8_t percent = shared_get_dimmer_power_percent();
  if (percent == 0u) {
    dimmer_gate_off();
    DIMMER_TIMER->inte &= ~(1u << DIMMER_ALARM_NUM);
    return;
  }

  // Full conduction: trigger immediately on ZC to minimize latency/jitter.
  if (percent >= 100u) {
    DIMMER_TIMER->inte &= ~(1u << DIMMER_ALARM_NUM);
    dimmer_fire_gate_pulse();
    return;
  }

  const uint32_t max_delay_us =
      (DIMMER_MAINS_HALF_CYCLE_US > (DIMMER_GATE_PULSE_US + 1u))
          ? (DIMMER_MAINS_HALF_CYCLE_US - DIMMER_GATE_PULSE_US - 1u)
          : 0u;

  uint32_t delay_us = 0;
  delay_us = (DIMMER_MAINS_HALF_CYCLE_US * (100u - (uint32_t)percent)) / 100u;
  if (delay_us > max_delay_us) {
    delay_us = max_delay_us;
  }

  // Program alarm relative to the captured ZC timestamp.
  DIMMER_TIMER->intr = 1u << DIMMER_ALARM_NUM; // clear pending
  DIMMER_TIMER->alarm[DIMMER_ALARM_NUM] = now + delay_us;
  DIMMER_TIMER->inte |= 1u << DIMMER_ALARM_NUM;
}

static void dimmer_core1_init(void) {
  // Dedicated alarm for Core1
  timer_hardware_alarm_claim(DIMMER_TIMER, DIMMER_ALARM_NUM);

  // GPIO setup
  gpio_init(DIMMER_ZC_GPIO);
  gpio_set_dir(DIMMER_ZC_GPIO, GPIO_IN);
  // Hardware-dependent: adjust pull-up/down to match the ZC detector output stage.
  gpio_pull_down(DIMMER_ZC_GPIO);

  gpio_init(DIMMER_GATE_GPIO);
  gpio_set_dir(DIMMER_GATE_GPIO, GPIO_OUT);
  dimmer_gate_off();

  // Install and enable ZC IRQ on Core1 only.
  // Use the SDK shared-handler mechanism so Core0 can also use IO_IRQ_BANK0 later without conflicts.
  gpio_add_raw_irq_handler_with_order_priority_masked(1u << DIMMER_ZC_GPIO,
                                                     dimmer_zc_irq_handler, 0);
  irq_set_priority(IO_IRQ_BANK0, 0);
  irq_set_enabled(IO_IRQ_BANK0, true);
  gpio_set_irq_enabled(DIMMER_ZC_GPIO, GPIO_IRQ_EDGE_RISE, true);

  // Install and enable the timer alarm IRQ on Core1 only
  const uint alarm_irq =
      timer_hardware_alarm_get_irq_num(DIMMER_TIMER, DIMMER_ALARM_NUM);
  irq_set_exclusive_handler(alarm_irq, dimmer_alarm_irq_handler);
  irq_set_priority(alarm_irq, 0);
  irq_set_enabled(alarm_irq, true);

  // Start disarmed; will arm on first valid ZC edge
  DIMMER_TIMER->inte &= ~(1u << DIMMER_ALARM_NUM);
}

void core1_entry(void) {
  // Core1 is dedicated to the dimmer: no FreeRTOS, no WiFi, no logging in ISR paths.
  dimmer_core1_init();

  for (;;) {
    // Sleep until the next interrupt (ZC GPIO IRQ or timer alarm IRQ).
    __wfi();
  }
}
