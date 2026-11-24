"""Common fixtures for the backend tests."""

import pytest

from src import settings
from src.auth import ws_tickets
from src.security.rate_limit import limiter
from src.store.memory import store


@pytest.fixture(autouse=True)
def _reset_state(monkeypatch, tmp_path):
    # In tests, each POST /instances must create a new instance and the
    # rate limits must not interfere between tests.
    monkeypatch.setattr(settings, "REUSE_DEVICE_INSTANCE", False)
    monkeypatch.setattr(settings, "SCHEDULES_DB", str(tmp_path / "schedules.db"))
    monkeypatch.setattr(
        settings, "PROVISIONING_DB", str(tmp_path / "provisioning.db")
    )
    monkeypatch.setattr(settings, "PROVISIONING_SEED_SSID", "")
    monkeypatch.setattr(
        settings, "PROVISIONING_NETWORKS", ["Casa-Demo", "Lab", "Guest"]
    )
    monkeypatch.setattr(settings, "PROVISIONING_ALLOW_ANY", False)
    monkeypatch.setattr(settings, "DEVICE_RATE_LIMIT", 1000)
    monkeypatch.setattr(settings, "INSTANCE_RATE_LIMIT", 1000)
    limiter.reset()
    ws_tickets.reset()
    store.reset()
    yield
    store.reset()
    limiter.reset()
    ws_tickets.reset()
