"""REST endpoints for the simulated onboarding."""

from __future__ import annotations

from fastapi import APIRouter, Depends
from pydantic import BaseModel, Field

from src.api.errors import ApiError
from src.auth.session import Session, require_session, require_sim_session
from src.provisioning import service

router = APIRouter(tags=["provisioning"])


class ConnectRequest(BaseModel):
    ssid: str = Field(min_length=1, max_length=64)
    # No length constraints: prevents a 422 from including the password.
    password: str


class PolicyRequest(BaseModel):
    networks: list[str] = Field(default_factory=list, max_length=16)
    passwords: dict[str, str] | None = None
    allow_any: bool = False
    failure: str = "none"


@router.get("/instances/{instance_id}/provisioning")
async def provisioning_info(
    instance_id: str, session: Session = Depends(require_session)
) -> dict:
    del instance_id
    return service.info(session.instance)


@router.get("/instances/{instance_id}/provisioning/policy")
async def get_policy(
    instance_id: str, session: Session = Depends(require_session)
) -> dict:
    del instance_id
    return service.policy(session.instance)


@router.put("/instances/{instance_id}/provisioning/policy")
async def put_policy(
    instance_id: str,
    request: PolicyRequest,
    session: Session = Depends(require_sim_session),
) -> dict:
    del instance_id
    try:
        return service.update_policy(
            session.instance,
            request.networks,
            request.allow_any,
            request.failure,
            request.passwords,
        )
    except service.ProvisioningError as exc:
        raise ApiError(400, exc.code, exc.message) from exc


@router.post("/instances/{instance_id}/provisioning/start")
async def start(
    instance_id: str, session: Session = Depends(require_sim_session)
) -> dict:
    del instance_id
    return await service.start_pairing(session.instance, source="manual")


@router.get("/instances/{instance_id}/provisioning/scan")
async def scan(
    instance_id: str, session: Session = Depends(require_session)
) -> dict:
    del instance_id
    instance = session.instance
    if instance.provisioning_state != service.PAIRING:
        return {"devices": []}
    return {
        "devices": [
            {
                "id": instance.id,
                "name": service.device_name(instance),
                "rssi": -42,
            }
        ]
    }


@router.post("/instances/{instance_id}/provisioning/connect")
async def connect(
    instance_id: str,
    request: ConnectRequest,
    session: Session = Depends(require_session),
) -> dict:
    del instance_id
    if not request.password:
        raise ApiError(400, "INVALID_WIFI", "password is required")
    try:
        return await service.connect(
            session.instance, request.ssid, request.password
        )
    except service.ProvisioningError as exc:
        status = {
            "UNKNOWN_NETWORK": 400,
            "INVALID_NETWORK": 400,
            "INVALID_PASSWORD": 403,
            "WIFI_TIMEOUT": 408,
            "INVALID_STATE": 409,
        }.get(exc.code, 400)
        raise ApiError(status, exc.code, exc.message) from exc


@router.post("/instances/{instance_id}/provisioning/cancel")
async def cancel(
    instance_id: str, session: Session = Depends(require_sim_session)
) -> dict:
    del instance_id
    return await service.cancel_pairing(session.instance)


@router.post("/instances/{instance_id}/provisioning/reset")
async def reset(
    instance_id: str, session: Session = Depends(require_sim_session)
) -> dict:
    del instance_id
    return await service.reset(session.instance)
