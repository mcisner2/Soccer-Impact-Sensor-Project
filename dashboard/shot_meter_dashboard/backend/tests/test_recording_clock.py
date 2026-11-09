from ble_central import DashboardViewModel, MockBLEManager
from ble_central.dashboard_state import build_snapshot
from ble_central.models import Event, EventClass, Status, Telemetry
from ble_central.session_store import SessionStore

READY, RECORDING, STOPPED = 3, 4, 5


def status(state, t_ms):
    return Status(state, 0, 0, 4000, 200, t_ms)


def ev(seq, t_ms):
    return Event(EventClass.KICK, seq, t_ms, 1500, 900, 80, 0)


def tel(seq, t_ms):
    return Telemetry(1, seq, t_ms, 0, 0, 100, 8, 12)


def test_recording_status_advances_the_clock():
    s = SessionStore()
    s.ingest(status(RECORDING, 1000))
    s.ingest(status(RECORDING, 2000))
    assert s.session.recording_ms == 2000


def test_statuses_before_recording_do_not_count():
    s = SessionStore()
    s.ingest(status(READY, 50000))
    assert s.session.recording_ms == 0


def test_clock_stops_when_the_session_stops():
    s = SessionStore()
    s.ingest(status(RECORDING, 5000))
    s.ingest(status(STOPPED, 6000))    # the status that reports the stop still counts
    s.ingest(status(STOPPED, 9000))    # later ones (the device clock may keep running) do not
    s.ingest(status(STOPPED, 12000))
    assert s.session.recording_ms == 6000


def test_events_and_telemetry_advance_the_clock_but_never_move_it_back():
    s = SessionStore()
    s.ingest(ev(1, 3000))
    s.ingest(tel(1, 4000))
    assert s.session.recording_ms == 4000
    s.ingest(ev(2, 3500))
    assert s.session.recording_ms == 4000


def test_new_session_starts_the_clock_from_zero():
    s = SessionStore()
    s.ingest(status(RECORDING, 5000))
    s.reset()
    assert s.session.recording_ms == 0


def test_dashboard_timer_freezes_on_stop_and_resets_for_a_new_session():
    vm = DashboardViewModel(MockBLEManager())
    store = vm._store
    store.ingest(status(RECORDING, 5000))
    store.ingest(status(STOPPED, 6000))
    store.ingest(status(STOPPED, 9000))
    assert build_snapshot(vm)["session"]["duration_s"] == 6.0
    vm.new_session()
    assert build_snapshot(vm)["session"]["duration_s"] == 0.0