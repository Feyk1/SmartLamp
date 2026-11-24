"""`.ota` artifacts: version check and download (docs/02 §4.2)."""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path

from fastapi import APIRouter, Depends, Response
from fastapi.responses import FileResponse

from src import settings
from src.api.device_auth import require_device_instance
from src.api.errors import ApiError
from src.store.memory import Instance

router = APIRouter(prefix="/ota", tags=["ota"])

MAGIC = b"LAMPOTA1"
HEADER_FMT = "<8sH6sII32s12s32s28s"
HEADER_SIZE = 128


@dataclass
class Artifact:
    path: Path
    version: tuple[int, int, int]
    size: int

    @property
    def version_str(self) -> str:
        return ".".join(str(part) for part in self.version)


def parse_version(text: str) -> tuple[int, int, int]:
    clean = text.strip().lstrip("vV")
    parts = clean.split(".")
    if len(parts) != 3:
        raise ValueError(f"invalid version: {text!r}")
    return tuple(int(part, 10) for part in parts)  # type: ignore[return-value]


def _artifact(path: Path) -> Artifact | None:
    try:
        with path.open("rb") as handle:
            header = handle.read(HEADER_SIZE)
    except OSError:
        return None
    if len(header) < HEADER_SIZE:
        return None
    fields = struct.unpack(HEADER_FMT, header)
    if fields[0] != MAGIC:
        return None
    version = struct.unpack("<HHH", fields[2])
    return Artifact(path=path, version=version, size=path.stat().st_size)


def artifacts() -> list[Artifact]:
    """Lists the valid `.ota` files in `OTA_DIR` (ignores corrupt files)."""
    base = Path(settings.OTA_DIR)
    if not base.is_dir():
        return []
    items = [item for item in (_artifact(p) for p in base.glob("*.ota")) if item]
    return sorted(items, key=lambda item: item.version)


def latest_artifact() -> Artifact | None:
    items = artifacts()
    return items[-1] if items else None


@router.get("/check")
async def check_ota(
    current: str = "",
    instance: Instance = Depends(require_device_instance),
) -> dict:
    """Returns the latest available version if it is greater than `current`."""
    del instance
    try:
        current_version = parse_version(current) if current else (0, 0, 0)
    except ValueError as exc:
        raise ApiError(400, "INVALID_VERSION", str(exc)) from exc

    candidates = [item for item in artifacts() if item.version > current_version]
    if not candidates:
        return {"available": False}

    latest = candidates[-1]
    data = latest.path.read_bytes()
    return {
        "available": True,
        "version": latest.version_str,
        "size": latest.size,
        "sha256": hashlib.sha256(data).hexdigest(),
        "url": f"/api/v1/ota/{latest.version_str}/download",
    }


@router.get("/latest")
async def latest_ota() -> dict:
    """Latest available version (for the dashboard; does not expose the binary)."""
    item = latest_artifact()
    return {"version": item.version_str if item else None}


@router.get("/versions")
async def list_versions() -> dict:
    """Available versions (Simulation Panel)."""
    return {
        "versions": [
            {
                "version": item.version_str,
                "size": item.size,
            }
            for item in reversed(artifacts())
        ]
    }


@router.get("/{version}/download")
async def download_ota(
    version: str,
    instance: Instance = Depends(require_device_instance),
) -> Response:
    """Serves the artifact for `version` (corrupt mode flips one byte)."""
    for item in artifacts():
        if item.version_str != version:
            continue
        if instance.ota_mode == "corrupt":
            data = bytearray(item.path.read_bytes())
            if len(data) > 200:
                data[200] ^= 0xFF
            return Response(
                content=bytes(data),
                media_type="application/octet-stream",
                headers={
                    "Content-Disposition": (
                        f'attachment; filename="{item.path.name}"'
                    )
                },
            )
        return FileResponse(
            item.path,
            media_type="application/octet-stream",
            filename=item.path.name,
        )
    raise ApiError(404, "OTA_NOT_FOUND", f"no artifact for {version}")
