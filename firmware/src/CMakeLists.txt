# Smart Lamp Demo — firmware/src
#
# Builds the `firmware` binary and the shared firmware libraries.

# --- Flash HAL (reusable by firmware and tests) -----------------------------
add_library(flash_hal STATIC
    flash/flash.c
    flash/crc32.c)

target_compile_features(flash_hal PRIVATE c_std_99)
target_compile_definitions(flash_hal PUBLIC _POSIX_C_SOURCE=200809L)
target_include_directories(flash_hal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# --- Supervisor (PMIC process, independent from the firmware) ---------------
add_executable(supervisor
    supervisor/main.c
    supervisor/supervisor.c
    supervisor/supervisor_core.c)

target_compile_features(supervisor PRIVATE c_std_99)
target_compile_definitions(supervisor PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(supervisor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# --- Bootloader core (OTA verification + A/B metadata) ----------------------
# Without FreeRTOS: used by the bootloader binary, the app (confirmation) and
# the tests. Links the vendored crypto.
add_library(bootloader_core STATIC
    bootloader/ota_verify.c
    bootloader/ab.c)

target_compile_features(bootloader_core PRIVATE c_std_99)
target_compile_definitions(bootloader_core PUBLIC _POSIX_C_SOURCE=200809L)
target_include_directories(bootloader_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(bootloader_core PUBLIC
    flash_hal
    sodium
    mbedtls)

# --- Bootloader (standalone process, no FreeRTOS) ---------------------------
add_executable(bootloader
    bootloader/main.c)

target_compile_features(bootloader PRIVATE c_std_99)
target_compile_definitions(bootloader PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(bootloader PRIVATE bootloader_core)

# --- Firmware version (override for release/OTA builds) ---------------------
set(FW_VERSION_MAJOR "1" CACHE STRING "Firmware major version")
set(FW_VERSION_MINOR "0" CACHE STRING "Firmware minor version")
set(FW_VERSION_PATCH "3" CACHE STRING "Firmware patch version")

target_compile_definitions(bootloader PRIVATE
    FW_VERSION_MAJOR=${FW_VERSION_MAJOR}
    FW_VERSION_MINOR=${FW_VERSION_MINOR}
    FW_VERSION_PATCH=${FW_VERSION_PATCH})

add_executable(firmware
    main.c
    app/freertos_hooks.c
    app/tasks.c
    app/telemetry.c
    app/boot_confirm.c
    comms/modbus_client.c
    comms/rest_client.c
    comms/rest_commands.c
    comms/ota_client.c
    comms/cmd_handler.c
    comms/ws_client.c
    comms/net_transport_posix.c
    hal/log.c)

# Explicit C99 at target level (in addition to the project-wide setting).
target_compile_features(firmware PRIVATE c_std_99)

# POSIX base to declare sleep()/nanosleep() with strict -std=c99
# (CMAKE_C_EXTENSIONS OFF hides the glibc POSIX declarations).
target_compile_definitions(firmware PRIVATE
    _POSIX_C_SOURCE=200809L
    FW_VERSION_MAJOR=${FW_VERSION_MAJOR}
    FW_VERSION_MINOR=${FW_VERSION_MINOR}
    FW_VERSION_PATCH=${FW_VERSION_PATCH})

# pthread (FreeRTOS POSIX port) + vendored dependencies.
target_link_libraries(firmware PRIVATE
    freertos_kernel
    flash_hal
    bootloader_core
    modbus
    cjson
    sodium
    mbedtls
    Threads::Threads)

# Public firmware headers (firmware/include).
target_include_directories(firmware PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
