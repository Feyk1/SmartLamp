"""REST fallback for commands when the device WS is down.

See docs/02 §4.3. It is a safety net: the main path is the WS.
"""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter, Body, Depends, Response

from src.api.device_auth import require_device_instance
from src.schemas import QueuedCommands
from src.store.memory import Instance
from src.ws.gateway import envelope

router = APIRouter(prefix="/commands", tags=["commands"])


def _resolve(
    instance: Instance,
    command_id: str,
    ok: bool,
    body: dict[str, Any] | None,
) -> None:
    future = instance.pending.get(command_id)
    if future is None or future.done():
        return
    instance.ws_pending.discard(command_id)
    body = body or {}
    future.set_result(
        envelope(
            "ack" if ok else "nack",
            id_=command_id,
            op=body.get("op"),
            payload=body.get("payload") or {},
        )
    )


@router.get("/pending", response_model=QueuedCommands)
async def pending_commands(
    instance: Instance = Depends(require_device_instance),
) -> QueuedCommands:
    """Delivers the queued commands (at-most-once) to the firmware."""
    commands = instance.command_queue
    instance.command_queue = []
    return QueuedCommands(commands=commands)


@router.post("/{command_id}/ack", status_code=204)
async def ack_command(
    command_id: str,
    body: dict[str, Any] | None = Body(default=None),
    instance: Instance = Depends(require_device_instance),
) -> Response:
    _resolve(instance, command_id, True, body)
    return Response(status_code=204)


@router.post("/{command_id}/nack", status_code=204)
async def nack_command(
    command_id: str,
    body: dict[str, Any] | None = Body(default=None),
    instance: Instance = Depends(require_device_instance),
) -> Response:
    _resolve(instance, command_id, False, body)
    return Response(status_code=204)
