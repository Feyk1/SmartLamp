"""Power-on behavior endpoints."""

from __future__ import annotations

from fastapi import APIRouter, Depends
from pydantic import BaseModel

from src.api.errors import ApiError
from src.auth.session import Session, require_session
from src.power_on import service

router = APIRouter(tags=["power-on"])


class PowerOnRequest(BaseModel):
    mode: str
    state: dict | None = None


@router.get("/instances/{instance_id}/power-on")
async def get_power_on(
    instance_id: str, session: Session = Depends(require_session)
) -> dict:
    del instance_id
    return service.policy(session.instance)


@router.put("/instances/{instance_id}/power-on")
async def put_power_on(
    instance_id: str,
    request: PowerOnRequest,
    session: Session = Depends(require_session),
) -> dict:
    del instance_id
    try:
        return service.update(session.instance, request.mode, request.state)
    except service.PowerOnError as exc:
        raise ApiError(400, exc.code, exc.message) from exc
