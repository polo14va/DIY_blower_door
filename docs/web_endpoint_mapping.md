# Web <-> Firmware Mapping (RP2350)

This document summarizes the contract between the web app (`app.js`) and the HTTP/SSE firmware.

## Endpoints used by `app.js`

1. `GET /events` (SSE)
   - Web usage: `connectEventStream()`.
   - Firmware implementation: `http_start_sse_stream()` + `sse_stream_task()` in `src/tasks/wifi_task.c`.
   - Behavior: push on state changes plus periodic keep-alive.
   - Note: the UI no longer performs periodic polling of `status/report`; it consumes runtime data through SSE.

2. `POST /api/pwm` with `{"value":0..100}`
   - Web usage: `sendUpdate('pwm', value)`.
   - Firmware implementation: `http_handle_api_post_route()` -> `blower_control_set_manual_pwm_percent()`.

3. `POST /api/led` with `{"value":0|1}`
   - Web usage: `sendUpdate('led', value)`.
   - Firmware implementation: `http_handle_api_post_route()` -> `blower_control_set_auto_hold_enabled()`.

4. `POST /api/relay` with `{"value":0|1}`
   - Web usage: `sendUpdate('relay', value)`.
   - Firmware implementation: `http_handle_api_post_route()` -> `blower_control_set_relay_enabled()`.

5. `POST /api/target` with `{"value":Pa}`
   - Web usage: continuous target mode (50/75 Pa).
   - Firmware implementation: `http_handle_api_post_route()` -> `blower_control_set_target_pressure_pa()`.

6. `POST /debug/stream` with `{"enabled":true|false}`
   - Web usage: `setDebugStreaming(enabled)`.
   - Firmware implementation: `http_handle_debug_route()` (POST mode).

7. `POST /debug/clear`
   - Web usage: terminal clear button.
   - Firmware implementation: `http_handle_debug_route()` (`/debug/clear` route).

8. `GET /api/ota/status`
   - Web usage: `refreshOtaStatus()`.
   - Firmware implementation: `http_handle_ota_status_route()` + `ota_update_service_get_status()`.
   - Response: active version + OTA state (progress, CRC, errors).

9. `POST /api/ota/begin` with `{"size":N,"crc32":CRC,"version":"x.y.z"}`
   - Web/CLI usage: OTA session start.
   - Firmware implementation: `http_handle_ota_post_route()` -> `ota_update_service_begin()`.

10. `POST /api/ota/chunk` with `{"offset":N,"data":"<base64>"}`
    - Web/CLI usage: incremental binary upload.
    - Firmware implementation: `http_handle_ota_post_route()` -> `ota_update_service_write_chunk()`.

11. `POST /api/ota/finish`
    - Web/CLI usage: finalization and validation (CRC + vector table).
    - Firmware implementation: `http_handle_ota_post_route()` -> `ota_update_service_finish()`.

12. `POST /api/ota/apply`
    - Web/CLI usage: apply staged image and reboot RP2350.
    - Firmware implementation: `http_handle_ota_post_route()` -> `ota_update_service_request_apply_async()`.

## Telemetry fields consumed by the web app

The web app uses these JSON fields from `/api/status` and SSE:

- `pwm`, `led`, `relay`
- `line_sync`, `input`, `frequency`
- `dp1_pressure`, `dp1_temperature`, `dp1_ok`
- `dp2_pressure`, `dp2_temperature`, `dp2_ok`
- Legacy aliases: `dp_pressure`, `dp_temperature`
- `fan_flow_m3h`, `target_pressure_pa`
- `logs_enabled`, `logs` (when debug is active)

## Firmware data origins

1. Control:
   - Service: `src/services/blower_control.c`
   - PD loop: `blower_control_step()`
   - Periodic execution: `src/tasks/dimmer_task.c`

2. ADP910 sensors:
   - Driver: `src/drivers/adp910/adp910_sensor.c`
   - Dual sampling task: `src/tasks/adp910_task.c`
   - Shared metrics: `src/services/blower_metrics.c`

3. Web/HTTP/SSE:
   - Server and routes: `src/tasks/wifi_task.c`
