"""Tests for the backend OTA endpoints (docs/02 §4.2)."""

from __future__ import annotations

import struct

from fastapi.testclient import TestClient

from src import settings
from src.main import app
from src.store.memory import store

client = TestClient(app)
TOKEN = settings.DEVICE_TOKEN
MAGIC = b"LAMPOTA1"
HEADER_FMT = "<8sH6sII32s12s32s28s"


def _ota_bytes(version: tuple[int, int, int] = (1, 1, 0)) -> bytes:
    payload = b"x" * 64
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        1,
        struct.pack("<HHH", *version),
        1,
        len(payload),
        b"\0" * 32,
        b"\0" * 12,
        b"\0" * 32,
        b"\0" * 28,
    )
    return header + payload + b"\0" * 16 + b"\0" * 64


def _create_instance() -> dict:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    return response.json()


def _auth(instance: dict) -> dict:
    return {"Authorization": f"Bearer {instance['session_token']}"}


def _prepare_ota_dir(tmp_path, monkeypatch) -> None:
    (tmp_path / "1.1.0.ota").write_bytes(_ota_bytes())
    monkeypatch.setattr(settings, "OTA_DIR", str(tmp_path))


def test_check_reports_available(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    _create_instance()

    response = client.get(f"/api/v1/ota/check?current=1.0.3&token={TOKEN}")

    assert response.status_code == 200
    body = response.json()
    assert body["available"] is True
    assert body["version"] == "1.1.0"
    assert body["url"].endswith("/api/v1/ota/1.1.0/download")


def test_check_up_to_date(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    _create_instance()

    response = client.get(f"/api/v1/ota/check?current=1.1.0&token={TOKEN}")

    assert response.status_code == 200
    assert response.json()["available"] is False


def test_check_invalid_version(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    _create_instance()

    response = client.get(f"/api/v1/ota/check?current=nope&token={TOKEN}")

    assert response.status_code == 400
    assert response.json()["error"]["code"] == "INVALID_VERSION"


def test_download_returns_artifact(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    _create_instance()

    response = client.get(f"/api/v1/ota/1.1.0/download?token={TOKEN}")

    assert response.status_code == 200
    assert response.content == _ota_bytes()


def test_download_unknown_version(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    _create_instance()

    response = client.get(f"/api/v1/ota/9.9.9/download?token={TOKEN}")

    assert response.status_code == 404
    assert response.json()["error"]["code"] == "OTA_NOT_FOUND"


def test_check_requires_device_token(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    _create_instance()

    response = client.get("/api/v1/ota/check?current=1.0.3")

    assert response.status_code == 401


def test_trigger_ota_dispatches_command(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    instance = _create_instance()
    captured: dict = {}

    async def fake_dispatch(inst, message):
        captured["instance"] = inst.id
        captured["message"] = message
        return {"id": message["id"], "type": "ack", "op": message["op"]}

    monkeypatch.setattr("src.api.instances.dispatch_command", fake_dispatch)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={"version": "1.1.0"},
        headers=_auth(instance),
    )

    assert response.status_code == 202
    assert response.json()["type"] == "ack"
    assert captured["instance"] == instance["id"]
    assert captured["message"]["op"] == "ota_update"
    assert captured["message"]["payload"] == {
        "version": "1.1.0",
        "mode": "legit",
    }


def test_trigger_ota_uses_latest(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    instance = _create_instance()
    captured: dict = {}

    async def fake_dispatch(inst, message):
        captured["message"] = message
        return {"id": message["id"], "type": "ack", "op": message["op"]}

    monkeypatch.setattr("src.api.instances.dispatch_command", fake_dispatch)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={},
        headers=_auth(instance),
    )

    assert response.status_code == 202
    assert captured["message"]["payload"] == {
        "version": "1.1.0",
        "mode": "legit",
    }


def _set_fw_version(instance_id: str, version: str) -> None:
    store.get(instance_id).last_health = {"fw_version": version}


def test_legit_ota_rejects_current_or_older(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    instance = _create_instance()
    _set_fw_version(instance["id"], "1.1.0")

    response = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={"version": "1.1.0", "mode": "legit"},
        headers=_auth(instance),
    )

    assert response.status_code == 409
    assert response.json()["error"]["code"] == "OTA_NOT_NEWER"


def test_downgrade_rejects_current_or_newer(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    instance = _create_instance()
    _set_fw_version(instance["id"], "1.1.0")

    response = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={"version": "1.1.0", "mode": "downgrade"},
        headers=_auth(instance),
    )

    assert response.status_code == 409
    assert response.json()["error"]["code"] == "OTA_NOT_OLDER"


def test_downgrade_of_older_artifact_is_dispatched(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    instance = _create_instance()
    _set_fw_version(instance["id"], "1.3.0")
    captured: dict = {}

    async def fake_dispatch(inst, message):
        captured["message"] = message
        return {"id": message["id"], "type": "ack", "op": message["op"]}

    monkeypatch.setattr("src.api.instances.dispatch_command", fake_dispatch)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={"version": "1.1.0", "mode": "downgrade"},
        headers=_auth(instance),
    )

    assert response.status_code == 202
    assert captured["message"]["payload"] == {
        "version": "1.1.0",
        "mode": "downgrade",
    }


def test_versions_lists_artifacts(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)

    response = client.get("/api/v1/ota/versions")

    assert response.status_code == 200
    assert response.json()["versions"][0]["version"] == "1.1.0"


def test_corrupt_mode_alters_artifact(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    instance = _create_instance()

    async def fake_dispatch(inst, message):
        return {"id": message["id"], "type": "ack", "op": message["op"]}

    monkeypatch.setattr("src.api.instances.dispatch_command", fake_dispatch)
    trigger = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={"version": "1.1.0", "mode": "corrupt"},
        headers=_auth(instance),
    )
    assert trigger.status_code == 202

    response = client.get(f"/api/v1/ota/1.1.0/download?token={TOKEN}")

    assert response.status_code == 200
    original = _ota_bytes()
    assert len(response.content) == len(original)
    assert response.content[200] != original[200]


def test_invalid_ota_mode(tmp_path, monkeypatch):
    _prepare_ota_dir(tmp_path, monkeypatch)
    instance = _create_instance()

    response = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={"version": "1.1.0", "mode": "explode"},
        headers=_auth(instance),
    )

    assert response.status_code == 400
    assert response.json()["error"]["code"] == "INVALID_MODE"


def test_trigger_ota_without_artifacts(tmp_path, monkeypatch):
    monkeypatch.setattr(settings, "OTA_DIR", str(tmp_path / "empty"))
    instance = _create_instance()

    response = client.post(
        f"/api/v1/instances/{instance['id']}/ota",
        json={},
        headers=_auth(instance),
    )

    assert response.status_code == 409
    assert response.json()["error"]["code"] == "OTA_UNAVAILABLE"
