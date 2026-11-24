"""Device REST telemetry (docs/02 §4.1)."""

from __future__ import annotations

from fastapi import APIRouter, Depends, Response

from src.api.device_auth import require_device_instance
from src.schemas import TelemetryEvents, TelemetryHealth, TelemetryState
from src.store.memory import Instance
from src.ws.gateway import broadcast, now_iso

router = APIRouter(prefix="/telemetry", tags=["telemetry"])


@router.post("/health", status_code=204)
async def post_health(
    payload: TelemetryHealth,
    instance: Instance = Depends(require_device_instance),
) -> Response:
    """Stores the firmware health and broadcasts it to the dashboards."""
    instance.last_health = payload.model_dump(exclude_none=True)
    instance.health_ts = payload.ts or now_iso()
    await broadcast(
        instance,
        {
            "type": "health",
            "payload": instance.last_health,
            "ts": instance.health_ts,
        },
    )
    return Response(status_code=204)


@router.post("/state", status_code=204)
async def post_state(
    payload: TelemetryState,
    instance: Instance = Depends(require_device_instance),
) -> Response:
    """Updates the real state (backup for `state` over WS)."""
    instance.last_state = payload.state
    instance.state_ts = payload.ts or now_iso()
    await broadcast(
        instance,
        {
            "type": "state",
            "payload": instance.last_state,
            "ts": instance.state_ts,
        },
    )
    return Response(status_code=204)


@router.post("/events", status_code=204)
async def post_events(
    payload: TelemetryEvents,
    instance: Instance = Depends(require_device_instance),
) -> Response:
    """Appends events (batch) to the timeline and broadcasts them."""
    for event in payload.events[:50]:
        message = {
            "type": "event",
            "op": str(event.get("type", "event")),
            "payload": event.get("detail", {}),
            "ts": event.get("ts") or now_iso(),
        }
        instance.events.append(message)
        await broadcast(instance, message)
    return Response(status_code=204)
