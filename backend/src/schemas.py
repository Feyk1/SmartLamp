"""Pydantic models for the REST API."""


from pydantic import BaseModel, ConfigDict, Field


class CreateInstanceRequest(BaseModel):
    profile: str = "lamp-v1"
    ttl_s: int = Field(default=3600, ge=60, le=86400)


class InstanceResponse(BaseModel):
    id: str
    created_at: str
    ttl_s: int
    ws_dashboard_url: str
    session_token: str
    firmware_versions: list[str]


class InstanceInfo(BaseModel):
    id: str
    created_at: str
    ttl_s: int
    last_heartbeat: str
    state: dict | None = None
    health: dict | None = None
    health_ts: str | None = None
    queued_commands: int = 0
    device_online: bool = False
    serial: str | None = None


class TelemetryHealth(BaseModel):
    """`POST /telemetry/health` (docs/02 §4.1)."""

    model_config = ConfigDict(extra="allow")

    device_id: str | None = None
    ts: str | None = None
    uptime_s: int | None = None
    serial: str | None = None
    hw_rev: int | None = None
    fw_version: str | None = None
    slot: str | None = None
    mem: dict | None = None
    tasks: dict | None = None
    led_temp_c: float | None = None
    power_estimate_w: float | None = None
    rssi: int | None = None


class TelemetryState(BaseModel):
    """`POST /telemetry/state` (on-change + deadband)."""

    device_id: str | None = None
    ts: str | None = None
    state: dict
    previous: dict | None = None


class TelemetryEvents(BaseModel):
    """`POST /telemetry/events` (batch)."""

    device_id: str | None = None
    events: list[dict]


class QueuedCommands(BaseModel):
    commands: list[dict]


class OtaTriggerRequest(BaseModel):
    """`POST /instances/{id}/ota` (deployment trigger)."""

    version: str | None = None
    mode: str = "legit"


class SequenceStep(BaseModel):
    """A step of a user sequence."""

    h: int = Field(ge=0, le=360)
    s: int = Field(default=1000, ge=0, le=1000)
    v: int = Field(default=1000, ge=0, le=1000)
    brightness: int = Field(default=1000, ge=0, le=1000)
    cct: int = Field(default=2700, ge=2000, le=6500)
    duration_ms: int = Field(default=4000, ge=100, le=60000)
    transition_ms: int = Field(default=0, ge=0, le=10000)


class CreateSequenceRequest(BaseModel):
    """`POST /instances/{id}/sequences`."""

    steps: list[SequenceStep] = Field(min_length=1, max_length=8)
    speed: int = Field(default=1000, ge=100, le=4000)
    direction: int = Field(default=0, ge=0, le=2)
    loop: int = Field(default=0, ge=0, le=65535)


class ScheduleAction(BaseModel):
    """Action of a schedule."""

    type: str = "scene"
    scene: str | None = None
    h: int | None = Field(default=None, ge=0, le=360)
    s: int | None = Field(default=None, ge=0, le=1000)
    v: int | None = Field(default=None, ge=0, le=1000)
    brightness: int | None = Field(default=None, ge=0, le=1000)
    kelvin: int | None = Field(default=None, ge=2000, le=6500)
    effect: str | None = None
    speed: int | None = Field(default=None, ge=100, le=4000)
    duration_min: int | None = Field(default=None, ge=1, le=60)


class CreateScheduleRequest(BaseModel):
    """`POST/PUT /instances/{id}/schedules`."""

    time: str
    timezone: str = "UTC"
    repeat: str = "once"
    days: list[int] = Field(default_factory=list)
    action: ScheduleAction = Field(default_factory=ScheduleAction)
    enabled: bool = True


class ScheduleEnabledRequest(BaseModel):
    enabled: bool


class SimSessionRequest(BaseModel):
    pin: str


class SimModbusWrite(BaseModel):
    kind: str = "holding"
    addr: int = Field(ge=0, le=65535)
    value: int = Field(ge=0, le=65535)
    apply: bool = True


class SimFaultRequest(BaseModel):
    type: str
    duration_s: int = Field(default=0, ge=0, le=3600)


class SimPowerRequest(BaseModel):
    action: str = "on"
    confirm: bool = False


class SimBootloaderRequest(BaseModel):
    action: str = "enter"
