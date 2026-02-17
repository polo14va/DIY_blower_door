#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
WEB_DIR="${WEB_DIR:-${REPO_ROOT}/include/web}"
FREERTOS_DIR="${FREERTOS_KERNEL_PATH:-${REPO_ROOT}/third_party/FreeRTOS-Kernel}"
TARGET_NAME="${TARGET_NAME:-blower_pico_c}"
FIRMWARE_VERSION_OVERRIDE="${FIRMWARE_VERSION_OVERRIDE:-${FIRMWARE_VERSION:-}}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
PROBE_CHIP="${PROBE_RS_CHIP:-RP235x}"
PROBE_PROTOCOL="${PROBE_RS_PROTOCOL:-swd}"
SKIP_FLASH="${SKIP_FLASH:-0}"

require_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Error: required command '$cmd' was not found." >&2
        exit 1
    fi
}

require_cmd cmake
require_cmd python3

if [[ "${SKIP_FLASH}" != "1" ]]; then
    require_cmd probe-rs
fi

if ! command -v ninja >/dev/null 2>&1; then
    CMAKE_GENERATOR="Unix Makefiles"
else
    CMAKE_GENERATOR="Ninja"
fi

BUILD_CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"

if [[ -f "${BUILD_CACHE_FILE}" ]]; then
    cached_build_dir="$(awk -F= '/^CMAKE_CACHEFILE_DIR:INTERNAL=/{print $2}' "${BUILD_CACHE_FILE}" | tail -n 1)"
    if [[ -n "${cached_build_dir}" && "${cached_build_dir}" != "${BUILD_DIR}" ]]; then
        echo "Detected stale CMake cache path in ${BUILD_DIR} (${cached_build_dir} -> ${BUILD_DIR})."
        echo "Resetting CMake cache for a clean reconfigure."
        rm -f "${BUILD_DIR}/CMakeCache.txt"
        rm -rf "${BUILD_DIR}/CMakeFiles"
    fi

    if [[ -f "${BUILD_CACHE_FILE}" ]]; then
        cached_generator="$(awk -F= '/^CMAKE_GENERATOR:INTERNAL=/{print $2}' "${BUILD_CACHE_FILE}" | tail -n 1)"
        if [[ -n "${cached_generator}" && "${cached_generator}" != "${CMAKE_GENERATOR}" ]]; then
            echo "Detected generator mismatch in ${BUILD_DIR} (${cached_generator} -> ${CMAKE_GENERATOR})."
            echo "Resetting CMake cache for a clean reconfigure."
            rm -f "${BUILD_DIR}/CMakeCache.txt"
            rm -rf "${BUILD_DIR}/CMakeFiles"
        fi
    fi
fi

if [[ -z "${PICO_SDK_PATH:-}" ]]; then
    if [[ -f "${BUILD_CACHE_FILE}" ]]; then
        cached_sdk_path="$(awk -F= '/^PICO_SDK_PATH:PATH=/{print $2}' "${BUILD_CACHE_FILE}" | tail -n 1)"
        if [[ -n "${cached_sdk_path}" && -d "${cached_sdk_path}" ]]; then
            export PICO_SDK_PATH="${cached_sdk_path}"
        fi
    elif [[ -f "${REPO_ROOT}/build/CMakeCache.txt" ]]; then
        cached_sdk_path="$(awk -F= '/^PICO_SDK_PATH:PATH=/{print $2}' "${REPO_ROOT}/build/CMakeCache.txt" | tail -n 1)"
        if [[ -n "${cached_sdk_path}" && -d "${cached_sdk_path}" ]]; then
            export PICO_SDK_PATH="${cached_sdk_path}"
        fi
    elif [[ -d "${HOME}/.pico-sdk/sdk/2.2.0" ]]; then
        export PICO_SDK_PATH="${HOME}/.pico-sdk/sdk/2.2.0"
    fi
fi

if [[ -z "${PICO_TOOLCHAIN_PATH:-}" ]] && ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    if [[ -d "${HOME}/.pico-sdk/toolchain/14_2_Rel1/bin" ]]; then
        export PICO_TOOLCHAIN_PATH="${HOME}/.pico-sdk/toolchain/14_2_Rel1/bin"
    fi
    latest_toolchain_gcc="$(ls -1 /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/arm-none-eabi-gcc 2>/dev/null | sort | tail -n 1 || true)"
    if [[ -z "${PICO_TOOLCHAIN_PATH:-}" && -n "${latest_toolchain_gcc}" ]]; then
        export PICO_TOOLCHAIN_PATH="$(dirname "${latest_toolchain_gcc}")"
    fi
fi

if [[ ! -f "${WEB_DIR}/index.html" ]]; then
    echo "Error: ${WEB_DIR}/index.html does not exist." >&2
    echo "Set a different path with WEB_DIR=/path/to/web when running the script." >&2
    exit 1
fi

if [[ ! -f "${FREERTOS_DIR}/tasks.c" ]]; then
    if [[ -f "${BUILD_CACHE_FILE}" ]]; then
        cached_freertos_path="$(awk -F= '/^FREERTOS_KERNEL_PATH:[A-Z]+=/{print $2}' "${BUILD_CACHE_FILE}" | tail -n 1)"
        if [[ -n "${cached_freertos_path}" && -d "${cached_freertos_path}" ]]; then
            FREERTOS_DIR="${cached_freertos_path}"
        fi
    fi

    if [[ ! -f "${FREERTOS_DIR}/tasks.c" && -f "${REPO_ROOT}/build/CMakeCache.txt" ]]; then
        cached_freertos_path="$(awk -F= '/^FREERTOS_KERNEL_PATH:[A-Z]+=/{print $2}' "${REPO_ROOT}/build/CMakeCache.txt" | tail -n 1)"
        if [[ -n "${cached_freertos_path}" && -d "${cached_freertos_path}" ]]; then
            FREERTOS_DIR="${cached_freertos_path}"
        fi
    fi

    if [[ ! -f "${FREERTOS_DIR}/tasks.c" && -f "${REPO_ROOT}/third_party/FreeRTOS-Kernel/tasks.c" ]]; then
        FREERTOS_DIR="${REPO_ROOT}/third_party/FreeRTOS-Kernel"
    fi

    if [[ ! -f "${FREERTOS_DIR}/tasks.c" && -f "/Users/pedro/Documents/Aproyectos/C++/FreeRTOS-Kernel/tasks.c" ]]; then
        FREERTOS_DIR="/Users/pedro/Documents/Aproyectos/C++/FreeRTOS-Kernel"
    fi
fi

if [[ ! -f "${FREERTOS_DIR}/tasks.c" ]]; then
    echo "Error: FreeRTOS-Kernel was not found in ${FREERTOS_DIR}" >&2
    echo "Set FREERTOS_KERNEL_PATH=/path/to/FreeRTOS-Kernel before running." >&2
    exit 1
fi

CPU_CORES="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if [[ -z "${PICO_SDK_PATH:-}" ]]; then
    echo "Error: could not detect PICO_SDK_PATH." >&2
    echo "Set PICO_SDK_PATH before running the script." >&2
    exit 1
fi

echo "[1/4] Configuring project"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G "${CMAKE_GENERATOR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPICO_BOARD=pico2_w \
    -DPICO_PLATFORM=rp2350 \
    -DFREERTOS_KERNEL_PATH="${FREERTOS_DIR}" \
    -DBLOWER_WEB_SOURCE_DIR="${WEB_DIR}" \
    ${FIRMWARE_VERSION_OVERRIDE:+-DBLOWER_FIRMWARE_VERSION_OVERRIDE="${FIRMWARE_VERSION_OVERRIDE}"} \
    ${PICO_TOOLCHAIN_PATH:+-DPICO_TOOLCHAIN_PATH="${PICO_TOOLCHAIN_PATH}"}

echo "[2/4] Building firmware and embedded web assets"
cmake --build "${BUILD_DIR}" --target "${TARGET_NAME}" --parallel "${CPU_CORES}"

ELF_PATH="${BUILD_DIR}/${TARGET_NAME}.elf"
if [[ ! -f "${ELF_PATH}" ]]; then
    echo "Error: ${ELF_PATH} was not generated." >&2
    exit 1
fi

GENERATED_WEB_C="${BUILD_DIR}/generated/web_assets.c"
if [[ ! -f "${GENERATED_WEB_C}" ]]; then
    echo "Error: ${GENERATED_WEB_C} was not generated." >&2
    exit 1
fi

for web_file in "${WEB_DIR}"/*; do
    if [[ -f "${web_file}" && "${GENERATED_WEB_C}" -ot "${web_file}" ]]; then
        echo "Error: embedded web assets are outdated (${web_file} is newer than ${GENERATED_WEB_C})." >&2
        echo "Run the script again to regenerate assets before flashing." >&2
        exit 1
    fi
done

if [[ "${ELF_PATH}" -ot "${GENERATED_WEB_C}" ]]; then
    echo "Error: ${ELF_PATH} is older than ${GENERATED_WEB_C}." >&2
    echo "Build again before flashing." >&2
    exit 1
fi

if [[ "${SKIP_FLASH}" == "1" ]]; then
    echo "Build completed without flashing (SKIP_FLASH=1)."
    echo "ELF ready at: ${ELF_PATH}"
    echo "Embedded web source: ${WEB_DIR}"
    exit 0
fi

echo "[3/4] Flashing RP2350 (${PROBE_CHIP})"
probe-rs download --chip "${PROBE_CHIP}" --protocol "${PROBE_PROTOCOL}" "${ELF_PATH}"

echo "[4/4] Resetting microcontroller"
probe-rs reset --chip "${PROBE_CHIP}" --protocol "${PROBE_PROTOCOL}"

echo "Firmware flashed successfully"
echo "ELF: ${ELF_PATH}"
echo "Embedded web source: ${WEB_DIR}"
