"""Supervisor power control tests from the Sim Panel."""

from __future__ import annotations

import json
import socketserver
import threading

import pytest
from fastapi.testclient import TestClient

from src import settings
from src.main import app
from src.store.memory import store

client = TestClient(app)


class _Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        line = self.rfile.readline().decode("ascii").strip()
        self.server.commands.append(line)  # type: ignore[attr-defined]
        command = line.split()[-1] if line else ""
        state = "APP_RUNNING"
        if command == "POWER_OFF":
            state = "POWERED_OFF"
        elif command == "FACTORY_RESET":
            state = "POWERED_OFF"
        elif command == "ENTER_BOOTLOADER":
            state = "BOOTLOADER_MODE"
        response = {"state": state, "pid": 7, "forced": False}
        self.wfile.write((json.dumps(response) + "\n").encode("ascii"))


@pytest.fixture()
def supervisor(monkeypatch):
    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), _Handler)
    server.commands = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    monkeypatch.setattr(settings, "SUPERVISOR_HOST", "127.0.0.1")
    monkeypatch.setattr(settings, "SUPERVISOR_PORT", server.server_address[1])
    monkeypatch.setattr(settings, "SUPERVISOR_TOKEN", "")
    yield server
    server.shutdown()
    server.server_close()


def _create_instance() -> dict:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    return response.json()


def _token(instance_id: str) -> str:
    response = client.post(
        f"/api/v1/instances/{instance_id}/sim/session",
        json={"pin": settings.SIM_PIN},
    )
    assert response.status_code == 200
    return response.json()["sim_token"]


def _headers(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


def test_power_status(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])

    response = client.get(
        f"/api/v1/instances/{instance['id']}/sim/power",
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert response.json()["state"] == "APP_RUNNING"
    assert supervisor.commands == ["STATUS"]


def test_power_off(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/power",
        json={"action": "off"},
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert response.json()["state"] == "POWERED_OFF"
    assert "POWER_OFF" in supervisor.commands

    logs = client.get(
        f"/api/v1/instances/{instance['id']}/sim/logs",
        headers=_headers(token),
    )
    assert any(item["op"] == "sim_power" for item in logs.json()["logs"])


def test_power_reset(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/power",
        json={"action": "reset"},
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert "RESET" in supervisor.commands


def test_factory_reset_requires_confirm(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/power",
        json={"action": "factory_reset"},
        headers=_headers(token),
    )

    assert response.status_code == 400
    assert response.json()["error"]["code"] == "CONFIRM_REQUIRED"
    assert "FACTORY_RESET" not in supervisor.commands


def test_factory_reset_with_confirm(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/power",
        json={"action": "factory_reset", "confirm": True},
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert "FACTORY_RESET" in supervisor.commands


def test_factory_reset_clears_panel_data(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])
    obj = store.get(instance["id"])
    obj.schedules.append(object())  # type: ignore[arg-type]
    obj.power_on_mode = "custom"
    obj.events.append({"type": "event", "op": "fault", "payload": {"code": 2}})

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/power",
        json={"action": "factory_reset", "confirm": True},
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert obj.schedules == []
    assert obj.power_on_mode == "default"
    assert not any(event.get("op") == "fault" for event in obj.events)


def test_invalid_power_action(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/power",
        json={"action": "explode"},
        headers=_headers(token),
    )

    assert response.status_code == 400
    assert response.json()["error"]["code"] == "INVALID_ACTION"


def test_bootloader_enter_and_exit(supervisor):
    instance = _create_instance()
    token = _token(instance["id"])

    enter = client.post(
        f"/api/v1/instances/{instance['id']}/sim/bootloader",
        json={"action": "enter"},
        headers=_headers(token),
    )
    assert enter.status_code == 200
    assert enter.json()["state"] == "BOOTLOADER_MODE"

    exit_ = client.post(
        f"/api/v1/instances/{instance['id']}/sim/bootloader",
        json={"action": "exit"},
        headers=_headers(token),
    )
    assert exit_.status_code == 200

    assert "ENTER_BOOTLOADER" in supervisor.commands
    assert "POWER_ON" in supervisor.commands


def test_supervisor_unreachable(monkeypatch):
    instance = _create_instance()
    token = _token(instance["id"])
    # No fixture: points to localhost on a closed port (fast failure).
    monkeypatch.setattr(settings, "SUPERVISOR_HOST", "127.0.0.1")
    monkeypatch.setattr(settings, "SUPERVISOR_PORT", 1)
    response = client.get(
        f"/api/v1/instances/{instance['id']}/sim/power",
        headers=_headers(token),
    )

    assert response.status_code == 502
    assert response.json()["error"]["code"] == "SUPERVISOR_ERROR"
