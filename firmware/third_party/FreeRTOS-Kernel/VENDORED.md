# FreeRTOS Kernel — vendored

- **Upstream:** https://github.com/FreeRTOS/FreeRTOS-Kernel
- **Versión:** V10.5.1 (fijada en ADR-001)
- **Fecha de vendorización:** 2026-11-18
- **Licencia:** MIT (ver [`LICENSE.md`](LICENSE.md))

## Subconjunto incluido

- Núcleo: `tasks.c`, `list.c`, `queue.c`, `timers.c`, `event_groups.c`, `stream_buffer.c`.
- Cabeceras: `include/`.
- Port POSIX: `portable/ThirdParty/GCC/Posix/` (`port.c`, `portmacro.h`, `utils/wait_for_event.{c,h}`).
- Gestor de memoria: `portable/MemMang/heap_4.c`.

## Por qué

ADR-001 fija FreeRTOS v10.5.1 como kernel del firmware. Se vendoriza para
que el build sea reproducible y funcione sin red, en lugar de descargarlo
en tiempo de configuración (`FetchContent`).
