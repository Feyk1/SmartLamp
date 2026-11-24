# ADR-007 — Ephemeral Fly App per visit

- **Date:** 2026-11-18
- **Status:** Accepted
- **Decision makers:** Project maintainer
- **Project context:** The demo must allow multiple simultaneous instances of a simulated lamp, each isolated from the others, and shut down on its own after inactivity, without manual infrastructure management.

## Context

The requirement of "multiple ephemeral instances per visit" implies:

- **Isolation:** one instance must not be able to access another (not over the network, not through the filesystem, not through tokens).
- **Ephemerality:** every instance is created on demand and destroyed after inactivity.
- **Controlled cost:** there must be no reserved resources when nobody visits the demo.
- **Observability:** every instance must report its state to its visitor's dashboard, without leaking data to others.
- **Operational simplicity:** the lifecycle of each instance must not be managed manually.

Three deployment architectures were evaluated:

1. **A single Machine with N supervised processes** (one VM, multiple firmwares in separate processes).
2. **A single Machine with nested Docker** (one VM, multiple containers per instance).
3. **An ephemeral Fly App per visit** (one App + dedicated Machine per instance).

## Decision

**An ephemeral Fly App per visit is chosen:**

- The orchestrator creates an App (`lamp-<uuid>`) with its own 6PN network.
- Inside the App, a Machine is created with `auto_destroy: true`.
- The Machine contains the supervisor, bootloader, firmware and LED driver.
- After inactivity (`IDLE_TIMEOUT_S`), the instance agent signals `auto_destroy` and Fly destroys the Machine.
- The orchestrator cleans its `instance_id → ws` map.

## Alternatives considered

### Alternative A: A single Machine with N supervised processes

**Advantages:**
- **No Machines API:** `FLY_ORG_TOKEN` and a Machines API client are not needed.
- **Simple deployment:** a single `fly.toml` and a `supervisord.conf`.
- **Fast startup:** there is no Fly cold start; the process starts in milliseconds.
- **Predictable cost:** a single Machine, always on or with scale-to-zero at the VM level.

**Drawbacks:**
- **No real isolation:** all instances share kernel, network and filesystem. A bug in one can affect the others.
- **No network isolation:** all instances share `localhost`; one firmware could talk to another instance's Modbus.
- **No token isolation:** all instances share the same environment; a leaked `device_token` could be used to impersonate another.
- **Scale-to-zero at the VM level:** if one instance is active, the whole VM is on, even if the others are idle.
- **One crash takes down all instances:** if the supervisor dies, all instances are lost.
- **Not faithful to a real product:** in a real lamp, each device is a separate physical unit.

**Discarded because:** it breaks isolation, which is a project requirement for the multi-tenant model.

### Alternative B: A single Machine with nested Docker

**Advantages:**
- **Process isolation:** each instance is a container with its own namespace.
- **Network isolation:** each container has its own network interface (bridge).
- **Relatively simple deployment:** a single `fly.toml` with Docker-in-Docker.
- **Predictable cost:** a single Machine.

**Drawbacks:**
- **Docker-in-Docker is problematic on Fly.io:** it requires privileges that Fly does not guarantee on standard Machines.
- **Docker overhead:** each container adds ~50-100 MB of overhead, which reduces instance density per VM.
- **Less isolation than microVMs:** containers share the kernel; a kernel exploit affects all instances.
- **Operational complexity:** managing container lifecycles inside a VM adds code and failure points.
- **Not faithful to a real product:** in a real lamp, each device has its own hardware, not a container in a shared VM.

**Discarded because:** Docker-in-Docker on Fly.io is fragile, and the isolation is lower than with microVMs.

### Alternative C: Ephemeral Fly App per visit (chosen)

**Advantages:**
- **Real isolation:** each instance is a Firecracker microVM with its own kernel, 6PN network and filesystem.
- **Network isolation:** each App has its own 6PN network; one instance cannot route to another.
- **Token isolation:** each instance has its own `device_token`, injected when the Machine is created.
- **Native ephemerality:** `auto_destroy: true` makes Fly destroy the Machine automatically.
- **Scale-to-zero per instance:** if an instance is idle, its Machine is destroyed without affecting the others.
- **Fidelity to a real product:** each instance is a separate physical unit, just like a real lamp.
- **Observability:** each instance reports its state to the orchestrator, which routes it to the correct visitor's dashboard.

**Drawbacks:**
- **Requires the Machines API:** the orchestrator needs `FLY_ORG_TOKEN` and a Machines API client.
- **Cold start:** creating an App + Machine takes ~2-5 s (although with `suspend` on the orchestrator, the cold start is ~hundreds of ms).
- **Cost per instance:** each Machine is billed per second of runtime (even if small).
- **Cleanup complexity:** if an instance is not destroyed correctly, it can be left orphaned. Mitigation: TTL in the orchestrator + `auto_destroy` on the Machine.
- **App limit per organization:** Fly has a (configurable) limit of Apps per organization; for a demo with 10 concurrent instances, it is enough.

**Chosen because:** it is the only option that preserves real isolation between instances, which is a project requirement.

## Consequences

### Positive

- **Real isolation:** each instance is a separate microVM, with its own kernel and network.
- **Native ephemerality:** `auto_destroy` + `IDLE_TIMEOUT_S` make the instance destroy itself.
- **Fidelity to a real product:** each instance is a separate physical unit.
- **Granular observability:** the orchestrator can route commands and telemetry to the correct instance.
- **Controlled cost:** you only pay for seconds of runtime; with 20 short visits per month, <$1/month.

### Negative

- **Machines API complexity:** the orchestrator needs an HTTP client to create/destroy Apps and Machines.
- **Cold start:** creating an instance takes ~2-5 s; the dashboard must show a spinner.
- **Orphan cleanup:** if the orchestrator fails between creating the App and creating the Machine, the App is left orphaned. Mitigation: TTL in the orchestrator + periodic cleanup job.
- **App limit:** Fly has a limit of Apps per organization; it is enough for a demo, but it does not scale to thousands of instances.

### Neutral

- **The orchestrator is a separate App:** `lamp-demo-orchestrator` with `auto_stop_machines = "suspend"`.
- **The base image is a "template" App:** `lamp-instance-base` with the supervisor + firmware + driver image.
- **Each instance is a new App:** `lamp-<uuid>` with one Machine.
- **The `device_token` is injected as an env var:** when creating the Machine, the orchestrator passes `DEVICE_TOKEN` as an environment variable.

## Implementation notes

### Creating an instance

```python
# backend/src/orchestrator/machines_api.py
async def create_instance(instance_id: str, device_token: str):
    app_name = f"lamp-{instance_id}"
    # 1. Create App
    await fly_api.post("/v1/apps", json={
        "app_name": app_name,
        "org_slug": FLY_ORG_SLUG,
        "network": app_name,  # own 6PN
    })
    # 2. Create Machine
    await fly_api.post(f"/v1/apps/{app_name}/machines", json={
        "config": {
            "image": "registry.fly.io/lamp-instance-base:latest",
            "auto_destroy": True,
            "env": {
                "DEVICE_TOKEN": device_token,
                "ORCH_WS_URL": "ws://lamp-demo-orchestrator.internal:8081/ws/device",
                "IDLE_TIMEOUT_S": "300",
            },
            "guest": {"cpu_kind": "shared", "cpus": 1, "memory_mb": 256},
        },
    })
```

### Destroying an instance

```python
async def destroy_instance(instance_id: str):
    app_name = f"lamp-{instance_id}"
    await fly_api.delete(f"/v1/apps/{app_name}")  # cascades to Machines
```

### Orphan cleanup

A periodic job (cron in the orchestrator or a GitHub Action) lists the `lamp-*` Apps and destroys those with no entry in the orchestrator map (orphaned by an orchestrator crash).

### Inactivity agent

```bash
#!/bin/bash
# agent-idle.sh
IDLE_TIMEOUT=${IDLE_TIMEOUT_S:-300}
LAST_ACTIVITY=$(date +%s)
while true; do
  sleep 10
  NOW=$(date +%s)
  [ -f /tmp/last_activity ] && LAST_ACTIVITY=$(cat /tmp/last_activity)
  if [ $((NOW - LAST_ACTIVITY)) -gt $IDLE_TIMEOUT ]; then
    kill -TERM 1  # signal to PID 1 (supervisor) so it exits cleanly
    exit 0
  fi
done
```

## References

- [`06-deployment.md`](../06-deployment.md) — section 4
- [`01-general-architecture.md`](../01-general-architecture.md) — section 2.3
- [Fly Machines API](https://fly.io/docs/machines/api/)
- [Fly.io — Private Networking (6PN)](https://fly.io/docs/reference/private-networking/)
- [Fly.io — Firecracker microVMs](https://fly.io/docs/reference/architecture/#firecracker)
