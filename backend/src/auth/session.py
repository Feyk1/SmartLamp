"""JWT session dependencies for the dashboard endpoints."""

from __future__ import annotations

from dataclasses import dataclass

from fastapi import Header

from src.api.errors import ApiError
from src.auth.tokens import TokenError, verify
from src.store.memory import Instance, store


@dataclass
class Session:
    instance: Instance
    role: str


def _extract_token(
    authorization: str | None, x_session_token: str | None
) -> str:
    if x_session_token:
        return x_session_token
    if authorization and authorization.lower().startswith("bearer "):
        return authorization[7:].strip()
    return ""


def require_session(
    instance_id: str,
    authorization: str | None = Header(default=None),
    x_session_token: str | None = Header(default=None),
) -> Session:
    """Validates the JWT and that it belongs to `instance_id`."""
    token = _extract_token(authorization, x_session_token)
    if not token:
        raise ApiError(401, "UNAUTHORIZED", "session required")
    try:
        claims = verify(token)
    except TokenError as exc:
        raise ApiError(401, "UNAUTHORIZED", str(exc)) from exc

    if claims.get("instance_id") != instance_id:
        raise ApiError(403, "FORBIDDEN", "token belongs to another instance")
    instance = store.get(instance_id)
    if instance is None:
        raise ApiError(
            404, "INSTANCE_NOT_FOUND", f"Instance {instance_id} does not exist"
        )
    return Session(instance=instance, role=str(claims["role"]))


def require_sim_session(
    instance_id: str,
    authorization: str | None = Header(default=None),
    x_session_token: str | None = Header(default=None),
) -> Session:
    """Like `require_session`, but requires `role: sim` (Simulation Panel)."""
    session = require_session(instance_id, authorization, x_session_token)
    if session.role != "sim":
        raise ApiError(403, "SIM_ONLY", "sim role required")
    return session
