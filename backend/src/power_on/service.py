"""Startup policy: last color, default white, or custom color."""

from __future__ import annotations

import asyncio
import logging
import uuid
from typing import Any

from src import settings
from src.power_on.storage import power_on_store
from src.store.memory import Instance
from src.ws.gateway import dispatch_command, envelope

logger = logging.getLogger("lamp.power_on")

VALID_MODES = frozenset({"last", "default", "custom"})
DEFAULT_STATE = {
    "mode": "cct",
    "cct": settings.POWER_ON_DEFAULT_CCT,
    "brightness": settings.POWER_ON_DEFAULT_BRIGHTNESS,
}


class PowerOnError(Exception):
    """Business error of the power-on policy."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def _clamp(value: Any, minimum: int, maximum: int, fallback: int) -> int:
    try:
        number = int(value)
    except (TypeError, ValueError):
        number = fallback
    return min(maximum, max(minimum, number))


def normalize_state(state: dict | None) -> dict:
    if not isinstance(state, dict):
        raise PowerOnError("INVALID_STATE", "state is required for custom")
    mode = state.get("mode")
    brightness = _clamp(state.get("brightness"), 0, 1000, 800)
    if mode == "cct":
        return {
            "mode": "cct",
            "cct": _clamp(state.get("cct"), 2000, 6500, 3000),
            "brightness": brightness,
        }
    if mode == "color":
        return {
            "mode": "color",
            "h": _clamp(state.get("h"), 0, 360, 30),
            "s": _clamp(state.get("s"), 0, 1000, 800),
            "v": _clamp(state.get("v"), 0, 1000, 1000),
            "brightness": brightness,
        }
    raise PowerOnError("INVALID_STATE", "state mode must be cct or color")


def attach(instance: Instance) -> None:
    if instance.power_on_loaded:
        return
    record = power_on_store.load(instance.device_token)
    if record is not None:
        instance.power_on_mode = record.mode
        instance.power_on_state = record.state
    else:
        instance.power_on_mode = "default"
        instance.power_on_state = None
    instance.power_on_loaded = True


def policy(instance: Instance) -> dict[str, Any]:
    return {
        "mode": instance.power_on_mode,
        "state": instance.power_on_state,
    }


def update(
    instance: Instance,
    mode: str,
    state: dict | None,
) -> dict[str, Any]:
    if mode not in VALID_MODES:
        raise PowerOnError("INVALID_MODE", "unknown power-on mode")
    normalized = normalize_state(state) if mode == "custom" else None
    instance.power_on_mode = mode
    instance.power_on_state = normalized
    power_on_store.save(instance.device_token, mode, normalized)
    return policy(instance)


def reset(instance: Instance) -> dict[str, Any]:
    """Returns to the default policy (`default`) and deletes the persisted one."""
    instance.power_on_mode = "default"
    instance.power_on_state = None
    power_on_store.delete(instance.device_token)
    return policy(instance)


async def _wait_for_device(instance: Instance, attempts: int = 40) -> bool:
    for _ in range(attempts):
        if instance.device is not None:
            return True
        await asyncio.sleep(0.5)
    return False


async def _send(
    instance: Instance,
    op: str,
    payload: dict,
    *,
    wait: bool = True,
) -> bool:
    command_id = f"pon-{uuid.uuid4().hex[:8]}"
    if not wait:
        # Auto-start is fire-and-forget: it must not delay a panel that is
        # still opening, and the firmware acks normally.
        if instance.device is None:
            return False
        try:
            await instance.device.send_json(
                envelope("command", id_=command_id, op=op, payload=payload)
            )
            return True
        except Exception:  # noqa: BLE001
            logger.warning("power-on command %s failed", op)
            return False
    try:
        result = await dispatch_command(
            instance,
            {
                "id": command_id,
                "type": "command",
                "op": op,
                "payload": payload,
            },
        )
    except Exception:  # noqa: BLE001
        logger.warning("power-on command %s failed", op)
        return False
    return result.get("type") != "nack"


async def apply_after_power_on(
    instance: Instance, *, wait: bool = True
) -> None:
    """Applies the configured color after a hard power on.

    It does not interfere with pairing/connecting or with the 3-cycle sequence.
    """
    if instance.power_on_mode == "last":
        return
    state = (
        DEFAULT_STATE
        if instance.power_on_mode == "default"
        else instance.power_on_state
    )
    if not state:
        return
    if not await _wait_for_device(instance):
        return
    if instance.provisioning_state in {"pairing", "connecting"}:
        return
    await _send(instance, "set_power", {"on": True}, wait=wait)
    if state.get("mode") == "cct":
        await _send(instance, "set_mode", {"mode": "cct"}, wait=wait)
        await _send(
            instance,
            "set_color_temperature",
            {"kelvin": _clamp(state.get("cct"), 2000, 6500, 3000)},
            wait=wait,
        )
    else:
        await _send(instance, "set_mode", {"mode": "manual"}, wait=wait)
        await _send(
            instance,
            "set_color",
            {
                "h": _clamp(state.get("h"), 0, 360, 30),
                "s": _clamp(state.get("s"), 0, 1000, 800),
                "v": _clamp(state.get("v"), 0, 1000, 1000),
            },
            wait=wait,
        )
    await _send(
        instance,
        "set_brightness",
        {"value": _clamp(state.get("brightness"), 0, 1000, 800)},
        wait=wait,
    )


async def auto_start(instance: Instance) -> None:
    """Turns the light on once per instance when the device is present.

    The supervisor starts the firmware by itself (like plugging in the lamp),
    so this only covers the light state after a fresh instance or a backend
    restart. An explicit Power Off from the Simulation Panel sets
    `power_explicitly_off` and is respected. A soft off or a "last" policy
    stays off because the hook runs at most once per instance.
    """
    if (
        instance.power_explicitly_off
        or instance.auto_power_on_done
        or instance.auto_start_running
        or instance.device is None
    ):
        return
    instance.auto_start_running = True
    try:
        await apply_after_power_on(instance, wait=False)
        instance.auto_power_on_done = True
    finally:
        instance.auto_start_running = False
