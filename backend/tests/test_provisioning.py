"""Simulated onboarding tests (pairing and fake Wi-Fi)."""

from __future__ import annotations

import asyncio
import json
import sqlite3

import pytest
from fastapi.testclient import TestClient

from src import settings
from src.main import app
from src.provisioning import service
from src.provisioning.storage import decrypt_password
from src.store.memory import store

client = TestClient(app)


@pytest.fixture(autouse=True)
def _fast_provisioning(monkeypatch):
    monkeypatch.setattr(settings, "PROVISIONING_DELAY_S", 0)
    monkeypatch.setattr(settings, "PROVISIONING_TIMEOUT_S", 0)
    monkeypatch.setattr(settings, "PROVISIONING_FAILURE_DELAY_S", 0)

    async def noop(_instance):
        return None

    monkeypatch.setattr(service, "blink_later", noop)
    monkeypatch.setattr(service, "restore", noop)


def _instance() -> tuple[dict, dict]:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    data = response.json()
    return data, {"Authorization": f"Bearer {data['session_token']}"}


def _sim_token(instance_id: str) -> dict:
    response = client.post(
        f"/api/v1/instances/{instance_id}/sim/session",
        json={"pin": "2025"},
    )
    assert response.status_code == 200
    return {"Authorization": f"Bearer {response.json()['sim_token']}"}


def test_pairing_name_uses_serial_suffix():
    instance, headers = _instance()
    live = store.get(instance["id"])
    live.last_health = {"serial": "SL-1-LA-2647-64508ED1"}

    data = client.get(
        f"/api/v1/instances/{instance['id']}/provisioning",
        headers=headers,
    ).json()
    assert data["name"] == "SmartLamp-8ED1"


def test_pairing_scan_connect_and_reset():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"

    initial = client.get(base, headers=headers)
    assert initial.status_code == 200
    assert initial.json()["state"] == "provisioned"
    assert client.get(f"{base}/scan", headers=headers).json()["devices"] == []

    sim_headers = _sim_token(instance["id"])
    started = client.post(f"{base}/start", headers=sim_headers)
    assert started.status_code == 200
    assert started.json()["state"] == "pairing"

    scan = client.get(f"{base}/scan", headers=headers)
    devices = scan.json()["devices"]
    assert len(devices) == 1
    assert devices[0]["name"].startswith("SmartLamp-")
    assert devices[0]["rssi"] < 0

    connected = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "demo-password"},
        headers=headers,
    )
    assert connected.status_code == 200, connected.text
    body = connected.json()
    assert body["state"] == "online"
    assert body["ssid"] == "Casa-Demo"
    assert body["ip"].startswith("192.168.1.")
    assert "demo-password" not in json.dumps(body)

    current = client.get(base, headers=headers).json()
    assert current["state"] == "online"

    events = store.get(instance["id"]).events
    assert "demo-password" not in json.dumps(events)
    assert any(event["op"] == "provisioning" for event in events)

    # Returning to pairing keeps the previous configuration.
    again = client.post(f"{base}/start", headers=sim_headers)
    assert again.status_code == 200
    assert again.json()["ssid"] == "Casa-Demo"

    canceled = client.post(f"{base}/cancel", headers=sim_headers)
    assert canceled.status_code == 200
    assert canceled.json()["state"] == "online"
    assert canceled.json()["ssid"] == "Casa-Demo"

    again = client.post(f"{base}/start", headers=sim_headers)
    assert again.status_code == 200
    reset = client.post(f"{base}/reset", headers=sim_headers)
    assert reset.status_code == 200
    assert reset.json()["state"] == "pairing"
    assert reset.json()["ssid"] is None


def test_connect_requires_pairing_and_session():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"

    anonymous = client.get(base)
    early = client.post(
        f"{base}/connect",
        json={"ssid": "Casa", "password": "x"},
        headers=headers,
    )

    assert anonymous.status_code == 401
    assert early.status_code == 409
    assert early.json()["error"]["code"] == "INVALID_STATE"


def test_three_power_cycles_trigger_pairing():
    instance, _headers = _instance()
    live = store.get(instance["id"])

    # 6 s off and 12 s on between cycles: 36 s total.
    assert service.note_power(live, "off", 100.0) is False
    assert service.note_power(live, "on", 106.0) is False
    assert service.note_power(live, "off", 118.0) is False
    assert service.note_power(live, "on", 124.0) is False
    assert service.note_power(live, "off", 136.0) is False
    assert service.note_power(live, "on", 142.0) is True


def test_fast_manual_cycles_count():
    instance, _headers = _instance()
    live = store.get(instance["id"])

    # 1 s per cycle: within the accepted minimum.
    assert service.note_power(live, "off", 100.0) is False
    assert service.note_power(live, "on", 101.0) is False
    assert service.note_power(live, "off", 102.0) is False
    assert service.note_power(live, "on", 103.0) is False
    assert service.note_power(live, "off", 104.0) is False
    assert service.note_power(live, "on", 105.0) is True


def test_cycles_spread_beyond_total_window_reset_count():
    instance, _headers = _instance()
    live = store.get(instance["id"])

    # Each off→on is valid, but the 3 cycles exceed 2 min.
    assert service.note_power(live, "off", 100.0) is False
    assert service.note_power(live, "on", 106.0) is False
    assert service.note_power(live, "off", 200.0) is False
    assert service.note_power(live, "on", 206.0) is False
    assert service.note_power(live, "off", 300.0) is False
    assert service.note_power(live, "on", 306.0) is False
    assert live.provisioning_cycles == []


def test_slow_or_fast_cycles_do_not_trigger_pairing():
    instance, _headers = _instance()
    live = store.get(instance["id"])

    # Too fast (<1 s): does not count.
    assert service.note_power(live, "off", 100.0) is False
    assert service.note_power(live, "on", 100.5) is False
    # Too slow (>10 s): resets the count.
    assert service.note_power(live, "off", 120.0) is False
    assert service.note_power(live, "on", 135.0) is False


def test_power_cycle_during_pairing_cancels_and_keeps_network():
    instance, _headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])

    started = client.post(f"{base}/start", headers=sim_headers)
    assert started.status_code == 200
    assert started.json()["state"] == "pairing"

    live = store.get(instance["id"])
    assert service.should_cancel_pairing(live, "off") is False
    assert service.should_cancel_pairing(live, "on") is False
    live.provisioning_last_off = 100.0
    assert service.should_cancel_pairing(live, "on") is True

    live.provisioning_ssid = "Casa"
    result = asyncio.run(service.cancel_pairing(live))
    assert result["state"] == "online"
    assert result["ssid"] == "Casa"


def test_start_pairing_is_sim_only():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"

    response = client.post(f"{base}/start", headers=headers)

    assert response.status_code == 403
    assert response.json()["error"]["code"] == "SIM_ONLY"


def test_connect_persists_encrypted_credentials():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])

    updated = client.put(
        f"{base}/policy",
        json={
            "networks": ["Casa-Demo"],
            "passwords": {"Casa-Demo": "super-secret"},
            "allow_any": False,
            "failure": "none",
        },
        headers=sim_headers,
    )
    assert updated.status_code == 200

    client.post(f"{base}/start", headers=sim_headers)
    response = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "super-secret"},
        headers=headers,
    )
    assert response.status_code == 200, response.text
    assert response.json()["state"] == "online"
    assert "super-secret" not in response.text

    live = store.get(instance["id"])
    assert live is not None
    assert all(
        "super-secret" not in json.dumps(event) for event in live.events
    )

    with sqlite3.connect(settings.PROVISIONING_DB) as connection:
        row = connection.execute(
            "SELECT ssid, password FROM credentials WHERE device_key = ?",
            (live.device_token,),
        ).fetchone()
    assert row is not None
    assert row[0] == "Casa-Demo"
    assert row[1] is not None
    assert b"super-secret" not in row[1]
    assert decrypt_password(row[1]) == "super-secret"

    # A new instance with the same device_token loads the saved network.
    second = client.post("/api/v1/instances", json={})
    assert second.status_code == 201
    data = second.json()
    info = client.get(
        f"/api/v1/instances/{data['id']}/provisioning",
        headers={"Authorization": f"Bearer {data['session_token']}"},
    )
    assert info.status_code == 200
    assert info.json()["state"] == "online"
    assert info.json()["ssid"] == "Casa-Demo"
    assert "super-secret" not in info.text


def test_forget_wifi_persists_tombstone():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])

    client.post(f"{base}/start", headers=sim_headers)
    connected = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "demo-password"},
        headers=headers,
    )
    assert connected.status_code == 200
    reset = client.post(f"{base}/reset", headers=sim_headers)
    assert reset.status_code == 200
    assert reset.json()["state"] == "pairing"
    assert reset.json()["ssid"] is None

    second = client.post("/api/v1/instances", json={})
    data = second.json()
    info = client.get(
        f"/api/v1/instances/{data['id']}/provisioning",
        headers={"Authorization": f"Bearer {data['session_token']}"},
    )
    assert info.json()["state"] == "provisioned"
    assert info.json()["ssid"] is None


def test_whitelist_blocks_unknown_network():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])

    client.post(f"{base}/start", headers=sim_headers)
    response = client.post(
        f"{base}/connect",
        json={"ssid": "Unknown", "password": "x"},
        headers=headers,
    )
    assert response.status_code == 400
    assert response.json()["error"]["code"] == "UNKNOWN_NETWORK"
    live = store.get(instance["id"])
    assert live.provisioning_state == "pairing"
    with sqlite3.connect(settings.PROVISIONING_DB) as connection:
        count = connection.execute(
            "SELECT COUNT(*) FROM credentials"
        ).fetchone()[0]
    assert count == 0


def test_allow_any_accepts_unknown_network():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])

    updated = client.put(
        f"{base}/policy",
        json={"networks": ["Casa-Demo"], "allow_any": True, "failure": "none"},
        headers=sim_headers,
    )
    assert updated.status_code == 200
    assert updated.json()["allow_any"] is True

    client.post(f"{base}/start", headers=sim_headers)
    response = client.post(
        f"{base}/connect",
        json={"ssid": "Otra", "password": "x"},
        headers=headers,
    )
    assert response.status_code == 200, response.text
    assert response.json()["ssid"] == "Otra"

    policy = client.get(f"{base}/policy", headers=headers)
    assert policy.status_code == 200
    assert policy.json()["allow_any"] is True


def test_simulated_failure_modes():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])
    live = store.get(instance["id"])

    def set_failure(mode: str) -> None:
        response = client.put(
            f"{base}/policy",
            json={
                "networks": ["Casa-Demo"],
                "allow_any": False,
                "failure": mode,
            },
            headers=sim_headers,
        )
        assert response.status_code == 200

    client.post(f"{base}/start", headers=sim_headers)

    set_failure("wrong_password")
    wrong = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "x"},
        headers=headers,
    )
    assert wrong.status_code == 403
    assert wrong.json()["error"]["code"] == "INVALID_PASSWORD"
    assert live.provisioning_state == "pairing"

    set_failure("timeout")
    timeout = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "x"},
        headers=headers,
    )
    assert timeout.status_code == 408
    assert timeout.json()["error"]["code"] == "WIFI_TIMEOUT"
    assert live.provisioning_state == "pairing"

    set_failure("unknown_network")
    unknown = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "x"},
        headers=headers,
    )
    assert unknown.status_code == 400
    assert unknown.json()["error"]["code"] == "UNKNOWN_NETWORK"
    assert live.provisioning_state == "pairing"


def test_policy_is_sim_only_and_validates():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])

    forbidden = client.put(
        f"{base}/policy",
        json={"networks": ["Casa-Demo"], "allow_any": False, "failure": "none"},
        headers=headers,
    )
    assert forbidden.status_code == 403
    assert forbidden.json()["error"]["code"] == "SIM_ONLY"

    invalid = client.put(
        f"{base}/policy",
        json={"networks": ["Casa-Demo"], "allow_any": False, "failure": "bogus"},
        headers=sim_headers,
    )
    assert invalid.status_code == 400
    assert invalid.json()["error"]["code"] == "INVALID_FAILURE"


def test_default_network_password_is_enforced_and_visible():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/provisioning"
    sim_headers = _sim_token(instance["id"])

    policy = client.get(f"{base}/policy", headers=headers)
    assert policy.status_code == 200
    assert policy.json()["passwords"]["Casa-Demo"] == "demo-password"
    assert policy.json()["passwords"]["Lab"] == "lab-password"

    client.post(f"{base}/start", headers=sim_headers)
    wrong = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "nope"},
        headers=headers,
    )
    assert wrong.status_code == 403
    assert wrong.json()["error"]["code"] == "INVALID_PASSWORD"
    live = store.get(instance["id"])
    assert live.provisioning_state == "pairing"

    ok = client.post(
        f"{base}/connect",
        json={"ssid": "Casa-Demo", "password": "demo-password"},
        headers=headers,
    )
    assert ok.status_code == 200, ok.text
    assert ok.json()["ssid"] == "Casa-Demo"


def test_seed_default_network(monkeypatch):
    monkeypatch.setattr(settings, "PROVISIONING_SEED_SSID", "Casa-Demo")
    monkeypatch.setattr(settings, "PROVISIONING_SEED_PASSWORD", "seeded-pass")
    instance, headers = _instance()

    response = client.get(
        f"/api/v1/instances/{instance['id']}/provisioning", headers=headers
    )
    body = response.json()
    assert body["state"] == "online"
    assert body["ssid"] == "Casa-Demo"
    assert "seeded-pass" not in response.text

    live = store.get(instance["id"])
    assert live is not None
    with sqlite3.connect(settings.PROVISIONING_DB) as connection:
        row = connection.execute(
            "SELECT ssid, password FROM credentials WHERE device_key = ?",
            (live.device_token,),
        ).fetchone()
    assert row is not None
    assert row[0] == "Casa-Demo"
    assert row[1] is not None
    assert b"seeded-pass" not in row[1]
    assert decrypt_password(row[1]) == "seeded-pass"
