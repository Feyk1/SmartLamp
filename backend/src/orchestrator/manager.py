"""Lifecycle of the ephemeral instances.

In `local` mode the instances are in-memory entries and the firmware runs in
a fixed compose container. In `fly` mode each instance is a Fly.io App +
Machine: here it is created when a session opens and destroyed when it expires,
when the dashboard closes, or when cleaning up orphans.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from src import settings
from src.orchestrator.machines_api import Machine, MachinesApi, MachinesApiError
from src.store.memory import Instance, store

logger = logging.getLogger("lamp.orchestrator")

APP_PREFIX = "lamp-inst-"
_API: MachinesApi | None = None


def get_machines_api() -> MachinesApi:
    """Singleton client (tests replace it with monkeypatch)."""
    global _API
    if _API is None:
        _API = MachinesApi()
    return _API


def reset_machines_api() -> None:
    """Forgets the client (tests and token changes)."""
    global _API
    _API = None


def app_name(instance_id: str) -> str:
    return f"{APP_PREFIX}{instance_id.removeprefix('inst-')}"


def machine_config(instance: Instance) -> dict[str, Any]:
    """Machine config: base image, its own 6PN, and auto-destroy."""
    env = {
        "DEVICE_TOKEN": instance.device_token,
        "DEVICE_ID": f"lamp-{instance.id.removeprefix('inst-')}",
        "ORCH_WS_URL": f"{settings.PUBLIC_WS_BASE}/ws/device",
        "ORCH_HTTP_URL": settings.PUBLIC_HTTP_BASE,
        "IDLE_TIMEOUT_S": str(settings.IDLE_TIMEOUT_S),
        "SUPERVISOR_PORT": "5022",
        "MODBUS_PORT": "5020",
        "MODBUS_HOST": "127.0.0.1",
    }
    # The AES key travels as a Machine env var: the Machines API has no
    # per-Machine secrets. Anyone with the organization token can
    # read it; for the demo that is acceptable (see docs/deployment.md).
    if settings.OTA_AES_KEY:
        env["OTA_AES_KEY"] = settings.OTA_AES_KEY

    return {
        "name": instance.id,
        "region": settings.FLY_REGION,
        "config": {
            "image": (
                f"registry.fly.io/{settings.FLY_INSTANCE_BASE_APP}"
                f":{settings.FLY_IMAGE_TAG}"
            ),
            "auto_destroy": True,
            "restart": {"policy": "no"},
            "guest": {"cpu_kind": "shared", "cpus": 1, "memory_mb": 256},
            "env": env,
            "metadata": {"instance_id": instance.id},
        },
    }


async def provision(instance: Instance) -> None:
    """Creates the instance's App + Machine and stores its private IP.

    If anything fails it tries to delete the App to avoid leaving orphaned
    resources and propagates `MachinesApiError`.
    """
    if settings.INSTANCE_MODE != "fly":
        return

    api = get_machines_api()
    app = app_name(instance.id)
    machine: Machine | None = None
    try:
        await api.create_app(app)
        machine = await api.create_machine(app, machine_config(instance))
        machine = await api.wait_started(machine, timeout_s=30)
    except MachinesApiError:
        await api.delete_app(app)
        raise

    instance.mode = "fly"
    instance.fly_app = app
    instance.machine_id = machine.id
    instance.supervisor_host = machine.private_ip
    logger.info(
        "instance %s on fly: app=%s machine=%s ip=%s",
        instance.id,
        app,
        machine.id,
        machine.private_ip,
    )


async def destroy(instance: Instance) -> None:
    """Deletes the App (and its Machine) if the instance belongs to Fly."""
    if instance.mode != "fly" or not instance.fly_app:
        return
    try:
        await get_machines_api().delete_app(instance.fly_app)
        logger.info("instance %s destroyed (%s)", instance.id, instance.fly_app)
    except MachinesApiError as exc:
        logger.warning("could not destroy %s: %s", instance.fly_app, exc)
    finally:
        instance.fly_app = None
        instance.machine_id = None
        instance.supervisor_host = None


async def cleanup_expired() -> int:
    """Destroys the expired instances; returns how many."""
    expired = store.pop_expired()
    for instance in expired:
        await store.close_instance(instance)
        await destroy(instance)
    return len(expired)


async def cleanup_orphans() -> list[str]:
    """Deletes `lamp-inst-*` Apps that no longer have an instance in memory."""
    if settings.INSTANCE_MODE != "fly":
        return []
    api = get_machines_api()
    known = {app_name(instance_id) for instance_id in store.ids()}
    removed: list[str] = []
    for app in await api.list_apps():
        if app.startswith(APP_PREFIX) and app not in known:
            try:
                await api.delete_app(app)
                removed.append(app)
            except MachinesApiError as exc:
                logger.warning("could not delete orphan %s: %s", app, exc)
    if removed:
        logger.info("orphan apps deleted: %s", ", ".join(removed))
    return removed


async def cleanup_loop(
    interval_s: float = 60.0, orphan_every_s: float = 600.0
) -> None:
    """Background task: expires instances and sweeps orphans periodically."""
    elapsed = 0.0
    while True:
        await asyncio.sleep(interval_s)
        elapsed += interval_s
        try:
            await cleanup_expired()
            if elapsed >= orphan_every_s:
                elapsed = 0.0
                await cleanup_orphans()
        except MachinesApiError as exc:
            logger.warning("cleanup failed: %s", exc)
