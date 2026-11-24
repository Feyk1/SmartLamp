#ifndef SMARTLAMP_MBEDTLS_CONFIG_H
#define SMARTLAMP_MBEDTLS_CONFIG_H

/*
 * Minimal mbedTLS configuration for Smart Lamp OTA.
 *
 * Only what the firmware uses is enabled: AES-256-GCM (image encryption) and
 * SHA-256 (digest). No TLS, X.509, bignum or PSA.
 */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

#endif /* SMARTLAMP_MBEDTLS_CONFIG_H */
