# Dependencias de terceros compartidas.
#
# libsodium 1.0.22 (ISC) y mbedTLS 3.6.8 (Apache-2.0): enlace estático y build
# autocontenido con CMake (no requieren paquetes del sistema).
#
# libmodbus v3.1.10 (LGPL-2.1+), compilado como librería COMPARTIDA `modbus`.
# El enlace dinámico mantiene la librería como componente separable (LGPL):
# el proyecto sigue siendo MIT y el receptor puede sustituir libmodbus.so.
# Ver third_party/libmodbus/VENDORED.md.

set(LIBMODBUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libmodbus")
set(LIBMODBUS_SRC_DIR "${LIBMODBUS_DIR}/src")

add_library(modbus SHARED
    ${LIBMODBUS_SRC_DIR}/modbus.c
    ${LIBMODBUS_SRC_DIR}/modbus-data.c
    ${LIBMODBUS_SRC_DIR}/modbus-rtu.c
    ${LIBMODBUS_SRC_DIR}/modbus-tcp.c)

# Mismo SONAME que el paquete de Ubuntu (libmodbus.so.5.1.0 / libmodbus.so.5).
set_target_properties(modbus PROPERTIES
    VERSION 5.1.0
    SOVERSION 5
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

target_include_directories(modbus
    PUBLIC ${LIBMODBUS_SRC_DIR}
    PRIVATE ${LIBMODBUS_DIR})

# _GNU_SOURCE para accept4/SOCK_CLOEXEC/MSG_NOSIGNAL; el config.h vendorizado
# aporta HAVE_ACCEPT4, HAVE_BYTESWAP_H y HAVE_DECL_TIOC*.
target_compile_definitions(modbus PRIVATE _GNU_SOURCE)
target_compile_features(modbus PRIVATE c_std_99)

# --- cJSON (JSON portable para el cliente WS del firmware) -------------------
add_library(cjson STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cJSON/cJSON.c)

target_include_directories(cjson PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cJSON)

target_compile_features(cjson PRIVATE c_std_99)

# --- libsodium (Ed25519) y mbedTLS (AES-GCM/SHA-256), vendorizados ----------
# Enlace estático y build autocontenido: no se usan paquetes del sistema.
add_subdirectory(libsodium)
add_subdirectory(mbedtls)

# --- Unity (tests, compartido por firmware y driver) ------------------------
if(BUILD_TESTS)
    add_library(unity STATIC
        ${CMAKE_SOURCE_DIR}/firmware/third_party/Unity/src/unity.c)

    target_include_directories(unity PUBLIC
        ${CMAKE_SOURCE_DIR}/firmware/third_party/Unity/src)

    target_compile_features(unity PRIVATE c_std_99)
endif()
