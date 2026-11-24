# Simulated LED driver

Modbus TCP server that simulates the LED driver of a smart lamp.
It is a separate process; it does not link FreeRTOS.

## Responsibilities

- Run animation sequences at 20 Hz.
- Keep target (holding) vs. actual (input) separation.
- Heartbeat watchdog (turns the LED off if the firmware dies).
- Internal REST API for fault injection from the Simulation Panel.

## Register map

See [`docs/03-modbus-register-map.md`](../docs/03-modbus-register-map.md).
The documented addresses are 1-based; the code uses 0-based indices.

## Build

```bash
cmake -B build -G Ninja
cmake --build build
./build/bin/driver
```

- `MODBUS_PORT` (default `5020`; `502` requires privileges).
- `libmodbus` vendored in `third_party/libmodbus/` (SHARED; the build rpath
  resolves `libmodbus.so.5`).

## Test

```bash
cmake -B build-test -G Ninja -DBUILD_TESTS=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

Current tests: `test_register_map`, `test_sequence`.

## Implemented

- Coils 00001-00011: commands (auto-reset after the edge) and watchdog enable.
- Discrete inputs 10001-10011: power, sequence, config dirty, faults,
  overtemp, overcurrent, heartbeat lost, selftest.
- Holding 40001-40041: target (color/brightness/transition), sequence,
  configuration and `HEARTBEAT_COUNTER`.
- Input 30001-30031: actual (interpolated) and simulated diagnostics.
- Animation at 20 Hz: `breathing`, `rainbow`, `strobe`, `candle`, `police`,
  `sunrise` (IDs 1-6).
- Transitions with `TARGET_TRANSITION_MS`.
- Heartbeat watchdog (`CFG_HEARTBEAT_TIMEOUT_S`, disabled by default).

## Pending

- Scenes, gamma and `CFG_POWER_ON_BEHAVIOR` are stored and range-checked, but
  the driver does not apply them yet; the dashboard implements scenes. Let a
  qualified reviewer sign off before enabling either in the driver.

## Notes

- The server handles one TCP client at a time (enough for the firmware).
  Multi-client (`select`/threads) remains a future improvement.
- `CFG_HEARTBEAT_TIMEOUT_S` and `ENABLE_HEARTBEAT_WATCHDOG` are configured by the
  firmware on connect (5 s by default). If the firmware disappears, the
  watchdog turns the LED off.
- After each request, only the written range (FC05/06/15/16) is applied to the
  register_map; this way commands that touch holdings (e.g.
  `CMD_POWER_ON` → `TARGET_POWER`) are not overwritten.

## Dependencies

- `libmodbus` (Modbus TCP server; vendored in `third_party/libmodbus/`).
- `pthread` (server, animation and watchdog threads).
- `libmicrohttpd` or similar (internal REST API, optional; not used yet).

## References

- [`docs/adr/adr-005-modbus-driver-led.md`](../docs/adr/adr-005-modbus-driver-led.md)
