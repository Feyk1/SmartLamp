"""REST tests for instances."""

from fastapi.testclient import TestClient

from src.main import app

client = TestClient(app)


def _auth(data: dict) -> dict:
    return {"Authorization": f"Bearer {data['session_token']}"}


def test_create_and_get_instance():
    response = client.post(
        "/api/v1/instances", json={"profile": "lamp-v1", "ttl_s": 3600}
    )
    assert response.status_code == 201
    data = response.json()
    assert data["id"].startswith("inst-")
    assert data["session_token"]
    assert "/ws/dashboard?instance=" in data["ws_dashboard_url"]
    assert "device_token" not in data
    assert data["firmware_versions"]

    instance_id = data["id"]
    got = client.get(f"/api/v1/instances/{instance_id}", headers=_auth(data))
    assert got.status_code == 200
    assert got.json()["id"] == instance_id


def test_missing_session_returns_401():
    response = client.get("/api/v1/instances/inst-nope")
    assert response.status_code == 401
    assert response.json()["error"]["code"] == "UNAUTHORIZED"


def test_unknown_instance_returns_error_envelope():
    created = client.post("/api/v1/instances", json={})
    data = created.json()
    client.delete(f"/api/v1/instances/{data['id']}", headers=_auth(data))

    response = client.get(
        f"/api/v1/instances/{data['id']}", headers=_auth(data)
    )
    assert response.status_code == 404
    body = response.json()
    assert body["error"]["code"] == "INSTANCE_NOT_FOUND"
    assert "request_id" in body["error"]


def test_delete_instance():
    created = client.post("/api/v1/instances", json={})
    assert created.status_code == 201
    instance_id = created.json()["id"]

    deleted = client.delete(
        f"/api/v1/instances/{instance_id}", headers=_auth(created.json())
    )
    assert deleted.status_code == 204

    missing = client.get(
        f"/api/v1/instances/{instance_id}", headers=_auth(created.json())
    )
    assert missing.status_code == 404


def test_heartbeat_and_state():
    created = client.post("/api/v1/instances", json={})
    instance_id = created.json()["id"]

    headers = _auth(created.json())
    heartbeat = client.post(
        f"/api/v1/instances/{instance_id}/heartbeat", headers=headers
    )
    assert heartbeat.status_code == 200
    assert heartbeat.json()["ok"] is True

    state = client.get(f"/api/v1/instances/{instance_id}/state", headers=headers)
    assert state.status_code == 200
    assert state.json()["state"] is None

    info = client.get(
        f"/api/v1/instances/{instance_id}", headers=headers
    ).json()
    assert info["device_online"] is False
