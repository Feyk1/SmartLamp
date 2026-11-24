"""State machine of the simulated onboarding.

There is no real BLE or Wi-Fi: the "scan" returns the instance in pairing, the
"connect" simulates joining the network and stores only SSID + a fake IP. The
password is never persisted, logged, or sent to the firmware.
"""

from __future__ import annotations

import asyncio
import copy
import logging
import secrets
import uuid
from datetime import datetime, timezone
from typing import Any

from src import settings
from src.provisioning.storage import encrypt_password, provisioning_store
from src.store.memory import Instance, iso
from src.ws.gateway import broadcast, dispatch_command

logger = logging.getLogger("lamp.provisioning")

PROVISIONED = "provisioned"
PAIRING = "pairing"
CONNECTING = "connecting"
ONLINE = "online"

PAIRING_SEQUENCE_ID = 7  # slow white pulse
PAIRING_SEQUENCE_SPEED = 1000

VALID_FAILURES = frozenset(
    {"none", "wrong_password", "unknown_network", "timeout"}
)


class ProvisioningError(Exception):
    """Business error with a stable code for the API."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def serial_suffix(instance: Instance) -> str | None:
    """Returns the last four alphanumeric characters of the product serial."""
    health = instance.last_health or {}
    serial = health.get("serial")
    if not isinstance(serial, str):
        return None
    cleaned = "".join(char for char in serial if char.isalnum())
    if len(cleaned) < 4:
        return None
    return cleaned[-4:].upper()


def device_name(instance: Instance) -> str:
    suffix = serial_suffix(instance)
    if suffix is None:
        suffix = instance.id.removeprefix("inst-").upper()
    return f"SmartLamp-{suffix}"


def fake_ip(instance: Instance) -> str:
    value = sum(ord(char) for char in instance.id) % 200
    return f"192.168.1.{20 + value}"


def info(instance: Instance) -> dict[str, Any]:
    return {
        "state": instance.provisioning_state,
        "name": device_name(instance),
        "ssid": instance.provisioning_ssid,
        "ip": instance.provisioning_ip,
        "cycles": len(instance.provisioning_cycles),
        "cycles_required": max(1, settings.PROVISIONING_CYCLES),
        "updated_at": iso(utc_now()),
    }


def attach(instance: Instance) -> None:
    """Loads persisted credentials or seeds the demo network.

    Called when the instance is created (including backend startup). If the
    user did "Forget Wi-Fi", the tombstone prevents re-seeding the network.
    """
    if instance.provisioning_loaded:
        return
    instance.provisioning_networks = list(settings.PROVISIONING_NETWORKS)
    instance.provisioning_passwords = {
        ssid: settings.DEFAULT_NETWORK_PASSWORDS.get(ssid, "")
        for ssid in instance.provisioning_networks
    }
    instance.provisioning_allow_any = settings.PROVISIONING_ALLOW_ANY
    instance.provisioning_failure = "none"

    record = provisioning_store.load(instance.device_token)
    if record is not None:
        if record.forgotten or not record.ssid:
            instance.provisioning_state = PROVISIONED
            instance.provisioning_ssid = None
            instance.provisioning_ip = None
        else:
            instance.provisioning_state = ONLINE
            instance.provisioning_ssid = record.ssid
            instance.provisioning_ip = record.ip or fake_ip(instance)
    elif settings.PROVISIONING_SEED_SSID:
        seed_ssid = settings.PROVISIONING_SEED_SSID
        seed_password = (
            settings.PROVISIONING_SEED_PASSWORD
            or settings.DEFAULT_NETWORK_PASSWORDS.get(seed_ssid, "")
            or secrets.token_urlsafe(24)
        )
        instance.provisioning_state = ONLINE
        instance.provisioning_ssid = seed_ssid
        instance.provisioning_ip = fake_ip(instance)
        instance.provisioning_passwords.setdefault(seed_ssid, seed_password)
        provisioning_store.save(
            instance.device_token,
            ssid=seed_ssid,
            password=encrypt_password(seed_password),
            ip=instance.provisioning_ip,
        )
    else:
        instance.provisioning_state = PROVISIONED
        instance.provisioning_ssid = None
        instance.provisioning_ip = None
    instance.provisioning_loaded = True


def policy(instance: Instance) -> dict[str, Any]:
    return {
        "networks": list(instance.provisioning_networks),
        "passwords": dict(instance.provisioning_passwords),
        "allow_any": instance.provisioning_allow_any,
        "failure": instance.provisioning_failure,
    }


def update_policy(
    instance: Instance,
    networks: list[str],
    allow_any: bool,
    failure: str,
    passwords: dict[str, str] | None = None,
) -> dict[str, Any]:
    if failure not in VALID_FAILURES:
        raise ProvisioningError("INVALID_FAILURE", "unknown failure mode")
    cleaned: list[str] = []
    for value in networks:
        ssid = value.strip()
        if not ssid:
            continue
        if len(ssid) > 64:
            raise ProvisioningError("INVALID_NETWORK", "SSID is too long")
        if ssid not in cleaned:
            cleaned.append(ssid)
    if len(cleaned) > 16:
        raise ProvisioningError("INVALID_NETWORK", "too many networks")
    next_passwords: dict[str, str] = {}
    for ssid in cleaned:
        if passwords is None:
            value = instance.provisioning_passwords.get(
                ssid, settings.DEFAULT_NETWORK_PASSWORDS.get(ssid, "")
            )
        else:
            value = passwords.get(ssid) or ""
        if len(value) > 64:
            raise ProvisioningError(
                "INVALID_NETWORK", "network password is too long"
            )
        next_passwords[ssid] = value
    instance.provisioning_networks = cleaned
    instance.provisioning_passwords = next_passwords
    instance.provisioning_allow_any = bool(allow_any)
    instance.provisioning_failure = failure
    return policy(instance)


async def _event(instance: Instance, state: str, **payload: Any) -> None:
    event = {
        "type": "event",
        "op": "provisioning",
        "payload": {"state": state, **payload},
        "ts": iso(utc_now()),
    }
    instance.events.append(event)
    await broadcast(instance, event)


async def _send(instance: Instance, op: str, payload: dict) -> bool:
    try:
        result = await dispatch_command(
            instance,
            {
                "id": f"prov-{uuid.uuid4().hex[:8]}",
                "type": "command",
                "op": op,
                "payload": payload,
            },
        )
    except Exception:  # noqa: BLE001
        logger.warning("provisioning command %s failed", op)
        return False
    return result.get("type") != "nack"


async def _wait_for_device(instance: Instance, attempts: int = 24) -> bool:
    """Waits for the firmware to come back after a power cycle (max 12 s)."""
    for _ in range(attempts):
        if instance.device is not None:
            return True
        await asyncio.sleep(0.5)
    return False


async def restore_later(instance: Instance) -> None:
    """Waits for the firmware and restores the light without blocking the API."""
    await _wait_for_device(instance)
    await restore(instance)


async def blink_later(instance: Instance) -> None:
    """Waits for the firmware to come back after the power cycle and blinks."""
    await _wait_for_device(instance)
    for _ in range(5):
        if instance.provisioning_state != PAIRING:
            return
        ok = await _send(
            instance,
            "start_sequence",
            {
                "id": PAIRING_SEQUENCE_ID,
                "speed": PAIRING_SEQUENCE_SPEED,
                "direction": 0,
                "loop": 0,
            },
        )
        if ok:
            return
        await asyncio.sleep(1.0)


async def restore(instance: Instance) -> None:
    """Stops blinking and returns the lamp to its previous state."""
    await _send(instance, "stop_sequence", {})
    state = instance.provisioning_prev_state
    if not state:
        state = {"power": True, "mode": 1, "cct": 4000, "brightness": 800}
    if not state.get("power"):
        await _send(instance, "set_power", {"on": False})
        return
    if int(state.get("mode", 0)) == 1:
        await _send(instance, "set_mode", {"mode": "cct"})
        await _send(
            instance,
            "set_color_temperature",
            {"kelvin": int(state.get("cct", 4000))},
        )
    else:
        color = state.get("color") or {}
        await _send(instance, "set_mode", {"mode": "manual"})
        await _send(
            instance,
            "set_color",
            {
                "h": int(color.get("h", 0)),
                "s": int(color.get("s", 1000)),
                "v": int(color.get("v", 1000)),
            },
        )
    await _send(
        instance,
        "set_brightness",
        {"value": int(state.get("brightness", 800))},
    )


async def start_pairing(instance: Instance, source: str = "manual") -> dict:
    if instance.provisioning_state == PAIRING:
        # A new power cycle during pairing must resume
        # blinking without touching the saved configuration.
        asyncio.create_task(blink_later(instance))
        return info(instance)
    if instance.provisioning_state == CONNECTING:
        return info(instance)
    instance.provisioning_prev_state = copy.deepcopy(instance.last_state)
    instance.provisioning_state = PAIRING
    # The previous configuration is kept until a connect replaces it.
    instance.provisioning_last_off = 0.0
    instance.provisioning_cycles.clear()
    await _event(instance, PAIRING, source=source)
    asyncio.create_task(blink_later(instance))
    return info(instance)


async def _connection_error(
    instance: Instance,
    ssid: str,
    code: str,
    message: str,
    delay: float,
) -> None:
    """Returns to pairing after a failure and emits an event without a password."""
    await asyncio.sleep(delay)
    instance.provisioning_state = PAIRING
    await _event(instance, PAIRING, ssid=ssid, error=code)
    raise ProvisioningError(code, message)


async def connect(instance: Instance, ssid: str, password: str) -> dict:
    """Simulates BLE→Wi-Fi and stores the SSID/encrypted password.

    The password only lives in this function: it is encrypted for SQLite and
    then discarded. It is not logged or included in events or responses.
    """
    if instance.provisioning_state != PAIRING:
        raise ProvisioningError("INVALID_STATE", "device is not pairing")
    ssid = ssid.strip()
    if not ssid:
        raise ProvisioningError("INVALID_NETWORK", "SSID is required")

    failure = instance.provisioning_failure
    instance.provisioning_state = CONNECTING
    await _event(instance, CONNECTING, ssid=ssid)

    if failure == "timeout":
        await _connection_error(
            instance,
            ssid,
            "WIFI_TIMEOUT",
            "Wi-Fi connection timed out",
            settings.PROVISIONING_TIMEOUT_S,
        )

    if failure == "wrong_password":
        await _connection_error(
            instance,
            ssid,
            "INVALID_PASSWORD",
            "incorrect Wi-Fi password",
            settings.PROVISIONING_FAILURE_DELAY_S,
        )

    if failure == "unknown_network" or (
        not instance.provisioning_allow_any
        and ssid not in instance.provisioning_networks
    ):
        await _connection_error(
            instance,
            ssid,
            "UNKNOWN_NETWORK",
            "network is not in the whitelist",
            settings.PROVISIONING_FAILURE_DELAY_S,
        )

    expected = instance.provisioning_passwords.get(ssid, "")
    if expected and password != expected:
        await _connection_error(
            instance,
            ssid,
            "INVALID_PASSWORD",
            "incorrect Wi-Fi password",
            settings.PROVISIONING_FAILURE_DELAY_S,
        )

    await asyncio.sleep(settings.PROVISIONING_DELAY_S)
    instance.provisioning_ssid = ssid
    instance.provisioning_ip = fake_ip(instance)
    instance.provisioning_state = ONLINE
    provisioning_store.save(
        instance.device_token,
        ssid=ssid,
        password=encrypt_password(password),
        ip=instance.provisioning_ip,
    )
    del password  # not kept in memory after encrypting it
    await _event(instance, ONLINE, ssid=ssid, ip=instance.provisioning_ip)
    await restore(instance)
    return info(instance)


async def cancel_pairing(instance: Instance) -> dict:
    """Leaves pairing without changing the saved credentials."""
    if instance.provisioning_state != PAIRING:
        return info(instance)
    instance.provisioning_state = (
        ONLINE if instance.provisioning_ssid else PROVISIONED
    )
    instance.provisioning_last_off = 0.0
    instance.provisioning_cycles.clear()
    await _event(instance, instance.provisioning_state, canceled=True)
    # The firmware may be starting after the power cycle: restoring in
    # the background avoids blocking the response and losing the command.
    asyncio.create_task(restore_later(instance))
    return info(instance)


async def reset(instance: Instance) -> dict:
    provisioning_store.forget(instance.device_token)
    instance.provisioning_ssid = None
    instance.provisioning_ip = None
    instance.provisioning_state = PROVISIONED
    instance.provisioning_cycles.clear()
    instance.provisioning_last_off = 0.0
    return await start_pairing(instance, source="reset")


def should_cancel_pairing(instance: Instance, action: str) -> bool:
    """True if an off→on during pairing must exit and keep the network."""
    return (
        action == "on"
        and instance.provisioning_state == PAIRING
        and instance.provisioning_last_off > 0.0
    )


def note_power(instance: Instance, action: str, now: float) -> bool:
    """Counts off→on cycles and detects the N pairing cycles.

    Each off→on must last between PROVISIONING_OFF_ON_MIN_S and
    PROVISIONING_OFF_ON_MAX_S; all cycles must fit within
    PROVISIONING_CYCLE_TOTAL_S. If the sequence drags on too long, the count
    is reset so as not to carry over old cycles.
    """
    if action == "off":
        instance.provisioning_last_off = now
        return False
    if action != "on" or instance.provisioning_last_off <= 0.0:
        return False

    gap = now - instance.provisioning_last_off
    instance.provisioning_last_off = 0.0
    if (
        gap < settings.PROVISIONING_OFF_ON_MIN_S
        or gap > settings.PROVISIONING_OFF_ON_MAX_S
    ):
        instance.provisioning_cycles.clear()
        return False

    instance.provisioning_cycles.append(now)
    count = max(1, settings.PROVISIONING_CYCLES)
    instance.provisioning_cycles = instance.provisioning_cycles[-count:]
    if len(instance.provisioning_cycles) >= count:
        if (
            instance.provisioning_cycles[-1]
            - instance.provisioning_cycles[0]
            <= settings.PROVISIONING_CYCLE_TOTAL_S
        ):
            instance.provisioning_cycles.clear()
            return True
        instance.provisioning_cycles.clear()
    return False
