CMAKE_MINIMUM_REQUIRED(VERSION 3.12)

SET(PRJ rp2350bm-audio)
SET(PICO_BOARD pico2_w)
SET(HOME $ENV{HOME})
SET(PICO_SDK_FETCH_FROM_GIT on)
INCLUDE(pico_sdk_import.cmake)
INCLUDE(opus_import.cmake)

PROJECT(${PRJ} C CXX ASM)
SET(CMAKE_C_STANDARD 11)
SET(CMAKE_CXX_STANDARD 17)

# pico_sdk_init must be called early, right after project()
pico_sdk_init()
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")

# Include stdint.h/stddef.h for C/CXX only (not for assembler)
add_compile_options(
  "$<$<COMPILE_LANGUAGE:C,CXX>:-includestdint.h>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-includestddef.h>"
)

# Fix for pico-sdk runtime.c missing CLOCKS_PER_SEC
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCLOCKS_PER_SEC=1000000")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DCLOCKS_PER_SEC=1000000")

# Configuration from environment variables.
#
# WIFI_SSID / WIFI_PASSWORD are no longer compile-time inputs — they're
# provisioned over BLE at first boot and persisted to the last flash sector
# (see src/credentials.c, src/ble_provision.c).
message(STATUS "=== RP2350 Build Configuration ===")
if(DEFINED ENV{API_URL})
  message(STATUS "API_URL: $ENV{API_URL}")
  add_definitions(-DAPI_URL="$ENV{API_URL}")
else()
  message(STATUS "API_URL: (not set, using default)")
endif()
message(STATUS "==================================")

IF (PICO_SDK_VERSION_STRING VERSION_LESS "1.3.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.3.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
ENDIF()
SET(CMAKE_INCLUDE_CURRENT_DIR ON)

set(OPUS_FIXED_POINT ON CACHE BOOL "OPUS_FIXED_POINT")
add_subdirectory(${OPUS_PATH} build/opus)

INCLUDE_DIRECTORIES(".")

INCLUDE_DIRECTORIES(
  ${CMAKE_CURRENT_LIST_DIR}/
  ${CMAKE_CURRENT_LIST_DIR}/inc/
  ${CMAKE_CURRENT_LIST_DIR}/../inc/
  ${CMAKE_BINARY_DIR}/
  ${CMAKE_CURRENT_LIST_DIR}/../vendor/cJSON/
)

ADD_EXECUTABLE(
    ${PRJ}
    ${CMAKE_CURRENT_LIST_DIR}/../vendor/cJSON/cJSON.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/st7789.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/audio.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/ic_ring.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/http.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/queue.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/led.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/wifi.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/gui.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/tts.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/opus_stream.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/credentials.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/ble_provision.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/mic.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/linephone.c
    ${CMAKE_CURRENT_LIST_DIR}/../main.c
)

pico_generate_pio_header(${PRJ} ${CMAKE_CURRENT_LIST_DIR}/../src/i2s.pio)
pico_generate_pio_header(${PRJ} ${CMAKE_CURRENT_LIST_DIR}/../src/i2s_in.pio)

# Compile the BLE provisioning GATT description into a C header
# (generates `wifi_provision.h` in the build dir, with profile_data[] and
# ATT_CHARACTERISTIC_..._VALUE_HANDLE macros — see src/ble_provision.c).
pico_btstack_make_gatt_header(${PRJ} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../src/wifi_provision.gatt
)

PROJECT(${PRJ} VERSION 1.0.0)

TARGET_LINK_LIBRARIES(
    ${PRJ}
    pico_stdlib
    pico_multicore
    pico_time
    hardware_spi
    hardware_timer
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_flash
    pico_flash
    pico_cyw43_arch_lwip_threadsafe_background
    pico_mbedtls
    pico_lwip_mbedtls
    # BLE for first-boot WiFi provisioning
    pico_btstack_ble
    pico_btstack_cyw43
    opus
)

pico_enable_stdio_usb(${PRJ} 0)
pico_enable_stdio_uart(${PRJ} 1)

# Explicit UART configuration for RP2350
target_compile_definitions(${PRJ} PRIVATE
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    PICO_DEFAULT_UART_BAUD_RATE=115200
    MBEDTLS_PLATFORM_TIME_ALT
    # CYW43 firmware loads BT support only when this is set; otherwise
    # hci_power_control(HCI_POWER_ON) is a no-op and no advertising happens.
    CYW43_ENABLE_BLUETOOTH=1
    # flash_safe_execute() is only called from creds_save() in provisioning
    # bootmode, where core1 hasn't been launched yet. Without this define
    # the SDK tries to multicore_lockout core1 and asserts because no
    # lockout victim was registered. In normal bootmode core1 is running
    # but never writes flash, so the assume-safe path is correct there too.
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    # Core 0's stack: 20 KB ending at the top of SCRATCH_X (see
    # memmap_bigstack.ld). opus is built with VAR_ARRAYS, so codec scratch
    # is VLAs on the caller's stack; measured peaks (stackdiag canary):
    # decode 4,864 B, encode 18,912 B — past the old 16 KB region.
    # __StackBottom stays at 0x2007c000, so the extra 4 KB comes from the
    # annexed SCRATCH_X bank, NOT the newlib heap (whose break high-water
    # already ratchets to 8 KB below the bottom; lowering __StackBottom
    # is what made PICO_STACK_SIZE=0x6000 kill WiFi in the past).
    PICO_STACK_SIZE=0x5000
    # Core 1's 4 KB stack lives in SCRATCH_Y (was SCRATCH_X, now annexed
    # by core0) — without this override PICO_CORE1_STACK_SIZE inherits
    # PICO_STACK_SIZE and the linker fails because core1_stack[] no
    # longer fits the 4 KB bank. Core 1 doesn't run opus so 4 KB is fine.
    PICO_CORE1_STACK_SIZE=0x1000
)

# Override the SDK's default RP2350 memmap to relocate .stack into main
# RAM. Must come AFTER the target is fully configured (linked-library
# count etc.) but BEFORE pico_add_extra_outputs reads the final layout.
pico_set_linker_script(${PRJ} ${CMAKE_CURRENT_LIST_DIR}/memmap_bigstack.ld)

pico_add_extra_outputs(${PRJ})

target_compile_options(${PRJ} PRIVATE
    -Wall
    -Wno-format
    -Wno-unused-function
    -Wno-maybe-uninitialized
)
