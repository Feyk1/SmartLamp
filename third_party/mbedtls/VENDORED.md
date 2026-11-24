# mbedTLS — vendored (subset)

- **Upstream:** https://github.com/Mbed-TLS/mbedtls
- **Version:** 3.6.8 (LTS)
- **Vendored on:** 2026-11-18
- **License:** **Apache-2.0** (see [`LICENSE`](LICENSE))

## Included subset

- `include/mbedtls/` (public headers from the release).
- `library/` with the required modules:
  - `aes.c`, `aesni.c`, `aesce.c`, `block_cipher.c`
  - `gcm.c`
  - `sha256.c`
  - `platform.c`, `platform_util.c`, `constant_time.c`
  - internal headers: `alignment.h`, `block_cipher_internal.h`,
    `common.h`, `constant_time_impl.h`, `constant_time_internal.h`, `ctr.h`.
- `LICENSE` and `ChangeLog`.
- TLS, X.509, bignum, PSA, tests, programs and documentation are excluded.

## Configuration

`config/smartlamp_config.h` enables only:

```c
MBEDTLS_AES_C
MBEDTLS_GCM_C
MBEDTLS_SHA256_C
MBEDTLS_PLATFORM_C
MBEDTLS_PLATFORM_MEMORY
```

`third_party/mbedtls/CMakeLists.txt` defines the static target **`mbedtls`**
and propagates `MBEDTLS_CONFIG_FILE` to consumers.

Intended use: **AES-256-GCM** and **SHA-256** for `.ota` files.

## Updating

1. Download the `mbedtls-X.Y.Z.tar.bz2` tarball.
2. Replace `include/mbedtls/`.
3. Copy the 9 `.c` files listed above and the 6 internal headers from `library/`.
4. Review `config/smartlamp_config.h` if the new release requires additional
   macros (building `test_crypto` detects this).
5. Update `LICENSE` and `ChangeLog`.
