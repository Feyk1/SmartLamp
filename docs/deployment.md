# Deployment guide

The demo runs locally with Docker Compose and does not require any cloud account.
This guide first describes the provider-neutral deployment and then the reference
implementation that ships with the repository: **Fly.io + GitHub Pages**.

The reference deployment is live:

- Dashboard: <https://feyk1.github.io/SmartLamp/>
- Documentation: <https://feyk1.github.io/SmartLamp/docs/>
- Orchestrator health: <https://lamp-demo-orchestrator.fly.dev/health>

The design and the trade-offs are in [`06-deployment.md`](06-deployment.md); here
you find the steps and what is already implemented.

## 1. Deployment models

| Model | When to use it | What runs |
| :--- | :--- | :--- |
| Local demo | Development, review, recording | `docker compose up --build`, one firmware container |
| Self-hosted Docker | A VM or server you control | Compose services behind your reverse proxy |
| Reference: Fly.io + Pages | Public demo | One orchestrator Machine, one ephemeral Machine per visit, static dashboard and docs on Pages |

The orchestrator has two instance modes (`INSTANCE_MODE`):

- `local`: instances are in-memory entries and the firmware runs in the fixed
  container from `docker compose` (default).
- `fly`: every `POST /api/v1/instances` creates a real App + Machine with its own
  `device_token`; the orchestrator talks to the supervisor over the private
  network. This is the adapter included in the repository.

A different provider needs an adapter with the same contract: create an isolated
instance, return its HTTP/WS and Modbus endpoints, and destroy it on expiry.

## 2. Deploy on any Docker host

### 2.1 Requirements

- Docker Engine with Compose v2.
- A reverse proxy with TLS (Caddy, nginx, Traefik) if the dashboard is public.
- Persistent storage for `backend-data`, `driver-state` and `firmware-flash`.
- The `.ota` artifacts in `firmware/releases/` (or a directory mounted at
  `/data/ota`).

### 2.2 Configuration

Copy `.env.example` to `.env` and change at least:

| Variable | Use |
| :--- | :--- |
| `JWT_SECRET` | Sign the dashboard sessions; use a long random value |
| `DEVICE_TOKEN` | Token the firmware uses in local mode |
| `SIM_PIN` | Simulation Panel PIN (demo: `2025`) |
| `OTA_AES_KEY` | Decrypt the `.ota` files |
| `DASHBOARD_ORIGINS` | Public dashboard origin allowed by CORS |
| `PUBLIC_WS_BASE` / `PUBLIC_ORCHESTRATOR_URL` | Public URLs the browser uses |
| `INSTANCE_MODE` | `local` for a single host |

Never commit `.env`, `OTA_AES_KEY` or the private signing key.

### 2.3 Run

```bash
docker compose up --build -d
curl -s http://localhost:8080/health
# {"status":"ok"}
```

Default ports: dashboard `3000`, orchestrator `8080`, Modbus `5020` (keep Modbus
private; it has no authentication), supervisor `5022` (bind to localhost or the
container network).

Put the reverse proxy in front of the orchestrator and the dashboard, terminate
TLS there and forward WebSocket upgrades. The dashboard expects HTTPS/WSS in
production; with plain HTTP the browser may block the WS.

### 2.4 Persistence and OTA artifacts

- `backend-data` keeps `schedules.db` and `provisioning.db`.
- `driver-state` keeps `driver_state.bin`.
- `firmware-flash` keeps `flash.bin` and the A/B slots.
- Mount the signed `.ota` files at `/data/ota`. Without artifacts,
  `/api/v1/ota/latest` returns `409` on purpose.

Back up `backend-data` if schedules and paired networks matter. Losing
`PROVISIONING_KEY` makes the stored Wi-Fi passwords undecryptable; devices must
be paired again.

### 2.5 Verify

1. Open the dashboard, complete pairing and turn the lamp on.
2. Send a command and confirm the state changes in the UI and Modbus.
3. Open the Simulation Panel with the PIN and inject a warning, then a trip.
4. Deploy an OTA artifact and confirm the new version and slot.
5. Restart the stack and check that the driver keeps its last state.

## 3. Reference deployment: Fly.io + GitHub Pages

This is the implementation that ships in `fly.toml`, the workflows and
`infra/`. Use it as a working example, not as a requirement.

### 3.1 What gets deployed

| Piece | Where | File |
| :--- | :--- | :--- |
| Orchestrator (FastAPI + WS) | Fly.io, 1 Machine awake (schedules) | `fly.toml`, `infra/Dockerfile.orchestrator` |
| Ephemeral instance (supervisor + firmware + driver) | 1 App + 1 Machine per visit | `infra/instance/Dockerfile`, `infra/instance/entrypoint.sh` |
| Static dashboard | GitHub Pages | `.github/workflows/deploy-pages.yml` |
| `.ota` artifacts | Pages (`firmware/<version>/`) | `.github/workflows/release-firmware.yml` |

### 3.2 Prerequisites

- Fly.io account with an organization token (`FLY_ORG_TOKEN`) that can create
  and delete Apps/Machines.
- `flyctl` installed and authenticated (`fly auth login`).
- A CI token (`FLY_API_TOKEN`) stored as a GitHub secret; it is different from
  the organization token the orchestrator uses at runtime.
- GitHub repository with Pages enabled (Settings → Pages → GitHub Actions).

### 3.3 Variables and secrets

In the orchestrator (Fly secrets; never in `fly.toml`):

| Variable | Use |
| :--- | :--- |
| `FLY_ORG_TOKEN` | Create/delete Apps and Machines |
| `JWT_SECRET` | Sign the session JWTs (HTTPS protects them in transit) |
| `DEVICE_TOKEN` | Device token in local mode; on Fly one is generated per instance |
| `SIM_PIN` | Simulation Panel PIN (demo: `2025`) |
| `OTA_AES_KEY` | Decryption key for the `.ota` files (each Machine receives it) |
| `DASHBOARD_ORIGINS` | Pages origin allowed by CORS |

In `fly.toml` (non-secret values): `INSTANCE_MODE=fly`, `PUBLIC_HTTP_BASE`,
`PUBLIC_WS_BASE`, `FLY_REGION`, `HEARTBEAT_TIMEOUT_S`, `INSTANCE_TTL_S`,
`OTA_DIR`, `SCHEDULES_DB` and `PROVISIONING_DB`.

In GitHub Actions:

- `FLY_API_TOKEN` (`production` environment): deploy the orchestrator and publish
  the instance image.
- `OTA_SIGNING_KEY` and `OTA_AES_KEY` (`release` environment): sign and encrypt
  the `.ota` files in `release-firmware.yml`.

### 3.4 Orchestrator

```bash
fly apps create lamp-demo-orchestrator

# Optional persistence of schedules/credentials and OTA artifacts.
# Uncomment [[mounts]] in fly.toml and create the volume:
fly volumes create lamp_data --size 1 --region iad

fly secrets set \
  FLY_ORG_TOKEN=... \
  JWT_SECRET=... \
  DEVICE_TOKEN=... \
  SIM_PIN=2025 \
  OTA_AES_KEY=... \
  DASHBOARD_ORIGINS=https://feyk1.github.io
fly deploy
```

`fly deploy` must be run from the repository root: the build context includes
`backend/` and `infra/`. The configuration lives in `/fly.toml` because flyctl
uses the directory of `fly.toml` as the context.

Verification:

```bash
curl -s https://lamp-demo-orchestrator.fly.dev/health
# {"status":"ok"}
```

### 3.5 Instance image

The image is published to the Fly registry. The base App must exist (it is only
used as an image repository, it is not deployed):

```bash
fly apps create lamp-instance-base
fly auth docker
docker build -f infra/instance/Dockerfile -t registry.fly.io/lamp-instance-base:latest .
docker push registry.fly.io/lamp-instance-base:latest
```

The `deploy-instance-image.yml` workflow does the same in CI. The image includes
the driver, firmware, bootloader and supervisor; the driver listens on
`127.0.0.1:5020` and the supervisor on `0.0.0.0:5022`; `entrypoint.sh` exposes
both on IPv6 for the 6PN with `socat`. It weighs
≈130 MB with `debian:bookworm-slim`; the binaries add up to less than 1 MB.

### 3.6 Dashboard and artifacts

1. Run `release-firmware.yml` with the version (`1.4.0`): it publishes
   `firmware/<version>/firmware.ota` to Pages.
2. Run `deploy-pages.yml` to publish the dashboard.
3. Set `DASHBOARD_ORIGINS` to the real Pages origin.

The orchestrator reads the `.ota` files from `OTA_DIR`. On Fly there is no shared
volume: copy the artifacts into the orchestrator image or publish a volume with
`fly volumes create` and mount it at `/data/ota` (see §3.9).

### 3.7 What happens during a visit

1. The dashboard sends `POST /api/v1/instances`.
2. The orchestrator generates a unique `device_token` and creates
   `lamp-inst-<id>` + a Machine with `auto_destroy: true`.
3. It waits for the Machine to start and stores its private IP (6PN).
4. The Machine starts the driver and the supervisor; the firmware connects to
   `wss://<orchestrator>/ws/device` with its token.
5. The dashboard connects to `ws://<orchestrator>/ws/dashboard?instance=...`.
6. The Simulation Panel uses the Machine's private IP for Modbus (`5020`) and
   for the supervisor channel (`5022`).

When the session closes (`DELETE /instances/{id}`) or expires (`INSTANCE_TTL_S`
or `HEARTBEAT_TIMEOUT_S`), the orchestrator deletes the App and its Machine.

### 3.8 Cleanup and costs

- Expired instances are destroyed every minute.
- Every 10 minutes the `lamp-inst-*` Apps are listed and those without an
  instance in memory are deleted (orphans left by an orchestrator restart).
- The orchestrator stays awake (`auto_stop_machines = "off"`,
  `min_machines_running = 1`) because the schedules run in the backend.
  Instances remain ephemeral and destroy themselves.
- Without schedules you can go back to `suspend` / `0` to save costs, accepting
  that schedules will not run while the orchestrator sleeps.
- A 256 MB Machine running costs cents per hour; the Fly trial covers the demo.
  Check `fly apps list` and `fly machines list` if something was not destroyed.

Manual cleanup:

```bash
fly apps list | grep lamp-inst-
fly apps destroy lamp-inst-xxxx
```

### 3.9 Known limitations

- **OTA artifacts**: without a volume, `/api/v1/ota/latest` responds `409`. This
  is deliberate: the orchestrator does not invent releases.
- **AES key**: the Machines API has no per-Machine secrets; `OTA_AES_KEY` travels
  as an environment variable of the Machine. Anyone with the organization token
  can read it. For the demo this is acceptable; in production it would use a
  per-instance key and a KMS.
- **TLS/WSS**: on Fly TLS terminates at the edge (`force_https = true`); locally
  the demo still uses HTTP. The dashboard WS uses a single-use ticket valid for
  30 s (`WS_TICKET_TTL_S`), so the JWT does not travel in the URL. The ticket
  appears in the uvicorn logs but is invalidated on connect.
- **Schedules, credentials and power-on**: they are stored in SQLite
  (`/data/schedules.db` and `/data/provisioning.db`). On Fly, without a volume
  mounted at `/data`, they are lost on every redeploy; create a volume and mount
  it if you want to keep them. Set `PROVISIONING_KEY` as a secret
  (`fly secrets set`) to encrypt the passwords; otherwise it is derived from
  `JWT_SECRET`. The whitelist and the simulated fault are per instance and return
  to their default values on restart. With the Machine suspended the schedules do
  not run either: keep the orchestrator awake or use an external cron.
- **Credentials key**: if you change `PROVISIONING_KEY` or `JWT_SECRET`, the
  stored passwords can no longer be decrypted. The SSID/IP is preserved, but the
  device must be paired again to update the encrypted password.
- **No HTTP checks on the Machine**: startup is considered successful when the
  Machines API reports `started`; the firmware connects its WS afterwards. A
  startup failure shows up as an instance with no telemetry and `FAULT`/no state
  in the dashboard.
- **WS ticket in memory**: the demo uses a single uvicorn worker; with several
  workers the tickets would have to be shared (e.g. Redis).
- **Single region**: `iad` by default; change `FLY_REGION` if needed.

### 3.10 Typical problems

| Symptom | Likely cause | Action |
| :--- | :--- | :--- |
| `503 ORCHESTRATOR_UNAVAILABLE` | Invalid `FLY_ORG_TOKEN` or insufficient permissions | Check `fly secrets list` and the orchestrator logs |
| The Machine starts but there is no WS | Wrong `PUBLIC_WS_BASE` or the firmware cannot reach the orchestrator | `fly logs` of the Machine; check `ORCH_WS_URL` |
| `502 SUPERVISOR_ERROR` in the Sim Panel | The 6PN IP changed or the Machine is stopped | Check `STATUS`; recreate the instance |
| CORS blocks the dashboard | `DASHBOARD_ORIGINS` does not match Pages | Fix the secret and redeploy |
| Accumulated `lamp-inst-*` Apps | Orchestrator restarted before the sweep | Wait 10 min or delete them by hand |

## 4. Local mode

None of this is necessary for development:

```bash
docker compose up --build
```

With `INSTANCE_MODE=local` (default) the firmware from the compose setup is reused
and no provider API is called. The orchestrator tests use `httpx.MockTransport`,
so they do not need network or a token either.
