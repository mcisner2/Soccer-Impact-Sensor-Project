"""Session database: keeps finished sessions in a SQLite file so they survive closing the app.

A session is saved automatically when the player presses Stop, and can be discarded later
from the Sessions page. Only what the dashboard needs is kept: a summary row, plus the
detected events and GPS readings. Raw IMU batches are NOT saved (they are large and only
exist in data-collection mode).
"""
from __future__ import annotations

import sqlite3
from contextlib import closing
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

from .models import Session

_SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at    TEXT NOT NULL,
    saved_at      TEXT NOT NULL,
    duration_s    REAL NOT NULL,
    kicks         INTEGER NOT NULL,
    events_total  INTEGER NOT NULL,
    peak_g        REAL,
    distance_m    REAL NOT NULL,
    avg_speed_mph REAL NOT NULL,
    max_speed_mph REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    session_id     INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    seq            INTEGER NOT NULL,
    t_ms           INTEGER NOT NULL,
    event_class    INTEGER NOT NULL,
    peak_accel_cg  INTEGER NOT NULL,
    peak_gyro_dps  INTEGER NOT NULL,
    confidence     INTEGER NOT NULL,
    flags          INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS telemetry (
    session_id   INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,
    t_ms         INTEGER NOT NULL,
    fix_quality  INTEGER NOT NULL,
    lat_e7       INTEGER NOT NULL,
    lon_e7       INTEGER NOT NULL,
    speed_cm_s   INTEGER NOT NULL,
    satellites   INTEGER NOT NULL,
    hdop_x10     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS events_by_session ON events(session_id);
CREATE INDEX IF NOT EXISTS telemetry_by_session ON telemetry(session_id);
"""


class SessionDatabase:
    def __init__(self, path: Union[str, Path]) -> None:
        self._path = str(path)
        self._last_saved: Optional[Session] = None
        # Goes up on every save / discard, so the server knows when to resend the list.
        self.version = 0
        with closing(self._connect()) as conn, conn:
            conn.executescript(_SCHEMA)

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self._path)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")  # needed so deleting a session deletes its rows
        return conn

    # --- writing ---
    def save_session(self, session: Session, summary: Dict[str, Any]) -> int:
        """Write one session (and its events / GPS readings) to disk. Returns the new session id.

        `summary` is the dict from build_snapshot(); its numbers are stored as-is so the
        Sessions page can list sessions without recomputing anything.
        """
        s = summary["session"]
        m = summary["movement"]
        with closing(self._connect()) as conn, conn:
            cur = conn.execute(
                "INSERT INTO sessions (started_at, saved_at, duration_s, kicks, events_total, peak_g,"
                " distance_m, avg_speed_mph, max_speed_mph) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    session.started_at.isoformat(timespec="seconds"),
                    datetime.now().isoformat(timespec="seconds"),
                    s["duration_s"], s["kicks"], s["events_total"], s["peak_g"],
                    m["distance_m"], m["avg_speed_mph"], m["max_speed_mph"],
                ),
            )
            session_id = cur.lastrowid
            conn.executemany(
                "INSERT INTO events (session_id, seq, t_ms, event_class, peak_accel_cg, peak_gyro_dps,"
                " confidence, flags) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                [
                    (session_id, e.sequence_number, e.t_ms, int(e.event_class), e.peak_accel_centi_g,
                     e.peak_gyro_dps, e.confidence, e.flags)
                    for e in session.events
                ],
            )
            conn.executemany(
                "INSERT INTO telemetry (session_id, seq, t_ms, fix_quality, lat_e7, lon_e7, speed_cm_s,"
                " satellites, hdop_x10) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                [
                    (session_id, t.sequence_number, t.t_ms, t.fix_quality, t.lat_e7, t.lon_e7,
                     t.speed_cm_s, t.satellites, t.hdop_x10)
                    for t in session.telemetry
                ],
            )
        self.version += 1
        return session_id

    def save_once(self, session: Session, summary: Dict[str, Any]) -> Optional[int]:
        """Save this session unless it was already saved or has nothing in it.

        Pressing Stop twice must not create two copies, and a session with no events and no
        GPS readings (Stop pressed right after Start) is not worth keeping.
        Returns the new id, or None when nothing was saved.
        """
        if session is self._last_saved:
            return None
        if not session.events and not session.telemetry:
            return None
        session_id = self.save_session(session, summary)
        self._last_saved = session
        return session_id

    def delete_session(self, session_id: int) -> bool:
        """Discard a saved session. Returns False if there was no session with that id."""
        with closing(self._connect()) as conn, conn:
            deleted = conn.execute("DELETE FROM sessions WHERE id = ?", (session_id,)).rowcount > 0
        if deleted:
            self.version += 1
        return deleted

    # --- reading ---
    def list_sessions(self) -> List[Dict[str, Any]]:
        """All saved sessions, newest first (what the Sessions page shows)."""
        with closing(self._connect()) as conn:
            rows = conn.execute(
                "SELECT id, started_at, saved_at, duration_s, kicks, events_total, peak_g, distance_m,"
                " avg_speed_mph, max_speed_mph FROM sessions ORDER BY id DESC"
            ).fetchall()
        return [dict(row) for row in rows]