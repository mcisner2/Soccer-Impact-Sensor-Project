"""Session Store: owns the current Session and detects missing packets.

Event, Telemetry and Raw each have their OWN sequence counter on the firmware, so each
gets its own tracker here (one shared counter would report false gaps). Status has no
sequence number, so it is never tracked.
"""
from __future__ import annotations

from datetime import datetime
from typing import Callable, Dict, List, Optional, Tuple

from .models import STATE_RECORDING, Event, Packet, RawBatch, Session, Status, Stream, Telemetry

_SEQ_MOD = 0x10000  # sequence numbers are UInt16 and wrap at 65535


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
            # A new recording may restart the firmware's sequence counters. Without this, a counter
            # that restarts at 0 would look "behind" and every event would be dropped as a duplicate.
            previous = s.latest_status
            if packet.state == STATE_RECORDING and (previous is None or previous.state != STATE_RECORDING):
                self.reset_sequence_tracking()
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
        self._notify()

    def _notify(self) -> None:
        for cb in list(self._subscribers):
            cb(self._session)