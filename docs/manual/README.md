# User Manual — Smart Lamp Demo

This is the guide for **using** the demo, not for understanding its architecture.
If you're after the internal design, start with the
[documentation home](../README.md) and the
[general architecture](../01-general-architecture.md).

## Before you start

- Have Docker and Docker Compose installed.
- Run the demo from the repository root:
  ```bash
  cp .env.example .env
  docker compose up --build
  ```
- Open:
  - Dashboard: <http://localhost:3000>
  - Simulation Panel: <http://localhost:3000/sim.html?pin=2025>
- The **demo PIN** is `2025`.
- The dashboard is in **English by default**; switch to Spanish with the
  `EN / ES` selector or add `?lang=es`.
- The lamp is **simulated**: there is no real hardware, but the firmware, the
  Modbus protocol, OTA and faults behave as they would on a real device.

## Quick path: 5 minutes

1. Turn the lamp on with the large button in the dashboard.
2. Switch between `White` and `Color`, move the brightness slider and try a scene.
3. Open the Sim panel with PIN `2025` and open `Modbus` to see registers.
4. Under `Faults`, inject an overtemperature warning and then a critical fault.
5. Under `Firmware`, check `Current: vX · slot A|B` and try a downgrade.
6. If you want to pair Wi-Fi, follow [Pairing](02-pairing.md).

## Task guides

| I want to… | Guide |
| :--- | :--- |
| Understand the main screen | [Dashboard](01-dashboard.md) |
| Pair the lamp with Wi-Fi | [Pairing](02-pairing.md) |
| Use scenes, effects and party mode | [Scenes and effects](03-scenes-effects.md) |
| Create a color sequence | [Sequences](04-sequences.md) |
| Schedule a time or a sunrise | [Schedules](05-schedules.md) |
| Understand and clear faults | [Faults](06-faults.md) |
| Update firmware and understand slots | [OTA and slots](07-ota-slots.md) |
| Use the Simulation Panel | [Sim panel](08-simulation-panel.md) |
| Solve a problem | [Common issues](09-troubleshooting.md) |
| See every control | [Controls reference](10-controls-reference.md) |
| Ask the local assistant | [Local assistant](11-assistant.md) |

## Conventions

- `Like this` indicates a button, label or value visible in the interface.
- `→` indicates a path: `Sim panel → Device → Power On`.
- UI text is in English by default; the Spanish translation appears in
  parentheses when it helps.
- Light changes are sent over WebSocket; if the backend or firmware is
  unavailable, the control is locked and a notice appears.

## Demo state

- The dashboard creates or reuses an instance and keeps a heartbeat.
- The LED driver remembers its last state across restarts.
- Schedules live in the backend; sequences and adaptive white live in the
  browser.
- The Sim panel factory reset clears flash, driver, schedules, power-on and
  history. The open dashboard resynchronizes.

## If something does not respond

Start with [Common issues](09-troubleshooting.md). The three most common
problems are: backend down, controls locked because there is no Wi-Fi, and a
critical fault still latched.

[Back to the documentation home](../README.md)
