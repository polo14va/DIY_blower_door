#ifndef DIMMER_CONTROL_H
#define DIMMER_CONTROL_H

#include <stdint.h>

void dimmer_control_set_power_percent(uint8_t power_percent);
uint8_t dimmer_control_get_power_percent(void);

#endif
