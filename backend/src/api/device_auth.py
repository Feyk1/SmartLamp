"""Authorization for the device REST endpoints (`device_token`)."""

from __future__ import annotations

from fastapi import Header, Query

from src.api.errors import ApiError
from src.security.rate_limit import limit_device
from src.store.memory import Instance, store


def require_device_instance(
    token: str = Query(default=""),
    x_device_token: str | None = Header(default=None),
) -> Instance:
    """Resolves the device's active instance or raises `ApiError`.

    Accepts the token via query (`?token=`) or header (`X-Device-Token`).
    Touching the instance on every device request keeps it from expiring
    while the firmware is alive (local demo). See D38 in progress.md.
    """
    provided = x_device_token or token
    if not provided:
        raise ApiError(401, "UNAUTHORIZED", "missing device token")

    # Local mode uses a shared DEVICE_TOKEN; Fly mode generates a unique token
    # per instance. In both cases the active instance is the authority.
    instance = store.find_by_device_token(provided)
    if instance is None:
        raise ApiError(
            404, "INSTANCE_NOT_FOUND", "no active instance for this device"
        )
    limit_device(provided)
    return store.touch(instance.id) or instance
