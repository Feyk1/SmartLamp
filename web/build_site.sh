#!/bin/sh
# Builds the static site published to GitHub Pages.
#
# Usage:
#   PUBLIC_ORCHESTRATOR_URL=https://... web/build_site.sh [_site]
#   OTA_FILE=firmware/releases/1.1.0.ota OTA_VERSION=1.1.0 web/build_site.sh _site
set -eu

OUT="${1:-_site}"
ORCH="${PUBLIC_ORCHESTRATOR_URL:-http://localhost:8080}"
OTA_FILE="${OTA_FILE:-}"
OTA_VERSION="${OTA_VERSION:-}"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

rm -rf "$OUT"
mkdir -p "$OUT/src"

cp "$ROOT/web/index.html" "$ROOT/web/sim.html" "$OUT/"
cp "$ROOT/web/src/"*.js "$ROOT/web/src/"*.css "$OUT/src/"

cat > "$OUT/config.js" <<EOF
window.LAMP_CONFIG = { orchestrator: "${ORCH}" };
EOF

if [ -n "$OTA_FILE" ] && [ -n "$OTA_VERSION" ]; then
    mkdir -p "$OUT/firmware/$OTA_VERSION"
    cp "$OTA_FILE" "$OUT/firmware/$OTA_VERSION/"
fi

echo "site generated at $OUT (orchestrator=$ORCH)"
