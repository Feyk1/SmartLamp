# mbedTLS — vendored (subconjunto)

- **Upstream:** https://github.com/Mbed-TLS/mbedtls
- **Versión:** 3.6.8 (LTS)
- **Fecha de vendorización:** 2026-11-18
- **Licencia:** **Apache-2.0** (ver [`LICENSE`](LICENSE))

## Subconjunto incluido

- `include/mbedtls/` (cabeceras públicas de la release).
- `library/` con los módulos necesarios:
  - `aes.c`, `aesni.c`, `aesce.c`, `block_cipher.c`
  - `gcm.c`
  - `sha256.c`
  - `platform.c`, `platform_util.c`, `constant_time.c`
  - cabeceras internas: `alignment.h`, `block_cipher_internal.h`,
    `common.h`, `constant_time_impl.h`, `constant_time_internal.h`, `ctr.h`.
- `LICENSE` y `ChangeLog`.
- Se excluyen TLS, X.509, bignum, PSA, tests, programas y documentación.

## Configuración

`config/smartlamp_config.h` habilita solo:

```c
MBEDTLS_AES_C
MBEDTLS_GCM_C
MBEDTLS_SHA256_C
MBEDTLS_PLATFORM_C
MBEDTLS_PLATFORM_MEMORY
```

`third_party/mbedtls/CMakeLists.txt` define el target estático **`mbedtls`**
y propaga `MBEDTLS_CONFIG_FILE` a los consumidores.

Uso previsto: **AES-256-GCM** y **SHA-256** del `.ota` (bloque 11).

## Actualizar

1. Descargar el tarball `mbedtls-X.Y.Z.tar.bz2`.
2. Sustituir `include/mbedtls/`.
3. Copiar de `library/` los 9 `.c` listados arriba y las 6 cabeceras internas.
4. Revisar `config/smartlamp_config.h` si la nueva versión exige macros
   adicionales (compilar `test_crypto` lo detecta).
5. Actualizar `LICENSE` y `ChangeLog`.
