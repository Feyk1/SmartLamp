# ADR-001 — FreeRTOS POSIX port as the firmware runtime environment

- **Date:** 2026-11-18
- **Status:** Accepted
- **Decision makers:** Project maintainer
- **Project context:** Public firmware demo for a smart lamp, with the requirement to run without hardware and without manual infrastructure management.

## Context

The project requires firmware that implements:

- RTOS scheduler with tasks and priorities.
- IPC mechanisms (mutexes).
- Measurement of RTOS metrics (stack high-water mark and heap usage).
- Integration with Modbus TCP, REST and WebSocket.

At the same time, it must be accessible from a browser without keeping an MCU powered on, and it must be able to run in a cloud container (Fly.io).

Three possible runtime environments for the firmware were evaluated:

1. **Bare-metal on QEMU** (Cortex-M emulation with `qemu-system-arm`).
2. **FreeRTOS POSIX port** (the FreeRTOS kernel compiled as a Linux process).
3. **FreeRTOS on real hardware** (physical STM32 or ATSAM, reachable over the network).

## Decision

**The FreeRTOS POSIX port is chosen** (`portable/ThirdParty/GCC/Posix`), running the firmware as a Linux process inside a container.

The application is structured in two layers:

- **Portable core:** tasks, mutexes, state machines, business logic, with no hardware dependencies.
- **HAL (Hardware Abstraction Layer):** `sensor_read()`, `actuator_set()`, `flash_read/write/erase()` interface, with a POSIX implementation for this demo and the possibility of an ARM implementation in the future.

## Alternatives considered

### Alternative A: Bare-metal on QEMU

**Advantages:**
- Higher fidelity to real hardware (memory map, emulated peripherals).
- Demonstrates cross-compilation (`arm-none-eabi-gcc`) and debugging with remote GDB.
- Allows validating the binary that would run on a real MCU.

**Drawbacks:**
- Configuring QEMU with virtual peripherals (UART, timers, NIC with SLIRP) consumes significant time.
- The firmware cannot run persistently outside CI; the public demo would be less interactive.
- Emulation latency adds artificial timing variation that would distort task timing.
- Requires maintaining an ARM toolchain in addition to the native toolchain.

**Discarded because:** the demo focuses on architecture and its trade-offs, not on emulation. Time invested in configuring QEMU would be taken from security, OTA and observability, which the demo prioritizes.

### Alternative B: Real hardware reachable over the network

**Advantages:**
- Maximum fidelity: what you see is a real MCU.
- Demonstrates hardware bring-up, JTAG/SWD debugging, and real electrical behavior.

**Drawbacks:**
- Requires an MCU powered on and reachable 24/7 (contradicts the requirement).
- Exposes a physical device to the Internet (attack surface and risk of abuse).
- Does not scale to multiple instances per visit.
- Hardware cost and cost of keeping the device online.

**Discarded because:** it directly contradicts the requirement of an "autonomous demo without manual intervention" and of "multiple ephemeral instances".

### Alternative C: FreeRTOS POSIX port (chosen)

**Advantages:**
- Real scheduler and mutexes: it is not a toy simulation.
- Runs as a native Linux process, without emulation.
- Portable to ARM by changing only the HAL: the core is the same.
- Compatible with Docker and Fly.io without additional layers.
- Allows measuring real RTOS metrics (stack HWM and heap) with standard tools.

**Drawbacks:**
- **No timing determinism:** the Linux scheduler decides when each pthread runs, so jitter is higher than on a real MCU.
- **No direct access to peripherals:** SPI, I2C, GPIO, timers and WDT must be simulated through the HAL.
- **No hardware-mapped memory:** "flash" is a file mapped with `mmap`, not a physical region.
- **No real interrupts:** ISRs are simulated with threads or signals.

**Chosen because:** the balance between fidelity, portability, implementation time and suitability for cloud deployment is the best of the three options.

## Consequences

### Positive

- **Reduced time-to-demo:** the firmware compiles with native GCC and runs with `./firmware` or `docker compose up`.
- **Future portability:** migrating to ARM means changing the HAL, not rewriting the logic.
- **Real observability:** FreeRTOS metrics (stack HWM, queue usage) are the same as on an MCU.
- **Integration with the Linux ecosystem:** the firmware can use `libmodbus`, `libsodium`, `mbedtls` and `pthread` without restrictions.
- **Container deployment:** the Docker image is small (<100 MB) and starts in seconds.

### Negative

- **Higher jitter than on real hardware:** execution timing is not deterministic, so no real-time guarantees are assumed.
- **Simulated peripherals:** the WDT, the timers and the "flash" are not hardware; they are implemented through the HAL. This is explained in the README and in `docs/threat-model.md`.
- **Risk of "this is not real firmware":** a Linux process is not an MCU. Mitigation: the HAL, the bootloader, A/B and cryptographic verification are identical to those of an MCU; only the substrate changes.
- **Dependence on the POSIX port:** if the port breaks in a FreeRTOS update, the demo breaks. Mitigation: pin the FreeRTOS kernel version in `CMakeLists.txt`.

### Neutral

- **Learning curve of the POSIX port:** it requires adjusting `FreeRTOSConfig.h` (stack sizes, `configUSE_PREEMPTION`, `configTICK_RATE_HZ`). It is a one-time cost, not a recurring one.
- **Multiple processes:** supervisor + bootloader + firmware + LED driver share the container. This is a design decision (see ADR-004), not a direct consequence of the POSIX port.

## Implementation notes

- The FreeRTOS version is pinned to `v10.5.1` (latest LTS at design time).
- The port is compiled with `-pthread` and requires `-DPORTABLE_POSIX` in `FreeRTOSConfig.h`.
- Tasks are mapped to pthreads; the FreeRTOS scheduler decides which one has the focus on each tick.
- Task stack high-water marks and heap usage are sampled with `uxTaskGetSystemState()` and published in `POST /telemetry/health`.
- The watchdog is simulated with a `vWatchdogTask` task that must be fed by the rest of the tasks; if it is not, the supervisor sends `SIGTERM` to the process.

## References

- [FreeRTOS POSIX port](https://www.freertos.org/FreeRTOS-simulator-for-Linux.html)
- [FreeRTOS Kernel v10.5.1](https://github.com/FreeRTOS/FreeRTOS-Kernel/tree/V10.5.1)
- [`01-general-architecture.md`](../01-general-architecture.md) — section 3.3
