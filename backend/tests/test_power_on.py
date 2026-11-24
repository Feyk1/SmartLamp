"""Power-on behavior tests."""

from __future__ import annotations

import asyncio

from fastapi.testclient import TestClient

from src.main import app
from src.power_on import service as power_on_service
from src.store.memory import store

client = TestClient(app)


def _instance() -> tuple[dict, dict]:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    data = response.json()
    return data, {"Authorization": f"Bearer {data['session_token']}"}


def test_default_policy_is_default():
    instance, headers = _instance()
    response = client.get(
        f"/api/v1/instances/{instance['id']}/power-on", headers=headers
    )
    assert response.status_code == 200
    assert response.json() == {"mode": "default", "state": None}


def test_custom_policy_persists_by_device_token():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/power-on"
    state = {
        "mode": "color",
        "h": 120,
        "s": 1000,
        "v": 1000,
        "brightness": 450,
    }

    updated = client.put(
        base, json={"mode": "custom", "state": state}, headers=headers
    )
    assert updated.status_code == 200
    assert updated.json()["mode"] == "custom"
    assert updated.json()["state"] == state

    second = client.post("/api/v1/instances", json={}).json()
    loaded = client.get(
        f"/api/v1/instances/{second['id']}/power-on",
        headers={"Authorization": f"Bearer {second['session_token']}"},
    )
    assert loaded.status_code == 200
    assert loaded.json()["mode"] == "custom"
    assert loaded.json()["state"]["h"] == 120
    assert loaded.json()["state"]["brightness"] == 450


def test_default_mode_has_no_custom_state():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/power-on"

    response = client.put(base, json={"mode": "default"}, headers=headers)
    assert response.status_code == 200
    assert response.json() == {"mode": "default", "state": None}

    second = client.post("/api/v1/instances", json={}).json()
    loaded = client.get(
        f"/api/v1/instances/{second['id']}/power-on",
        headers={"Authorization": f"Bearer {second['session_token']}"},
    )
    assert loaded.json()["mode"] == "default"


def test_invalid_power_on_mode_and_state():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/power-on"

    invalid_mode = client.put(
        base, json={"mode": "bogus"}, headers=headers
    )
    assert invalid_mode.status_code == 400
    assert invalid_mode.json()["error"]["code"] == "INVALID_MODE"

    invalid_state = client.put(
        base, json={"mode": "custom", "state": None}, headers=headers
    )
    assert invalid_state.status_code == 400
    assert invalid_state.json()["error"]["code"] == "INVALID_STATE"


def _capture(monkeypatch, live) -> list[tuple[str, dict]]:
    calls: list[tuple[str, dict]] = []

    async def fake_wait(_instance):
        return True

    async def fake_send(_instance, op, payload, *, wait=True):
        del wait
        calls.append((op, payload))
        return True

    live.device = object()
    monkeypatch.setattr(power_on_service, "_wait_for_device", fake_wait)
    monkeypatch.setattr(power_on_service, "_send", fake_send)
    return calls


def test_apply_custom_power_on(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    calls = _capture(monkeypatch, live)
    power_on_service.update(
        live,
        "custom",
        {"mode": "color", "h": 120, "s": 1000, "v": 1000, "brightness": 450},
    )

    asyncio.run(power_on_service.apply_after_power_on(live))

    ops = [op for op, _payload in calls]
    assert ops == [
        "set_power",
        "set_mode",
        "set_color",
        "set_brightness",
    ]
    assert calls[2][1] == {"h": 120, "s": 1000, "v": 1000}
    assert calls[3][1] == {"value": 450}


def test_apply_default_power_on(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    calls = _capture(monkeypatch, live)
    power_on_service.update(live, "default", None)

    asyncio.run(power_on_service.apply_after_power_on(live))

    ops = [op for op, _payload in calls]
    assert ops == [
        "set_power",
        "set_mode",
        "set_color_temperature",
        "set_brightness",
    ]
    assert calls[2][1] == {"kelvin": 3000}
    assert calls[3][1] == {"value": 700}


def test_auto_start_applies_once(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    calls = _capture(monkeypatch, live)

    asyncio.run(power_on_service.auto_start(live))

    ops = [op for op, _payload in calls]
    assert ops == [
        "set_power",
        "set_mode",
        "set_color_temperature",
        "set_brightness",
    ]

    calls.clear()
    asyncio.run(power_on_service.auto_start(live))
    assert calls == []


def test_auto_start_respects_explicit_off(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    calls = _capture(monkeypatch, live)
    live.power_explicitly_off = True

    asyncio.run(power_on_service.auto_start(live))

    assert calls == []


def test_last_and_pairing_do_not_apply(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    calls = _capture(monkeypatch, live)

    power_on_service.update(live, "last", None)
    asyncio.run(power_on_service.apply_after_power_on(live))
    assert calls == []

    power_on_service.update(live, "default", None)
    live.provisioning_state = "pairing"
    asyncio.run(power_on_service.apply_after_power_on(live))
    assert calls == []
