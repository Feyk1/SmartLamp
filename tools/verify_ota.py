#!/usr/bin/env python3
"""Verify (and optionally decrypt) a Smart Lamp `.ota` artifact.

Checks, in this order: header format, `aad_hash`, Ed25519 signature,
anti-downgrade (optional) and, if an AES key is given, GCM tag and
`payload_sha256`.

Usage:
  python3 tools/verify_ota.py --input firmware-1.2.0.ota
  python3 tools/verify_ota.py --input x.ota --active-version 1.0.3 \\
      --output payload.bin
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from nacl.exceptions import BadSignatureError
    from nacl.signing import VerifyKey
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
MIN_SIZE = HEADER_SIZE + GCM_TAG_SIZE + SIGNATURE_SIZE

# Fault codes aligned with the bootloader (docs/04 §7.1).
FAULT_SIGNATURE = 8
FAULT_DOWNGRADE = 9
FAULT_SHA = 10
FAULT_HARDWARE = 11
FAULT_FORMAT = 12


class OtaError(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass
class OtaHeader:
    magic: bytes
    format_version: int
    version: tuple[int, int, int]
    hw_compat: int
    payload_len: int
    payload_sha256: bytes
    nonce: bytes
    aad_hash: bytes
    raw: bytes


@dataclass
class OtaResult:
    header: OtaHeader
    payload: bytes | None = None
    checks: list[str] = field(default_factory=list)


def parse_version(text: str) -> tuple[int, int, int]:
    parts = text.strip().lstrip("vV").split(".")
    if len(parts) != 3:
        raise ValueError(f"invalid version: {text!r}")
    return tuple(int(part, 10) for part in parts)  # type: ignore[return-value]


def format_version(version: tuple[int, int, int]) -> str:
    return ".".join(str(part) for part in version)


def parse_header(data: bytes) -> OtaHeader:
    if len(data) < MIN_SIZE:
        raise OtaError(FAULT_FORMAT, f"artifact too short ({len(data)} B)")
    fields = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    magic, format_version, version_raw, hw_compat, payload_len = fields[:5]
    payload_sha256, nonce, aad_hash = fields[5], fields[6], fields[7]

    if magic != MAGIC:
        raise OtaError(FAULT_FORMAT, f"invalid magic: {magic!r}")
    if format_version != FORMAT_VERSION:
        raise OtaError(
            FAULT_FORMAT, f"unsupported format_version: {format_version}"
        )
    version = struct.unpack("<HHH", version_raw)
    expected_len = HEADER_SIZE + payload_len + GCM_TAG_SIZE + SIGNATURE_SIZE
    if len(data) != expected_len:
        raise OtaError(
            FAULT_FORMAT,
            f"size {len(data)} B != expected {expected_len} B "
            f"(payload_len={payload_len})",
        )
    return OtaHeader(
        magic=magic,
        format_version=format_version,
        version=version,
        hw_compat=hw_compat,
        payload_len=payload_len,
        payload_sha256=payload_sha256,
        nonce=nonce,
        aad_hash=aad_hash,
        raw=data[:HEADER_SIZE],
    )


def load_public_key(path: Path) -> bytes:
    """Load an Ed25519 public key in PEM (SPKI) or raw hex."""
    raw = path.read_bytes()
    if raw.lstrip().startswith(b"-----BEGIN"):
        key = serialization.load_pem_public_key(raw)
        return key.public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
    text = raw.decode("ascii").strip()
    return bytes.fromhex(text)


def verify(
    data: bytes,
    public_key: bytes,
    aes_key: bytes | None = None,
    active_version: tuple[int, int, int] | None = None,
) -> OtaResult:
    header = parse_header(data)
    result = OtaResult(header=header)
    result.checks.append(
        f"Format {MAGIC.decode()} v{header.format_version}, "
        f"version {format_version(header.version)}"
    )

    provisional = bytearray(header.raw)
    provisional[AAD_OFFSET:RESERVED_OFFSET] = b"\x00" * 32
    provisional[RESERVED_OFFSET:HEADER_SIZE] = b"\x00" * 28
    if hashlib.sha256(bytes(provisional)).digest() != header.aad_hash:
        raise OtaError(FAULT_FORMAT, "header aad_hash mismatch")
    result.checks.append("header aad_hash")

    message = data[: HEADER_SIZE + header.payload_len + GCM_TAG_SIZE]
    signature = data[-SIGNATURE_SIZE:]
    try:
        VerifyKey(public_key).verify(message, signature)
    except BadSignatureError as exc:
        raise OtaError(FAULT_SIGNATURE, "invalid Ed25519 signature") from exc
    result.checks.append("Ed25519 signature")

    if active_version is not None and header.version <= active_version:
        raise OtaError(
            FAULT_DOWNGRADE,
            f"downgrade blocked: {format_version(header.version)} <= "
            f"{format_version(active_version)}",
        )
    if active_version is not None:
        result.checks.append(
            f"Anti-downgrade: {format_version(header.version)} > "
            f"{format_version(active_version)}"
        )

    if aes_key is not None:
        ciphertext_with_tag = data[HEADER_SIZE : HEADER_SIZE + header.payload_len + GCM_TAG_SIZE]
        try:
            payload = AESGCM(aes_key).decrypt(
                header.nonce, ciphertext_with_tag, header.aad_hash
            )
        except Exception as exc:
            raise OtaError(FAULT_SIGNATURE, "invalid AES-GCM tag") from exc
        if hashlib.sha256(payload).digest() != header.payload_sha256:
            raise OtaError(FAULT_SHA, "payload_sha256 mismatch")
        result.payload = payload
        result.checks.append(
            f"AES-GCM + SHA-256 (payload {len(payload)} B)"
        )

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument(
        "--pubkey", type=Path, default=Path("keys/ota_public.pem")
    )
    parser.add_argument("--active-version")
    parser.add_argument("--aes-key-hex")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    try:
        data = args.input.read_bytes()
        public_key = load_public_key(args.pubkey)
        active = parse_version(args.active_version) if args.active_version else None
        aes_hex = args.aes_key_hex or os.environ.get("OTA_AES_KEY") or None
        aes_key = bytes.fromhex(aes_hex) if aes_hex else None
        if aes_key is not None and len(aes_key) != 32:
            raise ValueError("AES key must be 32 bytes")
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    try:
        result = verify(data, public_key, aes_key, active)
    except OtaError as exc:
        print(f"✗ FAULT {exc.code}: {exc.message}", file=sys.stderr)
        return 1

    for check in result.checks:
        print(f"✓ {check}")
    if result.payload is None:
        print("· Decryption skipped (no OTA_AES_KEY / --aes-key-hex)")
    elif args.output is not None:
        args.output.write_bytes(result.payload)
        print(f"✓ Payload written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
