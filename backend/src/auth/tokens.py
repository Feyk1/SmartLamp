"""Minimal HS256 JWT for dashboard sessions (no dependencies).

ADR-010: JWT for the dashboard and an opaque token for the device. The token
carries `instance_id` and `role` (`user` or `sim`) and expires per `JWT_TTL_S`.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import time
from typing import Any

from src import settings


class TokenError(Exception):
    """Token missing, malformed, expired, or with an invalid signature/role."""


def _b64e(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64d(text: str) -> bytes:
    padding = "=" * (-len(text) % 4)
    return base64.urlsafe_b64decode(text + padding)


def _sign(message: str) -> bytes:
    return hmac.new(
        settings.JWT_SECRET.encode("utf-8"),
        message.encode("ascii"),
        hashlib.sha256,
    ).digest()


def issue(instance_id: str, role: str = "user", ttl_s: int | None = None) -> str:
    """Issues a signed JWT for an instance and role."""
    now = int(time.time())
    header = {"alg": "HS256", "typ": "JWT"}
    payload = {
        "sub": f"{instance_id}:{role}",
        "instance_id": instance_id,
        "role": role,
        "iat": now,
        "exp": now + (ttl_s if ttl_s is not None else settings.JWT_TTL_S),
    }
    head = _b64e(json.dumps(header, separators=(",", ":")).encode("utf-8"))
    body = _b64e(json.dumps(payload, separators=(",", ":")).encode("utf-8"))
    signature = _b64e(_sign(f"{head}.{body}"))
    return f"{head}.{body}.{signature}"


def verify(token: str) -> dict[str, Any]:
    """Verifies signature, expiration, and shape; returns the claims."""
    if not token:
        raise TokenError("empty token")
    parts = token.split(".")
    if len(parts) != 3:
        raise TokenError("malformed token")
    head, body, signature = parts
    try:
        expected = _sign(f"{head}.{body}")
        provided = _b64d(signature)
    except (ValueError, UnicodeError) as exc:
        raise TokenError("malformed token") from exc
    if not hmac.compare_digest(expected, provided):
        raise TokenError("invalid signature")

    try:
        header = json.loads(_b64d(head))
        payload = json.loads(_b64d(body))
    except (ValueError, UnicodeError) as exc:
        raise TokenError("invalid payload") from exc
    if not isinstance(header, dict) or header.get("alg") != "HS256":
        raise TokenError("unsupported algorithm")
    if not isinstance(payload, dict):
        raise TokenError("invalid payload")
    if int(payload.get("exp", 0)) < int(time.time()):
        raise TokenError("expired token")
    if payload.get("role") not in ("user", "sim"):
        raise TokenError("invalid role")
    if not isinstance(payload.get("instance_id"), str):
        raise TokenError("missing instance_id")
    return payload
