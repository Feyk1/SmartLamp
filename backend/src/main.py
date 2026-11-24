"""FastAPI orchestrator entry point."""

from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from src import settings
from src.api import errors
from src.api.instances import router as instances_router
from src.commands.router import router as commands_router
from src.orchestrator import manager as orchestrator
from src.ota.router import router as ota_router
from src.power_on import service as power_on_service
from src.power_on.router import router as power_on_router
from src.provisioning import service as provisioning_service
from src.provisioning.router import router as provisioning_router
from src.schedules.router import router as schedules_router
from src.schedules.runner import schedules_loop as run_schedules_loop
from src.schedules.storage import schedule_store
from src.sequences.router import router as sequences_router
from src.sim.router import router as sim_router
from src.store.memory import store
from src.telemetry.router import router as telemetry_router
from src.ws.gateway import router as ws_router


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Creates the local demo instance on startup and cleans up ephemeral ones.

    With REUSE_DEVICE_INSTANCE, the firmware (static token) can connect
    even if the dashboard has not been opened yet; the browser POST
    reuses this same instance.
    """
    if settings.REUSE_DEVICE_INSTANCE:
        instance = store.create(
            ttl_s=settings.INSTANCE_TTL_S, device_token=settings.DEVICE_TOKEN
        )
        schedule_store.attach(instance)
        provisioning_service.attach(instance)
        power_on_service.attach(instance)

    cleanup = None
    if settings.INSTANCE_MODE == "fly":
        cleanup = asyncio.create_task(orchestrator.cleanup_loop())
    schedules = asyncio.create_task(run_schedules_loop())
    try:
        yield
    finally:
        for task in (cleanup, schedules):
            if task is not None:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass


app = FastAPI(title="Smart Lamp Orchestrator", version="0.1.0", lifespan=lifespan)

# Configurable CORS; locally, "*" for the dashboard.
app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.DASHBOARD_ORIGINS or ["*"],
    allow_methods=["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"],
    allow_headers=["Authorization", "Content-Type", "X-Session-Token",
                   "X-Sim-Token", "X-Device-Token"],
)


@app.middleware("http")
async def security_headers(request, call_next):
    response = await call_next(request)
    response.headers.setdefault("X-Content-Type-Options", "nosniff")
    response.headers.setdefault("Referrer-Policy", "no-referrer")
    response.headers.setdefault("X-Frame-Options", "DENY")
    if request.url.path.startswith("/api/"):
        response.headers.setdefault("Cache-Control", "no-store")
    return response

app.include_router(instances_router, prefix="/api/v1")
app.include_router(telemetry_router, prefix="/api/v1")
app.include_router(commands_router, prefix="/api/v1")
app.include_router(ota_router, prefix="/api/v1")
app.include_router(sequences_router, prefix="/api/v1")
app.include_router(schedules_router, prefix="/api/v1")
app.include_router(provisioning_router, prefix="/api/v1")
app.include_router(power_on_router, prefix="/api/v1")
app.include_router(sim_router, prefix="/api/v1")
app.include_router(ws_router)
errors.install(app)


@app.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}
