# Replication Guide

This guide is focused on getting a first successful build + flash with minimal friction.

## 1. Clone repository

```bash
git clone https://github.com/polo14va/DIY_blower_door.git
cd DIY_blower_door
```

## 2. Configure Wi-Fi secrets

```bash
cp .wifi-secrets.example .wifi-secrets
```

Edit `.wifi-secrets`:

```text
WIFI_SSID=your_wifi_ssid
WIFI_PASSWORD=your_wifi_password
```

## 3. Install required tooling

Required:

- Pico SDK
- ARM GNU toolchain (`arm-none-eabi-gcc`)
- CMake
- Python 3

Optional but recommended:

- Ninja
- `probe-rs` for SWD flashing

## 4. Validate local environment

```bash
./scripts/doctor.sh
```

Strict mode (also requires `probe-rs`):

```bash
./scripts/doctor.sh --strict
```

## 5. Build and flash

```bash
./scripts/build_flash_rp2350.sh
```

Build only:

```bash
SKIP_FLASH=1 ./scripts/build_flash_rp2350.sh
```

## 6. Open web UI

After boot, read the serial/log output and open the device IP in your browser.

## 7. Version and release workflow

- Version source of truth: `VERSION` (`x.y.z`).
- Bump version, validate build, create commit and tag:

```bash
./scripts/release.sh patch
```

- Trigger public release automation:

```bash
git push --follow-tags
```

This triggers `.github/workflows/release.yml` for tags like `v1.2.3`.

## Common pitfalls

- `FREERTOS_KERNEL_PATH` invalid:
  - export a valid FreeRTOS-Kernel path
  - or vendor it in `third_party/FreeRTOS-Kernel`
- Web does not reflect latest changes:
  - ensure `WEB_DIR` points to the intended assets
  - rerun the build script
- OTA upload errors:
  - verify device and host are on the same network
  - avoid multiple active browser sessions

## Related docs

- API mapping: `docs/web_endpoint_mapping.md`
- Wiring: `docs/PINOUT_PICO2W.md`
- Architecture notes: `docs/plan.md`
