"""Instance orchestrator tests (simulated Machines API)."""

from __future__ import annotations

import asyncio
import json
import time
from typing import Any

import httpx
import pytest
from fastapi.testclient import TestClient

from src import settings
from src.main import app
from src.orchestrator import manager
from src.orchestrator.machines_api import MachinesApi, MachinesApiError
from src.store.memory import store

client = TestClient(app)


class FakeFly:
    """`httpx.MockTransport` handler that mimics the Machines API."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, str]] = []
        self.bodies: list[dict[str, Any]] = []
        self.apps: list[str] = ["lamp-instance-base"]
        self.fail_machines = False
        self.private_ip = "fdaa:0:1::42"

    def __call__(self, request: httpx.Request) -> httpx.Response:
        path = request.url.path
        method = request.method
        self.calls.append((method, path))
        if request.content:
            self.bodies.append(json.loads(request.content))

        if method == "GET" and path == "/v1/apps":
            return httpx.Response(
                200, json={"apps": [{"name": name} for name in self.apps]}
            )
        if method == "POST" and path == "/v1/apps":
            app_name = json.loads(request.content)["app_name"]
            self.apps.append(app_name)
            return httpx.Response(201, json={"id": app_name})
        if method == "DELETE" and path.startswith("/v1/apps/"):
            app = path.split("/")[-1]
            if app in self.apps:
                self.apps.remove(app)
            return httpx.Response(200, json={})
        if method == "POST" and path.endswith("/machines"):
            if self.fail_machines:
                return httpx.Response(500, json={"error": "no capacity"})
            return httpx.Response(
                200, json={"id": "m-1", "state": "created"}
            )
        if method == "GET" and path.endswith("/wait"):
            return httpx.Response(
                200,
                json={
                    "id": "m-1",
                    "state": "started",
                    "private_ip": self.private_ip,
                },
            )
        return httpx.Response(404, json={"error": f"unexpected {method} {path}"})


@pytest.fixture
def fly(monkeypatch) -> FakeFly:
    fake = FakeFly()

    def factory() -> MachinesApi:
        return MachinesApi(
            token="test",
            client=httpx.AsyncClient(
                base_url="https://api.machines.dev/v1",
                transport=httpx.MockTransport(fake),
            ),
        )

    monkeypatch.setattr(settings, "INSTANCE_MODE", "fly")
    monkeypatch.setattr(settings, "PUBLIC_HTTP_BASE", "https://orch.example")
    monkeypatch.setattr(settings, "PUBLIC_WS_BASE", "wss://orch.example")
    monkeypatch.setattr(manager, "get_machines_api", factory)
    return fake


def _create() -> tuple[dict, dict]:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201, response.text
    data = response.json()
    instance = store.get(data["id"])
    assert instance is not None
    return data, instance.__dict__


def test_provision_creates_app_and_machine(fly: FakeFly):
    data, instance = _create()

    assert instance["mode"] == "fly"
    assert instance["fly_app"] == f"lamp-{data['id']}"
    assert instance["machine_id"] == "m-1"
    assert instance["supervisor_host"] == "fdaa:0:1::42"
    assert instance["device_token"].startswith("dev-")
    assert instance["device_token"] != settings.DEVICE_TOKEN

    assert ("POST", "/v1/apps") in fly.calls
    assert ("POST", f"/v1/apps/lamp-{data['id']}/machines") in fly.calls

    machine_body = fly.bodies[1]["config"]
    assert machine_body["auto_destroy"] is True
    assert machine_body["image"].startswith("registry.fly.io/")
    assert machine_body["env"]["DEVICE_TOKEN"] == instance["device_token"]
    assert machine_body["env"]["ORCH_WS_URL"] == "wss://orch.example/ws/device"
    assert machine_body["env"]["ORCH_HTTP_URL"] == "https://orch.example"
    assert machine_body["guest"]["memory_mb"] == 256
    assert "/ws/dashboard?instance=" in data["ws_dashboard_url"]


def test_provision_failure_returns_503_and_rolls_back(fly: FakeFly):
    fly.fail_machines = True

    response = client.post("/api/v1/instances", json={})

    assert response.status_code == 503
    assert response.json()["error"]["code"] == "ORCHESTRATOR_UNAVAILABLE"
    assert store.ids() == []
    # The newly created App is deleted so as not to leave orphaned resources.
    assert any(
        method == "DELETE" and path.startswith("/v1/apps/lamp-")
        for method, path in fly.calls
    )


def test_delete_instance_destroys_app(fly: FakeFly):
    data, instance = _create()
    app = instance["fly_app"]

    response = client.delete(
        f"/api/v1/instances/{data['id']}",
        headers={"Authorization": f"Bearer {data['session_token']}"},
    )

    assert response.status_code == 204
    assert ("DELETE", f"/v1/apps/{app}") in fly.calls
    assert app not in fly.apps


def test_cleanup_expired_destroys_app(fly: FakeFly):
    data, instance = _create()
    app = instance["fly_app"]
    live = store.get(data["id"])
    assert live is not None
    live.last_heartbeat = time.time() - 10_000

    removed = asyncio.run(manager.cleanup_expired())

    assert removed == 1
    assert ("DELETE", f"/v1/apps/{app}") in fly.calls
    assert store.get(data["id"]) is None


def test_cleanup_orphans_only_touches_instances(fly: FakeFly):
    fly.apps = ["lamp-instance-base", "lamp-inst-orphan", "lamp-other"]

    removed = asyncio.run(manager.cleanup_orphans())

    assert removed == ["lamp-inst-orphan"]
    assert "lamp-inst-orphan" not in fly.apps
    assert "lamp-instance-base" in fly.apps


def test_local_mode_does_not_call_fly(monkeypatch):
    monkeypatch.setattr(settings, "INSTANCE_MODE", "local")

    def boom() -> MachinesApi:
        raise AssertionError("Fly must not be called in local mode")

    monkeypatch.setattr(manager, "get_machines_api", boom)

    response = client.post("/api/v1/instances", json={})

    assert response.status_code == 201
    instance = store.get(response.json()["id"])
    assert instance is not None
    assert instance.mode == "local"
    assert instance.fly_app is None


def test_missing_token_is_an_error(monkeypatch):
    monkeypatch.setattr(settings, "FLY_ORG_TOKEN", "")

    with pytest.raises(MachinesApiError, match="FLY_ORG_TOKEN"):
        MachinesApi(token="", base_url="https://api.machines.dev/v1")
