# Documentation — Smart Lamp Demo

**Read this documentation as a site:**
<https://feyk1.github.io/SmartLamp/docs/>

This directory contains the project's public documentation. The root README is
the recommended entry point; here is the complete map for going deeper. The
same content is published as a MkDocs site under `/docs/` on GitHub Pages.

## Start here

| If you want to… | Read |
| :--- | :--- |
| Understand the project in 5 minutes | [Vision and scope](00-vision-and-scope.md) |
| See the architecture and components | [01 — Architecture](01-general-architecture.md) |
| Understand REST, WebSocket and the Modbus map | [02 — Communications](02-communications.md) and [03 — Registers](03-modbus-register-map.md) |
| Review OTA security | [04 — OTA and image security](04-ota-and-image-security.md) |
| Understand the panels and their flows | [07 — Dashboards and UX](07-dashboards-ux.md) |
| Use the demo step by step | [User manual](manual/README.md) |
| Ask the local rule-based assistant | [Local assistant](manual/11-assistant.md) |
| Review the threat model | [08 — Security](08-security-and-threat-model.md) and [threat model](threat-model.md) |
| Deploy the demo (Fly.io reference) | [deployment.md](deployment.md) and [06 — Deployment](06-deployment.md) |
| See the decisions and their rationale | [ADRs](adr/index.md) |

## Architecture and protocols

- [00 — Vision and scope](00-vision-and-scope.md): goal, out of scope and
  success criteria.
- [01 — General architecture](01-general-architecture.md): components, layers,
  flows, data model and diagrams.
- [02 — Communications](02-communications.md): REST, WebSocket, telemetry,
  commands, fallback and rate limiting.
- [03 — Modbus register map](03-modbus-register-map.md): coils, discrete,
  holding and input; conventions and fault semantics.
- [04 — OTA and image security](04-ota-and-image-security.md): `.ota` format,
  Ed25519, AES-GCM, anti-downgrade and rollback.
- [05 — CI/CD](05-ci-cd.md): workflows, gates and secret management.
- [06 — Deployment](06-deployment.md): provider-neutral architecture and the
  Fly.io + Pages reference design.

## Security and operations

- [08 — Security and threat model](08-security-and-threat-model.md):
  executive summary, STRIDE and preserved vs. simulated properties.
- [threat-model.md](threat-model.md): detailed analysis.
- [auth.md](auth.md): JWT, `device_token`, WS tickets and permission matrix.
- [key-management.md](key-management.md): key inventory, generation and
  rotation; what can and cannot be simulated.
- [deployment.md](deployment.md): deployment steps, secrets, volumes, CORS,
  TLS and cleanup.

## UX and dashboards

- [07 — Dashboards and UX](07-dashboards-ux.md): user dashboard, Simulation
  Panel, pairing, sequences, schedules, faults, responsive design and
  accessibility.
- [User manual](manual/README.md): task-based guide to using the demo:
  getting started, dashboard, pairing, scenes, sequences, schedules, faults,
  OTA and the Sim panel.

## Decisions (ADR)

- [ADR-001](adr/adr-001-freertos-posix-port.md) — FreeRTOS POSIX port.
- [ADR-002](adr/adr-002-mmap-flash.md) — `mmap` to simulate flash.
- [ADR-003](adr/adr-003-execve-transitions.md) — `execve` between stages.
- [ADR-004](adr/adr-004-supervisor-pmic.md) — supervisor as PMIC.
- [ADR-005](adr/adr-005-modbus-driver-led.md) — Modbus TCP for the driver.
- [ADR-006](adr/adr-006-rest-ws-hybrid.md) — REST + WebSocket.
- [ADR-007](adr/adr-007-ephemeral-fly-app.md) — Ephemeral app per visit.
- [ADR-008](adr/adr-008-binaries-on-pages.md) — binaries on Pages.
- [ADR-009](adr/adr-009-ed25519-aes-gcm.md) — Ed25519 + AES-GCM.
- [ADR-010](adr/adr-010-jwt-vs-opaque-token.md) — JWT + opaque token.

## Diagrams and screenshots

- [`assets/architecture.svg`](assets/architecture.svg) — architecture.
- [`assets/ota-flow.svg`](assets/ota-flow.svg) — OTA flow.
- [`assets/fault-state.svg`](assets/fault-state.svg) — fault states.
- [`diagrams/`](diagrams/index.md) — editable Mermaid sources.
- [`assets/`](assets/index.md) — dashboard and Simulation Panel screenshots.

## Conventions

- Documentation, code comments, the UI and API errors are in **English**; the UI
  has an ES translation.
- Architectural decisions live in the ADRs; if a proposal contradicts them, it
  is discussed before implementation.
