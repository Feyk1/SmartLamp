"""Backend configuration (environment variables)."""

import os

DEVICE_TOKEN = os.getenv("DEVICE_TOKEN", "dev-token-local")
PUBLIC_WS_BASE = os.getenv("PUBLIC_WS_BASE", "ws://localhost:8080")
COMMAND_TIMEOUT_S = float(os.getenv("COMMAND_TIMEOUT_S", "2.0"))
INSTANCE_TTL_S = int(os.getenv("INSTANCE_TTL_S", "3600"))
HEARTBEAT_TIMEOUT_S = float(os.getenv("HEARTBEAT_TIMEOUT_S", "300"))
PING_INTERVAL_S = float(os.getenv("PING_INTERVAL_S", "20"))
PONG_TIMEOUT_S = float(os.getenv("PONG_TIMEOUT_S", "40"))

# Directory with the `.ota` artifacts (firmware/releases mounted in Docker).
OTA_DIR = os.getenv("OTA_DIR", "firmware/releases")

# Modbus driver (Simulation Panel and diagnostics).
MODBUS_HOST = os.getenv("MODBUS_HOST", "127.0.0.1")
MODBUS_PORT = int(os.getenv("MODBUS_PORT", "5020"))
MODBUS_UNIT_ID = int(os.getenv("MODBUS_UNIT_ID", "1"))

# Simulation Panel PIN (minimal friction, not real security).
SIM_PIN = os.getenv("SIM_PIN", "2025")

# Supervisor control channel (ADR-004).
SUPERVISOR_HOST = os.getenv("SUPERVISOR_HOST", "firmware")
SUPERVISOR_PORT = int(os.getenv("SUPERVISOR_PORT", "5022"))
SUPERVISOR_TOKEN = os.getenv("SUPERVISOR_TOKEN", "")

# Session authentication (ADR-010): HS256 JWT for dashboard/sim.
JWT_SECRET = os.getenv("JWT_SECRET", "dev-secret")
JWT_TTL_S = int(os.getenv("JWT_TTL_S", "3600"))
# Ephemeral, single-use ticket to open the dashboard WebSocket.
WS_TICKET_TTL_S = int(os.getenv("WS_TICKET_TTL_S", "30"))

# Dashboard CORS (comma-separated; "*" in the local demo).
DASHBOARD_ORIGINS = [
    origin.strip()
    for origin in os.getenv("DASHBOARD_ORIGINS", "*").split(",")
    if origin.strip()
]

# Orchestration: local (Docker) or fly (Machines API).
INSTANCE_MODE = os.getenv("INSTANCE_MODE", "local").strip().lower()
PUBLIC_HTTP_BASE = os.getenv("PUBLIC_HTTP_BASE", "http://localhost:8080")
IDLE_TIMEOUT_S = int(os.getenv("IDLE_TIMEOUT_S", "300"))
OTA_AES_KEY = os.getenv("OTA_AES_KEY", "")

# Fly Machines API.
FLY_API_URL = os.getenv("FLY_API_URL", "https://api.machines.dev/v1")
FLY_ORG_TOKEN = os.getenv("FLY_ORG_TOKEN", "")
FLY_ORG_SLUG = os.getenv("FLY_ORG_SLUG", "personal")
FLY_REGION = os.getenv("FLY_REGION", "iad")
FLY_INSTANCE_BASE_APP = os.getenv(
    "FLY_INSTANCE_BASE_APP", "lamp-instance-base"
)
FLY_IMAGE_TAG = os.getenv("FLY_IMAGE_TAG", "latest")
FLY_ORCHESTRATOR_APP = os.getenv(
    "FLY_ORCHESTRATOR_APP", "lamp-demo-orchestrator"
)

# Schedule persistence (SQLite). Empty disables persistence.
SCHEDULES_DB = os.getenv("SCHEDULES_DB", "data/schedules.db")

# Simulated onboarding: Wi-Fi delay and the 3 power-cycle rule.
# Each off→on must last 1–10 s (5–10 s recommended) and the 3 cycles must
# complete within 2 min; that way a person can do it without rushing.
PROVISIONING_DELAY_S = float(os.getenv("PROVISIONING_DELAY_S", "1.5"))
PROVISIONING_CYCLES = int(os.getenv("PROVISIONING_CYCLES", "3"))
PROVISIONING_OFF_ON_MIN_S = float(
    os.getenv("PROVISIONING_OFF_ON_MIN_S", "1.0")
)
PROVISIONING_OFF_ON_MAX_S = float(
    os.getenv("PROVISIONING_OFF_ON_MAX_S", "10.0")
)
PROVISIONING_CYCLE_TOTAL_S = float(
    os.getenv("PROVISIONING_CYCLE_TOTAL_S", "120.0")
)

# Wi-Fi credentials: SQLite + AES-GCM. `PROVISIONING_KEY` is 32 B hex;
# if empty it is derived from JWT_SECRET (fine for the demo, not for prod).
PROVISIONING_DB = os.getenv("PROVISIONING_DB", "data/provisioning.db")
PROVISIONING_KEY = os.getenv("PROVISIONING_KEY", "")
PROVISIONING_SEED_SSID = os.getenv("PROVISIONING_SEED_SSID", "Casa-Demo")
# Empty: a random password is generated for the seed (not shown).
PROVISIONING_SEED_PASSWORD = os.getenv("PROVISIONING_SEED_PASSWORD", "")
PROVISIONING_NETWORKS = [
    value.strip()
    for value in os.getenv(
        "PROVISIONING_NETWORKS", "Casa-Demo,Lab,Guest"
    ).split(",")
    if value.strip()
]
PROVISIONING_ALLOW_ANY = os.getenv(
    "PROVISIONING_ALLOW_ANY", "false"
).strip().lower() in {"1", "true", "yes", "on"}
# Simulation credentials for the preloaded networks. They are shown in the
# Sim Panel and in Add device; they are not the device's stored password.
DEFAULT_NETWORK_PASSWORDS = {
    "Casa-Demo": "demo-password",
    "Lab": "lab-password",
    "Guest": "guest-password",
}
PROVISIONING_FAILURE_DELAY_S = float(
    os.getenv("PROVISIONING_FAILURE_DELAY_S", "0.8")
)
PROVISIONING_TIMEOUT_S = float(
    os.getenv("PROVISIONING_TIMEOUT_S", "3.0")
)

# Power-on behavior (hard power on).
POWER_ON_DEFAULT_CCT = 3000
POWER_ON_DEFAULT_BRIGHTNESS = 700

# In-memory rate limiting (1 s fixed window).
DEVICE_RATE_LIMIT = int(os.getenv("DEVICE_RATE_LIMIT", "50"))
INSTANCE_RATE_LIMIT = int(os.getenv("INSTANCE_RATE_LIMIT", "5"))
SIM_PIN_RATE_LIMIT = int(os.getenv("SIM_PIN_RATE_LIMIT", "5"))
SIM_PIN_RATE_WINDOW_S = 60.0

# Local demo: reuse the instance with DEVICE_TOKEN instead of creating a
# new one on every POST; the firmware (static token) is associated with it.
REUSE_DEVICE_INSTANCE = os.getenv("REUSE_DEVICE_INSTANCE", "true").strip().lower() in {
    "1",
    "true",
    "yes",
    "on",
}

FIRMWARE_VERSIONS = [
    version.strip()
    for version in os.getenv("FIRMWARE_VERSIONS", "1.0.3,1.1.0").split(",")
    if version.strip()
]
