#!/usr/bin/env python3
"""Self-test for `package_ota.py`/`verify_ota.py` and fixture generator.

With no arguments it runs the checks (roundtrip + expected failures).
With `--write-fixtures DIR` it generates the test artifacts used by the C test
`test_ota_verify`, using a **test** Ed25519 key (public, non-secret).

Usage:
  python3 tools/test_ota.py
  python3 tools/test_ota.py --write-fixtures firmware/test/fixtures/ota
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519
except ImportError:  # pragma: no cover
    print("ERROR: missing cryptography: pip install pynacl cryptography", file=sys.stderr)
    sys.exit(1)

sys.path.insert(0, str(Path(__file__).resolve().parent))

from package_ota import package  # noqa: E402
from verify_ota import (  # noqa: E402
    FAULT_DOWNGRADE,
    FAULT_FORMAT,
    FAULT_SIGNATURE,
    OtaError,
    format_version,
    verify,
)

# TEST keys (deterministic and public; never use in production).
TEST_SEED = hashlib.sha256(b"smartlamp-test-ed25519").digest()
TEST_AES_KEY = hashlib.sha256(b"smartlamp-test-aes").digest()
TEST_PAYLOAD = bytes(range(256)) * 2  # 512 B deterministic


def public_key_bytes() -> bytes:
    seed = TEST_SEED
    key = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
    return key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )


def public_key_pem() -> bytes:
    key = ed25519.Ed25519PrivateKey.from_private_bytes(TEST_SEED)
    return key.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )


def artifact(version: str, payload: bytes = TEST_PAYLOAD, hw_compat: int = 1) -> bytes:
    parts = tuple(int(part) for part in version.lstrip("v").split("."))
    return package(payload, parts, TEST_AES_KEY, TEST_SEED, hw_compat)  # type: ignore[arg-type]


def expect_fault(code: int, func, *args, **kwargs) -> None:
    try:
        func(*args, **kwargs)
    except OtaError as exc:
        assert exc.code == code, f"expected FAULT {code}, got {exc.code}"
        return
    raise AssertionError(f"expected FAULT {code} and no error was raised")


def run_checks() -> None:
    pub = public_key_bytes()
    v103 = artifact("1.0.3")
    result = verify(v103, pub, TEST_AES_KEY)
    assert result.payload == TEST_PAYLOAD, "payload differs after roundtrip"
    assert result.header.version == (1, 0, 3)
    print(f"✓ roundtrip {format_version(result.header.version)} ({len(v103)} B)")

    v120 = artifact("1.2.0")
    result = verify(v120, pub, TEST_AES_KEY, (1, 0, 3))
    assert result.header.version == (1, 2, 0)
    print("✓ anti-downgrade: 1.2.0 > 1.0.3")

    expect_fault(FAULT_DOWNGRADE, verify, v103, pub, TEST_AES_KEY, (1, 2, 0))
    print("✓ downgrade rejected (FAULT 9)")

    corrupted = bytearray(v120)
    corrupted[200] ^= 0x01  # inside the ciphertext
    expect_fault(FAULT_SIGNATURE, verify, bytes(corrupted), pub, TEST_AES_KEY)
    print("✓ tampered ciphertext rejected (FAULT 8)")

    bad_magic = bytearray(v120)
    bad_magic[0] ^= 0xFF
    expect_fault(FAULT_FORMAT, verify, bytes(bad_magic), pub)
    print("✓ invalid magic rejected (FAULT 12)")

    expect_fault(FAULT_FORMAT, verify, v120[:-1], pub)
    print("✓ truncated artifact rejected (FAULT 12)")

    expect_fault(FAULT_SIGNATURE, verify, v120, pub, hashlib.sha256(b"otra").digest())
    print("✓ wrong AES key rejected (FAULT 8)")


def write_fixtures(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "test_public.pem").write_bytes(public_key_pem())
    (directory / "test_firmware.bin").write_bytes(TEST_PAYLOAD)
    (directory / "test_v1.0.3.ota").write_bytes(artifact("1.0.3"))
    (directory / "test_v1.2.0.ota").write_bytes(artifact("1.2.0"))

    bad = bytearray(artifact("1.2.0"))
    bad[200] ^= 0x01
    (directory / "test_bad_signature.ota").write_bytes(bytes(bad))

    bad_hw = artifact("1.2.0", hw_compat=0x2)
    (directory / "test_bad_hw.ota").write_bytes(bad_hw)

    print(f"✓ fixtures in {directory}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-fixtures", type=Path)
    args = parser.parse_args()

    run_checks()
    if args.write_fixtures is not None:
        write_fixtures(args.write_fixtures)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"✗ {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
