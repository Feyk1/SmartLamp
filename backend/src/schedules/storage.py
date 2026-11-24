"""SQLite persistence for schedules.

Stored per `device_token` (not per the instance's ephemeral id) so that
schedules survive a backend restart and are reattached to the device
when it creates its instance again.
"""

from __future__ import annotations

import json
import logging
import sqlite3
from datetime import datetime
from pathlib import Path

from src import settings
from src.store.memory import Instance, Schedule

logger = logging.getLogger("lamp.schedules")

SCHEMA = """
CREATE TABLE IF NOT EXISTS schedules (
    id TEXT PRIMARY KEY,
    device_key TEXT NOT NULL,
    time TEXT NOT NULL,
    timezone TEXT NOT NULL,
    repeat TEXT NOT NULL,
    days TEXT NOT NULL,
    action TEXT NOT NULL,
    enabled INTEGER NOT NULL,
    next_run TEXT,
    created_at TEXT NOT NULL,
    last_run TEXT,
    last_status TEXT,
    last_error TEXT,
    attempts INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_schedules_device ON schedules(device_key);
"""


def _iso(value: datetime | None) -> str | None:
    return value.isoformat() if value is not None else None


def _date(value: str | None) -> datetime | None:
    if not value:
        return None
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


class ScheduleStore:
    def _connect(self) -> sqlite3.Connection | None:
        path = settings.SCHEDULES_DB
        if not path:
            return None
        try:
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            connection = sqlite3.connect(path, timeout=5.0)
            connection.executescript(SCHEMA)
            return connection
        except (OSError, sqlite3.Error) as exc:
            logger.warning("schedules DB unavailable: %s", exc)
            return None

    def attach(self, instance: Instance) -> None:
        """Loads the device's schedules into the instance."""
        connection = self._connect()
        if connection is None:
            return
        try:
            rows = connection.execute(
                "SELECT id, time, timezone, repeat, days, action, enabled, "
                "next_run, created_at, last_run, last_status, last_error, "
                "attempts FROM schedules WHERE device_key = ? ORDER BY created_at",
                (instance.device_token,),
            ).fetchall()
        except sqlite3.Error as exc:
            logger.warning("could not load schedules: %s", exc)
            return
        finally:
            connection.close()
        instance.schedules = [
            Schedule(
                id=row[0],
                time=row[1],
                timezone=row[2],
                repeat=row[3],
                days=json.loads(row[4]),
                action=json.loads(row[5]),
                enabled=bool(row[6]),
                next_run=_date(row[7]),
                created_at=_date(row[8]) or datetime.now(),
                last_run=_date(row[9]),
                last_status=row[10],
                last_error=row[11],
                attempts=int(row[12]),
            )
            for row in rows
        ]

    def save(self, device_key: str, schedule: Schedule) -> None:
        connection = self._connect()
        if connection is None:
            return
        try:
            connection.execute(
                "INSERT INTO schedules (id, device_key, time, timezone, repeat, "
                "days, action, enabled, next_run, created_at, last_run, "
                "last_status, last_error, attempts) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT(id) DO UPDATE SET time=excluded.time, "
                "timezone=excluded.timezone, repeat=excluded.repeat, "
                "days=excluded.days, action=excluded.action, "
                "enabled=excluded.enabled, next_run=excluded.next_run, "
                "last_run=excluded.last_run, last_status=excluded.last_status, "
                "last_error=excluded.last_error, attempts=excluded.attempts",
                (
                    schedule.id,
                    device_key,
                    schedule.time,
                    schedule.timezone,
                    schedule.repeat,
                    json.dumps(schedule.days),
                    json.dumps(schedule.action, sort_keys=True),
                    int(schedule.enabled),
                    _iso(schedule.next_run),
                    _iso(schedule.created_at),
                    _iso(schedule.last_run),
                    schedule.last_status,
                    schedule.last_error,
                    schedule.attempts,
                ),
            )
            connection.commit()
        except sqlite3.Error as exc:
            logger.warning("could not save schedule %s: %s", schedule.id, exc)
        finally:
            connection.close()

    def delete_all(self, device_key: str) -> None:
        connection = self._connect()
        if connection is None:
            return
        try:
            connection.execute(
                "DELETE FROM schedules WHERE device_key = ?", (device_key,)
            )
            connection.commit()
        except sqlite3.Error as exc:
            logger.warning("could not clear schedules: %s", exc)
        finally:
            connection.close()

    def delete(self, device_key: str, schedule_id: str) -> None:
        connection = self._connect()
        if connection is None:
            return
        try:
            connection.execute(
                "DELETE FROM schedules WHERE id = ? AND device_key = ?",
                (schedule_id, device_key),
            )
            connection.commit()
        except sqlite3.Error as exc:
            logger.warning("could not delete schedule %s: %s", schedule_id, exc)
        finally:
            connection.close()


schedule_store = ScheduleStore()
