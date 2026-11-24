"""Tests for the dashboard WebSocket's ephemeral ticket."""

from __future__ import annotations

import time

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from src import settings
from src.auth import ws_tickets
from src.main import app

client = TestClient(app)


def _instance() -> dict:
    response = client.post("/api/v1/instances", json={})
    assert response.status_code == 201
    return response.json()


def _ticket(instance: dict) -> str:
    response = client.post(
        f"/api/v1/instances/{instance['id']}/ws-ticket",
        headers={"Authorization": f"Bearer {instance['session_token']}"},
    )
    assert response.status_code == 200
    return response.json()["ticket"]


def test_ws_ticket_connects_once():
    instance = _instance()
    ticket = _ticket(instance)

    with client.websocket_connect(
        f"/ws/dashboard?instance={instance['id']}&ticket={ticket}"
    ) as dashboard:
        dashboard.send_json({"type": "subscribe", "channels": ["state"]})
        message = dashboard.receive_json()
        assert message["type"] == "subscribed"

    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect(
            f"/ws/dashboard?instance={instance['id']}&ticket={ticket}"
        ):
            pass


def test_ws_ticket_requires_session():
    instance = _instance()
    response = client.post(f"/api/v1/instances/{instance['id']}/ws-ticket")
    assert response.status_code == 401


def test_ws_ticket_is_bound_to_instance():
    first = _instance()
    second = _instance()
    ticket = _ticket(first)

    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect(
            f"/ws/dashboard?instance={second['id']}&ticket={ticket}"
        ):
            pass


def test_ws_ticket_expires(monkeypatch):
    instance = _instance()
    monkeypatch.setattr(settings, "WS_TICKET_TTL_S", 0)
    ticket = _ticket(instance)
    time.sleep(0.01)
    assert ws_tickets.consume(instance["id"], ticket) is None
