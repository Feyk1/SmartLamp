"""Telemetry REST and command fallback tests (docs/02 §4)."""

from __future__ import annotations

import time

from fastapi.testclient import TestClient

from src import settings
from src.main import app
from src.store.memory import store

client = TestClient(app)
TOKEN = settings.DEVICE_TOKEN


def _create_instance() -> dict:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    return response.json()


def _subscribe(dashboard) -> None:
    dashboard.send_json({"type": "subscribe", "channels": ["state", "events"]})
    assert dashboard.receive_json()["type"] == "subscribed"


def _receive_until(websocket, predicate, attempts: int = 10) -> dict:
    for _ in range(attempts):
        message = websocket.receive_json()
        if predicate(message):
            return message
    raise AssertionError("expected message was not received")


def test_health_requires_device_token():
    _create_instance()

    response = client.post("/api/v1/telemetry/health", json={"uptime_s": 1})

    assert response.status_code == 401
    assert response.json()["error"]["code"] == "UNAUTHORIZED"


def test_health_accepts_fly_instance_token():
    instance = store.create(ttl_s=3600, device_token="dev-fly-instance")

    response = client.post(
        "/api/v1/telemetry/health?token=dev-fly-instance",
        json={"device_id": "lamp-fly", "uptime_s": 5},
    )

    assert response.status_code == 204
    assert instance.last_health is not None
    assert instance.last_health["uptime_s"] == 5


def test_health_serial_is_exposed_in_instance():
    instance = _create_instance()

    response = client.post(
        f"/api/v1/telemetry/health?token={TOKEN}",
        json={
            "device_id": "lamp-8f2a",
            "uptime_s": 5,
            "serial": "SL-1-12345678",
            "hw_rev": 1,
        },
    )
    assert response.status_code == 204

    info = client.get(
        f"/api/v1/instances/{instance['id']}",
        headers={"Authorization": f"Bearer {instance['session_token']}"},
    ).json()
    assert info["serial"] == "SL-1-12345678"
    assert info["health"]["serial"] == "SL-1-12345678"
    assert info["health"]["hw_rev"] == 1


def test_health_is_stored_and_exposed():
    instance = _create_instance()

    response = client.post(
        f"/api/v1/telemetry/health?token={TOKEN}",
        json={
            "device_id": "lamp-8f2a",
            "ts": "2026-11-18T00:00:00Z",
            "uptime_s": 42,
            "fw_version": "1.0.3",
            "slot": "A",
            "mem": {"free_heap": 1000, "min_free_heap": 500},
            "tasks": {"comms": 2048},
            "led_temp_c": 41.5,
            "power_estimate_w": 5.5,
        },
    )
    assert response.status_code == 204

    info = client.get(
        f"/api/v1/instances/{instance['id']}",
        headers={"Authorization": f"Bearer {instance['session_token']}"},
    ).json()
    assert info["health"]["uptime_s"] == 42
    assert info["health"]["mem"]["free_heap"] == 1000
    assert info["health_ts"] == "2026-11-18T00:00:00Z"


def test_state_is_broadcast_to_dashboard():
    instance = _create_instance()

    with client.websocket_connect(
        f"/ws/dashboard?instance={instance['id']}"
        f"&session={instance['session_token']}"
    ) as dashboard:
        _subscribe(dashboard)

        response = client.post(
            f"/api/v1/telemetry/state?token={TOKEN}",
            json={
                "ts": "2026-11-18T00:00:01Z",
                "state": {"power": True, "brightness": 500},
            },
        )
        assert response.status_code == 204

        message = _receive_until(dashboard, lambda m: m["type"] == "state")
        assert message["payload"]["power"] is True
        assert message["ts"] == "2026-11-18T00:00:01Z"


def test_events_are_stored_and_replayed_on_subscribe():
    instance = _create_instance()

    response = client.post(
        f"/api/v1/telemetry/events?token={TOKEN}",
        json={
            "events": [
                {
                    "ts": "2026-11-18T00:00:02Z",
                    "type": "boot",
                    "detail": {"slot": "A", "cause": "power_on"},
                },
                {
                    "ts": "2026-11-18T00:00:03Z",
                    "type": "fault",
                    "detail": {"code": 2},
                },
            ]
        },
    )
    assert response.status_code == 204

    with client.websocket_connect(
        f"/ws/dashboard?instance={instance['id']}"
        f"&session={instance['session_token']}"
    ) as dashboard:
        _subscribe(dashboard)
        seen = set()
        for _ in range(4):
            message = dashboard.receive_json()
            if message["type"] == "event":
                seen.add(message["op"])
            if {"boot", "fault"} <= seen:
                break
        assert {"boot", "fault"} <= seen


def test_command_is_queued_and_acked_via_rest():
    instance = _create_instance()

    with client.websocket_connect(
        f"/ws/dashboard?instance={instance['id']}"
        f"&session={instance['session_token']}"
    ) as dashboard:
        _subscribe(dashboard)
        dashboard.send_json(
            {
                "id": "c-1",
                "type": "command",
                "op": "set_power",
                "payload": {"on": True},
            }
        )

        # Without a device WS, the command stays queued for the fallback.
        commands: list[dict] = []
        for _ in range(20):
            response = client.get(f"/api/v1/commands/pending?token={TOKEN}")
            assert response.status_code == 200
            commands = response.json()["commands"]
            if commands:
                break
            time.sleep(0.05)

        assert commands and commands[0]["id"] == "c-1"
        assert commands[0]["op"] == "set_power"
        assert commands[0]["payload"] == {"on": True}

        ack = client.post(
            f"/api/v1/commands/c-1/ack?token={TOKEN}",
            json={"op": "set_power", "payload": {}},
        )
        assert ack.status_code == 204

        message = _receive_until(dashboard, lambda m: m["type"] == "ack")
        assert message["id"] == "c-1"
        assert message["op"] == "set_power"


def test_pending_commands_requires_device_token():
    _create_instance()

    response = client.get("/api/v1/commands/pending")

    assert response.status_code == 401
    assert response.json()["error"]["code"] == "UNAUTHORIZED"
