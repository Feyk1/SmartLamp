#!/usr/bin/env python3
"""Update docs/firmware/index.json with a new version.

Usage (from the release workflow):
  python3 tools/update_firmware_index.py \\
    --version v1.1.0 \\
    --sha256 <hex> \\
    --size <bytes> \\
    --file firmware.ota
"""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
INDEX = ROOT / "docs" / "firmware" / "index.json"
FIRMWARE_DIR = ROOT / "docs" / "firmware"


def compute_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--sha256", default=None)
    args = parser.parse_args()

    sha = args.sha256 or compute_sha256(args.file)
    size = args.file.stat().st_size
    released_at = datetime.now(timezone.utc).isoformat()

    # Load the existing index or create a new one
    if INDEX.exists():
        data = json.loads(INDEX.read_text())
    else:
        data = {"versions": []}

    # Drop the previous version if it exists (idempotent)
    data["versions"] = [v for v in data["versions"] if v["version"] != args.version]

    # Insert the new version at the front
    data["versions"].insert(0, {
        "version": args.version,
        "sha256": sha,
        "size": size,
        "released_at": released_at,
        "url": f"/firmware/{args.version}/firmware.ota",
    })

    # Sort by date descending
    data["versions"].sort(key=lambda v: v["released_at"], reverse=True)

    INDEX.parent.mkdir(parents=True, exist_ok=True)
    INDEX.write_text(json.dumps(data, indent=2) + "\n")
    print(f"  ✓ {INDEX} ({len(data['versions'])} versions)")


if __name__ == "__main__":
    main()