"""WebSocket tests: device ↔ backend ↔ dashboard."""

from fastapi.testclient import TestClient

from src import settings
from src.main import app

client = TestClient(app)
DEVICE_TOKEN = settings.DEVICE_TOKEN


def _create_instance() -> dict:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    return response.json()


def _receive_type(websocket, message_type: str) -> dict:
    for _ in range(10):
        message = websocket.receive_json()
        if message.get("type") == message_type:
            return message
    raise AssertionError(f"no message of type {message_type} received")


def test_device_state_broadcast_to_dashboard():
    instance = _create_instance()

    with client.websocket_connect(
        f"/ws/device?token={DEVICE_TOKEN}"
    ) as device:
        with client.websocket_connect(
            f"/ws/dashboard?instance={instance['id']}"
            f"&session={instance['session_token']}"
        ) as dashboard:
            dashboard.send_json({"type": "subscribe", "channels": ["state"]})
            _receive_type(dashboard, "subscribed")

            device.send_json(
                {
                    "type": "state",
                    "payload": {
                        "power": True,
                        "color": {"h": 32, "s": 850, "v": 600},
                    },
                    "ts": "2026-11-18T00:00:00Z",
                }
            )
            state = _receive_type(dashboard, "state")
            assert state["type"] == "state"
            assert state["payload"]["power"] is True


def test_dashboard_command_roundtrip():
    instance = _create_instance()

    with client.websocket_connect(
        f"/ws/device?token={DEVICE_TOKEN}"
    ) as device:
        with client.websocket_connect(
            f"/ws/dashboard?instance={instance['id']}"
            f"&session={instance['session_token']}"
        ) as dashboard:
            dashboard.send_json(
                {
                    "id": "u-1",
                    "type": "command",
                    "op": "set_power",
                    "payload": {"on": True},
                }
            )

            command = device.receive_json()
            assert command["type"] == "command"
            assert command["id"] == "u-1"
            assert command["op"] == "set_power"

            device.send_json(
                {
                    "id": "u-1",
                    "type": "ack",
                    "op": "set_power",
                    "payload": {},
                }
            )
            ack = _receive_type(dashboard, "ack")
            assert ack["type"] == "ack"
            assert ack["id"] == "u-1"


def test_dashboard_command_without_device_returns_nack():
    instance = _create_instance()

    with client.websocket_connect(
        f"/ws/dashboard?instance={instance['id']}"
        f"&session={instance['session_token']}"
    ) as dashboard:
        dashboard.send_json(
            {
                "id": "u-2",
                "type": "command",
                "op": "set_power",
                "payload": {"on": True},
            }
        )
        nack = _receive_type(dashboard, "nack")
        assert nack["type"] == "nack"
        assert nack["id"] == "u-2"
        assert nack["payload"]["code"] == "TIMEOUT"
