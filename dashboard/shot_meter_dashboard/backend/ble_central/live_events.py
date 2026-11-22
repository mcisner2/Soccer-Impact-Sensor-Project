"""Live-test support: a full, timestamped event log for the live page, and the 'wrongly flagged' marks.

The marks are kept per session in memory and written to the database when the session is saved.
They are keyed by the event's position in the session's event list (0 = first event), which is
unique even when the firmware restarts its sequence numbers.
"""
from __future__ import annotations

from datetime import timedelta
from typing import Any, Dict, List, Optional, Set

from .models import Session


def build_live_events(session: Session, wrong: Optional[Set[int]] = None) -> List[Dict[str, Any]]:
    """Every event in the session, oldest first, with the times the live page shows."""
    wrong = wrong or set()
    out: List[Dict[str, Any]] = []
    for idx, e in enumerate(session.events):
        # The device clock restarts with each recording and the app-side session starts at about the
        # same moment, so start + device time is a close estimate of the wall-clock time of the event.
        clock = session.started_at + timedelta(milliseconds=e.t_ms)
        out.append({
            "idx": idx,
            "seq": e.sequence_number,
            "t_ms": e.t_ms,
            "t_s": e.t_ms / 1000.0,
            "clock": clock.isoformat(timespec="milliseconds"),
            "class": int(e.event_class),
            "name": e.event_class_name,
            "peak_g": e.peak_accel_g,
            "peak_dps": e.peak_gyro_dps,
            "confidence": e.confidence,
            "wrong": idx in wrong,
        })
    return out


class EventLabels:
    """Which events of the current session were marked as wrongly flagged."""

    def __init__(self) -> None:
        self._session: Optional[Session] = None
        self._wrong: Set[int] = set()

    def wrong_set(self, session: Session) -> Set[int]:
        # A new session object (new_session / reset) starts with no marks.
        if session is not self._session:
            self._session = session
            self._wrong = set()
        return self._wrong

    def set_wrong(self, session: Session, idx: int, wrong: bool = True) -> bool:
        """Mark (or unmark) one event. Returns False if there is no event at that position."""
        marks = self.wrong_set(session)
        if not 0 <= idx < len(session.events):
            return False
        if wrong:
            marks.add(idx)
        else:
            marks.discard(idx)
        return True