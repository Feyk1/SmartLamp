# Architecture Decision Records

| ADR | Decision |
| :--- | :--- |
| [ADR-001](adr-001-freertos-posix-port.md) | FreeRTOS POSIX port as the firmware runtime environment |
| [ADR-002](adr-002-mmap-flash.md) | `mmap` over a file to simulate flash memory |
| [ADR-003](adr-003-execve-transitions.md) | `execve` to simulate the jump between bootloader and application |
| [ADR-004](adr-004-supervisor-pmic.md) | Supervisor as PMIC and reset controller |
| [ADR-005](adr-005-modbus-driver-led.md) | Modbus TCP between firmware and LED driver |
| [ADR-006](adr-006-rest-ws-hybrid.md) | Hybrid REST + WebSocket between firmware and backend |
| [ADR-007](adr-007-ephemeral-fly-app.md) | Ephemeral Fly App per visit |
| [ADR-008](adr-008-binaries-on-pages.md) | `.ota` binaries served from GitHub Pages, not from Releases |
| [ADR-009](adr-009-ed25519-aes-gcm.md) | Ed25519 + AES-256-GCM for OTA image signing and encryption |
| [ADR-010](adr-010-jwt-vs-opaque-token.md) | JWT for the dashboard and opaque token for the firmware |
