"""Gateway WebSocket: /ws/device y /ws/dashboard."""

from __future__ import annotations

import asyncio
import time
import uuid
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from src import settings
from src.auth.tokens import TokenError, verify
from src.auth.ws_tickets import consume as consume_ws_ticket
from src.store.memory import Instance, store

router = APIRouter()

# Command timeout; 2 s per docs/02 §6.2.
COMMAND_TIMEOUT_S = settings.COMMAND_TIMEOUT_S


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def envelope(
    type_: str,
    *,
    id_: str | None = None,
    op: str | None = None,
    payload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    message: dict[str, Any] = {"id": id_ or uuid.uuid4().hex, "type": type_}
    if op is not None:
        message["op"] = op
    if payload is not None:
        message["payload"] = payload
    message["ts"] = now_iso()
    return message


async def broadcast(instance: Instance, message: dict[str, Any]) -> None:
    dead: list[WebSocket] = []
    for dashboard in list(instance.dashboards):
        try:
            await dashboard.send_json(message)
        except Exception:
            dead.append(dashboard)
    for dashboard in dead:
        instance.dashboards.discard(dashboard)


def _schedule_auto_start(instance: Instance) -> None:
    """Runs the one-shot light auto-on without importing at module load.

    `power_on.service` imports `dispatch_command` from this module, so the
    import stays local to avoid a cycle.
    """
    from src.power_on import service as power_on_service

    asyncio.create_task(power_on_service.auto_start(instance))


async def dispatch_command(
    instance: Instance, message: dict[str, Any]
) -> dict[str, Any]:
    command_id = message.get("id") or uuid.uuid4().hex
    op = message.get("op")
    payload = message.get("payload") or {}
    command = {"id": command_id, "op": op, "payload": payload}

    future: asyncio.Future = asyncio.get_running_loop().create_future()
    instance.pending[command_id] = future
    delivered = False

    if instance.device is not None:
        try:
            await instance.device.send_json(
                envelope("command", id_=command_id, op=op, payload=payload)
            )
            delivered = True
            instance.ws_pending.add(command_id)
        except Exception:
            delivered = False

    if not delivered:
        # WS down: the firmware picks it up with GET /commands/pending.
        instance.command_queue.append(command)

    try:
        return await asyncio.wait_for(future, timeout=COMMAND_TIMEOUT_S)
    except TimeoutError:
        discard_queued(instance, command_id)
        return envelope(
            "nack",
            id_=command_id,
            op=op,
            payload={
                "code": "TIMEOUT",
                "message": (
                    "device offline" if not delivered else "device did not respond"
                ),
            },
        )
    except asyncio.CancelledError:
        discard_queued(instance, command_id)
        return envelope(
            "nack",
            id_=command_id,
            op=op,
            payload={"code": "TIMEOUT", "message": "device disconnected"},
        )
    finally:
        instance.pending.pop(command_id, None)
        instance.ws_pending.discard(command_id)


def discard_queued(instance: Instance, command_id: str) -> None:
    """Removes a command from the REST queue if the dashboard stopped waiting."""
    instance.command_queue = [
        command
        for command in instance.command_queue
        if command.get("id") != command_id
    ]


async def ping_loop(websocket: WebSocket, last_pong: dict[str, float]) -> None:
    """Ping every PING_INTERVAL_S; closes if there is no PONG in PONG_TIMEOUT_S."""
    try:
        while True:
            await asyncio.sleep(settings.PING_INTERVAL_S)
            if (time.monotonic() - last_pong["t"]) > settings.PONG_TIMEOUT_S:
                await websocket.close(code=1001)
                return
            await websocket.send_json({"type": "ping", "ts": now_iso()})
    except Exception:
        return


@router.websocket("/ws/device")
async def ws_device(websocket: WebSocket, token: str = ""):
    instance = store.find_by_device_token(token)
    if instance is None:
        await websocket.close(code=1008)
        return

    await websocket.accept()
    previous = instance.device
    instance.device = websocket
    if previous is not None and previous is not websocket:
        try:
            await previous.close(code=1012)
        except Exception:
            pass

    # Delivers the commands that were left queued while the WS was down.
    for command in list(instance.command_queue):
        command_id = str(command.get("id") or "")
        instance.command_queue.remove(command)
        try:
            await websocket.send_json(
                envelope(
                    "command",
                    id_=command_id,
                    op=command.get("op"),
                    payload=command.get("payload") or {},
                )
            )
            instance.ws_pending.add(command_id)
        except Exception:
            instance.command_queue.append(command)
            break

    online = {
        "type": "event",
        "op": "device_online",
        "payload": {},
        "ts": now_iso(),
    }
    instance.events.append(online)
    await broadcast(instance, online)
    if instance.dashboards:
        _schedule_auto_start(instance)

    last_pong = {"t": time.monotonic()}
    ping_task = asyncio.create_task(ping_loop(websocket, last_pong))

    try:
        while True:
            message = await websocket.receive_json()
            last_pong["t"] = time.monotonic()
            type_ = message.get("type")

            if type_ in ("ack", "nack"):
                command_id = message.get("id")
                future = instance.pending.get(command_id)
                if future is not None and not future.done():
                    future.set_result(message)
                else:
                    await broadcast(instance, message)
            elif type_ == "state":
                instance.last_state = message.get("payload")
                instance.state_ts = message.get("ts") or now_iso()
                await broadcast(
                    instance,
                    {
                        "type": "state",
                        "payload": instance.last_state,
                        "ts": instance.state_ts,
                    },
                )
            elif type_ == "event":
                instance.events.append(message)
                await broadcast(instance, message)
            elif type_ == "ping":
                await websocket.send_json({"type": "pong", "ts": now_iso()})
            elif type_ == "pong":
                pass
            else:
                # Unknown ops: ignore (docs/02 §1).
                pass
    except WebSocketDisconnect:
        pass
    except Exception:
        pass
    finally:
        ping_task.cancel()
        if instance.device is websocket:
            instance.device = None
            offline = {
                "type": "event",
                "op": "device_offline",
                "payload": {},
                "ts": now_iso(),
            }
            instance.events.append(offline)
            await broadcast(instance, offline)
            # Only the commands delivered over this WS are cancelled; those
            # queued for the REST fallback keep waiting for their ack.
            for command_id in list(instance.ws_pending):
                future = instance.pending.get(command_id)
                if future is not None and not future.done():
                    future.cancel()
                instance.ws_pending.discard(command_id)


@router.websocket("/ws/dashboard")
async def ws_dashboard(
    websocket: WebSocket,
    instance: str = "",
    session: str = "",
    ticket: str = "",
):
    inst = store.get(instance)
    if inst is None:
        await websocket.close(code=1008)
        return
    if ticket:
        if consume_ws_ticket(inst.id, ticket) is None:
            await websocket.close(code=1008)
            return
    else:
        # Compatibility fallback; the dashboard uses an ephemeral ticket.
        try:
            claims = verify(session)
        except TokenError:
            await websocket.close(code=1008)
            return
        if claims.get("instance_id") != inst.id:
            await websocket.close(code=1008)
            return

    await websocket.accept()
    inst.dashboards.add(websocket)
    last_pong = {"t": time.monotonic()}
    ping_task = asyncio.create_task(ping_loop(websocket, last_pong))

    try:
        while True:
            message = await websocket.receive_json()
            last_pong["t"] = time.monotonic()
            type_ = message.get("type")

            if type_ == "subscribe":
                _schedule_auto_start(inst)
                await websocket.send_json(
                    {
                        "type": "subscribed",
                        "channels": message.get("channels", []),
                        "ts": now_iso(),
                    }
                )
                if inst.last_state is not None:
                    await websocket.send_json(
                        {
                            "type": "state",
                            "payload": inst.last_state,
                            "ts": inst.state_ts or now_iso(),
                        }
                    )
                for event in inst.events[-20:]:
                    await websocket.send_json(event)
            elif type_ == "command":
                result = await dispatch_command(inst, message)
                await websocket.send_json(result)
            elif type_ == "heartbeat":
                inst.last_heartbeat = time.time()
                await websocket.send_json({"type": "heartbeat_ack", "ts": now_iso()})
            elif type_ == "ping":
                await websocket.send_json({"type": "pong", "ts": now_iso()})
            elif type_ == "pong":
                pass
            else:
                pass
    except WebSocketDisconnect:
        pass
    except Exception:
        pass
    finally:
        ping_task.cancel()
        inst.dashboards.discard(websocket)
