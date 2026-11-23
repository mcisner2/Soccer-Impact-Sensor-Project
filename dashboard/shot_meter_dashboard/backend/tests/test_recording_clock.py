from ble_central import DashboardViewModel, MockBLEManager
from ble_central.dashboard_state import build_snapshot
from ble_central.models import Event, EventClass, Status, Telemetry
from ble_central.session_store import SessionStore

READY, RECORDING, STOPPED = 3, 4, 5


def status(state, t_ms):
    return Status(state, 0, 0, 4000, 200, t_ms)


def ev(seq, t_ms):
    return Event(EventClass.KICK, seq, t_ms, 1500, 900, 80, 0)


def tel(seq, t_ms, speed_cm_s=100):
    return Telemetry(1, seq, t_ms, 0, 0, speed_cm_s, 8, 12)


def test_recording_status_advances_the_clock():
    s = SessionStore()
    s.ingest(status(RECORDING, 0))
    s.ingest(status(RECORDING, 1000))
    s.ingest(status(RECORDING, 2000))
    assert s.session.recording_ms == 2000


def test_statuses_before_recording_do_not_count():
    s = SessionStore()
    s.ingest(status(READY, 50000))
    assert s.session.recording_ms == 0


def test_clock_stops_when_the_session_stops():
    s = SessionStore()
    s.ingest(status(RECORDING, 0))
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
    s.ingest(status(RECORDING, 0))
    s.ingest(status(RECORDING, 5000))
    s.reset()
    assert s.session.recording_ms == 0


def test_dashboard_timer_freezes_on_stop_and_resets_for_a_new_session():
    vm = DashboardViewModel(MockBLEManager())
    store = vm._store
    store.ingest(status(RECORDING, 0))
    store.ingest(status(STOPPED, 6000))
    store.ingest(status(STOPPED, 9000))
    assert build_snapshot(vm)["session"]["duration_s"] == 6.0
    vm.new_session()
    assert build_snapshot(vm)["session"]["duration_s"] == 0.0


# ---- the real firmware stamps Status and Telemetry with the board's clock (time since power-on) ----

def test_status_and_gps_stamped_with_the_board_clock_are_re_based_to_the_recording():
    s = SessionStore()
    s.ingest(status(READY, 300000))
    s.ingest(status(RECORDING, 312000))     # the recording starts; the board has been on for 312 s
    s.ingest(status(RECORDING, 313000))
    s.ingest(tel(1, 314500))
    s.ingest(ev(1, 2200))                   # events already count from the start of the recording
    assert s.session.latest_status.t_ms == 1000
    assert s.session.telemetry[0].t_ms == 2500
    assert s.session.events[0].t_ms == 2200  # never adjusted
    assert s.session.recording_ms == 2500


def test_clock_still_stops_on_stop_when_the_board_clock_is_large():
    s = SessionStore()
    s.ingest(status(RECORDING, 312000))
    s.ingest(status(STOPPED, 318000))
    s.ingest(status(STOPPED, 330000))
    assert s.session.recording_ms == 6000


def test_a_firmware_that_already_counts_from_zero_is_barely_changed():
    s = SessionStore()
    s.ingest(status(RECORDING, 40))         # the first status goes out a few ms after the start
    s.ingest(status(RECORDING, 1040))
    assert s.session.recording_ms == 1000


def test_a_new_session_forgets_the_old_clock_origin():
    s = SessionStore()
    s.ingest(status(RECORDING, 312000))
    s.reset()
    s.ingest(status(RECORDING, 400000))
    s.ingest(status(RECORDING, 401000))
    assert s.session.recording_ms == 1000


def test_gps_before_any_recording_status_is_left_alone():
    s = SessionStore()
    s.ingest(tel(1, 4000))
    assert s.session.telemetry[0].t_ms == 4000
    assert s.session.recording_ms == 4000


def test_dashboard_shows_session_time_for_gps_stamped_with_the_board_clock():
    vm = DashboardViewModel(MockBLEManager())
    store = vm._store
    store.ingest(status(RECORDING, 500000))
    store.ingest(tel(1, 503000, speed_cm_s=300))
    snap = build_snapshot(vm)
    assert snap["movement"]["max_speed_t_s"] == 3.0
    assert snap["session"]["duration_s"] == 3.0