"""Encrypted persistence of Wi-Fi credentials per `device_token`.

The password is never stored in plaintext: it is encrypted with AES-256-GCM and
a 32 B key (`PROVISIONING_KEY` or derived from `JWT_SECRET`). The API and
events only expose SSID/IP; the password is not returned, logged, or sent to the
firmware.
"""

from __future__ import annotations

import hashlib
import logging
import os
import sqlite3
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from src import settings

logger = logging.getLogger("lamp.provisioning")

AAD = b"smartlamp-provisioning-v1"

SCHEMA = """
CREATE TABLE IF NOT EXISTS credentials (
    device_key TEXT PRIMARY KEY,
    ssid TEXT,
    password BLOB,
    ip TEXT,
    forgotten INTEGER NOT NULL DEFAULT 0,
    updated_at TEXT NOT NULL
);
"""


@dataclass(frozen=True)
class CredentialRecord:
    device_key: str
    ssid: str | None
    password: bytes | None
    ip: str | None
    forgotten: bool
    updated_at: str


def _key() -> bytes:
    """32 B AES-256 key; accepts hex or text and falls back to JWT_SECRET."""
    raw = settings.PROVISIONING_KEY.strip()
    if raw:
        try:
            key = bytes.fromhex(raw)
        except ValueError:
            key = hashlib.sha256(raw.encode()).digest()
        return key if len(key) == 32 else hashlib.sha256(key).digest()
    return hashlib.sha256(settings.JWT_SECRET.encode()).digest()


def encrypt_password(password: str) -> bytes:
    """Encrypts a password with AES-GCM and a random 12 B nonce."""
    nonce = os.urandom(12)
    return nonce + AESGCM(_key()).encrypt(nonce, password.encode(), AAD)


def decrypt_password(blob: bytes) -> str:
    """Decrypts a stored password; useful for tests and diagnostics."""
    return AESGCM(_key()).decrypt(blob[:12], blob[12:], AAD).decode()


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


class ProvisioningStore:
    """Minimal SQLite; one connection per operation like the schedules store."""

    def _connect(self) -> sqlite3.Connection | None:
        path = settings.PROVISIONING_DB
        if not path:
            return None
        try:
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            connection = sqlite3.connect(path, timeout=5.0)
            connection.executescript(SCHEMA)
            return connection
        except (OSError, sqlite3.Error) as exc:
            logger.warning("provisioning DB unavailable: %s", exc)
            return None

    def load(self, device_key: str) -> CredentialRecord | None:
        connection = self._connect()
        if connection is None:
            return None
        try:
            row = connection.execute(
                "SELECT device_key, ssid, password, ip, forgotten, updated_at "
                "FROM credentials WHERE device_key = ?",
                (device_key,),
            ).fetchone()
        except sqlite3.Error as exc:
            logger.warning("could not load credentials: %s", exc)
            return None
        finally:
            connection.close()
        if row is None:
            return None
        return CredentialRecord(
            device_key=row[0],
            ssid=row[1],
            password=row[2],
            ip=row[3],
            forgotten=bool(row[4]),
            updated_at=row[5],
        )

    def save(
        self,
        device_key: str,
        ssid: str,
        password: bytes,
        ip: str,
    ) -> None:
        connection = self._connect()
        if connection is None:
            return
        try:
            connection.execute(
                "INSERT INTO credentials "
                "(device_key, ssid, password, ip, forgotten, updated_at) "
                "VALUES (?, ?, ?, ?, 0, ?) "
                "ON CONFLICT(device_key) DO UPDATE SET "
                "ssid=excluded.ssid, password=excluded.password, "
                "ip=excluded.ip, forgotten=0, updated_at=excluded.updated_at",
                (device_key, ssid, password, ip, _now()),
            )
            connection.commit()
        except sqlite3.Error as exc:
            logger.warning("could not save credentials: %s", exc)
        finally:
            connection.close()

    def forget(self, device_key: str) -> None:
        """Deletes the credentials and leaves a tombstone to avoid re-seeding."""
        connection = self._connect()
        if connection is None:
            return
        try:
            connection.execute(
                "INSERT INTO credentials "
                "(device_key, ssid, password, ip, forgotten, updated_at) "
                "VALUES (?, NULL, NULL, NULL, 1, ?) "
                "ON CONFLICT(device_key) DO UPDATE SET "
                "ssid=NULL, password=NULL, ip=NULL, forgotten=1, "
                "updated_at=excluded.updated_at",
                (device_key, _now()),
            )
            connection.commit()
        except sqlite3.Error as exc:
            logger.warning("could not forget credentials: %s", exc)
        finally:
            connection.close()


provisioning_store = ProvisioningStore()
