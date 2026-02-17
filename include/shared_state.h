#ifndef BLOWER_SHARED_STATE_H
#define BLOWER_SHARED_STATE_H

#include <stdint.h>

// Shared between cores:
// - Core0 writes desired dimmer power percent [0..100]
// - Core1 reads it from ISR for phase-angle control

void shared_state_init(void);

void shared_set_dimmer_power_percent(uint8_t percent);
uint8_t shared_get_dimmer_power_percent(void);

#endif // BLOWER_SHARED_STATE_H
