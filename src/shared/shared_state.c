#include "shared_state.h"

#include <stdatomic.h>

#include "pico/stdlib.h"

static atomic_uchar g_dimmer_power_percent;

void __time_critical_func(shared_state_init)(void) {
  atomic_store_explicit(&g_dimmer_power_percent, 0, memory_order_relaxed);
}

void __time_critical_func(shared_set_dimmer_power_percent)(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  atomic_store_explicit(&g_dimmer_power_percent, (unsigned char)percent,
                        memory_order_relaxed);
}

uint8_t __time_critical_func(shared_get_dimmer_power_percent)(void) {
  return (uint8_t)atomic_load_explicit(&g_dimmer_power_percent,
                                       memory_order_relaxed);
}
