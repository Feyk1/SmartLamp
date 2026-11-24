"""In-memory rate limiting (fixed window) for public endpoints."""

from __future__ import annotations

import math
import threading
import time

from fastapi import Request

from src import settings
from src.api.errors import ApiError


class RateLimiter:
    def __init__(self) -> None:
        self._windows: dict[str, tuple[float, int]] = {}
        self._lock = threading.Lock()

    def reset(self) -> None:
        with self._lock:
            self._windows.clear()

    def check(self, key: str, limit: int, window_s: float = 1.0) -> float | None:
        """Counts one request; returns `Retry-After` if the limit is exceeded."""
        now = time.monotonic()
        with self._lock:
            start, count = self._windows.get(key, (now, 0))
            if (now - start) >= window_s:
                start, count = now, 0
            count += 1
            self._windows[key] = (start, count)
            if len(self._windows) > 10000:
                self._windows = {
                    k: v
                    for k, v in self._windows.items()
                    if (now - v[0]) < window_s
                }
            if count > limit:
                return max(0.0, window_s - (now - start))
        return None


limiter = RateLimiter()


def _enforce(key: str, limit: int) -> None:
    retry = limiter.check(key, limit)
    if retry is not None:
        raise ApiError(
            429,
            "RATE_LIMITED",
            "too many requests; retry later",
            headers={"Retry-After": str(max(1, math.ceil(retry)))},
        )


def limit_device(token: str) -> None:
    """Limit per `device_token` (telemetry, commands, OTA)."""
    _enforce(f"device:{token}", settings.DEVICE_RATE_LIMIT)


def limit_instance_creation(request: Request) -> None:
    """Limit per IP for `POST /instances`."""
    client = request.client.host if request.client else "unknown"
    _enforce(f"ip:{client}", settings.INSTANCE_RATE_LIMIT)


def limit_sim_pin(request: Request) -> None:
    """PIN attempt limit per IP (anti brute force)."""
    client = request.client.host if request.client else "unknown"
    retry = limiter.check(
        f"simip:{client}",
        settings.SIM_PIN_RATE_LIMIT,
        settings.SIM_PIN_RATE_WINDOW_S,
    )
    if retry is not None:
        raise ApiError(
            429,
            "RATE_LIMITED",
            "too many PIN attempts; retry later",
            headers={"Retry-After": str(max(1, math.ceil(retry)))},
        )
