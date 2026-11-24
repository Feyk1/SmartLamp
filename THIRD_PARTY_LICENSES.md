# Third-party licenses

This project's code is released under the **MIT** license (see
[`LICENSE`](LICENSE)). Vendored dependencies keep their original licenses:

## libmodbus v3.1.10

- **License:** LGPL-2.1-or-later.
- **Source:** [`third_party/libmodbus/`](third_party/libmodbus/).
- **License text:** [`third_party/libmodbus/COPYING.LESSER`](third_party/libmodbus/COPYING.LESSER).
- **Linking:** shared library `libmodbus.so.5` built from the vendored source.
- **Upstream:** https://github.com/stephane/libmodbus

## libsodium v1.0.22

- **License:** ISC.
- **Source:** [`third_party/libsodium/`](third_party/libsodium/).
- **License text:** [`third_party/libsodium/LICENSE`](third_party/libsodium/LICENSE).
- **Linking:** static library built from the vendored source (no autotools).
- **Use:** Ed25519 verification of `.ota` images.
- **Upstream:** https://github.com/jedisct1/libsodium

## mbedTLS v3.6.8 (subset)

- **License:** Apache-2.0.
- **Source:** [`third_party/mbedtls/`](third_party/mbedtls/).
- **License text:** [`third_party/mbedtls/LICENSE`](third_party/mbedtls/LICENSE).
- **Linking:** static library; only AES, AES-NI/CE, GCM, SHA-256, block cipher and platform.
- **Use:** AES-256-GCM and SHA-256 for `.ota` images.
- **Upstream:** https://github.com/Mbed-TLS/mbedtls

## FreeRTOS Kernel v10.5.1

- **License:** MIT.
- **Source:** [`firmware/third_party/FreeRTOS-Kernel/`](firmware/third_party/FreeRTOS-Kernel/).
- **License text:** [`firmware/third_party/FreeRTOS-Kernel/LICENSE.md`](firmware/third_party/FreeRTOS-Kernel/LICENSE.md).
- **Upstream:** https://github.com/FreeRTOS/FreeRTOS-Kernel

## Unity v2.7.0

- **License:** MIT.
- **Source:** [`firmware/third_party/Unity/`](firmware/third_party/Unity/).
- **License text:** [`firmware/third_party/Unity/LICENSE.txt`](firmware/third_party/Unity/LICENSE.txt).
- **Upstream:** https://github.com/ThrowTheSwitch/Unity

## cJSON

- **License:** MIT.
- **Source:** [`third_party/cJSON/`](third_party/cJSON/).
- **License text:** [`third_party/cJSON/LICENSE`](third_party/cJSON/LICENSE).
- **Upstream:** https://github.com/DaveGamble/cJSON

## Vendored source policy

Vendored code is built from source and is not downloaded at runtime. When
updating a dependency, update its version, license text and this document in the
same change.
