"""User sequence tests (upload + start)."""

from __future__ import annotations

from fastapi.testclient import TestClient

from src.main import app
from src.sequences.crc import crc16_ccitt, sequence_crc
from src.sim.modbus import ModbusError

client = TestClient(app)


class FakeModbus:
    def __init__(self) -> None:
        self.registers: dict[int, int] = {}
        self.blocks: dict[int, list[int]] = {}
        self.calls: list[tuple] = []
        self.host = ""
        self.fail = False

    def write_register(self, addr: int, value: int) -> None:
        if self.fail:
            raise ModbusError("driver unreachable")
        self.registers[addr] = value
        self.calls.append(("register", addr, value))

    def write_registers(self, addr: int, values: list[int]) -> None:
        if self.fail:
            raise ModbusError("driver unreachable")
        self.blocks[addr] = list(values)
        self.calls.append(("registers", addr, list(values)))


def _instance() -> tuple[dict, dict]:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    data = response.json()
    return data, {"Authorization": f"Bearer {data['session_token']}"}


def _steps() -> list[dict]:
    return [
        {
            "h": 0,
            "s": 1000,
            "v": 1000,
            "brightness": 800,
            "cct": 2700,
            "duration_ms": 1000,
        },
        {
            "h": 200,
            "s": 900,
            "v": 1000,
            "brightness": 600,
            "cct": 4000,
            "duration_ms": 2000,
            "transition_ms": 500,
        },
    ]


def test_crc_known_vector():
    assert crc16_ccitt(b"123456789") == 0x29B1
    assert sequence_crc([0, 1000, 1000, 0, 800, 100, 0, 2700]) == 0xD035


def test_upload_writes_registers_and_starts(monkeypatch):
    instance, headers = _instance()
    fake = FakeModbus()
    captured: dict = {}

    def fake_modbus(_instance):
        return fake

    async def fake_dispatch(_instance, message):
        captured["message"] = message
        return {"type": "ack", "id": message["id"], "op": message["op"]}

    monkeypatch.setattr("src.sequences.router.modbus_for", fake_modbus)
    monkeypatch.setattr("src.sequences.router.dispatch_command", fake_dispatch)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sequences",
        json={"steps": _steps()},
        headers=headers,
    )

    assert response.status_code == 202, response.text
    body = response.json()
    assert body["ok"] is True
    assert body["steps"] == 2
    assert body["sequence_id"] == 100
    assert body["ack"]["type"] == "ack"

    assert fake.registers[15] == 2  # SEQ_TOTAL_STEPS
    blocks = [call[2] for call in fake.calls if call[0] == "registers"]
    assert blocks[0] == [0, 1000, 1000, 0, 800, 100, 0, 2700]
    assert blocks[1] == [200, 900, 1000, 0, 600, 200, 50, 4000]
    assert ("register", 14, 0) in fake.calls
    assert ("register", 14, 1) in fake.calls

    registers = []
    for step in _steps():
        registers += [
            step["h"],
            step["s"],
            step["v"],
            0,
            step["brightness"],
            step["duration_ms"] // 10,
            step.get("transition_ms", 0) // 10,
            step["cct"],
        ]
    assert fake.registers[24] == sequence_crc(registers)

    message = captured["message"]
    assert message["op"] == "start_sequence"
    assert message["payload"] == {
        "id": 100,
        "speed": 1000,
        "direction": 0,
        "loop": 0,
    }


def test_upload_requires_session():
    instance, _headers = _instance()

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sequences",
        json={"steps": _steps()},
    )

    assert response.status_code == 401
    assert response.json()["error"]["code"] == "UNAUTHORIZED"


def test_upload_validates_step_count_and_ranges():
    instance, headers = _instance()

    empty = client.post(
        f"/api/v1/instances/{instance['id']}/sequences",
        json={"steps": []},
        headers=headers,
    )
    too_many = client.post(
        f"/api/v1/instances/{instance['id']}/sequences",
        json={"steps": _steps() * 5},
        headers=headers,
    )
    bad_hue = client.post(
        f"/api/v1/instances/{instance['id']}/sequences",
        json={"steps": [{"h": 400}]},
        headers=headers,
    )

    assert empty.status_code == 422
    assert too_many.status_code == 422
    assert bad_hue.status_code == 422


def test_modbus_failure_returns_502(monkeypatch):
    instance, headers = _instance()
    fake = FakeModbus()
    fake.fail = True

    monkeypatch.setattr(
        "src.sequences.router.modbus_for", lambda _instance: fake
    )

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sequences",
        json={"steps": _steps()},
        headers=headers,
    )

    assert response.status_code == 502
    assert response.json()["error"]["code"] == "MODBUS_ERROR"


def test_duration_is_sent_in_10ms_units(monkeypatch):
    instance, headers = _instance()
    fake = FakeModbus()

    monkeypatch.setattr(
        "src.sequences.router.modbus_for", lambda _instance: fake
    )

    async def fake_dispatch(_instance, _message):
        return {"type": "ack"}

    monkeypatch.setattr("src.sequences.router.dispatch_command", fake_dispatch)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sequences",
        json={
            "steps": [
                {"h": 120, "duration_ms": 1500, "transition_ms": 250}
            ]
        },
        headers=headers,
    )

    assert response.status_code == 202
    assert fake.blocks[16] == [120, 1000, 1000, 0, 1000, 150, 25, 2700]
