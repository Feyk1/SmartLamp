"""Ephemeral, single-use tickets for the dashboard WebSocket.

The session JWT does not travel in the WS URL. The dashboard exchanges its session
for a short-lived ticket; the gateway consumes it before accepting the connection.
"""

from __future__ import annotations

import secrets
import time

from src import settings

_tickets: dict[str, dict] = {}


def _purge(now: float) -> None:
    expired = [
        ticket
        for ticket, record in _tickets.items()
        if record["expires_at"] <= now
    ]
    for ticket in expired:
        _tickets.pop(ticket, None)


def issue(instance_id: str, role: str) -> str:
    now = time.monotonic()
    _purge(now)
    ticket = secrets.token_urlsafe(24)
    _tickets[ticket] = {
        "instance_id": instance_id,
        "role": role,
        "expires_at": now + settings.WS_TICKET_TTL_S,
    }
    return ticket


def consume(instance_id: str, ticket: str) -> dict | None:
    """Consumes the ticket; returns the record only if it is valid."""
    if not ticket:
        return None
    record = _tickets.pop(ticket, None)
    if record is None:
        return None
    if record["expires_at"] <= time.monotonic():
        return None
    if record["instance_id"] != instance_id:
        return None
    return record


def reset() -> None:
    """Clears tickets; useful in tests."""
    _tickets.clear()
