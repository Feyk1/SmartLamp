"""REST endpoints for instances."""

from __future__ import annotations

import secrets
import uuid
from datetime import datetime, timezone
from urllib.parse import urlparse

from fastapi import APIRouter, Depends, Request, Response

from src import settings
from src.api.errors import ApiError, error_response
from src.auth.session import Session, require_session
from src.auth.tokens import issue
from src.auth.ws_tickets import issue as issue_ws_ticket
from src.orchestrator import manager as orchestrator
from src.orchestrator.machines_api import MachinesApiError
from src.ota.router import latest_artifact, parse_version
from src.power_on import service as power_on_service
from src.provisioning import service as provisioning_service
from src.schedules.storage import schedule_store
from src.schemas import (
    CreateInstanceRequest,
    InstanceInfo,
    InstanceResponse,
    OtaTriggerRequest,
)
from src.security.rate_limit import limit_instance_creation
from src.store.memory import iso, store
from src.ws.gateway import dispatch_command

router = APIRouter(prefix="/instances", tags=["instances"])

LOCAL_HOSTS = {"localhost", "127.0.0.1", "::1", "0.0.0.0"}


def public_ws_base(http_request: Request) -> str:
    """Returns the public WS, deriving it from the Host if the config is local."""
    configured = settings.PUBLIC_WS_BASE.rstrip("/")
    hostname = urlparse(configured).hostname
    if configured and hostname not in LOCAL_HOSTS:
        return configured
    proto = (
        http_request.headers.get("x-forwarded-proto")
        or http_request.url.scheme
    ).split(",")[0].strip()
    host = (
        http_request.headers.get("x-forwarded-host")
        or http_request.headers.get("host")
        or http_request.url.netloc
    ).split(",")[0].strip()
    scheme = "wss" if proto == "https" else "ws"
    return f"{scheme}://{host}"


@router.post("", status_code=201, response_model=InstanceResponse)
async def create_instance(
    request: CreateInstanceRequest,
    http_request: Request,
    _limit: None = Depends(limit_instance_creation),
):
    instance = None
    # On Fly every visit has its own Machine and its own `device_token`;
    # reusing the local instance would break isolation.
    if settings.REUSE_DEVICE_INSTANCE and settings.INSTANCE_MODE != "fly":
        instance = store.find_by_device_token(settings.DEVICE_TOKEN)
    if instance is None:
        device_token = (
            f"dev-{secrets.token_urlsafe(24)}"
            if settings.INSTANCE_MODE == "fly"
            else settings.DEVICE_TOKEN
        )
        instance = store.create(ttl_s=request.ttl_s, device_token=device_token)

    # Schedules persist by device_token and are reattached to the device.
    if not instance.schedules:
        schedule_store.attach(instance)
    provisioning_service.attach(instance)
    power_on_service.attach(instance)

    if settings.INSTANCE_MODE == "fly" and instance.mode != "fly":
        try:
            await orchestrator.provision(instance)
        except MachinesApiError as exc:
            await store.delete(instance.id)
            raise ApiError(
                503,
                "ORCHESTRATOR_UNAVAILABLE",
                f"could not create instance: {exc}",
            ) from exc

    # Session JWT (ADR-010); it is rotated on every POST so as not to hand out
    # an expired one when the instance is reused.
    token = issue(instance.id, "user")
    instance.session_token = token
    ws_url = (
        f"{public_ws_base(http_request)}/ws/dashboard"
        f"?instance={instance.id}"
    )
    return InstanceResponse(
        id=instance.id,
        created_at=iso(instance.created_at),
        ttl_s=instance.ttl_s,
        ws_dashboard_url=ws_url,
        session_token=token,
        firmware_versions=settings.FIRMWARE_VERSIONS,
    )


@router.post("/{instance_id}/ws-ticket")
async def ws_ticket(
    instance_id: str, session: Session = Depends(require_session)
) -> dict:
    del instance_id
    return {
        "ticket": issue_ws_ticket(session.instance.id, session.role),
        "expires_in": settings.WS_TICKET_TTL_S,
    }


@router.get("/{instance_id}", response_model=InstanceInfo)
async def get_instance(
    instance_id: str, session: Session = Depends(require_session)
):
    del instance_id
    instance = session.instance
    heartbeat = datetime.fromtimestamp(instance.last_heartbeat, timezone.utc)
    health = instance.last_health or {}
    return InstanceInfo(
        id=instance.id,
        created_at=iso(instance.created_at),
        ttl_s=instance.ttl_s,
        last_heartbeat=iso(heartbeat),
        state=instance.last_state,
        health=instance.last_health,
        health_ts=instance.health_ts,
        queued_commands=len(instance.command_queue),
        device_online=instance.device is not None,
        serial=health.get("serial"),
    )


@router.delete("/{instance_id}", status_code=204)
async def delete_instance(
    instance_id: str, session: Session = Depends(require_session)
):
    deleted = await store.delete(instance_id)
    if not deleted:
        return error_response(
            404,
            "INSTANCE_NOT_FOUND",
            f"Instance {instance_id} does not exist or expired",
        )
    await orchestrator.destroy(session.instance)
    return Response(status_code=204)


@router.post("/{instance_id}/heartbeat")
async def heartbeat(
    instance_id: str, session: Session = Depends(require_session)
):
    del instance_id
    instance = store.touch(session.instance.id)
    if instance is None:
        return error_response(404, "INSTANCE_NOT_FOUND", "instance expired")
    expires_at = datetime.fromtimestamp(
        instance.last_heartbeat + instance.ttl_s, timezone.utc
    )
    return {"ok": True, "expires_at": iso(expires_at)}


@router.get("/{instance_id}/state")
async def get_state(
    instance_id: str, session: Session = Depends(require_session)
):
    del instance_id
    instance = session.instance
    return {"state": instance.last_state, "ts": instance.state_ts}


@router.post("/{instance_id}/ota", status_code=202)
async def trigger_ota(
    instance_id: str,
    request: OtaTriggerRequest,
    session: Session = Depends(require_session),
):
    """Deploys an `.ota` to the instance (`ota_update` command to the device)."""
    del instance_id
    instance = session.instance

    mode = request.mode
    if mode not in ("legit", "corrupt", "downgrade"):
        return error_response(
            400, "INVALID_MODE", f"unknown OTA mode: {mode}"
        )

    version = request.version
    if version is None:
        artifact = latest_artifact()
        if artifact is None:
            return error_response(
                409, "OTA_UNAVAILABLE", "no .ota artifacts configured"
            )
        version = artifact.version_str

    current_version = (instance.last_health or {}).get("fw_version")
    if current_version and version:
        try:
            current = parse_version(str(current_version))
            target = parse_version(version)
        except ValueError:
            current = None
            target = None
        if current is not None and target is not None:
            if mode == "legit" and target <= current:
                return error_response(
                    409,
                    "OTA_NOT_NEWER",
                    f"v{version} is not newer than v{current_version}; "
                    "choose a newer artifact",
                )
            if mode == "downgrade" and target >= current:
                return error_response(
                    409,
                    "OTA_NOT_OLDER",
                    f"v{version} is not older than v{current_version}; "
                    "choose an older artifact",
                )

    instance.ota_mode = mode
    instance.ota_version = version
    return await dispatch_command(
        instance,
        {
            "id": f"ota-{uuid.uuid4().hex[:8]}",
            "type": "command",
            "op": "ota_update",
            "payload": {"version": version, "mode": mode},
        },
    )
