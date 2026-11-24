# libmodbus — vendored

- **Upstream:** https://github.com/stephane/libmodbus
- **Version:** v3.1.10 (pinned)
- **Vendored on:** 2026-11-18
- **License:** **LGPL-2.1-or-later** (see [`COPYING.LESSER`](COPYING.LESSER))

## Included subset

- `src/modbus.c`, `src/modbus-data.c`, `src/modbus-rtu.c`, `src/modbus-tcp.c`.
- Public and private headers from `src/`.
- Minimal `config.h` for Linux (without autotools).
- `src/modbus-version.h` resolved for 3.1.10.

## Build

`third_party/CMakeLists.txt` defines the **shared** target `modbus`
(`libmodbus.so.5.1.0`, SONAME `libmodbus.so.5`). Dynamic linking keeps the
library as a separable LGPL-2.1+ component: the project can remain MIT and the
recipient can replace the `.so`. The source is vendored and the library code
was not modified (only `config.h` and `modbus-version.h` were added).

## License note

libmodbus is **LGPL-2.1+**, not BSD.
