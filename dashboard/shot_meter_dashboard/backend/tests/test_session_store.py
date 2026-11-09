from ble_central.models import Event, EventClass, RawBatch, Status, Stream, Telemetry
from ble_central.session_store import SequenceTracker, SessionStore


def ev(seq):
    return Event(EventClass.KICK, seq, seq * 10, 1500, 900, 80, 0)


def tel(seq):
    return Telemetry(1, seq, seq * 10, 0, 0, 100, 8, 12)


def raw(seq):
    return RawBatch(seq, seq * 10, ())


def status():
    return Status(3, 0, 0, 4000, 200, 1)


def test_in_order_packets_no_gaps():
    s = SessionStore()
    for i in range(1, 6):
        s.ingest(ev(i))
    assert len(s.session.events) == 5 and s.session.missed_packet_count == 0


def test_streams_have_independent_counters_no_false_gaps():
    """The old single-counter bug: interleaved streams with different counters looked like gaps."""
    s = SessionStore()
    # event seq 1,2,3 ; telemetry seq 100,101,102 ; raw seq 5000,5001 -- interleaved
    for e, t, r in [(1, 100, 5000), (2, 101, 5001), (3, 102, None)]:
        s.ingest(ev(e))
        s.ingest(tel(t))
        if r is not None:
            s.ingest(raw(r))
    assert s.session.missed_packet_count == 0
    assert all(g == [] for g in s.session.sequence_gaps.values())
    assert (len(s.session.events), len(s.session.telemetry), len(s.session.raw_batches)) == (3, 3, 2)


def test_gap_in_one_stream_only_affects_that_stream():
    s = SessionStore()
    for seq in (1, 2, 6):  # events 3,4,5 missing
        s.ingest(ev(seq))
    for seq in (1, 2, 3):
        s.ingest(tel(seq))
    assert s.session.sequence_gaps[Stream.EVENT] == [(3, 5)]
    assert s.session.missed_by_stream[Stream.EVENT] == 3
    assert s.session.missed_by_stream[Stream.TELEMETRY] == 0
    assert s.session.missed_by_stream[Stream.RAW] == 0
    assert s.session.missed_packet_count == 3
    assert len(s.session.events) == 3  # processing continues normally


def test_gap_across_uint16_wraparound():
    for make, stream in ((ev, Stream.EVENT), (tel, Stream.TELEMETRY), (raw, Stream.RAW)):
        s = SessionStore()
        s.ingest(make(65534))
        s.ingest(make(1))  # 65535 and 0 missing
        assert s.session.sequence_gaps[stream] == [(65535, 0)]
        assert s.session.missed_by_stream[stream] == 2


def test_normal_wraparound_is_not_a_gap():
    s = SessionStore()
    s.ingest(ev(65535))
    s.ingest(ev(0))
    assert s.session.missed_packet_count == 0


def test_duplicate_packet_dropped():
    s = SessionStore()
    s.ingest(ev(1))
    s.ingest(ev(2))
    s.ingest(ev(2))
    assert len(s.session.events) == 2 and s.session.missed_packet_count == 0


def test_status_has_no_sequence_and_is_never_a_gap():
    s = SessionStore()
    for _ in range(5):
        s.ingest(status())
    assert s.session.latest_status is not None
    assert s.session.missed_packet_count == 0


def test_battery_level_stored_and_range_checked():
    s = SessionStore()
    s.set_battery_level(87)
    assert s.session.battery_level == 87
    s.set_battery_level(150)  # out of 0-100: ignored and counted as a bad packet
    assert s.session.battery_level == 87 and s.session.parse_error_count == 1


def test_parse_error_counted_and_session_untouched():
    s = SessionStore()
    s.ingest(ev(1))
    s.record_parse_error()
    assert s.session.parse_error_count == 1 and len(s.session.events) == 1


def test_subscribers_notified_and_reset_clears_everything():
    s = SessionStore()
    calls = []
    s.subscribe(lambda sess: calls.append(len(sess.events)))
    s.ingest(ev(1))
    s.reset()
    assert calls == [1, 0]
    assert s.session.events == []
    s.ingest(ev(500))  # after reset the first packet never counts as a gap
    assert s.session.missed_packet_count == 0


def test_tracker_runs_per_stream():
    a, b = SequenceTracker(), SequenceTracker()
    assert a.check(10) == (True, None, 0)
    assert b.check(500) == (True, None, 0)
    assert a.check(11) == (True, None, 0)
    assert b.check(503) == (True, (501, 502), 2)


# ---- new recording: firmware may restart its sequence counters ----

def test_restarted_counter_after_reset_is_not_dropped_as_duplicate():
    s = SessionStore()
    for q in range(1, 501):
        s.ingest(ev(q))
    before = len(s.session.events)
    s.reset_sequence_tracking()                    # what start_session() does
    for q in range(0, 10):
        s.ingest(ev(q))
    assert len(s.session.events) - before == 10    # all accepted
    assert s.session.missed_packet_count == 0
    assert len(s.session.events) == 510            # earlier data kept


def test_status_entering_recording_resets_tracking():
    s = SessionStore()
    s.ingest(Status(3, 0, 0, 4000, 200, 1))        # READY
    for q in range(1, 301):
        s.ingest(ev(q))
    s.ingest(Status(4, 0, 0, 4000, 200, 2))        # -> RECORDING
    s.ingest(ev(0))
    s.ingest(ev(1))
    assert len(s.session.events) == 302 and s.session.missed_packet_count == 0


def test_status_already_recording_does_not_reset_tracking():
    s = SessionStore()
    s.ingest(Status(4, 0, 0, 4000, 200, 1))        # first status: RECORDING
    s.ingest(ev(1)); s.ingest(ev(2))
    s.ingest(Status(4, 0, 0, 4000, 200, 2))        # 1 Hz status while still recording: no reset
    s.ingest(ev(2))                                # duplicate must still be dropped
    s.ingest(ev(5))                                # real gap must still be seen
    assert len(s.session.events) == 3
    assert s.session.sequence_gaps[Stream.EVENT] == [(3, 4)]
