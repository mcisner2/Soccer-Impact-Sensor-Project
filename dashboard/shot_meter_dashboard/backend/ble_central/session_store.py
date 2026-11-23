"""Session Store: owns the current Session and detects missing packets.

Event, Telemetry and Raw each have their OWN sequence counter on the firmware, so each
gets its own tracker here (one shared counter would report false gaps). Status has no
sequence number, so it is never tracked.
"""
from __future__ import annotations

from dataclasses import replace
from datetime import datetime
from typing import Callable, Dict, List, Optional, Tuple, TypeVar

from .models import STATE_RECORDING, Event, Packet, RawBatch, Session, Status, Stream, Telemetry

_SEQ_MOD = 0x10000  # sequence numbers are UInt16 and wrap at 65535

_Stamped = TypeVar("_Stamped", Status, Telemetry)


class SequenceTracker:
    """Tracks one stream's UInt16 sequence counter (wraparound-safe)."""

    def __init__(self) -> None:
        self.last: Optional[int] = None

    def check(self, seq: int) -> Tuple[bool, Optional[Tuple[int, int]], int]:
        """Returns (accepted, gap, missed).

        accepted=False -> duplicate / late packet, drop it.
        gap=(first, last) inclusive, and missed=N, when packets were skipped.
        """
        if self.last is None:
            self.last = seq
            return True, None, 0

        expected = (self.last + 1) % _SEQ_MOD
        skipped = (seq - expected) % _SEQ_MOD

        if skipped == 0:
            self.last = seq
            return True, None, 0
        if skipped >= _SEQ_MOD // 2:
            return False, None, 0  # "behind" what we expected: duplicate or out of order

        self.last = seq
        return True, (expected, (seq - 1) % _SEQ_MOD), skipped


class SessionStore:
    def __init__(self) -> None:
        self._session = Session()
        self._trackers: Dict[Stream, SequenceTracker] = {s: SequenceTracker() for s in Stream}
        self._subscribers: List[Callable[[Session], None]] = []
        # What the sensor's clock read on the Status packet that started the current recording.
        # See _in_recording_time().
        self._clock_origin_ms: Optional[int] = None

    # --- reading ---
    @property
    def session(self) -> Session:
        return self._session

    def subscribe(self, callback: Callable[[Session], None]) -> None:
        self._subscribers.append(callback)

    # --- writing ---
    def ingest(self, packet: Packet) -> None:
        """Add one parsed packet; record a gap if its stream's sequence numbers skipped."""
        s = self._session

        if isinstance(packet, Status):
            previous = s.latest_status
            entering_recording = packet.state == STATE_RECORDING and (
                previous is None or previous.state != STATE_RECORDING)
            if entering_recording:
                # A new recording may restart the firmware's sequence counters. Without this, a counter
                # that restarts at 0 would look "behind" and every event would be dropped as a duplicate.
                self.reset_sequence_tracking()
                # ...and this Status is "time zero" for the recording's Status and Telemetry stamps.
                self._clock_origin_ms = packet.t_ms
            packet = self._in_recording_time(packet)
            # The session timer only follows RECORDING time. A Status counts while RECORDING, and the
            # one that reports the stop still counts, but the STOPPED / READY ones that keep arriving
            # after it do not (the device clock may keep running, the session has ended).
            if packet.state == STATE_RECORDING or (previous is not None and previous.state == STATE_RECORDING):
                s.recording_ms = max(s.recording_ms, packet.t_ms)
            s.latest_status = packet  # no sequence number on Status
            self._notify()
            return

        if isinstance(packet, Event):
            stream = Stream.EVENT
        elif isinstance(packet, Telemetry):
            stream = Stream.TELEMETRY
            packet = self._in_recording_time(packet)  # GPS packets carry the board's clock, see below
        elif isinstance(packet, RawBatch):
            stream = Stream.RAW
        else:  # pragma: no cover
            raise TypeError(f"unsupported packet type: {type(packet).__name__}")

        accepted, gap, missed = self._trackers[stream].check(packet.sequence_number)
        if not accepted:
            return
        if gap is not None:
            s.sequence_gaps[stream].append(gap)
            s.missed_by_stream[stream] += missed

        if stream is Stream.EVENT:
            s.events.append(packet)
            s.recording_ms = max(s.recording_ms, packet.t_ms)  # events are only sent while RECORDING
        elif stream is Stream.TELEMETRY:
            s.telemetry.append(packet)
            s.recording_ms = max(s.recording_ms, packet.t_ms)  # so is telemetry
        else:
            s.raw_batches.append(packet)
        self._notify()

    def _in_recording_time(self, packet: _Stamped) -> _Stamped:
        """Re-stamp a Status or Telemetry packet so its t_ms counts from the start of the recording.

        The spec says t_ms is milliseconds since the session started, and the mock sensor does that.
        The real firmware stamps Status and Telemetry with the board's own clock (millis(), the time
        since power-on) but stamps Events with the session clock. So the stamp on the Status that
        started the recording is taken as time zero, and subtracted from the later Status and
        Telemetry stamps. A firmware that already counts from zero has a first stamp of about 0, so
        nothing changes for it. Events are never adjusted.
        """
        origin = self._clock_origin_ms
        if origin is None or packet.t_ms < origin:
            return packet
        return replace(packet, t_ms=packet.t_ms - origin)

    def reset_sequence_tracking(self) -> None:
        """Forget the last-seen sequence numbers (keeps all data). Called when a recording starts."""
        self._trackers = {s: SequenceTracker() for s in Stream}

    def set_battery_level(self, level: int) -> None:
        """Standard Battery Level characteristic (one byte, 0-100)."""
        if not 0 <= level <= 100:
            self.record_parse_error()
            return
        self._session.battery_level = level
        self._notify()

    def record_parse_error(self) -> None:
        """Called when the parser rejected a payload; session data is untouched."""
        self._session.parse_error_count += 1
        self._notify()

    def reset(self) -> None:
        """Start a fresh session (e.g. a new drill)."""
        self._session = Session(started_at=datetime.now())
        self._trackers = {s: SequenceTracker() for s in Stream}
        self._clock_origin_ms = None
        self._notify()

    def _notify(self) -> None:
        for cb in list(self._subscribers):
            cb(self._session)