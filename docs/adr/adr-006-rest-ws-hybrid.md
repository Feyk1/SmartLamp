# ADR-006 — Hybrid REST + WebSocket between firmware and backend

- **Date:** 2026-11-18
- **Status:** Accepted
- **Decision makers:** Project maintainer
- **Project context:** The firmware needs to report telemetry to the backend and receive commands from the dashboard.

## Context

Communication between firmware and backend has two directions with very different characteristics:

**Firmware → Backend (telemetry):**
- Variable frequency: health every 30 s, state on-change (with deadband), events immediately or in batches.
- Tolerates latency of seconds.
- Does not require acknowledgment (fire-and-forget).
- Is easy to test with `curl`, Bruno or Postman.
- Benefits from caching and idempotent endpoints.

**Backend → Firmware (commands):**
- Low frequency but **critical latency**: the user presses "turn on" and expects <300 ms.
- Requires acknowledgment (ack/nack).
- Cannot be initiated from the backend if the firmware is behind NAT (which is the realistic case of a lamp in a home).
- Benefits from a persistent connection to avoid TLS handshakes per command.

Three transport architectures were evaluated:

1. **REST only** (with polling or long-polling for commands).
2. **WebSocket only** (everything over a single bidirectional channel).
3. **Hybrid REST + WebSocket** (REST for telemetry, WS for commands).

**MQTT** was also evaluated as an alternative, but it was discarded for the reasons detailed below.

## Decision

**A hybrid REST + WebSocket is chosen:**

- **REST over HTTPS** for telemetry (firmware → backend): `POST /telemetry/health`, `POST /telemetry/state`, `POST /telemetry/events`, `GET /ota/check`, `GET /ota/{version}/download`.
- **WebSocket over WSS** for commands (backend → firmware): `WS /ws/device` with commands, acks, nacks and urgent events.

Both channels are initiated **from the firmware** (outgoing client), which is NAT-friendly.

## Alternatives considered

### Alternative A: REST only (with polling or long-polling)

**Advantages:**
- A single protocol, simpler to implement.
- Easy to test with `curl`.
- No connection state in the backend.

**Drawbacks:**
- **Polling:** either you poll fast (traffic and battery) or you poll slow (latency). In a demo, 2 s of latency looks bad.
- **Long-polling:** every command is a new TCP+TLS connection (full handshake, ~2 RTT of overhead). With TLS, that is ~200-400 ms per command.
- **Long-polling adds complexity:** timeouts, reconnections, commands lost between connections.
- **It is not what commercial products do:** Yeelight, Shelly and Tuya use WS variants for commands.

**Discarded because:** polling latency is unacceptable for the demo, and long-polling adds complexity without benefit.

### Alternative B: WebSocket only

**Advantages:**
- A single bidirectional channel, minimal latency.
- No polling or long-polling.
- Immediate command acknowledgment.

**Drawbacks:**
- **Telemetry does not fit well in WS:** health events (30 s) and state events (on-change) are unidirectional and do not need acknowledgment. Sending them over WS occupies a persistent connection for data that could go over REST.
- **Hard to test:** you cannot use `curl`; you have to use a WS client.
- **No caching:** REST allows caching responses (for example, `GET /ota/versions`); WS does not.
- **No natural idempotency:** commands over WS require a manual correlation `id`; REST has idempotent methods (GET, PUT).
- **If the WS goes down, everything is lost:** telemetry and commands share the same channel, which increases the impact of a disconnection.

**Discarded because:** telemetry does not benefit from a persistent bidirectional channel, and the testability of REST is lost.

### Alternative C: Hybrid REST + WebSocket (chosen)

**Advantages:**
- **Each protocol does what it does best:** REST for reporting/querying (idempotent, cacheable, testable), WS for ordering/notifying (minimal latency, bidirectional).
- **Minimal command latency:** a persistent WS connection avoids TLS handshakes per command.
- **Testability:** REST telemetry can be tested with `curl`, Bruno or Postman.
- **Idempotency:** REST endpoints are naturally idempotent (GET, PUT), which simplifies the backend logic.
- **Caching:** `GET /ota/versions` can be cached; telemetry responses do not need caching.
- **Resilience:** if the WS goes down, REST telemetry keeps working (with retries); if REST goes down, WS commands keep working.
- **Realism:** it is the pattern used by commercial products such as Yeelight, Shelly and Tuya.

**Drawbacks:**
- **Two transports to maintain:** the firmware needs a REST client (`libcurl` or similar) and a WS client (`libwebsockets` or similar).
- **Two reconnections:** the firmware must handle WS reconnection (with backoff) and REST retries (with backoff).
- **Two sets of tests:** unit tests for REST and for WS.
- **More code in the firmware:** two clients, two error handlers, two TLS configurations.

**Chosen because:** the balance between latency, testability, realism and resilience is the best of the three options.

### Alternative D: MQTT (discarded)

**Advantages:**
- De facto standard in IoT.
- Guaranteed QoS (0, 1, 2).
- Decoupled pub/sub.
- Scales to millions of devices.

**Drawbacks:**
- **Requires an additional broker** (Mosquitto, HiveMQ, EMQX), which adds a moving piece and a single point of failure.
- **The backend would have to subscribe to the broker** and forward to the dashboard, which adds latency and complexity.
- **Adds no advantages over REST+WS for a single-instance demo:** massive scalability and guaranteed QoS are not project requirements.
- **Unnecessary complexity for the demo scope:** maintaining a broker, configuring TLS, managing topics, etc.
- **The firmware already uses REST and WS:** MQTT would add a third technology without proportional demonstrative value.

**Discarded because:** it adds an infrastructure piece (the broker) without a proportional benefit for a single-instance demo.

## Consequences

### Positive

- **Minimal command latency:** the user presses "turn on" and the LED responds in milliseconds.
- **Testability:** REST telemetry is tested with `curl`, Bruno or Postman.
- **Realism:** it is the pattern of commercial products.
- **Resilience:** if one transport fails, the other keeps working.
- **Separation of responsibilities:** REST for reporting, WS for ordering.
- **NAT-friendly:** both channels are initiated from the firmware.

### Negative

- **Two transports:** the firmware needs two clients (REST and WS) and two TLS configurations.
- **Two reconnections:** the firmware must handle backoff for both.
- **More code:** two error handlers, two sets of tests.
- **Larger attack surface:** two channels to authenticate, two endpoints to rate-limit.

### Neutral

- **The firmware opens two outgoing connections to the backend:** a persistent WS and REST connections on demand.
- **The orchestrator keeps an `instance_id → ws_device` map:** to route commands.
- **The `GET /commands/pending` fallback:** if the WS goes down, the firmware can keep receiving commands through slow polling until it reconnects.

## Implementation notes

### Firmware

- **REST client:** `libcurl` (or `mbedtls` directly if you want to avoid the dependency).
- **WS client:** `libwebsockets` (with TLS) or `mongoose` (lighter).
- **WS reconnection:** exponential backoff (1s, 2s, 4s, 8s, 16s, max 30s; no jitter).
- **REST retries:** exponential backoff (1s, 2s, 4s, max 3 retries).
- **Fallback:** if the WS is down, `vCommsTask` performs `GET /commands/pending` every 5 s.

### Backend

- **WS gateway:** FastAPI with `WebSocket` (native) or `starlette`.
- **REST API:** FastAPI with `pydantic` for validation.
- **Routing:** the orchestrator keeps `instance_id → ws_device` and forwards commands.
- **Command timeout:** 2 s; if the firmware does not respond, a `nack` with `TIMEOUT` is returned.

### TLS

- Both channels use TLS (`https://` and `wss://`).
- In local development, `http://` and `ws://` can be used (with an `INSECURE=true` environment variable).
- In production, Fly.io provides automatic TLS for the orchestrator.

## References

- [`02-communications.md`](../02-communications.md) — detail of endpoints and messages
- [`01-general-architecture.md`](../01-general-architecture.md) — section 6
- [`libcurl` documentation](https://curl.se/libcurl/)
- [`libwebsockets` documentation](https://libwebsockets.org/)
- [Yeelight Inter-Operation Spec](https://www.yeelight.com/download/Yeelight_Inter-Operation_Spec.pdf)
- [Shelly API documentation](https://shelly-api-docs.shelly.cloud/)
