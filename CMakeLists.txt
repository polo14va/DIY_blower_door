# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# Set the target board (RP2350 - Pico 2 W) BEFORE initializing the SDK
if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Pico board target")
endif()

if (NOT DEFINED PICO_PLATFORM)
    set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform target")
endif()

# Initialize Pico SDK
include(pico_sdk_import.cmake)

project(blower_pico_c C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_version_file "${CMAKE_CURRENT_LIST_DIR}/VERSION")
set(_version_from_file "0.0.0-dev")
if (EXISTS "${_version_file}")
    file(READ "${_version_file}" _version_raw)
    string(STRIP "${_version_raw}" _version_raw)
    if (NOT _version_raw STREQUAL "")
        set(_version_from_file "${_version_raw}")
    endif()
endif()

set(BLOWER_FIRMWARE_VERSION_OVERRIDE "" CACHE STRING
    "Optional firmware version override. Leave empty to use VERSION file.")
if (BLOWER_FIRMWARE_VERSION_OVERRIDE STREQUAL "")
    set(BLOWER_FIRMWARE_VERSION "${_version_from_file}")
else()
    set(BLOWER_FIRMWARE_VERSION "${BLOWER_FIRMWARE_VERSION_OVERRIDE}")
endif()
message(STATUS "Firmware version: ${BLOWER_FIRMWARE_VERSION}")

set(_project_web_include_dir "${CMAKE_CURRENT_LIST_DIR}/include/web")
set(_project_web_assets_dir "${CMAKE_CURRENT_LIST_DIR}/web")
if (EXISTS "${_project_web_include_dir}/index.html")
    set(_default_web_dir "${_project_web_include_dir}")
elseif (EXISTS "${_project_web_assets_dir}/index.html")
    set(_default_web_dir "${_project_web_assets_dir}")
else()
    set(_default_web_dir "${_project_web_assets_dir}")
endif()
set(BLOWER_WEB_SOURCE_DIR "${_default_web_dir}" CACHE PATH "Directory with static web assets to embed")
if (NOT EXISTS "${BLOWER_WEB_SOURCE_DIR}/index.html")
    message(WARNING "Invalid BLOWER_WEB_SOURCE_DIR (${BLOWER_WEB_SOURCE_DIR}); using fallback ${_default_web_dir}")
    set(BLOWER_WEB_SOURCE_DIR "${_default_web_dir}" CACHE PATH "Directory with static web assets to embed" FORCE)
endif()
if (NOT EXISTS "${BLOWER_WEB_SOURCE_DIR}/index.html")
    message(FATAL_ERROR "index.html was not found in BLOWER_WEB_SOURCE_DIR (${BLOWER_WEB_SOURCE_DIR})")
endif()

file(GLOB _web_asset_dependencies CONFIGURE_DEPENDS "${BLOWER_WEB_SOURCE_DIR}/*")
set(_generated_web_assets_c "${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.c")
add_custom_command(
    OUTPUT "${_generated_web_assets_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/scripts/generate_web_assets.py"
            --input-dir "${BLOWER_WEB_SOURCE_DIR}"
            --output-c "${_generated_web_assets_c}"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/scripts/generate_web_assets.py"
        ${_web_asset_dependencies}
    VERBATIM
)
message(STATUS "Embedded web source: ${BLOWER_WEB_SOURCE_DIR}")

# FreeRTOS-Kernel location (prefer explicit CMake var, then env var, then vendored copy)
if (NOT DEFINED FREERTOS_KERNEL_PATH OR FREERTOS_KERNEL_PATH STREQUAL "" OR NOT EXISTS "${FREERTOS_KERNEL_PATH}/tasks.c")
    if (DEFINED ENV{FREERTOS_KERNEL_PATH} AND EXISTS "$ENV{FREERTOS_KERNEL_PATH}/tasks.c")
        set(FREERTOS_KERNEL_PATH "$ENV{FREERTOS_KERNEL_PATH}")
    elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/FreeRTOS-Kernel/tasks.c")
        set(FREERTOS_KERNEL_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/FreeRTOS-Kernel")
    elseif (EXISTS "/Users/pedro/Documents/Aproyectos/C++/FreeRTOS-Kernel/tasks.c")
        set(FREERTOS_KERNEL_PATH "/Users/pedro/Documents/Aproyectos/C++/FreeRTOS-Kernel")
    elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/build/CMakeCache.txt")
        file(READ "${CMAKE_CURRENT_LIST_DIR}/build/CMakeCache.txt" _build_cache_content)
        string(REGEX MATCH "FREERTOS_KERNEL_PATH:[A-Z]+=([^\r\n]+)" _m "${_build_cache_content}")
        if (NOT CMAKE_MATCH_1 STREQUAL "" AND EXISTS "${CMAKE_MATCH_1}/tasks.c")
            set(FREERTOS_KERNEL_PATH "${CMAKE_MATCH_1}")
        endif()
    endif()
endif()

if (NOT DEFINED FREERTOS_KERNEL_PATH OR FREERTOS_KERNEL_PATH STREQUAL "" OR NOT EXISTS "${FREERTOS_KERNEL_PATH}/tasks.c")
    message(FATAL_ERROR "FREERTOS_KERNEL_PATH is not defined or invalid. Export FREERTOS_KERNEL_PATH or pass -DFREERTOS_KERNEL_PATH=/path/to/FreeRTOS-Kernel. Checked: environment variable, third_party/FreeRTOS-Kernel, /Users/pedro/Documents/Aproyectos/C++/FreeRTOS-Kernel, and build/CMakeCache.txt.")
endif()

# Initialize the SDK
pico_sdk_init()

add_executable(blower_pico_c
    src/main.c
    src/app/task_bootstrap.c
    src/platform/runtime_faults.c
    src/drivers/adp910/adp910_sensor.c
    src/services/blower_metrics.c
    src/services/blower_control.c
    src/services/ota_update_service.c
    src/services/dimmer_control.c
    "${_generated_web_assets_c}"
    src/tasks/wifi_task.c
    src/tasks/dimmer_task.c
    src/tasks/adp910_task.c
)

# Add include directories
target_include_directories(blower_pico_c PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${FREERTOS_KERNEL_PATH}/include
    ${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM33_NTZ/non_secure
)

# Add FreeRTOS sources manually to avoid "Threads" error from its CMakeLists.txt
target_sources(blower_pico_c PRIVATE
    ${FREERTOS_KERNEL_PATH}/tasks.c
    ${FREERTOS_KERNEL_PATH}/queue.c
    ${FREERTOS_KERNEL_PATH}/list.c
    ${FREERTOS_KERNEL_PATH}/timers.c
    ${FREERTOS_KERNEL_PATH}/event_groups.c
    ${FREERTOS_KERNEL_PATH}/stream_buffer.c
    ${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM33_NTZ/non_secure/port.c
    ${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM33_NTZ/non_secure/portasm.c
    ${FREERTOS_KERNEL_PATH}/portable/MemMang/heap_4.c
)

# Enable FreeRTOS and CYW43 background functionality
target_link_libraries(blower_pico_c 
    pico_stdlib
    pico_cyw43_arch_lwip_sys_freertos
    pico_flash
    hardware_i2c
    hardware_gpio
    hardware_timer
    hardware_irq
    hardware_clocks
    hardware_flash
    hardware_watchdog
    pico_stdio_rtt
)

# Enable USB, UART and RTT output for printf
pico_enable_stdio_usb(blower_pico_c 1)
pico_enable_stdio_uart(blower_pico_c 1)
pico_enable_stdio_rtt(blower_pico_c 1)

# Allow using large files if needed
target_compile_definitions(blower_pico_c PRIVATE
    NO_SYS=0
)

target_compile_definitions(blower_pico_c PRIVATE
    APP_FIRMWARE_VERSION=\"${BLOWER_FIRMWARE_VERSION}\"
)

# WiFi credentials (optional): load from .wifi-secrets or env vars
set(_wifi_secrets_file "${CMAKE_CURRENT_LIST_DIR}/.wifi-secrets")
set(_wifi_ssid "")
set(_wifi_password "")

if (EXISTS "${_wifi_secrets_file}")
    file(READ "${_wifi_secrets_file}" _wifi_secrets_content)
    string(REGEX MATCH "WIFI_SSID=([^\r\n]+)" _m "${_wifi_secrets_content}")
    set(_wifi_ssid "${CMAKE_MATCH_1}")
    string(REGEX MATCH "WIFI_PASSWORD=([^\r\n]+)" _m "${_wifi_secrets_content}")
    set(_wifi_password "${CMAKE_MATCH_1}")
elseif (DEFINED ENV{WIFI_SSID} AND DEFINED ENV{WIFI_PASSWORD})
    set(_wifi_ssid "$ENV{WIFI_SSID}")
    set(_wifi_password "$ENV{WIFI_PASSWORD}")
endif()

if (NOT _wifi_ssid STREQUAL "" AND NOT _wifi_password STREQUAL "")
    message(STATUS "WiFi: using credentials from .wifi-secrets/env (SSID defined)")
    target_compile_definitions(blower_pico_c PRIVATE
        WIFI_SSID=\"${_wifi_ssid}\"
        WIFI_PASSWORD=\"${_wifi_password}\"
    )
else()
    message(STATUS "WiFi: no credentials found (use .wifi-secrets or WIFI_SSID/WIFI_PASSWORD)")
endif()

# Create map file
pico_add_extra_outputs(blower_pico_c)
