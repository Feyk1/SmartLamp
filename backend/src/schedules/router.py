"""Per-instance schedule endpoints."""

from __future__ import annotations

import json
import uuid
from datetime import datetime

from fastapi import APIRouter, Depends, Response

from src.api.errors import ApiError
from src.auth.session import Session, require_session
from src.schedules.runner import (
    next_occurrence,
    utc_now,
    validate_schedule,
)
from src.schedules.storage import schedule_store
from src.schemas import (
    CreateScheduleRequest,
    ScheduleEnabledRequest,
)
from src.store.memory import Schedule, iso

router = APIRouter(tags=["schedules"])

MAX_SCHEDULES = 32


def _info(schedule: Schedule) -> dict:
    return {
        "id": schedule.id,
        "time": schedule.time,
        "timezone": schedule.timezone,
        "repeat": schedule.repeat,
        "days": schedule.days,
        "action": schedule.action,
        "enabled": schedule.enabled,
        "next_run": iso(schedule.next_run) if schedule.next_run else None,
        "created_at": iso(schedule.created_at),
        "last_run": iso(schedule.last_run) if schedule.last_run else None,
        "last_status": schedule.last_status,
        "last_error": schedule.last_error,
    }


def _signature(
    time_text: str,
    timezone_name: str,
    repeat: str,
    days: list[int],
    action: dict,
) -> tuple:
    return (
        time_text,
        timezone_name,
        repeat,
        tuple(sorted(set(days))),
        json.dumps(action, sort_keys=True),
    )


def _prepare(
    instance,
    request: CreateScheduleRequest,
    current: Schedule | None = None,
) -> tuple[str, str, str, list[int], dict, datetime | None]:
    try:
        action = validate_schedule(
            request.time,
            request.timezone,
            request.repeat,
            request.days,
            request.action.model_dump(),
        )
    except ValueError as exc:
        raise ApiError(400, "INVALID_SCHEDULE", str(exc)) from exc

    days = sorted({int(day) for day in request.days})
    signature = _signature(
        request.time, request.timezone, request.repeat, days, action
    )
    for other in instance.schedules:
        if current is not None and other.id == current.id:
            continue
        if _signature(
            other.time, other.timezone, other.repeat, other.days, other.action
        ) == signature:
            raise ApiError(409, "DUPLICATE_SCHEDULE", "schedule already exists")

    next_run = None
    if request.enabled:
        next_run = next_occurrence(
            request.time,
            request.timezone,
            request.repeat,
            days,
            utc_now(),
        )
    return (
        request.time,
        request.timezone,
        request.repeat,
        days,
        action,
        next_run,
    )


@router.get("/instances/{instance_id}/schedules")
async def list_schedules(
    instance_id: str, session: Session = Depends(require_session)
) -> dict:
    del instance_id
    return {"schedules": [_info(item) for item in session.instance.schedules]}


@router.post("/instances/{instance_id}/schedules", status_code=201)
async def create_schedule(
    instance_id: str,
    request: CreateScheduleRequest,
    session: Session = Depends(require_session),
) -> dict:
    del instance_id
    instance = session.instance
    if len(instance.schedules) >= MAX_SCHEDULES:
        raise ApiError(409, "SCHEDULE_LIMIT", "too many schedules")
    time_text, tz, repeat, days, action, next_run = _prepare(instance, request)
    schedule = Schedule(
        id=f"sch-{uuid.uuid4().hex[:8]}",
        time=time_text,
        timezone=tz,
        repeat=repeat,
        days=days,
        action=action,
        enabled=request.enabled,
        next_run=next_run,
        created_at=utc_now(),
    )
    instance.schedules.append(schedule)
    schedule_store.save(instance.device_token, schedule)
    return _info(schedule)


@router.put("/instances/{instance_id}/schedules/{schedule_id}")
async def update_schedule(
    instance_id: str,
    schedule_id: str,
    request: CreateScheduleRequest,
    session: Session = Depends(require_session),
) -> dict:
    del instance_id
    instance = session.instance
    schedule = next(
        (item for item in instance.schedules if item.id == schedule_id), None
    )
    if schedule is None:
        raise ApiError(404, "SCHEDULE_NOT_FOUND", "schedule not found")
    time_text, tz, repeat, days, action, next_run = _prepare(
        instance, request, schedule
    )
    schedule.time = time_text
    schedule.timezone = tz
    schedule.repeat = repeat
    schedule.days = days
    schedule.action = action
    schedule.enabled = request.enabled
    schedule.next_run = next_run
    schedule_store.save(instance.device_token, schedule)
    return _info(schedule)


@router.patch("/instances/{instance_id}/schedules/{schedule_id}")
async def set_schedule_enabled(
    instance_id: str,
    schedule_id: str,
    request: ScheduleEnabledRequest,
    session: Session = Depends(require_session),
) -> dict:
    del instance_id
    instance = session.instance
    schedule = next(
        (item for item in instance.schedules if item.id == schedule_id), None
    )
    if schedule is None:
        raise ApiError(404, "SCHEDULE_NOT_FOUND", "schedule not found")
    schedule.enabled = request.enabled
    schedule.next_run = (
        next_occurrence(
            schedule.time,
            schedule.timezone,
            schedule.repeat,
            schedule.days,
            utc_now(),
        )
        if request.enabled
        else None
    )
    schedule_store.save(instance.device_token, schedule)
    return _info(schedule)


@router.delete(
    "/instances/{instance_id}/schedules/{schedule_id}", status_code=204
)
async def delete_schedule(
    instance_id: str,
    schedule_id: str,
    session: Session = Depends(require_session),
) -> Response:
    del instance_id
    instance = session.instance
    remaining = [
        item for item in instance.schedules if item.id != schedule_id
    ]
    if len(remaining) == len(instance.schedules):
        raise ApiError(404, "SCHEDULE_NOT_FOUND", "schedule not found")
    instance.schedules = remaining
    schedule_store.delete(instance.device_token, schedule_id)
    return Response(status_code=204)
