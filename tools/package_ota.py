#!/usr/bin/env python3
"""Package a firmware binary into a signed and encrypted `.ota` artifact.

Format (docs/04-ota-and-image-security.md §4): 128 B cleartext header,
payload encrypted with AES-256-GCM (ciphertext + 16 B tag) and a 64 B Ed25519
signature over `header || ciphertext || tag`.

Usage:
  OTA_AES_KEY=<hex> OTA_SIGNING_KEY=<hex> \\
    python3 tools/package_ota.py --input firmware.bin \\
        --version 1.2.0 --output firmware-1.2.0.ota
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from nacl.signing import SigningKey
except ImportError:  # pragma: no cover - environment-dependent
    print(
        "ERROR: missing dependencies. Run: pip install pynacl cryptography",
        file=sys.stderr,
    )
    sys.exit(1)

MAGIC = b"LAMPOTA1"
FORMAT_VERSION = 1
HEADER_FMT = "<8sH6sII32s12s32s28s"
HEADER_SIZE = 128
AAD_OFFSET = 68
RESERVED_OFFSET = 100
GCM_TAG_SIZE = 16
SIGNATURE_SIZE = 64
DEFAULT_HW_COMPAT = 0x1


def parse_version(text: str) -> tuple[int, int, int]:
    """Convert "v1.2.3" or "1.2.3" into (major, minor, patch)."""
    clean = text.strip().lstrip("vV")
    parts = clean.split(".")
    if len(parts) != 3:
        raise ValueError(f"invalid version: {text!r} (expected vX.Y.Z)")
    values = tuple(int(part, 10) for part in parts)
    if any((value < 0) or (value > 0xFFFF) for value in values):
        raise ValueError(f"version out of range: {text!r}")
    return values  # type: ignore[return-value]


def pack_version(version: tuple[int, int, int]) -> bytes:
    return struct.pack("<HHH", *version)


def format_version(version: tuple[int, int, int]) -> str:
    return ".".join(str(part) for part in version)


def hex_to_bytes(value: str, size: int, name: str) -> bytes:
    try:
        raw = bytes.fromhex(value)
    except ValueError as exc:
        raise ValueError(f"{name} is not valid hex") from exc
    if len(raw) != size:
        raise ValueError(f"{name} must be {size} bytes, got {len(raw)}")
    return raw


def build_header(
    version: tuple[int, int, int],
    hw_compat: int,
    payload_len: int,
    payload_sha256: bytes,
    nonce: bytes,
    aad_hash: bytes,
) -> bytes:
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        FORMAT_VERSION,
        pack_version(version),
        hw_compat,
        payload_len,
        payload_sha256,
        nonce,
        aad_hash,
        b"\x00" * 28,
    )
    assert len(header) == HEADER_SIZE
    return header


def aad_hash_for(header: bytes) -> bytes:
    """SHA-256 of the header with `aad_hash` and `reserved` zeroed out."""
    provisional = bytearray(header)
    provisional[AAD_OFFSET:RESERVED_OFFSET] = b"\x00" * 32
    provisional[RESERVED_OFFSET:HEADER_SIZE] = b"\x00" * 28
    return hashlib.sha256(bytes(provisional)).digest()


def package(payload: bytes, version: tuple[int, int, int], aes_key: bytes, signing_seed: bytes, hw_compat: int) -> bytes:
    """Build the complete .ota artifact."""
    payload_sha256 = hashlib.sha256(payload).digest()
    nonce = os.urandom(12)

    provisional = build_header(
        version, hw_compat, len(payload), payload_sha256, nonce, b"\x00" * 32
    )
    aad_hash = aad_hash_for(provisional)
    header = build_header(
        version, hw_compat, len(payload), payload_sha256, nonce, aad_hash
    )

    ciphertext_with_tag = AESGCM(aes_key).encrypt(nonce, payload, aad_hash)
    message = header + ciphertext_with_tag
    signature = SigningKey(signing_seed).sign(message).signature
    return message + signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--hw-compat", default=DEFAULT_HW_COMPAT, type=lambda v: int(v, 0))
    args = parser.parse_args()

    try:
        version = parse_version(args.version)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    aes_hex = os.environ.get("OTA_AES_KEY", "")
    signing_hex = os.environ.get("OTA_SIGNING_KEY", "")
    if not aes_hex or not signing_hex:
        print(
            "ERROR: define OTA_AES_KEY and OTA_SIGNING_KEY (see .env.example)",
            file=sys.stderr,
        )
        return 2
    try:
        aes_key = hex_to_bytes(aes_hex, 32, "OTA_AES_KEY")
        signing_seed = hex_to_bytes(signing_hex, 32, "OTA_SIGNING_KEY")
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    try:
        payload = args.input.read_bytes()
    except OSError as exc:
        print(f"ERROR: could not read {args.input}: {exc}", file=sys.stderr)
        return 2
    if not payload:
        print("ERROR: payload is empty", file=sys.stderr)
        return 2

    artifact = package(payload, version, aes_key, signing_seed, args.hw_compat)
    args.output.write_bytes(artifact)

    print(
        f"OK: {args.output} ({len(artifact)} B) "
        f"version={format_version(version)} payload={len(payload)} B "
        f"hw_compat=0x{args.hw_compat:08x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
