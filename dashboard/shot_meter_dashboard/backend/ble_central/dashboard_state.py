"""Dashboard state: turns the view model into one JSON-friendly dict for the front end.

Per the Interface Specification (6.4), total distance, average / max speed and similar
numbers are computed HERE (on the dashboard side) from telemetry, so the firmware stays simple.

Assumptions (tune with real data):
  * distance = sum(speed * dt) from the GPS speed reading (smoother than differencing positions,
    which jitters when standing still). Gaps longer than MAX_GAP_S between samples are skipped.
  * the sensor does NOT classify activity. The Standing / Walking / Running split is ESTIMATED
    from GPS speed using the thresholds below.
"""
from __future__ import annotations

from typing import Any, Dict, List, Optional

from .models import ConnectionState, EventClass
from .view_model import DashboardViewModel

MPS_TO_MPH = 2.2369362920544
STANDING_MAX_MPS = 0.5   # below this: standing
WALKING_MAX_MPS = 2.2    # below this: walking; at/above: running
MAX_GAP_S = 5.0          # ignore longer gaps between GPS samples (disconnects, lost fix)
MAX_SERIES_POINTS = 60
MAX_EVENTS_SENT = 30


def movement_zone(speed_m_s: float) -> str:
    if speed_m_s < STANDING_MAX_MPS:
        return "standing"
    if speed_m_s < WALKING_MAX_MPS:
        return "walking"
    return "running"


def _downsample(points: List[Dict[str, float]], limit: int) -> List[Dict[str, float]]:
    if len(points) <= limit:
        return points
    step = (len(points) - 1) / (limit - 1)
    return [points[round(i * step)] for i in range(limit)]


def build_snapshot(vm: DashboardViewModel) -> Dict[str, Any]:
    session = vm.session
    status = vm.latest_status

    # ---- GPS-derived numbers (only samples that have a fix) ----
    fixed = [t for t in session.telemetry if t.has_fix]
    distance_m = 0.0
    zone_s = {"standing": 0.0, "walking": 0.0, "running": 0.0}
    speed_series: List[Dict[str, float]] = []
    max_speed, max_speed_t = 0.0, None
    prev = None
    for t in fixed:
        v = t.speed_m_s
        speed_series.append({"t_s": t.t_ms / 1000.0, "v_m_s": v})
        if max_speed_t is None or v > max_speed:
            max_speed, max_speed_t = v, t.t_ms / 1000.0
        if prev is not None:
            dt = (t.t_ms - prev.t_ms) / 1000.0
            if 0 < dt <= MAX_GAP_S:
                distance_m += prev.speed_m_s * dt
                zone_s[movement_zone(prev.speed_m_s)] += dt
        prev = t
    avg_speed = (sum(p["v_m_s"] for p in speed_series) / len(speed_series)) if speed_series else 0.0
    moved_s = sum(zone_s.values())

    latest_tel = vm.latest_telemetry
    current_zone = movement_zone(latest_tel.speed_m_s) if latest_tel and latest_tel.has_fix else None

    # ---- events ----
    events = session.events
    kicks = [e for e in events if e.event_class == EventClass.KICK]
    latest_event = events[-1] if events else None
    peak_event = max(events, key=lambda e: e.peak_accel_centi_g) if events else None

    # ---- session clock: the device reports t_ms since the recording started ----
    t_candidates = [0]
    if status:
        t_candidates.append(status.t_ms)
    if events:
        t_candidates.append(events[-1].t_ms)
    if latest_tel:
        t_candidates.append(latest_tel.t_ms)
    duration_s = max(t_candidates) / 1000.0

    def event_dict(e) -> Dict[str, Any]:
        return {
            "seq": e.sequence_number, "t_s": e.t_ms / 1000.0, "class": e.event_class,
            "name": e.event_class_name, "peak_g": e.peak_accel_g, "peak_dps": e.peak_gyro_dps,
            "confidence": e.confidence,
        }

    return {
        "connection": {
            "state": vm.connection_state.value,
            "message": vm.status_message,
            "connected": vm.connection_state == ConnectionState.SUBSCRIBED,
        },
        "device": {
            "state": status.state_name if status else None,
            "recording": bool(status and status.state_name == "RECORDING"),
            "error_code": status.error_code if status else 0,
            "error_name": status.error_name if status else "NONE",
            "error_message": vm.error_message,
            "battery_pct": vm.battery_percentage,
            "battery_mv": vm.battery_mv,
            "low_battery": vm.low_battery,
            "temperature_c": vm.temperature_c,
            "calibration_stale": vm.calibration_stale,
            "gps_degraded": vm.gps_degraded,
            "raw_stream_on": vm.raw_stream_on,
        },
        "session": {
            "started_at": session.started_at.isoformat(timespec="seconds"),
            "duration_s": duration_s,
            "kicks": len(kicks),
            "events_total": len(events),
            "kicks_per_min": (len(kicks) / (duration_s / 60.0)) if duration_s >= 30 else None,
            "peak_g": peak_event.peak_accel_g if peak_event else None,
            "peak_event": event_dict(peak_event) if peak_event else None,
            "latest_event": event_dict(latest_event) if latest_event else None,
            "events": [event_dict(e) for e in events[-MAX_EVENTS_SENT:]][::-1],  # newest first
        },
        "gps": {
            "has_fix": bool(latest_tel and latest_tel.has_fix),
            "fix_name": latest_tel.fix_name if latest_tel else None,
            "satellites": latest_tel.satellites if latest_tel else None,
            "hdop": latest_tel.hdop if latest_tel else None,
            "speed_m_s": latest_tel.speed_m_s if latest_tel and latest_tel.has_fix else None,
            "speed_mph": latest_tel.speed_m_s * MPS_TO_MPH if latest_tel and latest_tel.has_fix else None,
            "zone": current_zone,
        },
        "movement": {
            "distance_m": distance_m,
            "distance_km": distance_m / 1000.0,
            "avg_speed_mph": avg_speed * MPS_TO_MPH,
            "max_speed_mph": max_speed * MPS_TO_MPH,
            "max_speed_t_s": max_speed_t,
            "seconds": zone_s,
            "percent": {z: (100.0 * s / moved_s if moved_s > 0 else 0.0) for z, s in zone_s.items()},
            "series": [{"t_s": p["t_s"], "mph": p["v_m_s"] * MPS_TO_MPH}
                       for p in _downsample(speed_series, MAX_SERIES_POINTS)],
            "estimated_from_gps_speed": True,
        },
        "quality": {
            "missed_packets": vm.missed_packet_count,
            "bad_packets": vm.parse_error_count,
            "missed_by_stream": {s.value: n for s, n in session.missed_by_stream.items()},
        },
    }
