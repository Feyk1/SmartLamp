# Shared third-party dependencies.
#
# libsodium 1.0.22 (ISC) and mbedTLS 3.6.8 (Apache-2.0): static linking and a
# self-contained CMake build (no system packages required).
#
# libmodbus v3.1.10 (LGPL-2.1+), built as the SHARED library `modbus`.
# Dynamic linking keeps the library as a separable LGPL component: the project
# remains MIT and the recipient can replace libmodbus.so.
# See third_party/libmodbus/VENDORED.md.

set(LIBMODBUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libmodbus")
set(LIBMODBUS_SRC_DIR "${LIBMODBUS_DIR}/src")

add_library(modbus SHARED
    ${LIBMODBUS_SRC_DIR}/modbus.c
    ${LIBMODBUS_SRC_DIR}/modbus-data.c
    ${LIBMODBUS_SRC_DIR}/modbus-rtu.c
    ${LIBMODBUS_SRC_DIR}/modbus-tcp.c)

# Same SONAME as the Ubuntu package (libmodbus.so.5.1.0 / libmodbus.so.5).
set_target_properties(modbus PROPERTIES
    VERSION 5.1.0
    SOVERSION 5
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

target_include_directories(modbus
    PUBLIC ${LIBMODBUS_SRC_DIR}
    PRIVATE ${LIBMODBUS_DIR})

# _GNU_SOURCE for accept4/SOCK_CLOEXEC/MSG_NOSIGNAL; the vendored config.h
# provides HAVE_ACCEPT4, HAVE_BYTESWAP_H and HAVE_DECL_TIOC*.
target_compile_definitions(modbus PRIVATE _GNU_SOURCE)
target_compile_features(modbus PRIVATE c_std_99)

# --- cJSON (portable JSON for the firmware WS client) ------------------------
add_library(cjson STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cJSON/cJSON.c)

target_include_directories(cjson PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cJSON)

target_compile_features(cjson PRIVATE c_std_99)

# --- libsodium (Ed25519) and mbedTLS (AES-GCM/SHA-256), vendored -------------
# Static linking and a self-contained build: no system packages are used.
add_subdirectory(libsodium)
add_subdirectory(mbedtls)

# --- Unity (tests, shared by firmware and driver) ---------------------------
if(BUILD_TESTS)
    add_library(unity STATIC
        ${CMAKE_SOURCE_DIR}/firmware/third_party/Unity/src/unity.c)

    target_include_directories(unity PUBLIC
        ${CMAKE_SOURCE_DIR}/firmware/third_party/Unity/src)

    target_compile_features(unity PRIVATE c_std_99)
endif()
