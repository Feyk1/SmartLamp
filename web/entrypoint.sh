#!/bin/sh
set -eu

ORCH="${PUBLIC_ORCHESTRATOR_URL:-http://localhost:8080}"

cat > /app/config.js <<EOF
window.LAMP_CONFIG = { orchestrator: "${ORCH}" }
EOF

exec python3 /app/server.py
