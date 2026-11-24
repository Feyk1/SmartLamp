#!/usr/bin/env python3
"""Generate the Ed25519, AES-256 and JWT_SECRET keys for the demo.

Output:
  - keys/ota_public.pem       (Ed25519 public key, committed)
  - .env                      (private keys + JWT_SECRET, NOT committed)

Usage:
  python3 tools/generate_keys.py
"""
import os
import secrets
import sys
from pathlib import Path

try:
    import binascii
    from nacl.signing import SigningKey
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519
except ImportError:
    print("ERROR: missing dependencies. Run:", file=sys.stderr)
    print("  pip install pynacl cryptography", file=sys.stderr)
    sys.exit(1)


ROOT = Path(__file__).resolve().parent.parent
KEYS_DIR = ROOT / "keys"
ENV_FILE = ROOT / ".env"
ENV_EXAMPLE = ROOT / ".env.example"


def generate_ed25519() -> tuple[str, bytes]:
    """Generate an Ed25519 pair. Return (private_hex, public_pem)."""
    sk = SigningKey.generate()
    private_hex = binascii.hexlify(bytes(sk)).decode()
    public_bytes = bytes(sk.verify_key)
    # Convert to PEM
    pub = ed25519.Ed25519PublicKey.from_public_bytes(public_bytes)
    pem = pub.public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    return private_hex, pem


def generate_aes256() -> str:
    return secrets.token_hex(32)  # 32 bytes = 64 hex chars


def generate_jwt_secret() -> str:
    return secrets.token_urlsafe(48)


def read_env_example() -> str:
    if ENV_EXAMPLE.exists():
        return ENV_EXAMPLE.read_text()
    return ""


def main() -> None:
    if ENV_FILE.exists():
        response = input(f"{ENV_FILE} already exists. Overwrite? [y/N] ")
        if response.lower() != "y":
            print("Cancelled.")
            return

    print("Generating keys...")
    signing_key, public_pem = generate_ed25519()
    aes_key = generate_aes256()
    jwt_secret = generate_jwt_secret()

    # 1. Save the public key
    KEYS_DIR.mkdir(exist_ok=True)
    (KEYS_DIR / "ota_public.pem").write_bytes(public_pem)
    print(f"  ✓ keys/ota_public.pem")

    # 2. Generate .env from .env.example, filling in the values
    env_content = read_env_example()
    replacements = {
        "OTA_SIGNING_KEY=": f"OTA_SIGNING_KEY={signing_key}",
        "OTA_AES_KEY=": f"OTA_AES_KEY={aes_key}",
        "JWT_SECRET=": f"JWT_SECRET={jwt_secret}",
    }
    for placeholder, value in replacements.items():
        env_content = env_content.replace(placeholder + "\n", value + "\n")
        # Also handle the case where it is at the end without a newline
        env_content = env_content.replace(placeholder, value)

    ENV_FILE.write_text(env_content)
    print(f"  ✓ .env")

    print()
    print("=" * 60)
    print(" Private keys (copy to secrets, do NOT commit)")
    print("=" * 60)
    print(f"OTA_SIGNING_KEY = {signing_key}")
    print(f"OTA_AES_KEY     = {aes_key}")
    print(f"JWT_SECRET      = {jwt_secret}")
    print()
    print("These keys are already in .env (which is gitignored).")
    print("Copy them to GitHub Secrets (environment 'release') and Fly Secrets.")


if __name__ == "__main__":
    main()