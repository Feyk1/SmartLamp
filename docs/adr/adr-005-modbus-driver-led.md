# ADR-005 — Modbus TCP between firmware and LED driver

- **Date:** 2026-11-18
- **Status:** Accepted
- **Decision makers:** Project maintainer
- **Project context:** The firmware needs to communicate with a simulated LED driver that runs the animation sequences and keeps the real state of the LED.

## Context

The firmware must:

- Send the **commanded state** (color, brightness, mode, sequence) to the LED driver.
- Read the **actual state** (interpolated color, current sequence step, LED temperature) from the driver.
- Detect faults (overtemperature, overcurrent, open LED) reported by the driver.
- Coordinate sequence execution (the driver animates them locally at 20 Hz).

The LED driver is a separate process (see ADR-004), and the communication between the two must be:

- **Bidirectional** (the firmware writes commands, reads state).
- **With a clear separation between commands and state** (target vs actual).
- **With low latency** (the firmware polls the state at 20 Hz).
- **Faithful to a real product** (in a smart lamp, the main MCU communicates with a dedicated LED driver over an industrial protocol).

Three protocols were evaluated:

1. **Modbus TCP** (standard industrial protocol).
2. **REST over HTTP** (the driver exposes REST endpoints).
3. **WebSocket** (persistent bidirectional communication).

## Decision

**Modbus TCP is chosen** for the communication between firmware and LED driver.

The LED driver acts as a **Modbus TCP server** (slave), and the firmware as a **Modbus TCP client** (master). The register map is defined in [`03-modbus-register-map.md`](../03-modbus-register-map.md).

## Alternatives considered

### Alternative A: REST over HTTP

**Advantages:**
- Easy to implement and debug (`curl`, Postman, Bruno).
- Easy to test with standard tools.
- No additional dependencies in the firmware (if it already uses REST for telemetry).
- Clear semantics (GET for state, PUT/POST for commands).

**Drawbacks:**
- **Not faithful to a real product:** in a smart lamp, the main MCU does not speak HTTP to the LED driver; it speaks SPI, I2C, UART or Modbus.
- **High overhead:** every state read is an HTTP request with headers, JSON parsing, etc. At 20 Hz, that is a lot of overhead.
- **No natural separation between target and actual:** in REST, both would be endpoints of the same server, which blurs the separation.
- **Higher latency:** HTTP has more overhead than Modbus (TCP handshake, headers, parsing).
- **Does not allow representing 16-bit registers naturally:** a JSON schema would have to be defined, which adds complexity.

**Discarded because:** it breaks fidelity to real hardware and adds unnecessary overhead for a low-latency communication.

### Alternative B: WebSocket

**Advantages:**
- Persistent bidirectional communication (the driver can notify changes to the firmware without polling).
- Low latency (no handshake per message).
- Clear semantics (messages with `type` and `payload`).

**Drawbacks:**
- **Not faithful to a real product:** in a smart lamp, the LED driver does not speak WebSocket to the main MCU.
- **Requires a WebSocket server in the driver,** which adds complexity (library, connection handling, reconnection).
- **Does not allow representing 16-bit registers naturally:** a message schema would have to be defined, which adds complexity.
- **The firmware already uses WebSocket for the backend:** adding another WebSocket for the driver complicates the firmware (two WS clients, two reconnections, two message handlers).
- **No natural separation between target and actual:** both would be messages on the same channel.

**Discarded because:** it breaks fidelity to real hardware and adds unnecessary complexity to the firmware.

### Alternative C: Modbus TCP (chosen)

**Advantages:**
- **Faithful to a real product:** Modbus is the standard protocol in industrial integrations, and many commercial smart lamps use variants (Modbus RTU over RS-485, or Modbus TCP over Ethernet).
- **Natural separation between target and actual:** holding registers are target (write), input registers are actual (read). It is the standard Modbus separation.
- **Low latency:** a Modbus TCP frame is ~20-40 bytes, with no headers or JSON parsing.
- **Efficient at 20 Hz:** the firmware polls 9 registers every 50 ms with a single FC04 transaction.
- **Clear semantics:** coils for commands, discrete inputs for flags, holding for configuration, input for state. It is the standard Modbus model.
- **Mature library:** `libmodbus` is stable, BSD, and widely used.
- **Easy to test:** `modbus-cli`, `mbpoll`, or any standard Modbus client can read/write registers.

**Drawbacks:**
- **Harder to implement than REST:** it requires understanding the register model, the function codes (FC01, FC03, FC04, FC05, FC06, FC16), and the byte order (CDAB for 32-bit).
- **Not as easy to debug as REST:** you cannot use `curl`; you have to use Modbus-specific tools.
- **Unit ID and byte order:** they are classic sources of interoperability bugs. Mitigation: CDAB is documented explicitly.
- **No push notifications:** the firmware must poll the state; the driver cannot notify changes. This is intentional (Modbus is request-response), but it adds up to 50 ms of latency.

**Chosen because:** it is the option that best preserves fidelity to real hardware, the target/actual separation, and efficiency at 20 Hz.

## Consequences

### Positive

- **Fidelity to real hardware:** the communication model is the same as in an industrial product.
- **Target/actual separation:** holding registers are target, input registers are actual. It is the standard Modbus separation.
- **Efficiency:** at 20 Hz, traffic is ~800 bytes/s, negligible.
- **Testability:** any standard Modbus client can read/write registers.
- **Portability:** the same register map works over Modbus TCP or Modbus RTU (with a TCP↔RTU bridge).
- **Simulation Panel:** it can write directly to the registers to inject faults or force values.

### Negative

- **More code in the firmware:** the Modbus client must handle connection, reconnection, timeouts, and byte order.
- **More code in the driver:** the Modbus server must handle multiple clients, function codes, and the register map.
- **No push:** the firmware must poll; the driver cannot notify. This adds up to 50 ms of latency.
- **CDAB byte order:** it is a classic source of bugs. Mitigation: unit tests that verify the conversion.
- **Unit ID 1:** in TCP it is not strictly necessary, but it is kept for compatibility with future TCP↔RTU bridges.

### Neutral

- **The LED driver is a Modbus TCP server:** it listens on port 502 (or 5020 to avoid privileges).
- **The firmware is a Modbus TCP client:** it connects to the driver at `localhost:502` (or `driver:502` in Docker).
- **The register map is the same as in a real product:** it is documented in [`03-modbus-register-map.md`](../03-modbus-register-map.md).

## Implementation notes

### LED driver (Modbus TCP server)

- Uses `libmodbus` in server mode (`modbus_new_tcp`, `modbus_tcp_listen`, `modbus_tcp_accept`).
- Implements the register map in `register_map.c`:
  - Coils: `CMD_APPLY_TARGET`, `CMD_POWER_ON`, etc.
  - Discrete inputs: `STATUS_POWER_ON`, `STATUS_OVERTEMP_CRITICAL`, etc.
  - Holding registers: `TARGET_HUE`, `TARGET_SAT`, `SEQ_ID`, etc.
  - Input registers: `ACTUAL_HUE`, `DIAG_TEMP_C_X10`, etc.
- The sequence animation runs in a separate thread at 20 Hz, updating the input registers.
- The heartbeat watchdog is implemented in another thread: if `HEARTBEAT_COUNTER` does not change within `CFG_HEARTBEAT_TIMEOUT_S`, it turns the LED off.

### Firmware (Modbus TCP client)

- Uses `libmodbus` in client mode (`modbus_new_tcp`, `modbus_connect`).
- `vLightPollTask` polls `ACTUAL_*` at 20 Hz with FC04.
- `vDiagPollTask` polls `DIAG_*` at 1 Hz with FC04.
- `vHeartbeatTask` writes `HEARTBEAT_COUNTER` at 1 Hz with FC06.
- `vWatchdogTask` reads `STATUS_*` at 5 Hz with FC01.
- Commands are written with FC16 (multiple registers) or FC05 (coils).
- Automatic reconnection if the driver does not respond (timeout 1 s, 2 retries).

### Byte order

- 16-bit: big-endian (Modbus standard).
- 32-bit: **CDAB** (low word first, big-endian within each word). This convention is documented in this ADR and in the register map.
- Unit test that verifies: `uint32_t x = 0x12345678; assert(read_reg(addr) == 0x5678); assert(read_reg(addr+1) == 0x1234);`

## References

- [`03-modbus-register-map.md`](../03-modbus-register-map.md) — complete register map
- [`01-general-architecture.md`](../01-general-architecture.md) — section 3.4
- [Modbus Application Protocol V1.1b3](https://modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
- [libmodbus documentation](https://libmodbus.org/documentation/)
