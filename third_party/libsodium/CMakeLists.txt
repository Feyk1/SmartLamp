# libsodium 1.0.22 — build estático vendorizado (ISC).
#
# El tarball oficial usa autotools; aquí se compilan sus fuentes directamente.
# El único header generado por configure es sodium/version.h; el resto del
# árbol es autocontenido (no hay config.h).

file(GLOB_RECURSE SODIUM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libsodium/*.c")

# Valores de configure.ac de la release 1.0.22.
set(VERSION "1.0.22")
set(SODIUM_LIBRARY_VERSION_MAJOR 26)
set(SODIUM_LIBRARY_VERSION_MINOR 4)
set(SODIUM_LIBRARY_MINIMAL_DEF "")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libsodium/include/sodium/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sodium/version.h"
    @ONLY)

add_library(sodium STATIC ${SODIUM_SOURCES})

target_include_directories(sodium PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libsodium/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libsodium/include/sodium"
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sodium")

# SODIUM_STATIC evita el dllimport de Windows; CONFIGURED silencia el aviso
# de "build no soportado" y _GNU_SOURCE declara syscall()/getrandom().
target_compile_definitions(sodium PUBLIC SODIUM_STATIC)
target_compile_definitions(sodium PRIVATE CONFIGURED=1 _GNU_SOURCE)
target_compile_features(sodium PRIVATE c_std_99)
target_link_libraries(sodium PUBLIC Threads::Threads)
