# libsodium — vendored

- **Upstream:** https://github.com/jedisct1/libsodium
- **Versión:** 1.0.22 (tag `1.0.22-RELEASE`)
- **Fecha de vendorización:** 2026-11-18
- **Licencia:** **ISC** (ver [`LICENSE`](LICENSE))

## Subconjunto incluido

- `src/libsodium/` completo (fuentes y cabeceras públicas).
- `LICENSE`, `AUTHORS`, `README.markdown`.
- Se eliminan el sistema de build (autotools: `configure`, `Makefile.*`,
  `m4/`, `build-aux/`), los tests y la documentación.

## Build

`third_party/libsodium/CMakeLists.txt` compila los 141 `.c` con CMake
(no se ejecuta autotools) y genera `sodium/version.h` desde
`sodium/version.h.in`. Target estático: **`sodium`**.

Uso previsto: verificación de firmas **Ed25519** del `.ota` (bloque 11) y
generación de claves de prueba. Enlace estático; el proyecto sigue siendo MIT
y libsodium conserva su licencia ISC.

## Actualizar

1. Descargar el tarball de la release (`libsodium-X.Y.Z.tar.gz`) y sustituir
   `src/libsodium/`, `LICENSE`, `AUTHORS` y `README.markdown`.
2. Ajustar `VERSION` y `SODIUM_LIBRARY_VERSION_MAJOR/MINOR` en el
   `CMakeLists.txt` con los valores de `configure.ac`.
3. Si la release añade un header generado nuevo, replicarlo en el
   `CMakeLists.txt` (con `configure_file`).
4. Compilar y ejecutar `test_crypto`.
