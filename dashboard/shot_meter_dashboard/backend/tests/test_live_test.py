import asyncio
import sqlite3
from contextlib import closing
from datetime import datetime
from types import SimpleNamespace

from ble_central.live_events import EventLabels, build_live_events
from ble_central.models import Event, EventClass, Session
from ble_central.session_db import SessionDatabase
from server import run_command


def ev(seq, t_ms, cls=EventClass.KICK, cg=920, dps=202, conf=50):
    return Event(cls, seq, t_ms, cg, dps, conf, 0)


def session_with(*events):
    s = Session(started_at=datetime(2026, 11, 22, 17, 48, 0))
    s.events.extend(events)
    return s


def summary():
    return {
        "session": {"duration_s": 12.5, "kicks": 1, "events_total": 3, "peak_g": 15.0},
        "movement": {"distance_m": 40.0, "avg_speed_mph": 3.0, "max_speed_mph": 8.0},
    }


# ---- the live event log ----

def test_live_events_carry_the_values_and_times_the_page_shows():
    s = session_with(ev(0, 3250), ev(1, 5000, EventClass.PASS, cg=1523, dps=640, conf=71))
    rows = build_live_events(s)
    assert [r["idx"] for r in rows] == [0, 1]               # oldest first
    first = rows[0]
    assert first["clock"] == "2026-11-22T17:48:03.250"      # session start + device time
    assert first["t_ms"] == 3250 and first["t_s"] == 3.25
    assert first["name"] == "KICK" and first["class"] == 1
    assert first["peak_g"] == 9.2 and first["peak_dps"] == 202 and first["confidence"] == 50
    assert first["wrong"] is False
    assert rows[1]["name"] == "PASS" and rows[1]["peak_g"] == 15.23


def test_live_events_show_which_events_are_marked_wrong():
    s = session_with(ev(0, 1000), ev(1, 2000), ev(2, 3000))
    rows = build_live_events(s, {1})
    assert [r["wrong"] for r in rows] == [False, True, False]


# ---- marking ----

def test_marking_and_unmarking_an_event():
    s = session_with(ev(0, 1000), ev(1, 2000))
    labels = EventLabels()
    assert labels.set_wrong(s, 1) is True
    assert labels.wrong_set(s) == {1}
    assert labels.set_wrong(s, 1, wrong=False) is True
    assert labels.wrong_set(s) == set()


def test_marking_an_event_that_does_not_exist_is_refused():
    s = session_with(ev(0, 1000))
    labels = EventLabels()
    assert labels.set_wrong(s, 5) is False
    assert labels.set_wrong(s, -1) is False
    assert labels.wrong_set(s) == set()


def test_a_new_session_starts_with_no_marks():
    first = session_with(ev(0, 1000))
    labels = EventLabels()
    labels.set_wrong(first, 0)
    second = session_with(ev(0, 1000))
    assert labels.wrong_set(second) == set()


# ---- the commands ----

def test_flag_and_unflag_commands_change_the_marks():
    s = session_with(ev(0, 1000), ev(1, 2000))
    vm = SimpleNamespace(session=s)
    labels = EventLabels()
    assert asyncio.run(run_command(vm, "flag_event", None, 1, True, labels)) is None
    assert labels.wrong_set(s) == {1}
    assert asyncio.run(run_command(vm, "unflag_event", None, 1, True, labels)) is None
    assert labels.wrong_set(s) == set()


def test_marking_needs_live_test_mode():
    vm = SimpleNamespace(session=session_with(ev(0, 1000)))
    message = asyncio.run(run_command(vm, "flag_event", None, 0, False, None))
    assert "--live-test" in message


def test_marking_rejects_a_bad_or_missing_event():
    s = session_with(ev(0, 1000))
    vm = SimpleNamespace(session=s)
    labels = EventLabels()
    assert "Pick an event" in asyncio.run(run_command(vm, "flag_event", None, None, True, labels))
    assert "Pick an event" in asyncio.run(run_command(vm, "flag_event", None, True, True, labels))
    assert "no longer" in asyncio.run(run_command(vm, "flag_event", None, 9, True, labels))


# ---- saving ----

def test_marks_and_the_live_test_tag_are_saved_with_the_session(tmp_path):
    path = tmp_path / "s.db"
    db = SessionDatabase(path)
    s = session_with(ev(0, 1000), ev(1, 2000), ev(2, 3000))
    db.save_session(s, summary(), live_test=True, wrong_events={1})
    with closing(sqlite3.connect(path)) as conn:
        rows = conn.execute("SELECT t_ms, wrong FROM events ORDER BY t_ms").fetchall()
    assert rows == [(1000, 0), (2000, 1), (3000, 0)]
    assert db.list_sessions()[0]["live_test"] is True


def test_a_normal_session_is_not_tagged_and_has_no_marks(tmp_path):
    path = tmp_path / "s.db"
    db = SessionDatabase(path)
    db.save_session(session_with(ev(0, 1000)), summary())
    with closing(sqlite3.connect(path)) as conn:
        assert conn.execute("SELECT wrong FROM events").fetchall() == [(0,)]
    assert db.list_sessions()[0]["live_test"] is False


def test_an_older_database_gets_the_new_columns_and_keeps_its_rows(tmp_path):
    path = tmp_path / "old.db"
    with closing(sqlite3.connect(path)) as conn:
        conn.executescript("""
            CREATE TABLE sessions (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                started_at TEXT NOT NULL, saved_at TEXT NOT NULL, duration_s REAL NOT NULL,
                kicks INTEGER NOT NULL, events_total INTEGER NOT NULL, peak_g REAL,
                distance_m REAL NOT NULL, avg_speed_mph REAL NOT NULL, max_speed_mph REAL NOT NULL
            );
            CREATE TABLE events (
                session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
                seq INTEGER NOT NULL, t_ms INTEGER NOT NULL, event_class INTEGER NOT NULL,
                peak_accel_cg INTEGER NOT NULL, peak_gyro_dps INTEGER NOT NULL,
                confidence INTEGER NOT NULL, flags INTEGER NOT NULL
            );
            INSERT INTO sessions VALUES (1, '2026-11-01T10:00:00', '2026-11-01T10:05:00', 300.0, 2, 2, 15.0, 100.0, 3.0, 8.0);
            INSERT INTO events VALUES (1, 1, 1000, 1, 1500, 900, 80, 0);
        """)
        conn.commit()

    db = SessionDatabase(path)           # opening it adds the two columns
    old = db.list_sessions()[0]
    assert old["id"] == 1 and old["kicks"] == 2 and old["live_test"] is False
    with closing(sqlite3.connect(path)) as conn:
        assert conn.execute("SELECT t_ms, wrong FROM events").fetchall() == [(1000, 0)]