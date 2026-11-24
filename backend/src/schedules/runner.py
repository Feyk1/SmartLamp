"""Logic and execution of schedules (one-shot, recurring, and sunrise).

The time is stored as HH:MM + IANA zone; the next occurrence is computed with
zoneinfo so that DST changes do not shift the schedule. The actions are
translated into the commands the firmware already understands. The sunrise runs
as a background ramp and failures are retried before marking the schedule as
skipped.
"""

from __future__ import annotations

import asyncio
import logging
import re
import uuid
from dataclasses import dataclass
from datetime import datetime, time, timedelta, timezone
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from src.schedules.storage import schedule_store
from src.store.memory import Instance, Schedule, store
from src.ws.gateway import dispatch_command

logger = logging.getLogger("lamp.schedules")

SCENES: dict[str, tuple[int, int]] = {
    "reading": (4000, 800),
    "relax": (2700, 450),
    "focus": (5600, 1000),
    "night": (2200, 120),
}

EFFECTS: dict[str, int] = {
    "breathing": 1,
    "rainbow": 2,
    "strobe": 3,
    "candle": 4,
    "police": 5,
    "sunrise": 6,
}

REPEATS = {"once", "daily", "weekdays", "weekends", "custom"}
TIME_RE = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")
MAX_RETRIES = 2
RETRY_DELAY_S = 10
SUNRISE_START_KELVIN = 2200
SUNRISE_START_BRIGHTNESS = 20


@dataclass
class SunriseRun:
    key: str
    instance_id: str
    schedule_id: str
    started_at: datetime
    duration_s: int
    kelvin: int
    brightness: int
    error: str | None = None


active_sunrises: dict[str, SunriseRun] = {}


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def parse_time(value: str) -> time:
    if not TIME_RE.match(value):
        raise ValueError("time must be HH:MM")
    hours, minutes = (int(part) for part in value.split(":"))
    return time(hour=hours, minute=minutes)


def zone(name: str) -> ZoneInfo:
    try:
        return ZoneInfo(name)
    except ZoneInfoNotFoundError as exc:
        raise ValueError(f"unknown timezone: {name}") from exc


def day_matches(repeat: str, days: list[int], weekday: int) -> bool:
    if repeat == "daily" or repeat == "once":
        return True
    if repeat == "weekdays":
        return weekday < 5
    if repeat == "weekends":
        return weekday >= 5
    if repeat == "custom":
        return weekday in days
    return False


def next_occurrence(
    time_text: str,
    timezone_name: str,
    repeat: str,
    days: list[int],
    after: datetime,
) -> datetime | None:
    """Next occurrence in UTC after `after` (DST-safe)."""
    moment = parse_time(time_text)
    tz = zone(timezone_name)
    if after.tzinfo is None:
        after = after.replace(tzinfo=timezone.utc)
    local_after = after.astimezone(tz)
    for offset in range(8):
        day = local_after.date() + timedelta(days=offset)
        if not day_matches(repeat, days, day.weekday()):
            continue
        candidate = datetime.combine(day, moment, tzinfo=tz)
        if candidate > local_after:
            return candidate.astimezone(timezone.utc)
    return None


def _int(value, default: int) -> int:
    return int(value) if value is not None else default


def normalize_action(action: dict) -> dict:
    kind = action.get("type") or "scene"
    if kind == "scene":
        scene = action.get("scene")
        if scene not in SCENES:
            raise ValueError(f"unknown scene: {scene}")
        return {"type": "scene", "scene": scene}
    if kind == "off":
        return {"type": "off"}
    if kind == "color":
        return {
            "type": "color",
            "h": _int(action.get("h"), 0),
            "s": _int(action.get("s"), 1000),
            "v": _int(action.get("v"), 1000),
            "brightness": _int(action.get("brightness"), 800),
        }
    if kind == "cct":
        kelvin = _int(action.get("kelvin"), 2700)
        if not 2000 <= kelvin <= 6500:
            raise ValueError("kelvin must be 2000-6500")
        return {
            "type": "cct",
            "kelvin": kelvin,
            "brightness": _int(action.get("brightness"), 800),
        }
    if kind == "effect":
        effect = action.get("effect")
        if effect not in EFFECTS:
            raise ValueError(f"unknown effect: {effect}")
        speed = _int(action.get("speed"), 1000)
        if not 100 <= speed <= 4000:
            raise ValueError("speed must be 100-4000")
        return {"type": "effect", "effect": effect, "speed": speed}
    if kind == "sunrise":
        kelvin = _int(action.get("kelvin"), 4000)
        if not 2000 <= kelvin <= 6500:
            raise ValueError("kelvin must be 2000-6500")
        duration = _int(action.get("duration_min"), 30)
        if not 1 <= duration <= 60:
            raise ValueError("sunrise duration must be 1-60 minutes")
        return {
            "type": "sunrise",
            "kelvin": kelvin,
            "brightness": _int(action.get("brightness"), 800),
            "duration_min": duration,
        }
    raise ValueError(f"unknown action: {kind}")


def validate_schedule(
    time_text: str,
    timezone_name: str,
    repeat: str,
    days: list[int],
    action: dict,
) -> dict:
    parse_time(time_text)
    zone(timezone_name)
    if repeat not in REPEATS:
        raise ValueError(f"unknown repeat: {repeat}")
    if repeat == "custom" and not days:
        raise ValueError("custom repeat needs at least one day")
    if any(day < 0 or day > 6 for day in days):
        raise ValueError("days must be 0-6 (Monday-Sunday)")
    return normalize_action(action)


def action_commands(action: dict) -> list[tuple[str, dict]]:
    kind = action["type"]
    if kind == "off":
        return [("set_power", {"on": False})]
    if kind == "scene":
        cct, brightness = SCENES[action["scene"]]
        return [
            ("set_power", {"on": True}),
            ("set_mode", {"mode": "cct"}),
            ("set_color_temperature", {"kelvin": cct}),
            ("set_brightness", {"value": brightness}),
        ]
    if kind == "color":
        return [
            ("set_power", {"on": True}),
            ("set_mode", {"mode": "manual"}),
            (
                "set_color",
                {
                    "h": action["h"],
                    "s": action["s"],
                    "v": action["v"],
                },
            ),
            ("set_brightness", {"value": action["brightness"]}),
        ]
    if kind == "cct":
        return [
            ("set_power", {"on": True}),
            ("set_mode", {"mode": "cct"}),
            ("set_color_temperature", {"kelvin": action["kelvin"]}),
            ("set_brightness", {"value": action["brightness"]}),
        ]
    if kind == "effect":
        return [
            (
                "start_sequence",
                {
                    "id": EFFECTS[action["effect"]],
                    "speed": action["speed"],
                    "direction": 0,
                    "loop": 0,
                },
            )
        ]
    if kind == "sunrise":
        return [
            ("set_power", {"on": True}),
            ("set_mode", {"mode": "cct"}),
            ("set_color_temperature", {"kelvin": SUNRISE_START_KELVIN}),
            ("set_brightness", {"value": SUNRISE_START_BRIGHTNESS}),
        ]
    raise ValueError(f"unknown action: {kind}")


async def _send(instance: Instance, op: str, payload: dict) -> tuple[bool, str | None]:
    result = await dispatch_command(
        instance,
        {
            "id": f"sch-{uuid.uuid4().hex[:8]}",
            "type": "command",
            "op": op,
            "payload": payload,
        },
    )
    if result.get("type") == "nack":
        code = result.get("payload", {}).get("code", "nack")
        logger.warning(
            "schedule for %s skipped at %s (%s)", instance.id, op, code
        )
        return False, str(code)
    return True, None


async def apply_action(instance: Instance, action: dict) -> tuple[str, str | None]:
    """Applies an action; returns (status, error)."""
    for op, payload in action_commands(action):
        ok, error = await _send(instance, op, payload)
        if not ok:
            return "skipped", error
    return "ok", None


async def start_sunrise(
    instance: Instance, schedule: Schedule, when: datetime
) -> tuple[str, str | None]:
    action = schedule.action
    for op, payload in action_commands(action):
        ok, error = await _send(instance, op, payload)
        if not ok:
            return "skipped", error
    active_sunrises[f"{instance.id}:{schedule.id}"] = SunriseRun(
        key=f"{instance.id}:{schedule.id}",
        instance_id=instance.id,
        schedule_id=schedule.id,
        started_at=when,
        duration_s=max(60, int(action["duration_min"]) * 60),
        kelvin=int(action["kelvin"]),
        brightness=int(action["brightness"]),
    )
    return "running", None


async def tick_sunrises(now: datetime | None = None) -> None:
    when = now or utc_now()
    if when.tzinfo is None:
        when = when.replace(tzinfo=timezone.utc)
    for key, run in list(active_sunrises.items()):
        instance = store.get(run.instance_id)
        if instance is None:
            active_sunrises.pop(key, None)
            continue
        progress = (when - run.started_at).total_seconds() / run.duration_s
        progress = max(0.0, min(1.0, progress))
        kelvin = round(
            SUNRISE_START_KELVIN
            + (run.kelvin - SUNRISE_START_KELVIN) * progress
        )
        brightness = round(
            SUNRISE_START_BRIGHTNESS
            + (run.brightness - SUNRISE_START_BRIGHTNESS) * progress
        )
        for op, payload in (
            ("set_color_temperature", {"kelvin": kelvin}),
            ("set_brightness", {"value": brightness}),
        ):
            ok, error = await _send(instance, op, payload)
            if not ok:
                run.error = error
        if progress >= 1.0:
            active_sunrises.pop(key, None)
            schedule = next(
                (item for item in instance.schedules if item.id == run.schedule_id),
                None,
            )
            if schedule is not None:
                schedule.last_run = when
                schedule.last_status = "skipped" if run.error else "ok"
                schedule.last_error = run.error
                schedule_store.save(instance.device_token, schedule)


async def run_due(now: datetime | None = None) -> int:
    """Executes the due schedules and advances their next occurrence."""
    when = now or utc_now()
    if when.tzinfo is None:
        when = when.replace(tzinfo=timezone.utc)
    executed = 0
    for instance in store.all():
        for schedule in list(instance.schedules):
            if (
                not schedule.enabled
                or schedule.next_run is None
                or schedule.next_run > when
            ):
                continue
            try:
                if schedule.action.get("type") == "sunrise":
                    status, error = await start_sunrise(instance, schedule, when)
                else:
                    status, error = await apply_action(instance, schedule.action)
            except Exception:  # noqa: BLE001 - the loop must not die
                logger.exception(
                    "schedule %s for %s failed", schedule.id, instance.id
                )
                status, error = "failed", "exception"

            schedule.last_run = when
            schedule.last_status = status
            schedule.last_error = error

            if status == "skipped" and schedule.attempts < MAX_RETRIES:
                schedule.attempts += 1
                schedule.last_status = "retry"
                schedule.next_run = when + timedelta(seconds=RETRY_DELAY_S)
                schedule_store.save(instance.device_token, schedule)
                executed += 1
                continue

            schedule.attempts = 0
            if schedule.repeat == "once":
                schedule.enabled = False
                schedule.next_run = None
            else:
                try:
                    schedule.next_run = next_occurrence(
                        schedule.time,
                        schedule.timezone,
                        schedule.repeat,
                        schedule.days,
                        when + timedelta(seconds=1),
                    )
                except ValueError:
                    schedule.enabled = False
                    schedule.next_run = None
            schedule_store.save(instance.device_token, schedule)
            executed += 1
    return executed


async def schedules_loop(interval_s: float = 5.0) -> None:
    while True:
        await asyncio.sleep(interval_s)
        try:
            await run_due()
            await tick_sunrises()
        except Exception:  # noqa: BLE001
            logger.exception("schedules loop failed")
