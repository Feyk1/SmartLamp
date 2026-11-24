# Backend

FastAPI orchestrator with REST + WebSocket and a Fly Machines API client.

## Structure

| Directory | Purpose |
| :--- | :--- |
| `src/main.py` | FastAPI entry point |
| `src/api/` | REST endpoints (instances, telemetry, firmware) |
| `src/ws/` | WebSocket gateway (dashboard and device) |
| `src/auth/` | JWT, `device_token`, rate limiting |
| `src/orchestrator/` | Fly Machines API client |
| `src/store/` | In-memory instance state |
| `tests/` | Tests with pytest |

## Development

```bash
cd backend
python -m venv .venv && source .venv/bin/activate
pip install -e ".[test]"
uvicorn src.main:app --reload
```

## Test

```bash
pytest
ruff check src/
mypy src/
```

## Current status

- REST: instances, heartbeat, state, telemetry, commands, schedules, sequences,
  OTA, provisioning, power-on and the Simulation Panel API.
- WS: `/ws/device?token=...` and `/ws/dashboard?instance=...` with a single-use
  ticket; fan-out to multiple tabs and a 2 s command timeout.
- Instances are in memory with TTL/heartbeat; schedules and provisioning data
  use SQLite. Local mode reuses the compose firmware; `fly` mode creates one
  ephemeral Machine per visit.
- Security: JWT dashboard sessions, opaque device tokens, rate limiting and
  configurable CORS. The Simulation Panel uses a PIN.
- Variables: `DEVICE_TOKEN` (dev-token-local), `PUBLIC_WS_BASE`
  (ws://localhost:8080), `COMMAND_TIMEOUT_S` (2.0).

## References

- [`docs/02-communications.md`](../docs/02-communications.md)
- [`docs/08-security-and-threat-model.md`](../docs/08-security-and-threat-model.md)
- [`docs/auth.md`](../docs/auth.md)
