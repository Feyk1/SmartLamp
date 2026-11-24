# libsodium — vendored

- **Upstream:** https://github.com/jedisct1/libsodium
- **Version:** 1.0.22 (tag `1.0.22-RELEASE`)
- **Vendored on:** 2026-11-18
- **License:** **ISC** (see [`LICENSE`](LICENSE))

## Included subset

- Full `src/libsodium/` tree (sources and public headers).
- `LICENSE`, `AUTHORS`, `README.markdown`.
- The build system (autotools: `configure`, `Makefile.*`, `m4/`, `build-aux/`),
  tests and documentation are removed.

## Build

`third_party/libsodium/CMakeLists.txt` compiles the 141 `.c` files with CMake
(autotools is not run) and generates `sodium/version.h` from
`sodium/version.h.in`. Static target: **`sodium`**.

Intended use: **Ed25519** signature verification for `.ota` files and test key
generation. Static linking; the project remains MIT and libsodium keeps its ISC
license.

## Updating

1. Download the release tarball (`libsodium-X.Y.Z.tar.gz`) and replace
   `src/libsodium/`, `LICENSE`, `AUTHORS` and `README.markdown`.
2. Update `VERSION` and `SODIUM_LIBRARY_VERSION_MAJOR/MINOR` in
   `CMakeLists.txt` with the values from `configure.ac`.
3. If the release adds a new generated header, replicate it in
   `CMakeLists.txt` (with `configure_file`).
4. Build and run `test_crypto`.
