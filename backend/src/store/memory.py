"""In-memory state of instances, WS connections, and pending commands."""

from __future__ import annotations

import asyncio
import secrets
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from fastapi import WebSocket

from src import settings


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def iso(value: datetime) -> str:
    return value.isoformat().replace("+00:00", "Z")


@dataclass
class Schedule:
    """A schedule of an instance (one-shot or recurring)."""

    id: str
    time: str
    timezone: str
    repeat: str
    days: list[int]
    action: dict[str, Any]
    enabled: bool
    next_run: datetime | None
    created_at: datetime
    last_run: datetime | None = None
    last_status: str | None = None
    last_error: str | None = None
    attempts: int = 0


@dataclass
class Instance:
    id: str
    created_at: datetime
    ttl_s: int
    session_token: str
    device_token: str
    last_heartbeat: float
    last_state: dict[str, Any] | None = None
    state_ts: str | None = None
    last_health: dict[str, Any] | None = None
    health_ts: str | None = None
    events: list[dict[str, Any]] = field(default_factory=list)
    dashboards: set[WebSocket] = field(default_factory=set)
    device: WebSocket | None = None
    pending: dict[str, asyncio.Future] = field(default_factory=dict)
    command_queue: list[dict[str, Any]] = field(default_factory=list)
    ws_pending: set[str] = field(default_factory=set)
    ota_mode: str = "legit"
    ota_version: str | None = None
    schedules: list[Schedule] = field(default_factory=list)
    # Simulated onboarding.
    provisioning_state: str = "provisioned"
    provisioning_ssid: str | None = None
    provisioning_ip: str | None = None
    provisioning_prev_state: dict[str, Any] | None = None
    provisioning_last_off: float = 0.0
    provisioning_cycles: list[float] = field(default_factory=list)
    provisioning_networks: list[str] = field(default_factory=list)
    provisioning_passwords: dict[str, str] = field(default_factory=dict)
    provisioning_allow_any: bool = False
    provisioning_failure: str = "none"
    provisioning_loaded: bool = False
    # Power-on behavior (hard power on).
    power_on_mode: str = "default"
    power_on_state: dict[str, Any] | None = None
    power_on_loaded: bool = False
    # Device lifecycle: auto-on once per instance, but never after an explicit
    # Power Off from the Simulation Panel.
    power_explicitly_off: bool = False
    auto_power_on_done: bool = False
    auto_start_running: bool = False
    # Orchestration: "local" or "fly".
    mode: str = "local"
    fly_app: str | None = None
    machine_id: str | None = None
    supervisor_host: str | None = None
    supervisor_port: int | None = None


class MemoryStore:
    def __init__(self) -> None:
        self._instances: dict[str, Instance] = {}

    def create(self, ttl_s: int, device_token: str) -> Instance:
        instance_id = "inst-" + secrets.token_hex(2)
        while instance_id in self._instances:
            instance_id = "inst-" + secrets.token_hex(2)
        instance = Instance(
            id=instance_id,
            created_at=utc_now(),
            ttl_s=ttl_s,
            session_token=secrets.token_urlsafe(24),
            device_token=device_token,
            last_heartbeat=time.time(),
        )
        self._instances[instance_id] = instance
        return instance

    def _expired(self, instance: Instance) -> bool:
        age = (utc_now() - instance.created_at).total_seconds()
        if age > instance.ttl_s:
            return True
        return (time.time() - instance.last_heartbeat) > settings.HEARTBEAT_TIMEOUT_S

    def get(self, instance_id: str) -> Instance | None:
        instance = self._instances.get(instance_id)
        if instance is None:
            return None
        if self._expired(instance):
            self._instances.pop(instance_id, None)
            return None
        return instance

    def touch(self, instance_id: str) -> Instance | None:
        instance = self.get(instance_id)
        if instance is not None:
            instance.last_heartbeat = time.time()
        return instance

    def find_by_device_token(self, token: str) -> Instance | None:
        """Most recent live instance of the device.

        All device channels (WS, REST telemetry, commands) and the
        `POST /instances` with `REUSE_DEVICE_INSTANCE` must resolve to the same
        instance; that is why the most recently created one is always chosen
        (the dict preserves insertion order).
        """
        candidates = [
            instance
            for instance in self._instances.values()
            if instance.device_token == token and not self._expired(instance)
        ]
        if not candidates:
            return None
        return candidates[-1]

    async def close_instance(self, instance: Instance) -> None:
        """Closes an instance's sockets and pending commands."""
        for dashboard in list(instance.dashboards):
            try:
                await dashboard.close(code=1000)
            except Exception:
                pass
        if instance.device is not None:
            try:
                await instance.device.close(code=1000)
            except Exception:
                pass
        for future in instance.pending.values():
            if not future.done():
                future.cancel()

    async def delete(self, instance_id: str) -> bool:
        instance = self._instances.pop(instance_id, None)
        if instance is None:
            return False
        await self.close_instance(instance)
        return True

    def cleanup(self) -> None:
        for instance_id in list(self._instances):
            self.get(instance_id)

    def all(self) -> list[Instance]:
        """Live instances (without forcing expiration)."""
        return [
            instance
            for instance in self._instances.values()
            if not self._expired(instance)
        ]

    def ids(self) -> list[str]:
        """Ids of the live instances (without forcing expiration)."""
        return [
            instance_id
            for instance_id, instance in self._instances.items()
            if not self._expired(instance)
        ]

    def pop_expired(self) -> list[Instance]:
        """Removes and returns the expired instances (for cleanup)."""
        expired: list[Instance] = []
        for instance_id, instance in list(self._instances.items()):
            if self._expired(instance):
                self._instances.pop(instance_id, None)
                expired.append(instance)
        return expired

    def reset(self) -> None:
        self._instances.clear()


store = MemoryStore()
