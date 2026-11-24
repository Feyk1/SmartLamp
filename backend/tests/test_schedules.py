"""Schedule tests (recurrence, actions, editing, and the runner)."""

from __future__ import annotations

import asyncio
from datetime import datetime, timedelta, timezone

from fastapi.testclient import TestClient

from src.main import app
from src.schedules import runner
from src.schedules.runner import next_occurrence
from src.schedules.storage import schedule_store
from src.store.memory import Schedule, store

client = TestClient(app)


def _instance() -> tuple[dict, dict]:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    data = response.json()
    return data, {"Authorization": f"Bearer {data['session_token']}"}


def _payload(
    time: str = "07:30",
    repeat: str = "daily",
    action: dict | None = None,
) -> dict:
    return {
        "time": time,
        "timezone": "America/Mexico_City",
        "repeat": repeat,
        "days": [],
        "action": action or {"type": "scene", "scene": "reading"},
        "enabled": True,
    }


def test_create_edit_toggle_and_delete_schedule():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/schedules"

    created = client.post(base, json=_payload(), headers=headers)

    assert created.status_code == 201, created.text
    body = created.json()
    assert body["id"].startswith("sch-")
    assert body["repeat"] == "daily"
    assert body["action"] == {"type": "scene", "scene": "reading"}
    assert body["next_run"] is not None
    assert body["enabled"] is True

    updated = client.put(
        f"{base}/{body['id']}",
        json=_payload(
            time="21:00",
            action={"type": "cct", "kelvin": 2200, "brightness": 150},
        ),
        headers=headers,
    )
    assert updated.status_code == 200, updated.text
    assert updated.json()["time"] == "21:00"
    assert updated.json()["action"]["type"] == "cct"

    disabled = client.patch(
        f"{base}/{body['id']}", json={"enabled": False}, headers=headers
    )
    assert disabled.status_code == 200
    assert disabled.json()["enabled"] is False
    assert disabled.json()["next_run"] is None

    enabled = client.patch(
        f"{base}/{body['id']}", json={"enabled": True}, headers=headers
    )
    assert enabled.status_code == 200
    assert enabled.json()["next_run"] is not None

    listed = client.get(base, headers=headers)
    assert len(listed.json()["schedules"]) == 1

    deleted = client.delete(f"{base}/{body['id']}", headers=headers)
    assert deleted.status_code == 204
    assert store.get(instance["id"]).schedules == []


def test_schedule_validation_and_auth():
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/schedules"

    anonymous = client.post(base, json=_payload())
    bad_zone = client.post(
        base, json={**_payload(), "timezone": "Mars/Olympus"}, headers=headers
    )
    bad_action = client.post(
        base,
        json=_payload(action={"type": "effect", "effect": "disco"}),
        headers=headers,
    )
    bad_custom = client.post(
        base, json=_payload(repeat="custom"), headers=headers
    )
    bad_time = client.post(base, json=_payload(time="25:99"), headers=headers)

    assert anonymous.status_code == 401
    for response in (bad_zone, bad_action, bad_custom, bad_time):
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "INVALID_SCHEDULE"


def test_duplicate_and_limit(monkeypatch):
    instance, headers = _instance()
    base = f"/api/v1/instances/{instance['id']}/schedules"

    first = client.post(base, json=_payload(), headers=headers)
    second = client.post(base, json=_payload(), headers=headers)

    assert first.status_code == 201
    assert second.status_code == 409
    assert second.json()["error"]["code"] == "DUPLICATE_SCHEDULE"


def test_next_occurrence_respects_days_and_timezone():
    after = datetime(2026, 1, 15, 10, 0, tzinfo=timezone.utc)

    daily = next_occurrence(
        "07:30", "America/Mexico_City", "daily", [], after
    )
    assert daily == datetime(2026, 1, 15, 13, 30, tzinfo=timezone.utc)

    after_friday = datetime(2026, 1, 16, 23, 0, tzinfo=timezone.utc)
    monday = next_occurrence(
        "07:30", "America/Mexico_City", "weekdays", [], after_friday
    )
    assert monday == datetime(2026, 1, 19, 13, 30, tzinfo=timezone.utc)

    custom = next_occurrence(
        "07:30", "America/Mexico_City", "custom", [2], after
    )
    assert custom == datetime(2026, 1, 21, 13, 30, tzinfo=timezone.utc)


def test_next_occurrence_handles_dst():
    winter = next_occurrence(
        "08:00",
        "Europe/Madrid",
        "daily",
        [],
        datetime(2026, 1, 15, 0, 0, tzinfo=timezone.utc),
    )
    summer = next_occurrence(
        "08:00",
        "Europe/Madrid",
        "daily",
        [],
        datetime(2026, 7, 15, 0, 0, tzinfo=timezone.utc),
    )

    assert winter is not None and summer is not None
    assert winter.hour == 7  # UTC+1
    assert summer.hour == 6  # UTC+2


def test_due_schedule_applies_action_and_reschedules(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    now = datetime.now(timezone.utc)
    live.schedules.append(
        Schedule(
            id="sch-test",
            time="07:30",
            timezone="UTC",
            repeat="daily",
            days=[],
            action={"type": "cct", "kelvin": 2200, "brightness": 120},
            enabled=True,
            next_run=now - timedelta(seconds=1),
            created_at=now,
        )
    )
    calls: list[tuple[str, dict]] = []

    async def fake_dispatch(_instance, message):
        calls.append((message["op"], message["payload"]))
        return {"type": "ack"}

    monkeypatch.setattr("src.schedules.runner.dispatch_command", fake_dispatch)

    executed = asyncio.run(runner.run_due(now))

    schedule = live.schedules[0]
    assert executed == 1
    assert schedule.last_status == "ok"
    assert schedule.next_run is not None and schedule.next_run > now
    assert [op for op, _ in calls] == [
        "set_power",
        "set_mode",
        "set_color_temperature",
        "set_brightness",
    ]
    assert calls[2][1] == {"kelvin": 2200}
    assert calls[3][1] == {"value": 120}


def test_once_schedule_retries_then_disables(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    now = datetime.now(timezone.utc)
    live.schedules.append(
        Schedule(
            id="sch-once",
            time="07:30",
            timezone="UTC",
            repeat="once",
            days=[],
            action={"type": "off"},
            enabled=True,
            next_run=now - timedelta(seconds=1),
            created_at=now,
        )
    )

    async def nack_dispatch(_instance, _message):
        return {"type": "nack", "payload": {"code": "TIMEOUT"}}

    monkeypatch.setattr(
        "src.schedules.runner.dispatch_command", nack_dispatch
    )

    schedule = live.schedules[0]
    for attempt in range(3):
        schedule.next_run = now
        assert asyncio.run(runner.run_due(now)) == 1
        if attempt < 2:
            assert schedule.last_status == "retry"
            assert schedule.enabled is True
        else:
            assert schedule.enabled is False
            assert schedule.next_run is None
            assert schedule.last_status == "skipped"
            assert schedule.last_error == "TIMEOUT"


def test_future_schedule_is_not_executed(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    now = datetime.now(timezone.utc)
    live.schedules.append(
        Schedule(
            id="sch-future",
            time="07:30",
            timezone="UTC",
            repeat="daily",
            days=[],
            action={"type": "scene", "scene": "focus"},
            enabled=True,
            next_run=now + timedelta(hours=1),
            created_at=now,
        )
    )

    async def fail_dispatch(_instance, _message):
        raise AssertionError("must not run")

    monkeypatch.setattr("src.schedules.runner.dispatch_command", fail_dispatch)

    assert asyncio.run(runner.run_due(now)) == 0


def test_sunrise_ramps_and_finishes(monkeypatch):
    instance, _headers = _instance()
    live = store.get(instance["id"])
    now = datetime.now(timezone.utc)
    runner.active_sunrises.clear()
    live.schedules.append(
        Schedule(
            id="sch-sunrise",
            time="07:00",
            timezone="UTC",
            repeat="once",
            days=[],
            action={
                "type": "sunrise",
                "kelvin": 4000,
                "brightness": 800,
                "duration_min": 1,
            },
            enabled=True,
            next_run=now - timedelta(seconds=1),
            created_at=now,
        )
    )
    calls: list[tuple[str, dict]] = []

    async def fake_dispatch(_instance, message):
        calls.append((message["op"], message["payload"]))
        return {"type": "ack"}

    monkeypatch.setattr("src.schedules.runner.dispatch_command", fake_dispatch)

    assert asyncio.run(runner.run_due(now)) == 1
    schedule = live.schedules[0]
    assert schedule.last_status == "running"
    assert schedule.enabled is False
    assert calls[2] == ("set_color_temperature", {"kelvin": 2200})
    assert calls[3] == ("set_brightness", {"value": 20})
    assert "sch-sunrise" in list(runner.active_sunrises.values())[0].schedule_id

    asyncio.run(runner.tick_sunrises(now + timedelta(seconds=30)))
    middle = calls[-2:]
    assert middle[0][0] == "set_color_temperature"
    assert 2200 < middle[0][1]["kelvin"] < 4000
    assert 20 < middle[1][1]["value"] < 800

    asyncio.run(runner.tick_sunrises(now + timedelta(seconds=60)))
    assert runner.active_sunrises == {}
    assert live.schedules[0].last_status == "ok"


def test_schedule_persists_by_device_token():
    instance, headers = _instance()
    created = client.post(
        f"/api/v1/instances/{instance['id']}/schedules",
        json=_payload(),
        headers=headers,
    )
    assert created.status_code == 201
    live = store.get(instance["id"])
    token = live.device_token

    store.reset()
    restored = store.create(ttl_s=3600, device_token=token)
    schedule_store.attach(restored)

    assert len(restored.schedules) == 1
    assert restored.schedules[0].time == "07:30"
