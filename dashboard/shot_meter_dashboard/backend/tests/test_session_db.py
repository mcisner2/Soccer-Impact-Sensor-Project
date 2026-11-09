import sqlite3
from contextlib import closing

from ble_central.models import Event, EventClass, Session, Telemetry
from ble_central.session_db import SessionDatabase


def make_session(n_events=2, n_telemetry=3):
    s = Session()
    for i in range(1, n_events + 1):
        s.events.append(Event(EventClass.KICK, i, i * 1000, 1500, 900, 80, 0))
    for i in range(1, n_telemetry + 1):
        s.telemetry.append(Telemetry(1, i, i * 1000, 411234567, -862345678, 250, 8, 12))
    return s


def summary(kicks=2, peak_g=15.0):
    return {
        "session": {"duration_s": 12.5, "kicks": kicks, "events_total": kicks, "peak_g": peak_g},
        "movement": {"distance_m": 40.0, "avg_speed_mph": 3.0, "max_speed_mph": 8.0},
    }


def count(path, table):
    with closing(sqlite3.connect(path)) as conn:
        return conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]


def test_saved_session_is_listed_with_its_summary(tmp_path):
    db = SessionDatabase(tmp_path / "s.db")
    session_id = db.save_session(make_session(), summary())
    rows = db.list_sessions()
    assert len(rows) == 1
    row = rows[0]
    assert row["id"] == session_id
    assert row["kicks"] == 2 and row["events_total"] == 2
    assert row["duration_s"] == 12.5 and row["peak_g"] == 15.0
    assert row["distance_m"] == 40.0 and row["max_speed_mph"] == 8.0
    assert len(row["started_at"]) == 19 and len(row["saved_at"]) == 19


def test_sessions_are_listed_newest_first(tmp_path):
    db = SessionDatabase(tmp_path / "s.db")
    first = db.save_session(make_session(), summary())
    second = db.save_session(make_session(), summary())
    assert [r["id"] for r in db.list_sessions()] == [second, first]


def test_events_and_gps_readings_are_stored(tmp_path):
    path = tmp_path / "s.db"
    db = SessionDatabase(path)
    db.save_session(make_session(n_events=2, n_telemetry=3), summary())
    assert count(path, "events") == 2
    assert count(path, "telemetry") == 3


def test_discarding_removes_the_session_and_its_rows(tmp_path):
    path = tmp_path / "s.db"
    db = SessionDatabase(path)
    session_id = db.save_session(make_session(), summary())
    assert db.delete_session(session_id) is True
    assert db.list_sessions() == []
    assert count(path, "events") == 0 and count(path, "telemetry") == 0


def test_discarding_an_unknown_id_returns_false(tmp_path):
    db = SessionDatabase(tmp_path / "s.db")
    assert db.delete_session(999) is False


def test_save_once_saves_a_session_only_once(tmp_path):
    db = SessionDatabase(tmp_path / "s.db")
    session = make_session()
    assert db.save_once(session, summary()) is not None
    assert db.save_once(session, summary()) is None  # pressing Stop twice
    assert len(db.list_sessions()) == 1
    assert db.save_once(make_session(), summary()) is not None  # a new session saves again
    assert len(db.list_sessions()) == 2


def test_empty_session_is_not_saved(tmp_path):
    db = SessionDatabase(tmp_path / "s.db")
    assert db.save_once(Session(), summary(kicks=0, peak_g=None)) is None
    assert db.list_sessions() == []


def test_version_changes_on_save_and_discard_only(tmp_path):
    db = SessionDatabase(tmp_path / "s.db")
    v0 = db.version
    session_id = db.save_session(make_session(), summary())
    assert db.version == v0 + 1
    db.delete_session(999)  # nothing deleted, so nothing changed
    assert db.version == v0 + 1
    db.delete_session(session_id)
    assert db.version == v0 + 2


def test_saved_sessions_survive_reopening_the_database(tmp_path):
    path = tmp_path / "s.db"
    SessionDatabase(path).save_session(make_session(), summary())
    assert len(SessionDatabase(path).list_sessions()) == 1