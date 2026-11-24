# mbedTLS 3.6.8 (LTS) — subconjunto mínimo vendorizado (Apache-2.0).
#
# Solo se compilan los módulos necesarios para AES-256-GCM y SHA-256 del OTA.
# La configuración vive en config/smartlamp_config.h y se propaga a los
# consumidores con MBEDTLS_CONFIG_FILE.

add_library(mbedtls STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/library/aes.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/aesni.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/aesce.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/block_cipher.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/constant_time.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/gcm.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/platform.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/platform_util.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library/sha256.c")

target_include_directories(mbedtls PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(mbedtls PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/library")

target_compile_definitions(mbedtls PUBLIC
    MBEDTLS_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/config/smartlamp_config.h")

target_compile_features(mbedtls PRIVATE c_std_99)
