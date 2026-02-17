#include "services/dimmer_control.h"

#include "hardware/sync.h"

static volatile uint8_t g_dimmer_power_percent;

void dimmer_control_set_power_percent(uint8_t power_percent) {
  uint32_t irq_state = save_and_disable_interrupts();

  g_dimmer_power_percent = power_percent <= 100u ? power_percent : 100u;

  restore_interrupts(irq_state);
}

uint8_t dimmer_control_get_power_percent(void) {
  uint32_t irq_state = save_and_disable_interrupts();
  uint8_t power_percent = g_dimmer_power_percent;
  restore_interrupts(irq_state);
  return power_percent;
}
