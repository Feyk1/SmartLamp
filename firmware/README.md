# Firmware

FreeRTOS firmware running as a POSIX process.

## Contents

| Directory | Purpose |
| :--- | :--- |
| `include/` | Public headers (`log.h`, `tasks.h`, `flash.h`, `crc32.h`, `modbus_client.h`, `ws_client.h`, `net_transport.h`, `rest_client.h`, `cmd_handler.h`, `telemetry.h`, `fw_version.h`) |
| `src/main.c` | Firmware entry point (app) |
| `src/supervisor/` | Supervisor (PMIC): fork/execve and signals |
| `src/bootloader/` | `.ota` validation, A/B slots, rollback |
| `src/app/` | FreeRTOS application tasks (includes `telemetry.c`) |
| `src/comms/` | REST, WS and Modbus clients plus command fallback |
| `src/flash/` | `flash.bin` HAL with `mmap` and double buffering |
| `src/hal/` | Hardware Abstraction Layer (POSIX) |
| `config/` | `FreeRTOSConfig.h` |
| `test/` | Unit tests with Unity/CMock |
| `third_party/FreeRTOS-Kernel/` | Vendored FreeRTOS kernel v10.5.1 (ADR-001) |
| `third_party/Unity/` | Unity v2.7.0 unit test framework (MIT) |

## Build

```bash
cmake -B build -G Ninja
cmake --build build
./build/bin/firmware
```

No system dependencies: FreeRTOS, cJSON, libmodbus, libsodium and mbedTLS
are vendored in `third_party/` (and `firmware/third_party/`).

## Logging

- `printf`-like macros: `LOG_INFO("x=%d", x);` (they append `\n` if missing).
- Levels: `ERROR`, `WARN`, `INFO`, `DEBUG`, `TRACE` (default `INFO`).
- Runtime verbosity: `LOG_LEVEL=debug ./build/bin/firmware` or `log_set_level()`.
- Compile-time filtering: `LOG_LEVEL_MIN` (default `TRACE` on POSIX; use `INFO` on an MCU to trim).
- Configurable lock: `LOG_LOCK_MUTEX` by default; `LOG_LOCK_CRITICAL` for MCUs with a buffered UART.
- Swappable sink: `log_set_sink()` to replace stdout with a UART.
- Formatting happens outside the lock; only the write to the sink is serialized.

## Flash

- `flash.bin` (1 MB) is created if it does not exist and mapped with `mmap` (ADR-002).
- Path configurable with `FLASH_PATH` (default `./flash.bin`).
- Layout: bootloader 64 KB, A/B slots 352 KB, metadata 16 KB, config 16 KB, staging 208 KB, reserved 16 KB (ADR-002 amended in block 11).
- `flash_read/write/erase/sync` with range validation; `msync(MS_SYNC)` aligned to page.

## Modbus client

- The firmware is the driver's Modbus TCP master; it uses `libmodbus.so.5` (vendored; the build rpath resolves it).
- Variables: `MODBUS_HOST` (127.0.0.1), `MODBUS_PORT` (5020), `MODBUS_UNIT_ID` (1) and `MODBUS_WATCHDOG_TIMEOUT_S` (5; 0 disables it).
- Tasks: `vLightPollTask` (20 Hz), `vHeartbeatTask` (1 Hz), `vDiagPollTask` (1 Hz) and `vWatchdogTask` (5 Hz).
- `LOG_LEVEL=debug` shows the actual state per poll, the heartbeat and the diagnostics.
- On connect, the firmware configures the driver watchdog and re-applies it on every reconnection.

## WebSocket client

- Minimal WS client (RFC6455: handshake, text frames, ping/pong and close) with vendored `cJSON`.
- POSIX transport in `net_transport_posix.c` (shared with HTTP); an MCU port only reimplements `net_transport_*` (lwIP + mbedTLS).
- Variables: `ORCH_WS_URL` (`ws://127.0.0.1:8080/ws/device`) and `DEVICE_TOKEN` (`dev-token-local`).
- `vCommsTask` runs the loop and reconnects with backoff; `vLightPollTask` sends `state` on-change (200 ms deadband).
- Commands: `set_power`, `set_brightness`, `set_color`, `set_color_temperature`, `set_mode`, `start_sequence`, `stop_sequence`, `clear_faults`, `run_selftest`, `request_state`.
- Local `ws://`; WSS/TLS and `Sec-WebSocket-Accept` verification pending.

## REST telemetry and fallback

- `rest_client.c`: minimal HTTP/1.1 (POST/GET with `?token=`), no TLS yet.
- `vTelemetryTask`: `POST /api/v1/telemetry/health` every 30 s (heap, per-task HWM,
  uptime, version, temperature and power from the diagnostics) and batched events.
- Fallback state: with the WS down, `vLightPollTask` sends
  `POST /api/v1/telemetry/state` (max. 1 Hz).
- `vCommandPollTask`: with the WS down, it polls `GET /api/v1/commands/pending`
  every 1 s, executes the command and acknowledges it with `POST /api/v1/commands/{id}/ack|nack`.
- Variables: `ORCH_HTTP_URL` (`http://127.0.0.1:8080`) and `DEVICE_ID` (`lamp-8f2a`).

## Vendored crypto

- **libsodium 1.0.22** (ISC) → Ed25519; **mbedTLS 3.6.8 LTS** (Apache-2.0) → AES-256-GCM and SHA-256.
- Static build with CMake (`third_party/libsodium`, `third_party/mbedtls`); no system packages.
- `test_crypto` validates the RFC 8032 (Ed25519), NIST SP 800-38D (AES-256-GCM) and SHA-256("abc") vectors.

## Bootloader and OTA

- `build/bin/bootloader` (standalone process, no FreeRTOS) applies the `.ota`
  from staging, chooses the A/B slot and runs the app with `memfd_create` +
  `fexecve` (fallback to `/tmp`).
- Variables: `FLASH_PATH`, `FACTORY_APP` (slot A provisioning),
  `OTA_PUBLIC_KEY_PATH`, `OTA_AES_KEY` (32 B hex) and `HW_REV` (1).
- The app streams the `.ota` into staging (`ota_client.c`), disarms
  `ITIMER_REAL`/SIGALRM (the FreeRTOS tick survives `execve`) and does an
  `execv` to `BOOTLOADER_BIN`.
- Command `ota_update` (`{"version":"1.1.0"}`), `sim_reboot` (restart from the
  Simulation Panel) and `POST /instances/{id}/ota`.
- Confirmation: `v_confirm_task` confirms the slot after ~3 s if Modbus is
  connected; otherwise the bootloader rolls back (FAULT 13) on the next
  boot.
- Fault codes: 8 signature/GCM, 9 downgrade, 10 SHA, 11 hardware, 12 format,
  13 rollback.

## Test

```bash
cmake -B build-test -G Ninja -DBUILD_TESTS=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

Current tests (9): `test_flash`, `test_net_transport`, `test_cmd_handler`,
`test_rest_client`, `test_crypto`, `test_ota_verify` (fixtures generated by
`tools/package_ota.py`), `test_ab` (rollback/double buffering), `test_register_map`
and `test_sequence`.

## Supervisor

- `./build/bin/supervisor` launches `./build/bin/bootloader` with `fork`/`execve`;
  the bootloader runs the app from the active slot.
- Line-based TCP control channel on `SUPERVISOR_PORT` (5022; private network):
  `STATUS`, `POWER_ON`, `POWER_OFF`, `RESET`, `ENTER_BOOTLOADER`,
  `FACTORY_RESET` (ADR-004 amended). Optional token `SUPERVISOR_TOKEN`.
- Auto power-on when the container starts; `POWER_OFF` leaves the supervisor alive
  in `POWERED_OFF` so it can be powered on again.
- `SIGTERM`/`SIGINT` sent to the supervisor → `SIGTERM` to the child, wait ~3 s and `SIGKILL`.
- States: `POWERED_OFF → BOOTING → APP_RUNNING / BOOTLOADER_MODE`; an unexpected
  crash moves to `FAULT` with the exit code.
- `ENTER_BOOTLOADER` persists the flag and the bootloader waits in recovery
  until `POWER_ON`. `FACTORY_RESET` erases `FLASH_PATH` and reprovisions from
  `FACTORY_APP`.
- Known limitation: if the supervisor receives `SIGKILL`, the child may be left orphaned (option A, documented).
