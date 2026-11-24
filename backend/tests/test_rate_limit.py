"""Rate limiting (fixed window) and 429 endpoint tests."""

from __future__ import annotations

from fastapi.testclient import TestClient

from src import settings
from src.main import app
from src.security.rate_limit import RateLimiter, limiter

client = TestClient(app)


def test_limiter_counts_and_resets():
    bucket = RateLimiter()

    assert bucket.check("k", 2) is None
    assert bucket.check("k", 2) is None
    retry = bucket.check("k", 2)
    assert retry is not None
    assert retry > 0

    bucket.reset()
    assert bucket.check("k", 2) is None


def test_device_rate_limit_returns_429(monkeypatch):
    created = client.post("/api/v1/instances", json={})
    assert created.status_code == 201
    token = settings.DEVICE_TOKEN

    monkeypatch.setattr(settings, "DEVICE_RATE_LIMIT", 1)
    limiter.reset()

    url = f"/api/v1/telemetry/health?token={token}"
    first = client.post(url, json={"uptime_s": 1})
    second = client.post(url, json={"uptime_s": 2})

    assert first.status_code == 204
    assert second.status_code == 429
    assert second.json()["error"]["code"] == "RATE_LIMITED"
    assert int(second.headers["Retry-After"]) >= 1


def test_instance_creation_is_rate_limited(monkeypatch):
    monkeypatch.setattr(settings, "INSTANCE_RATE_LIMIT", 1)
    limiter.reset()

    first = client.post("/api/v1/instances", json={})
    second = client.post("/api/v1/instances", json={})

    assert first.status_code == 201
    assert second.status_code == 429
    assert second.json()["error"]["code"] == "RATE_LIMITED"


def test_pin_attempts_are_rate_limited(monkeypatch):
    instance = client.post("/api/v1/instances", json={}).json()
    monkeypatch.setattr(settings, "SIM_PIN_RATE_LIMIT", 1)
    limiter.reset()

    url = f"/api/v1/instances/{instance['id']}/sim/session"
    first = client.post(url, json={"pin": "0000"})
    second = client.post(url, json={"pin": "0000"})

    assert first.status_code == 403
    assert second.status_code == 429
    assert second.json()["error"]["code"] == "RATE_LIMITED"
