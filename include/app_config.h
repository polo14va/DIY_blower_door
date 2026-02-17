#ifndef BLOWER_APP_CONFIG_H
#define BLOWER_APP_CONFIG_H

// ─────────────────────────────────────────────────────────────
// Board / application configuration (RP2350 / Pico 2 W)
// ─────────────────────────────────────────────────────────────

// Dimmer I/O (Core1)
#define DIMMER_ZC_GPIO 2
#define DIMMER_GATE_GPIO 3

// Mains frequency assumptions (50 Hz => 10 ms half-cycle)
#define DIMMER_MAINS_HALF_CYCLE_US 10000u

// Gate pulse width (triac trigger pulse)
#define DIMMER_GATE_PULSE_US 100u

// Filter duplicate/bounce edges on ZC input
#define DIMMER_ZC_DEBOUNCE_US 1500u

#endif // BLOWER_APP_CONFIG_H
