# Smart Lamp Demo — firmware
#
# Defines the `firmware` binary (FreeRTOS POSIX) and detects the external
# dependencies still pending vendoring.
#
# libmodbus is vendored in third_party/libmodbus (target `modbus`);
# libsodium and mbedTLS are vendored too (targets `sodium`/`mbedtls`).

# Binary in ./build/bin/firmware. It cannot be ./build/firmware: that path is
# already the subproject build directory (binary/directory collision).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- pthread -----------------------------------------------------------------
# Required by the FreeRTOS POSIX port and by direct thread usage.
find_package(Threads REQUIRED)

# --- External dependencies ---------------------------------------------------
# All vendored: libmodbus (target `modbus`, shared) and
# libsodium/mbedtls (targets `sodium`/`mbedtls`, static from third_party/).
# No system packages are detected.

# --- FreeRTOS Kernel (vendored, v10.5.1 — ADR-001) --------------------------
set(FREERTOS_KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FreeRTOS-Kernel")
set(FREERTOS_PORT_DIR "${FREERTOS_KERNEL_DIR}/portable/ThirdParty/GCC/Posix")
set(FREERTOS_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config")

add_library(freertos_kernel STATIC
    ${FREERTOS_KERNEL_DIR}/tasks.c
    ${FREERTOS_KERNEL_DIR}/list.c
    ${FREERTOS_KERNEL_DIR}/queue.c
    ${FREERTOS_KERNEL_DIR}/timers.c
    ${FREERTOS_KERNEL_DIR}/event_groups.c
    ${FREERTOS_KERNEL_DIR}/stream_buffer.c
    ${FREERTOS_PORT_DIR}/port.c
    ${FREERTOS_PORT_DIR}/utils/wait_for_event.c
    ${FREERTOS_KERNEL_DIR}/portable/MemMang/heap_4.c)

target_include_directories(freertos_kernel PUBLIC
    ${FREERTOS_KERNEL_DIR}/include
    ${FREERTOS_PORT_DIR}
    ${FREERTOS_CONFIG_DIR})

# -pthread (Threads::Threads) and the POSIX base for the port.
# PORTABLE_POSIX=1 is the ADR-001 convention marker.
target_compile_definitions(freertos_kernel PUBLIC
    _POSIX_C_SOURCE=200809L
    PORTABLE_POSIX=1)

target_link_libraries(freertos_kernel PUBLIC Threads::Threads)

# --- Sources -----------------------------------------------------------------
add_subdirectory(src)

# --- Tests -------------------------------------------------------------------
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
