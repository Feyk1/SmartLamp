# FreeRTOS Kernel — vendored

- **Upstream:** https://github.com/FreeRTOS/FreeRTOS-Kernel
- **Version:** V10.5.1 (pinned in ADR-001)
- **Vendored on:** 2026-11-18
- **License:** MIT (see [`LICENSE.md`](LICENSE.md))

## Included subset

- Core: `tasks.c`, `list.c`, `queue.c`, `timers.c`, `event_groups.c`, `stream_buffer.c`.
- Headers: `include/`.
- POSIX port: `portable/ThirdParty/GCC/Posix/` (`port.c`, `portmacro.h`, `utils/wait_for_event.{c,h}`).
- Memory manager: `portable/MemMang/heap_4.c`.

## Why

ADR-001 pins FreeRTOS v10.5.1 as the firmware kernel. It is vendored so the
build is reproducible and works without network access, instead of downloading
it at configure time (`FetchContent`).
