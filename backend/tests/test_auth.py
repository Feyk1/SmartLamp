"""Session JWT and per-instance authorization tests."""

from __future__ import annotations

import time

from fastapi.testclient import TestClient

from src.auth.tokens import TokenError, issue, verify
from src.main import app

client = TestClient(app)


def _create_instance() -> dict:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    return response.json()


def _auth(instance: dict) -> dict:
    return {"Authorization": f"Bearer {instance['session_token']}"}


def test_issue_and_verify():
    token = issue("inst-test", "user")
    claims = verify(token)
    assert claims["instance_id"] == "inst-test"
    assert claims["role"] == "user"


def test_tampered_token_is_rejected():
    token = issue("inst-test", "user")
    head, body, signature = token.split(".")
    tampered = f"{head}.{body}.{signature[:-2]}AA"

    try:
        verify(tampered)
    except TokenError as exc:
        assert "signature" in str(exc)
    else:
        raise AssertionError("tampered token accepted")


def test_expired_token_is_rejected():
    token = issue("inst-test", "user", ttl_s=-1)

    try:
        verify(token)
    except TokenError as exc:
        assert "expired" in str(exc)
    else:
        raise AssertionError("expired token accepted")


def test_session_is_required():
    instance = _create_instance()

    response = client.get(f"/api/v1/instances/{instance['id']}")

    assert response.status_code == 401
    assert response.json()["error"]["code"] == "UNAUTHORIZED"


def test_token_for_another_instance_is_forbidden():
    first = _create_instance()
    second = _create_instance()

    response = client.get(
        f"/api/v1/instances/{second['id']}", headers=_auth(first)
    )

    assert response.status_code == 403
    assert response.json()["error"]["code"] == "FORBIDDEN"


def test_bearer_and_header_are_equivalent():
    instance = _create_instance()
    path = f"/api/v1/instances/{instance['id']}"

    bearer = client.get(path, headers=_auth(instance))
    header = client.get(
        path, headers={"X-Session-Token": instance["session_token"]}
    )

    assert bearer.status_code == 200
    assert header.status_code == 200


def test_sim_requires_sim_role():
    instance = _create_instance()

    response = client.get(
        f"/api/v1/instances/{instance['id']}/sim/registers",
        headers=_auth(instance),
    )

    assert response.status_code == 403
    assert response.json()["error"]["code"] == "SIM_ONLY"


def test_token_ttl_is_bounded():
    token = issue("inst-test", "sim", ttl_s=2)
    claims = verify(token)
    assert claims["exp"] - claims["iat"] == 2
    assert claims["role"] == "sim"
    assert int(time.time()) <= claims["exp"]
