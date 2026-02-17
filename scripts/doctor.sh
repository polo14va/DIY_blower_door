#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

STRICT=0
if [[ "${1:-}" == "--strict" ]]; then
    STRICT=1
fi

ISSUES=0
WARNINGS=0

ok() {
    echo "[OK] $1"
}

warn() {
    echo "[WARN] $1"
    WARNINGS=$((WARNINGS + 1))
}

fail() {
    echo "[FAIL] $1"
    ISSUES=$((ISSUES + 1))
}

check_cmd() {
    local cmd="$1"
    local mandatory="$2"
    if command -v "$cmd" >/dev/null 2>&1; then
        ok "command available: ${cmd}"
    elif [[ "$mandatory" == "1" ]]; then
        fail "missing command: ${cmd}"
    else
        warn "missing optional command: ${cmd}"
    fi
}

echo "Running environment checks for DIY Blower Door firmware"

check_cmd cmake 1
check_cmd python3 1
check_cmd ninja 0
check_cmd probe-rs "$STRICT"

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    ok "command available: arm-none-eabi-gcc"
elif [[ -n "${PICO_TOOLCHAIN_PATH:-}" && -x "${PICO_TOOLCHAIN_PATH}/bin/arm-none-eabi-gcc" ]]; then
    ok "toolchain available at PICO_TOOLCHAIN_PATH: ${PICO_TOOLCHAIN_PATH}"
elif [[ -x "${HOME}/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-gcc" ]]; then
    ok "toolchain available in default location: ${HOME}/.pico-sdk/toolchain/14_2_Rel1"
else
    fail "missing ARM toolchain: arm-none-eabi-gcc"
fi

SDK_PATH="${PICO_SDK_PATH:-}"
if [[ -z "$SDK_PATH" && -d "${HOME}/.pico-sdk/sdk/2.2.0" ]]; then
    SDK_PATH="${HOME}/.pico-sdk/sdk/2.2.0"
fi

if [[ -n "$SDK_PATH" && -d "$SDK_PATH" ]]; then
    ok "PICO_SDK_PATH resolved: ${SDK_PATH}"
else
    fail "PICO_SDK_PATH not resolved. Export PICO_SDK_PATH or install Pico SDK in ~/.pico-sdk/sdk/2.2.0"
fi

FREERTOS_PATH="${FREERTOS_KERNEL_PATH:-}"
if [[ -z "$FREERTOS_PATH" && -f "${REPO_ROOT}/third_party/FreeRTOS-Kernel/tasks.c" ]]; then
    FREERTOS_PATH="${REPO_ROOT}/third_party/FreeRTOS-Kernel"
fi
if [[ -z "$FREERTOS_PATH" && -f "/Users/pedro/Documents/Aproyectos/C++/FreeRTOS-Kernel/tasks.c" ]]; then
    FREERTOS_PATH="/Users/pedro/Documents/Aproyectos/C++/FreeRTOS-Kernel"
fi
if [[ -z "$FREERTOS_PATH" && -f "${REPO_ROOT}/build/CMakeCache.txt" ]]; then
    cached_freertos="$(awk -F= '/^FREERTOS_KERNEL_PATH:[A-Z]+=/{print $2}' "${REPO_ROOT}/build/CMakeCache.txt" | tail -n 1)"
    if [[ -n "$cached_freertos" && -f "$cached_freertos/tasks.c" ]]; then
        FREERTOS_PATH="$cached_freertos"
    fi
fi

if [[ -n "$FREERTOS_PATH" && -f "$FREERTOS_PATH/tasks.c" ]]; then
    ok "FREERTOS_KERNEL_PATH resolved: ${FREERTOS_PATH}"
else
    fail "FreeRTOS-Kernel not found. Export FREERTOS_KERNEL_PATH or add third_party/FreeRTOS-Kernel"
fi

if [[ -f "${REPO_ROOT}/.wifi-secrets" ]]; then
    if rg -q '^WIFI_SSID=' "${REPO_ROOT}/.wifi-secrets" && rg -q '^WIFI_PASSWORD=' "${REPO_ROOT}/.wifi-secrets"; then
        ok ".wifi-secrets found with required keys"
    else
        fail ".wifi-secrets exists but WIFI_SSID/WIFI_PASSWORD keys are missing"
    fi
else
    warn ".wifi-secrets not found. Copy .wifi-secrets.example to .wifi-secrets and set credentials"
fi

if [[ -f "${REPO_ROOT}/include/web/index.html" && -f "${REPO_ROOT}/include/web/app.js" && -f "${REPO_ROOT}/include/web/style.css" ]]; then
    ok "embedded web assets found in include/web"
else
    fail "embedded web assets are incomplete in include/web"
fi

echo
if [[ "$ISSUES" -gt 0 ]]; then
    echo "Doctor finished with ${ISSUES} blocking issue(s) and ${WARNINGS} warning(s)."
    exit 1
fi

echo "Doctor finished successfully with ${WARNINGS} warning(s)."
