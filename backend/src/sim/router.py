"""Simulation Panel endpoints (`/instances/{id}/sim/*`)."""

from __future__ import annotations

import asyncio
import secrets
import time
import uuid
from typing import Any

from fastapi import APIRouter, Depends, Query

from src import settings
from src.api.errors import ApiError
from src.auth.session import Session, require_session, require_sim_session
from src.auth.tokens import issue
from src.power_on import service as power_on_service
from src.provisioning import service as provisioning
from src.schedules.storage import schedule_store
from src.schemas import (
    SimBootloaderRequest,
    SimFaultRequest,
    SimModbusWrite,
    SimPowerRequest,
    SimSessionRequest,
)
from src.security.rate_limit import limit_sim_pin
from src.sim.modbus import ModbusClient, ModbusError
from src.sim.supervisor import SupervisorClient, SupervisorError
from src.store.memory import Instance, store
from src.ws.gateway import broadcast, dispatch_command, now_iso

router = APIRouter(tags=["sim"])

SIM_FAULT_MAGIC = 0x5A5A
SIM_HOLDING_CODE = 45
SIM_HOLDING_DURATION = 46
SIM_HOLDING_MAGIC = 47
COIL_CLEAR_FAULTS = 8
COIL_APPLY_TARGET = 0
COIL_FACTORY_RESET = 7

FAULTS: dict[str, dict[str, Any]] = {
    "led_overtemp_warn": {
        "code": 1,
        "label": "Overheating (warning)",
        "severity": "warning",
        "effect": "derate",
    },
    "led_overtemp": {
        "code": 2,
        "label": "Overheating (critical)",
        "severity": "critical",
        "effect": "trip",
    },
    "overcurrent": {
        "code": 3,
        "label": "Overcurrent",
        "severity": "critical",
        "effect": "trip",
    },
    "led_open": {
        "code": 4,
        "label": "LED circuit open",
        "severity": "critical",
        "effect": "trip",
    },
    "config_corrupt": {
        "code": 5,
        "label": "Corrupt configuration (safe mode)",
        "severity": "critical",
        "effect": "safe_mode",
    },
    "heartbeat_lost": {
        "code": 7,
        "label": "Heartbeat lost",
        "severity": "warning",
        "effect": "off",
    },
    "ws_drop": {
        "code": None,
        "label": "WebSocket drop",
        "severity": "warning",
        "effect": "network",
    },
}

REGISTERS: dict[str, list[dict[str, Any]]] = {
    "holding": [
        {"addr": 0, "name": "TARGET_POWER"},
        {"addr": 1, "name": "TARGET_MODE"},
        {"addr": 2, "name": "TARGET_HUE"},
        {"addr": 3, "name": "TARGET_SAT"},
        {"addr": 4, "name": "TARGET_VAL"},
        {"addr": 5, "name": "TARGET_CCT"},
        {"addr": 6, "name": "TARGET_BRIGHTNESS"},
        {"addr": 7, "name": "TARGET_TRANSITION_MS"},
        {"addr": 10, "name": "SEQ_ID"},
        {"addr": 11, "name": "SEQ_SPEED"},
        {"addr": 12, "name": "SEQ_DIRECTION"},
        {"addr": 13, "name": "SEQ_LOOP_COUNT"},
        {"addr": 30, "name": "CFG_MAX_BRIGHTNESS"},
        {"addr": 34, "name": "CFG_HEARTBEAT_TIMEOUT_S"},
        {"addr": 35, "name": "CFG_OVERTEMP_WARN_C"},
        {"addr": 36, "name": "CFG_OVERTEMP_CRIT_C"},
        {"addr": 37, "name": "CFG_MAX_CURRENT_MA"},
        {"addr": 40, "name": "HEARTBEAT_COUNTER"},
        {"addr": 45, "name": "SIM_FAULT_CODE"},
        {"addr": 46, "name": "SIM_FAULT_DURATION_S"},
        {"addr": 47, "name": "SIM_FAULT_MAGIC"},
    ],
    "input": [
        {"addr": 0, "name": "ACTUAL_POWER"},
        {"addr": 1, "name": "ACTUAL_MODE"},
        {"addr": 2, "name": "ACTUAL_HUE"},
        {"addr": 3, "name": "ACTUAL_SAT"},
        {"addr": 4, "name": "ACTUAL_VAL"},
        {"addr": 5, "name": "ACTUAL_CCT"},
        {"addr": 6, "name": "ACTUAL_BRIGHTNESS"},
        {"addr": 10, "name": "DIAG_TEMP_C_X10"},
        {"addr": 11, "name": "DIAG_CURRENT_MA"},
        {"addr": 13, "name": "DIAG_POWER_MW"},
        {"addr": 14, "name": "DIAG_FAULT_CODE"},
        {"addr": 16, "name": "DIAG_UPTIME_S_LO"},
        {"addr": 17, "name": "DIAG_UPTIME_S_HI"},
        {"addr": 20, "name": "DIAG_FW_MAJOR"},
        {"addr": 21, "name": "DIAG_FW_MINOR"},
        {"addr": 22, "name": "DIAG_FW_PATCH"},
    ],
    "coil": [
        {"addr": 0, "name": "CMD_APPLY_TARGET"},
        {"addr": 1, "name": "CMD_POWER_ON"},
        {"addr": 2, "name": "CMD_POWER_OFF"},
        {"addr": 3, "name": "CMD_START_SEQUENCE"},
        {"addr": 4, "name": "CMD_STOP_SEQUENCE"},
        {"addr": 8, "name": "CMD_CLEAR_FAULTS"},
        {"addr": 9, "name": "CMD_SELFTEST"},
    ],
    "discrete": [
        {"addr": 0, "name": "STATUS_POWER_ON"},
        {"addr": 1, "name": "STATUS_SEQUENCE_RUNNING"},
        {"addr": 3, "name": "STATUS_FAULT_ACTIVE"},
        {"addr": 4, "name": "STATUS_OVERTEMP_WARNING"},
        {"addr": 5, "name": "STATUS_OVERTEMP_CRITICAL"},
        {"addr": 6, "name": "STATUS_OVERCURRENT"},
        {"addr": 8, "name": "STATUS_HEARTBEAT_LOST"},
        {"addr": 11, "name": "STATUS_FAULT_LATCHED"},
        {"addr": 12, "name": "STATUS_CONFIG_SAFE_MODE"},
    ],
}


def modbus(instance: Instance | None = None) -> ModbusClient:
    """Modbus client for the instance's driver (or the local compose one)."""
    host = settings.MODBUS_HOST
    if instance is not None and instance.supervisor_host:
        host = instance.supervisor_host
    return ModbusClient(host, settings.MODBUS_PORT, settings.MODBUS_UNIT_ID)


def _run_modbus(func, *args):
    try:
        return func(*args)
    except ModbusError as exc:
        raise ApiError(502, "MODBUS_ERROR", str(exc)) from exc


@router.post("/instances/{instance_id}/sim/session")
async def sim_session(
    instance_id: str,
    request: SimSessionRequest,
    _limit: None = Depends(limit_sim_pin),
) -> dict:
    """Validates the PIN and opens a simulation session for the instance."""
    instance = store.get(instance_id)
    if instance is None:
        raise ApiError(
            404, "INSTANCE_NOT_FOUND", f"Instance {instance_id} does not exist"
        )
    if not secrets.compare_digest(request.pin, settings.SIM_PIN):
        raise ApiError(403, "INVALID_PIN", "incorrect PIN")
    return {"sim_token": issue(instance.id, "sim")}


@router.get("/instances/{instance_id}/sim/registers")
async def sim_registers(
    session: Session = Depends(require_sim_session),
) -> dict:
    del session
    return {"registers": REGISTERS, "faults": FAULTS}


@router.get("/instances/{instance_id}/sim/modbus/read")
async def sim_modbus_read(
    instance_id: str,
    kind: str = Query("holding", pattern="^(holding|input|coil|discrete)$"),
    addr: int = Query(0, ge=0, le=65535),
    count: int = Query(1, ge=1, le=125),
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    client = modbus(session.instance)
    if kind == "holding":
        values = _run_modbus(client.read_holding, addr, count)
    elif kind == "input":
        values = _run_modbus(client.read_input, addr, count)
    elif kind == "coil":
        values = _run_modbus(client.read_coils, addr, count)
    else:
        values = _run_modbus(client.read_discrete, addr, count)
    return {"kind": kind, "addr": addr, "values": values}


@router.get("/instances/{instance_id}/sim/modbus/snapshot")
async def sim_modbus_snapshot(
    instance_id: str,
    session: Session = Depends(require_sim_session),
) -> dict:
    """Reads the map blocks over a single connection (Sim Panel)."""
    del instance_id
    client = modbus(session.instance)
    holding_max = max(row["addr"] for row in REGISTERS["holding"]) + 1
    input_max = max(row["addr"] for row in REGISTERS["input"]) + 1
    coil_max = max(row["addr"] for row in REGISTERS["coil"]) + 1
    discrete_max = max(row["addr"] for row in REGISTERS["discrete"]) + 1
    return {
        "holding": _run_modbus(client.read_holding, 0, holding_max),
        "input": _run_modbus(client.read_input, 0, input_max),
        "coil": _run_modbus(client.read_coils, 0, coil_max),
        "discrete": _run_modbus(client.read_discrete, 0, discrete_max),
    }


@router.post("/instances/{instance_id}/sim/modbus/write")
async def sim_modbus_write(
    instance_id: str,
    request: SimModbusWrite,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    client = modbus(session.instance)
    if request.kind == "coil":
        _run_modbus(client.write_coil, request.addr, 1 if request.value else 0)
        return {"ok": True}
    if request.kind != "holding":
        raise ApiError(400, "INVALID_KIND", "holding or coil only")
    _run_modbus(client.write_register, request.addr, request.value)
    if request.apply and 0 <= request.addr <= 8:
        _run_modbus(client.write_coil, COIL_APPLY_TARGET, 1)
    return {"ok": True}


@router.post("/instances/{instance_id}/sim/fault")
async def sim_fault(
    instance_id: str,
    request: SimFaultRequest,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    instance = session.instance
    fault = FAULTS.get(request.type)
    if fault is None:
        raise ApiError(400, "INVALID_FAULT", f"unknown fault: {request.type}")

    if request.type == "ws_drop":
        if instance.device is not None:
            try:
                await instance.device.close(code=1012)
            except Exception:
                pass
        result = {"type": request.type, "code": None, "duration_s": 0}
    else:
        client = modbus(instance)
        _run_modbus(
            client.write_registers,
            SIM_HOLDING_CODE,
            [int(fault["code"]), request.duration_s, SIM_FAULT_MAGIC],
        )
        result = {
            "type": request.type,
            "code": fault["code"],
            "duration_s": request.duration_s,
        }

    event = {
        "type": "event",
        "op": "sim_fault",
        "payload": result,
        "ts": now_iso(),
    }
    instance.events.append(event)
    await broadcast(instance, event)
    return {"ok": True, **result}


FAULT_EVENT_OPS = {
    "fault",
    "sim_fault",
    "sim_faults_cleared",
    "fault_history_cleared",
}


def _clear_fault_events(instance: Instance) -> int:
    """Removes only fault events from the history, not the rest of the log."""
    before = len(instance.events)
    instance.events = [
        event
        for event in instance.events
        if event.get("op") not in FAULT_EVENT_OPS
    ]
    return before - len(instance.events)


def _clear_faults(instance: Instance) -> None:
    client = modbus(instance)
    _run_modbus(client.write_registers, SIM_HOLDING_CODE, [0, 0, 0])
    _run_modbus(client.write_coil, COIL_CLEAR_FAULTS, 1)


async def _clear_faults_response(instance: Instance) -> dict:
    _clear_faults(instance)
    event = {
        "type": "event",
        "op": "sim_faults_cleared",
        "payload": {},
        "ts": now_iso(),
    }
    instance.events.append(event)
    await broadcast(instance, event)
    return {"ok": True}


@router.post("/instances/{instance_id}/sim/faults/clear")
async def sim_clear_faults(
    instance_id: str,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    return await _clear_faults_response(session.instance)


@router.post("/instances/{instance_id}/faults/clear")
async def clear_faults(
    instance_id: str,
    session: Session = Depends(require_session),
) -> dict:
    del instance_id
    return await _clear_faults_response(session.instance)


@router.delete("/instances/{instance_id}/faults/history")
async def clear_fault_history(
    instance_id: str,
    session: Session = Depends(require_session),
) -> dict:
    """Deletes the instance's fault history (not the full log)."""
    del instance_id
    instance = session.instance
    removed = _clear_fault_events(instance)
    event = {
        "type": "event",
        "op": "fault_history_cleared",
        "payload": {"removed": removed},
        "ts": now_iso(),
    }
    instance.events.append(event)
    await broadcast(instance, event)
    return {"ok": True, "removed": removed}


@router.get("/instances/{instance_id}/sim/logs")
async def sim_logs(
    instance_id: str,
    limit: int = Query(100, ge=1, le=500),
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    instance = session.instance
    return {"logs": instance.events[-limit:]}


@router.post("/instances/{instance_id}/sim/reboot")
async def sim_reboot(
    instance_id: str,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    instance = session.instance
    return await dispatch_command(
        instance,
        {
            "id": f"sim-{uuid.uuid4().hex[:8]}",
            "type": "command",
            "op": "sim_reboot",
            "payload": {},
        },
    )


POWER_COMMANDS = {
    "on": "POWER_ON",
    "off": "POWER_OFF",
    "reset": "RESET",
    "factory_reset": "FACTORY_RESET",
}


def supervisor_client(instance: Instance | None = None) -> SupervisorClient:
    host = settings.SUPERVISOR_HOST
    port = settings.SUPERVISOR_PORT
    if instance is not None and instance.supervisor_host:
        host = instance.supervisor_host
        port = 5022
    return SupervisorClient(host, port, settings.SUPERVISOR_TOKEN or None)


async def supervisor_command(
    command: str, instance: Instance | None = None
) -> dict:
    try:
        return await asyncio.to_thread(
            supervisor_client(instance).command, command
        )
    except SupervisorError as exc:
        raise ApiError(502, "SUPERVISOR_ERROR", str(exc)) from exc


@router.get("/instances/{instance_id}/sim/power")
async def sim_power_status(
    instance_id: str,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    return await supervisor_command("STATUS", session.instance)


@router.post("/instances/{instance_id}/sim/power")
async def sim_power(
    instance_id: str,
    request: SimPowerRequest,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    instance = session.instance
    command = POWER_COMMANDS.get(request.action)
    if command is None:
        raise ApiError(
            400, "INVALID_ACTION", f"unknown action: {request.action}"
        )
    if request.action == "factory_reset" and not request.confirm:
        raise ApiError(400, "CONFIRM_REQUIRED", "factory_reset requires confirm")

    result = await supervisor_command(command, instance)
    pairing_triggered = False
    if request.action == "off":
        # Explicit hard power off: do not auto-start it again until Power On.
        instance.power_explicitly_off = True
        instance.auto_power_on_done = True
    elif request.action == "on":
        instance.power_explicitly_off = False
        instance.auto_power_on_done = True
    elif request.action == "factory_reset":
        instance.power_explicitly_off = True
        instance.auto_power_on_done = True
    if request.action in ("on", "off"):
        if provisioning.should_cancel_pairing(instance, request.action):
            # An off→on while in pairing exits the mode and keeps the network.
            instance.provisioning_last_off = 0.0
            instance.provisioning_cycles.clear()
            asyncio.create_task(provisioning.cancel_pairing(instance))
        elif provisioning.note_power(
            instance, request.action, time.monotonic()
        ):
            pairing_triggered = True
            asyncio.create_task(
                provisioning.start_pairing(instance, source="power_cycle")
            )
    if (
        request.action == "on"
        and not pairing_triggered
        and instance.provisioning_state
        not in (provisioning.PAIRING, provisioning.CONNECTING)
    ):
        asyncio.create_task(power_on_service.apply_after_power_on(instance))
    if request.action == "factory_reset":
        # Also clears the driver's persisted state (driver_state.bin).
        try:
            await asyncio.to_thread(
                modbus(instance).write_coil, COIL_FACTORY_RESET, 1
            )
        except ModbusError:
            pass
        # The factory reset also leaves the panel and the instance
        # configuration as new: schedules, power-on policy, and history.
        instance.schedules = []
        schedule_store.delete_all(instance.device_token)
        power_on_service.reset(instance)
        instance.last_health = None
        instance.health_ts = None
        instance.last_state = None
        instance.state_ts = None
        _clear_fault_events(instance)
    event = {
        "type": "event",
        "op": "sim_power",
        "payload": {"action": request.action, "state": result.get("state")},
        "ts": now_iso(),
    }
    instance.events.append(event)
    await broadcast(instance, event)
    return {"ok": True, **result}


@router.post("/instances/{instance_id}/sim/bootloader")
async def sim_bootloader(
    instance_id: str,
    request: SimBootloaderRequest,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    instance = session.instance
    if request.action == "enter":
        command = "ENTER_BOOTLOADER"
    elif request.action == "exit":
        command = "POWER_ON"
    else:
        raise ApiError(
            400, "INVALID_ACTION", f"unknown action: {request.action}"
        )

    result = await supervisor_command(command, instance)
    event = {
        "type": "event",
        "op": "sim_bootloader",
        "payload": {"action": request.action, "state": result.get("state")},
        "ts": now_iso(),
    }
    instance.events.append(event)
    await broadcast(instance, event)
    return {"ok": True, **result}
