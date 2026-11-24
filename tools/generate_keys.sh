#!/usr/bin/env bash
# Generate all the keys the project needs.
# Usage: ./tools/generate_keys.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

echo "============================================"
echo " Smart Lamp Demo — Key generator"
echo "============================================"
echo ""

# 1. Check dependencies
if ! command -v python3 &>/dev/null; then
  echo "ERROR: python3 is not installed." >&2
  exit 1
fi

if ! python3 -c "import nacl" &>/dev/null; then
  echo "Installing pynacl and cryptography..."
  pip install pynacl cryptography
fi

# 2. Generate keys
cd "$ROOT_DIR"
python3 "$SCRIPT_DIR/generate_keys.py"

echo ""
echo "============================================"
echo " Summary"
echo "============================================"
echo ""
echo "Generated files:"
echo "  keys/ota_public.pem     (safe to commit)"
echo "  .env                    (do NOT commit)"
echo ""
echo "Next steps:"
echo "  1. Copy OTA_SIGNING_KEY and OTA_AES_KEY from .env"
echo "     to GitHub Secrets (environment 'release')."
echo "  2. Copy JWT_SECRET to the orchestrator's Fly Secrets."
echo "  3. NEVER commit .env."
echo ""