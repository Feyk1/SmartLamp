"""Upload and start of user sequences (IDs 100-255)."""

from __future__ import annotations

import asyncio
import uuid

from fastapi import APIRouter, Depends

from src import settings
from src.api.errors import ApiError
from src.auth.session import Session, require_session
from src.schemas import CreateSequenceRequest
from src.sequences.crc import sequence_crc
from src.sim.modbus import ModbusClient, ModbusError
from src.store.memory import Instance
from src.ws.gateway import dispatch_command

router = APIRouter(tags=["sequences"])

# Modbus map (docs/03): the upload writes 8 registers per step.
HOLDING_SEQ_TOTAL_STEPS = 15
HOLDING_SEQ_STEP_INDEX = 14
HOLDING_SEQ_STEP_HUE = 16
HOLDING_SEQ_CRC = 24

STEP_FIELDS = 8
SEQUENCE_ID = 100


def modbus_for(instance: Instance) -> ModbusClient:
    """Modbus client for the instance's driver (compose or 6PN)."""
    host = settings.MODBUS_HOST
    if instance.supervisor_host:
        host = instance.supervisor_host
    return ModbusClient(host, settings.MODBUS_PORT, settings.MODBUS_UNIT_ID)


def build_registers(request: CreateSequenceRequest) -> list[int]:
    """Serializes the steps into the protocol's 8 registers."""
    registers: list[int] = []
    for step in request.steps:
        duration = max(1, (step.duration_ms + 5) // 10)
        transition = max(0, (step.transition_ms + 5) // 10)
        registers.extend(
            [
                step.h,
                step.s,
                step.v,
                0,
                step.brightness,
                duration,
                transition,
                step.cct,
            ]
        )
    return registers


@router.post("/instances/{instance_id}/sequences", status_code=202)
async def upload_sequence(
    instance_id: str,
    request: CreateSequenceRequest,
    session: Session = Depends(require_session),
) -> dict:
    """Uploads the steps to the driver and starts the sequence in the firmware."""
    del instance_id
    instance = session.instance
    registers = build_registers(request)
    crc = sequence_crc(registers)

    def upload() -> None:
        client = modbus_for(instance)
        client.write_register(HOLDING_SEQ_TOTAL_STEPS, len(request.steps))
        for index in range(len(request.steps)):
            client.write_register(HOLDING_SEQ_STEP_INDEX, index)
            client.write_registers(
                HOLDING_SEQ_STEP_HUE,
                registers[index * STEP_FIELDS : (index + 1) * STEP_FIELDS],
            )
        client.write_register(HOLDING_SEQ_CRC, crc)

    try:
        await asyncio.to_thread(upload)
    except ModbusError as exc:
        raise ApiError(502, "MODBUS_ERROR", str(exc)) from exc

    ack = await dispatch_command(
        instance,
        {
            "id": f"seq-{uuid.uuid4().hex[:8]}",
            "type": "command",
            "op": "start_sequence",
            "payload": {
                "id": SEQUENCE_ID,
                "speed": request.speed,
                "direction": request.direction,
                "loop": request.loop,
            },
        },
    )
    return {
        "ok": True,
        "steps": len(request.steps),
        "sequence_id": SEQUENCE_ID,
        "crc": crc,
        "ack": ack,
    }
