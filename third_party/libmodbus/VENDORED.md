# libmodbus — vendored

- **Upstream:** https://github.com/stephane/libmodbus
- **Versión:** v3.1.10 (fijada)
- **Fecha de vendorización:** 2026-11-18
- **Licencia:** **LGPL-2.1-or-later** (ver [`COPYING.LESSER`](COPYING.LESSER))

## Subconjunto incluido

- `src/modbus.c`, `src/modbus-data.c`, `src/modbus-rtu.c`, `src/modbus-tcp.c`.
- Cabeceras públicas y privadas de `src/`.
- `config.h` mínimo para Linux (sin autotools).
- `src/modbus-version.h` resuelto para 3.1.10.

## Build

`third_party/CMakeLists.txt` define el target **compartido** `modbus`
(`libmodbus.so.5.1.0`, SONAME `libmodbus.so.5`). Se eligió enlace dinámico
para que la LGPL-2.1+ sea un componente separable: el proyecto puede seguir
siendo MIT y el receptor puede sustituir el `.so`. La fuente queda vendorizada
y no se modificó el código de la librería (solo se añadieron `config.h` y
`modbus-version.h`).

## Nota de licencia

El diseño original (`docs/09-plan-de-3-semanas.md` §4.2) describía libmodbus
como BSD; en realidad es **LGPL-2.1+**. Se corrigió la documentación.
