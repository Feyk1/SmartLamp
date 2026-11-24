"""Simulation Panel tests (auth, Modbus, faults, logs, reboot)."""

from __future__ import annotations

from fastapi.testclient import TestClient

from src import settings
from src.main import app
from src.store.memory import store

client = TestClient(app)
DEVICE_TOKEN = settings.DEVICE_TOKEN


class FakeModbus:
    def __init__(self) -> None:
        self.registers = [0] * 64
        self.coils = [0] * 16
        self.calls: list[tuple] = []

    def read_holding(self, addr, count):
        self.calls.append(("read_holding", addr, count))
        return self.registers[addr : addr + count]

    def read_input(self, addr, count):
        self.calls.append(("read_input", addr, count))
        return [100 + addr + i for i in range(count)]

    def read_coils(self, addr, count):
        self.calls.append(("read_coils", addr, count))
        return self.coils[addr : addr + count]

    def read_discrete(self, addr, count):
        self.calls.append(("read_discrete", addr, count))
        return [1] * count

    def write_register(self, addr, value):
        self.calls.append(("write_register", addr, value))
        self.registers[addr] = value

    def write_registers(self, addr, values):
        self.calls.append(("write_registers", addr, list(values)))
        for offset, value in enumerate(values):
            self.registers[addr + offset] = value

    def write_coil(self, addr, value):
        self.calls.append(("write_coil", addr, value))
        self.coils[addr] = 1 if value else 0


def _create_instance() -> dict:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    return response.json()


def _sim_token(instance_id: str) -> str:
    response = client.post(
        f"/api/v1/instances/{instance_id}/sim/session",
        json={"pin": settings.SIM_PIN},
    )
    assert response.status_code == 200
    return response.json()["sim_token"]


def _headers(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


def test_pin_is_required(monkeypatch):
    instance = _create_instance()

    bad = client.post(
        f"/api/v1/instances/{instance['id']}/sim/session",
        json={"pin": "0000"},
    )
    assert bad.status_code == 403
    assert bad.json()["error"]["code"] == "INVALID_PIN"

    good = client.post(
        f"/api/v1/instances/{instance['id']}/sim/session",
        json={"pin": settings.SIM_PIN},
    )
    assert good.status_code == 200
    assert good.json()["sim_token"]


def test_sim_endpoints_require_session():
    instance = _create_instance()

    response = client.get(
        f"/api/v1/instances/{instance['id']}/sim/registers"
    )

    assert response.status_code == 401
    assert response.json()["error"]["code"] == "UNAUTHORIZED"


def test_sim_rejects_user_role():
    instance = _create_instance()
    headers = {"Authorization": f"Bearer {instance['session_token']}"}

    response = client.get(
        f"/api/v1/instances/{instance['id']}/sim/registers", headers=headers
    )

    assert response.status_code == 403
    assert response.json()["error"]["code"] == "SIM_ONLY"


def test_registers_and_reading(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    fake = FakeModbus()
    monkeypatch.setattr("src.sim.router.modbus", lambda *_args: fake)

    registers = client.get(
        f"/api/v1/instances/{instance['id']}/sim/registers",
        headers=_headers(token),
    )
    assert registers.status_code == 200
    assert registers.json()["registers"]["holding"]

    read = client.get(
        f"/api/v1/instances/{instance['id']}/sim/modbus/read",
        params={"kind": "holding", "addr": 2, "count": 2},
        headers=_headers(token),
    )
    assert read.status_code == 200
    assert read.json()["values"] == [0, 0]
    assert ("read_holding", 2, 2) in fake.calls


def test_write_applies_target(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    fake = FakeModbus()
    monkeypatch.setattr("src.sim.router.modbus", lambda *_args: fake)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/modbus/write",
        json={"kind": "holding", "addr": 2, "value": 120},
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert fake.registers[2] == 120
    assert ("write_coil", 0, 1) in fake.calls


def test_write_without_apply(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    fake = FakeModbus()
    monkeypatch.setattr("src.sim.router.modbus", lambda *_args: fake)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/modbus/write",
        json={"kind": "holding", "addr": 2, "value": 30, "apply": False},
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert ("write_coil", 0, 1) not in fake.calls


def test_fault_injection_writes_sim_registers(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    fake = FakeModbus()
    monkeypatch.setattr("src.sim.router.modbus", lambda *_args: fake)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/fault",
        json={"type": "led_overtemp", "duration_s": 60},
        headers=_headers(token),
    )

    assert response.status_code == 200
    body = response.json()
    assert body["code"] == 2
    assert ("write_registers", 45, [2, 60, 0x5A5A]) in fake.calls

    logs = client.get(
        f"/api/v1/instances/{instance['id']}/sim/logs",
        headers=_headers(token),
    )
    assert logs.status_code == 200
    assert any(item["op"] == "sim_fault" for item in logs.json()["logs"])


def test_unknown_fault_rejected(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    monkeypatch.setattr("src.sim.router.modbus", lambda: FakeModbus())

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/fault",
        json={"type": "explode"},
        headers=_headers(token),
    )

    assert response.status_code == 400
    assert response.json()["error"]["code"] == "INVALID_FAULT"


def test_clear_faults(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    fake = FakeModbus()
    monkeypatch.setattr("src.sim.router.modbus", lambda *_args: fake)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/faults/clear",
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert ("write_registers", 45, [0, 0, 0]) in fake.calls
    assert ("write_coil", 8, 1) in fake.calls


def test_user_can_clear_faults(monkeypatch):
    instance = _create_instance()
    fake = FakeModbus()
    monkeypatch.setattr("src.sim.router.modbus", lambda *_args: fake)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/faults/clear",
        headers=_headers(instance["session_token"]),
    )

    assert response.status_code == 200
    assert ("write_registers", 45, [0, 0, 0]) in fake.calls
    assert ("write_coil", 8, 1) in fake.calls


def test_clear_fault_history_keeps_device_log():
    instance = _create_instance()
    instance_obj = store.get(instance["id"])
    instance_obj.events.extend(
        [
            {"type": "event", "op": "fault", "payload": {"code": 2}},
            {"type": "event", "op": "sim_fault", "payload": {"type": "overcurrent"}},
            {"type": "event", "op": "device_online", "payload": {}},
        ]
    )

    response = client.delete(
        f"/api/v1/instances/{instance['id']}/faults/history",
        headers=_headers(instance["session_token"]),
    )

    assert response.status_code == 200
    assert response.json()["removed"] == 2
    ops = [event["op"] for event in instance_obj.events]
    assert "device_online" in ops
    assert "fault" not in ops
    assert "sim_fault" not in ops
    assert "fault_history_cleared" in ops


def test_fault_catalog_describes_effects(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    monkeypatch.setattr("src.sim.router.modbus", lambda *_args: FakeModbus())

    response = client.get(
        f"/api/v1/instances/{instance['id']}/sim/registers",
        headers=_headers(token),
    )

    assert response.status_code == 200
    faults = response.json()["faults"]
    assert faults["led_overtemp"]["effect"] == "trip"
    assert faults["led_overtemp_warn"]["effect"] == "derate"
    assert faults["config_corrupt"]["effect"] == "safe_mode"
    assert faults["config_corrupt"]["severity"] == "critical"
    assert faults["ws_drop"]["effect"] == "network"


def test_ws_drop_without_device(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/fault",
        json={"type": "ws_drop"},
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert response.json()["code"] is None


def test_reboot_dispatches_command(monkeypatch):
    instance = _create_instance()
    token = _sim_token(instance["id"])
    captured: dict = {}

    async def fake_dispatch(inst, message):
        captured["message"] = message
        return {"id": message["id"], "type": "ack", "op": message["op"]}

    monkeypatch.setattr("src.sim.router.dispatch_command", fake_dispatch)

    response = client.post(
        f"/api/v1/instances/{instance['id']}/sim/reboot",
        headers=_headers(token),
    )

    assert response.status_code == 200
    assert response.json()["type"] == "ack"
    assert captured["message"]["op"] == "sim_reboot"
