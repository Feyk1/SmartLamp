# 00 — Vision and scope

## 1. Objective

Build a **public, functional and self-contained demo** that demonstrates practical embedded firmware work, integrating:

- FreeRTOS firmware running as a POSIX process.
- Bootloader with cryptographic verification and A/B update with rollback.
- Modbus TCP communication between the firmware and a simulated LED driver.
- Backend with REST + WebSocket, orchestration of ephemeral instances.
- User dashboard and simulation panel (two distinct surfaces).
- CI/CD with OTA image signing and encryption.
- Observability: RTOS metrics, state timeline, logs, events.

The demo must be reachable from a browser **without manual infrastructure management**.

## 2. Use case

**Smart lamp** with:

- Power, color (HSV), brightness and color temperature control.
- Animated sequences (breathing, rainbow, strobe, candle, sunrise).
- Predefined scenes.
- Secure OTA update.
- Diagnostics (LED temperature, current, faults).

The lamp was chosen because:

1. **Universally understood:** any visitor understands what it does.
2. **Visually appealing:** the actual color can be shown in real time.
3. **Enough technical depth:** it allows modeling Modbus, sequences, alarms, OTA and A/B without forcing the use case.

## 3. Functional scope

### 3.1 Firmware

| Feature | Included |
| :--- | :---: |
| FreeRTOS scheduler with tasks, priorities and mutexes | ✅ |
| Modbus TCP client towards the LED driver | ✅ |
| REST client (telemetry) | ✅ |
| WebSocket client (commands, acks, events) | ✅ |
| Simulated watchdog | ✅ |
| A/B metadata and OTA staging in `flash.bin` (mmap); driver config in `driver_state.bin` | ✅ |
| RTOS metrics (stack HWM, heap) | ✅ |
| Device state machine | ✅ |

### 3.2 Bootloader

| Feature | Included |
| :--- | :---: |
| Ed25519 verification of `.ota` | ✅ |
| AES-256-GCM decryption | ✅ |
| Anti-downgrade via monotonic version | ✅ |
| A/B slots and rollback | ✅ |
| Recovery mode (`force_bootloader`) | ✅ |
| Boot confirmation by the app | ✅ |

### 3.3 Simulated LED driver

| Feature | Included |
| :--- | :---: |
| Modbus TCP server with the full register map | ✅ |
| Local sequence animation at 20 Hz | ✅ |
| Commanded state vs. actual state kept separate | ✅ |
| Heartbeat watchdog | ✅ |
| Internal REST API for fault injection | ✅ |

### 3.4 Backend

| Feature | Included |
| :--- | :---: |
| `POST /instances` with ephemeral Fly App creation | ✅ |
| Dashboard WS and device WS | ✅ |
| REST telemetry (health, state, events) | ✅ |
| OTA orchestration (3 modes: legit, corrupt, downgrade) | ✅ |
| Rate limiting and authentication | ✅ |
| Per-heartbeat TTL and destruction on inactivity | ✅ |

### 3.5 Dashboards

| Feature | Included |
| :--- | :---: |
| User Panel (control, state, scenes, manual firmware update) | ✅ |
| Simulation Panel (6 tabs: device, firmware, modbus, faults, logs, network) | ✅ |
| Single actual-state swatch | ✅ |
| Real-time state updates (no client-side interpolation) | ✅ |
| Reconnection with backoff and resync | ✅ |

> The dashboard has **one** swatch with the actual state. A dual swatch
> (actual vs commanded) is a future improvement, not implemented; the
> Simulation Panel's Modbus tab shows target and actual registers side by side.
>
> A ✅ in these tables means the feature is part of the demo scope, not that it
> is deployed. Fly.io and Pages deployment are pending; see the README for the
> current status.

### 3.6 CI/CD and infrastructure

| Feature | Included |
| :--- | :---: |
| POSIX build + unit tests | ✅ |
| OTA packaging (Ed25519 + AES-GCM) | ✅ |
| Independent verification of the `.ota` | ✅ |
| Release to GitHub Releases + copy to Pages | ✅ |
| Deploy orchestrator and instance to Fly.io | ✅ |
| Deploy dashboard to Pages | ✅ |

## 4. Out of scope (explicit)

| Excluded | Reason | Documented next step |
| :--- | :--- | :--- |
| Firmware compilation by the visitor | Heavy WASM toolchain (~70 MB) and disconnected from the real system | Isolated WASM playground |
| Cortex-M emulation with QEMU on the critical path | The POSIX port already runs the same firmware logic; QEMU adds a second toolchain and no new demo behavior (ADR-001) | Optional hardware-in-the-loop smoke test |
| Real hardware | The goal is faithful simulation in POSIX | Bring-up on STM32/ATSAM |
| Remote signing via HSM | Production key management; the demo signs with a local Ed25519 seed in GitHub Secrets | AWS KMS/CloudHSM, YubiHSM or a PKCS#11 signing service |
| SSO in the Simulation Panel | A static PIN is enough for a demo | OAuth2 + auditing |
| mTLS firmware↔backend | PKI complexity out of scope for the demo | Certificate rotation |
| RDP (Read-Out Protection) | Not simulatable in POSIX | Documented in the threat model |
| SoC Secure Boot | Not simulatable in POSIX | Documented in the threat model |
| WASM in the browser | Route 3 discarded for the main scope | Optional static demo |

> The table mixes alternatives that were evaluated and discarded with production
> hardening that is intentionally outside the demo. HSM, mTLS and SSO belong to
> the second group: the demo implements the functional path, not the production
> key/identity infrastructure.

## 5. Success criteria

The demo is considered successful if a visitor can, **without installing anything**:

1. Open the public URL; the dashboard creates or reuses an ephemeral instance automatically.
2. See an ephemeral instance boot in <5 s.
3. Turn the lamp on, change color and brightness, and see the change reflected in <300 ms.
4. Start a sequence and observe smooth animation.
5. From the Simulation Panel, deploy a legitimate OTA and watch the full cycle.
6. Inject a corrupt OTA and see it rejected without switching slots.
7. Inject a fault (overtemp) and see it reflected in both views with different levels of detail.
8. Close the tab and trust that the instance will shut itself down.

Additionally, anyone must be able to clone the repo and run `docker compose up` to reproduce the demo locally.

## 6. Design principles

| Principle | Implication |
| :--- | :--- |
| **Simulation honesty** | The documentation states what is preserved and what cannot be simulated in POSIX |
| **Separation of surfaces** | The User Panel and the Simulation Panel share neither permissions nor level of detail |
| **Target vs actual** | The driver owns the actual state; the firmware owns the target |
| **Zero business logic in the frontend** | The dashboard only renders and reconciles |
| **The backend never initiates connections** | Firmware and dashboard are always outbound clients (NAT-friendly) |
| **Incremental validation** | Each part is tested individually as it is built, and the full flow is exercised end to end |
| **Documented decisions** | Every non-obvious choice has an ADR |

## 7. High-level risks

The three main risks:

1. **Fly Machines API complexity** → plan B with a single Machine and supervised processes.
2. **Fly.io trial expiring during development** → local development with `docker compose`, deploy at the end.
3. **Scope creep** → frozen MoSCoW backlog; new wishes go to "Won't".
