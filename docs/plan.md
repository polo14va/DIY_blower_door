Architecture plan (FreeRTOS + RP2350 + ADP910 refactor)

Summary
Modular Blower Door firmware using Pico SDK + FreeRTOS, focused on extensibility:
- WiFi/HTTP task
- Dimmer task
- Dual ADP910 acquisition task
- Decoupled services for metrics and control

Layers
- `platform`: faults, panic, runtime hooks.
- `app`: composition and task creation.
- `tasks`: periodic execution and coordination.
- `drivers`: hardware/protocol access for ADP910.
- `services`: shared state and formulas.

Purpose of the 2 ADP910 sensors
- Sensor 1 (fan): measure fan-related differential pressure.
- Sensor 2 (envelope): measure indoor/outdoor differential pressure.
- Metrics service: compute estimated speed and total leakage using pluggable models.

Open/Closed principle
- New tasks are added through descriptors in `task_bootstrap`.
- New formulas are added by implementing model callbacks without changing the core service.

Natural next extensions
- Add a coefficient configuration endpoint (gain) without recompiling.
- Persist calibrations in flash.
- Add dimmer power control through HTTP.

Implemented script (web + control at 50 Pa)

1. API contract and streaming
- `GET /api/status`: full snapshot for UI.
- `POST /api/pwm`: manual setpoint 0..100.
- `POST /api/led`: enable/disable pressure auto-hold (PD).
- `POST /api/relay`: enable/disable fan output.
- `GET /events`: SSE stream (push on change + keep-alive).
- `POST /debug/stream`: enable/disable log streaming.
- `POST /debug/clear`: clear log buffer.
- `GET /debug/logs`: read accumulated logs.

2. Firmware control
- PD control runs in the dimmer task using envelope pressure (sensor 2).
- Default target: 50 Pa.
- Stability filter:
  - configurable deadband in Pa
  - per-cycle step limiter to avoid oscillations.
- Safety logic:
  - relay off forces PWM=0
  - without a valid measurement, auto-hold does not act.

3. ADP910 integration
- Sensor 1: fan pressure/temperature.
- Sensor 2: envelope pressure/temperature.
- Sampling and bus settings adjusted for finer control:
  - 20 ms sampling period
  - I2C at 100 kHz.

4. Exposed telemetry
- UI-compatible fields:
  - `pwm`, `led`, `relay`, `line_sync`, `input`, `frequency`
  - `dp1_pressure`, `dp1_temperature`, `dp1_ok`
  - `dp2_pressure`, `dp2_temperature`, `dp2_ok`
  - aliases `dp_pressure`, `dp_temperature`
  - `logs_enabled`.
- Additional field:
  - `fan_flow_m3h` computed with power law (`C*|dP|^n`).
