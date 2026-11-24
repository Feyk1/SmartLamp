#ifndef SMARTLAMP_MBEDTLS_CONFIG_H
#define SMARTLAMP_MBEDTLS_CONFIG_H

/*
 * Configuración mínima de mbedTLS para el OTA del Smart Lamp.
 *
 * Solo se habilita lo que usa el firmware: AES-256-GCM (cifrado de la
 * imagen) y SHA-256 (huella). Nada de TLS, X.509, bignum ni PSA.
 */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

#endif /* SMARTLAMP_MBEDTLS_CONFIG_H */
