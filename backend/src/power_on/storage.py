"""Power-on behavior persistence, per `device_token`.

Reuses provisioning's SQLite (`PROVISIONING_DB`) with a separate table.
"""

from __future__ import annotations

import json
import logging
import sqlite3
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from src import settings

logger = logging.getLogger("lamp.power_on")

SCHEMA = """
CREATE TABLE IF NOT EXISTS power_on (
    device_key TEXT PRIMARY KEY,
    mode TEXT NOT NULL,
    state TEXT,
    updated_at TEXT NOT NULL
);
"""


@dataclass(frozen=True)
class PowerOnRecord:
    device_key: str
    mode: str
    state: dict | None
    updated_at: str


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


class PowerOnStore:
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
            logger.warning("power-on DB unavailable: %s", exc)
            return None

    def load(self, device_key: str) -> PowerOnRecord | None:
        connection = self._connect()
        if connection is None:
            return None
        try:
            row = connection.execute(
                "SELECT device_key, mode, state, updated_at "
                "FROM power_on WHERE device_key = ?",
                (device_key,),
            ).fetchone()
        except sqlite3.Error as exc:
            logger.warning("could not load power-on policy: %s", exc)
            return None
        finally:
            connection.close()
        if row is None:
            return None
        state = None
        if row[2]:
            try:
                state = json.loads(row[2])
            except json.JSONDecodeError:
                state = None
        return PowerOnRecord(
            device_key=row[0],
            mode=row[1],
            state=state,
            updated_at=row[3],
        )

    def delete(self, device_key: str) -> None:
        connection = self._connect()
        if connection is None:
            return
        try:
            connection.execute(
                "DELETE FROM power_on WHERE device_key = ?", (device_key,)
            )
            connection.commit()
        except sqlite3.Error as exc:
            logger.warning("could not reset power-on policy: %s", exc)
        finally:
            connection.close()

    def save(self, device_key: str, mode: str, state: dict | None) -> None:
        connection = self._connect()
        if connection is None:
            return
        try:
            connection.execute(
                "INSERT INTO power_on (device_key, mode, state, updated_at) "
                "VALUES (?, ?, ?, ?) "
                "ON CONFLICT(device_key) DO UPDATE SET "
                "mode=excluded.mode, state=excluded.state, "
                "updated_at=excluded.updated_at",
                (
                    device_key,
                    mode,
                    json.dumps(state, sort_keys=True) if state else None,
                    _now(),
                ),
            )
            connection.commit()
        except sqlite3.Error as exc:
            logger.warning("could not save power-on policy: %s", exc)
        finally:
            connection.close()


power_on_store = PowerOnStore()
