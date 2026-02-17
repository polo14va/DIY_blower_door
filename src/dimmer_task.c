#include "FreeRTOS.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "task.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define ZC_PIN 2
#define GATE_PIN 3

volatile int power_percent = 0; // 0-100
volatile bool gate_active = false;

// Hardware alarm for firing triac
static int64_t alarm_callback(alarm_id_t id, void *user_data) {
  // Fire gate
  gpio_put(GATE_PIN, 1);
  // Pulse width is 100us, short enough to busy wait inside ISR
  busy_wait_us(100);
  gpio_put(GATE_PIN, 0);
  return 0; // Don't repeat
}

// Zero-crossing interrupt
void gpio_callback(uint gpio, uint32_t events) {
  if (gpio == ZC_PIN) {
    if (power_percent > 0 && power_percent < 100) {
      // Calculate delay: 0% = 10ms (never), 100% = 0ms (immediate)
      // 50Hz half cycle = 10ms = 10000us
      // We want to fire LATER for LOWER power.
      // Power 10% -> delay 9000us
      // Power 90% -> delay 1000us
      uint32_t delay_us = (100 - power_percent) * 100; // Simplified mapping

      // Should verify half-cycle limits (8000us-12000us typically for 50Hz)
      // Just basic logic for now.
      add_alarm_in_us(delay_us, alarm_callback, NULL, false);
    } else if (power_percent >= 100) {
      // Full on
      gpio_put(GATE_PIN, 1);
    } else {
      // Full off
      gpio_put(GATE_PIN, 0);
    }
  }
}

void dimmer_task_entry(__unused void *params) {
  printf("Dimmer Task starting...\n");

  gpio_init(ZC_PIN);
  gpio_set_dir(ZC_PIN, GPIO_IN);
  gpio_pull_up(
      ZC_PIN); // Depending on hardware, ZC board might need internal pullup

  gpio_init(GATE_PIN);
  gpio_set_dir(GATE_PIN, GPIO_OUT);
  gpio_put(GATE_PIN, 0);

  // Setup interrupt
  gpio_set_irq_enabled_with_callback(ZC_PIN, GPIO_IRQ_EDGE_RISE, true,
                                     &gpio_callback);

  // Test loop - ramp up/down
  while (1) {
    // Just keep alive, power is controlled globally or via other mechanism
    // (e.g. queue)
    vTaskDelay(1000);
  }
}
