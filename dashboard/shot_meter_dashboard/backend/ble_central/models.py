"""Data models: what the rest of the app works with (matches Ed's interface spec)."""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum, IntEnum
from typing import Dict, List, Optional, Tuple, Union


class ConnectionState(Enum):
    """The nine connection states (architecture doc, Section 2)."""

    IDLE = "idle"
    SCANNING = "scanning"
    CONNECTING = "connecting"
    DISCOVERING_SERVICES = "discoveringServices"
    SUBSCRIBED = "subscribed"
    DISCONNECTED = "disconnected"
    RECONNECTING = "reconnecting"
    BLUETOOTH_OFF = "bluetoothOff"
    UNAUTHORIZED = "unauthorized"


class EventClass(IntEnum):
    """Known event classes.

    CONFLICT TO RESOLVE: the firmware owner's list is 1 kick, 2 pass, 3 trap, 4 ground impact
    (used here). Ed's Interface Specification v0.1 says 0 UNKNOWN, 1 KICK, 2 HARD_IMPACT and
    treats pass/trap as a later refinement. Class 2 therefore means different things in the two
    documents. The parser accepts ANY class value (never drops an event over it); only the
    display names below depend on this table.
    """

    UNKNOWN = 0
    KICK = 1
    PASS = 2
    TRAP = 3
    GROUND_IMPACT = 4


EVENT_CLASS_NAMES: Dict[int, str] = {c.value: c.name for c in EventClass}


class Stream(str, Enum):
    """Streams that carry their own sequence counter. (Status has no sequence number.)"""

    EVENT = "event"
    TELEMETRY = "telemetry"
    RAW = "raw"


# Device state codes from the Status packet (Interface Specification, section 6.2 / 7).
STATE_NAMES: Dict[int, str] = {
    0: "BOOT", 1: "SENSOR_INIT", 2: "CALIBRATING", 3: "READY",
    4: "RECORDING", 5: "STOPPED", 6: "ERROR",
}
STATE_RECORDING = 4

# Error / warning codes (Interface Specification, section 8): code -> (name, dashboard message)
ERROR_INFO: Dict[int, Tuple[str, str]] = {
    0x01: ("NOT_STILL_ACCEL", "Stand still while the sensor calibrates"),
    0x02: ("NOT_STILL_GYRO", "Stand still while the sensor calibrates"),
    0x03: ("GYRO_BIAS_OUT_OF_RANGE", "Sensor fault"),
    0x04: ("GRAVITY_OUT_OF_RANGE", "Sensor fault"),
    0x10: ("ERR_CAL_FAILED", "Calibration failed - try recalibrating"),
    0x11: ("ERR_NOT_CALIBRATED", "Calibrate the sensor before starting a session"),
    0x20: ("ERR_IMU_COMM", "Sensor communication problem - data may have gaps"),
    0x21: ("ERR_IMU_FIFO_OVERFLOW", "Sensor data gap in the timeline"),
    0x30: ("WARN_GPS_NO_FIX", "No GPS fix - distance and speed are low-confidence"),
    0x31: ("WARN_GPS_POOR_ACCURACY", "GPS accuracy is poor - distance and speed are low-confidence"),
    0x40: ("WARN_CAL_STALE", "Sensor has drifted - recalibration suggested"),
    0x50: ("WARN_LOW_BATTERY", "Low battery"),
    0x51: ("ERR_BATTERY_CRITICAL", "Session ended - battery critical"),
    0x60: ("WARN_NO_EVENTS", "No events detected for 10 minutes - check sensor mounting"),
}

FIX_NAMES: Dict[int, str] = {0: "none", 1: "GPS", 2: "DGPS/WAAS"}


@dataclass(frozen=True)
class Status:
    """Status packet (0x01): device state, flags, battery voltage, temperature."""

    state: int
    error_code: int
    flags: int
    battery_mv: int
    temperature_tenths_c: int
    t_ms: int

    @property
    def state_name(self) -> str:
        return STATE_NAMES.get(self.state, f"STATE_{self.state}")

    @property
    def error_name(self) -> str:
        if self.error_code == 0:
            return "NONE"
        return ERROR_INFO.get(self.error_code, (f"ERROR_0x{self.error_code:02x}", ""))[0]

    @property
    def error_message(self) -> Optional[str]:
        """Plain-language message for the dashboard, or None when there is no error."""
        if self.error_code == 0:
            return None
        return ERROR_INFO.get(self.error_code, ("", f"Sensor reported error 0x{self.error_code:02x}"))[1]

    @property
    def temperature_c(self) -> float:
        return self.temperature_tenths_c / 10.0

    @property
    def calibration_stale(self) -> bool:
        return bool(self.flags & 0x01)

    @property
    def gps_degraded(self) -> bool:
        return bool(self.flags & 0x02)

    @property
    def low_battery(self) -> bool:
        return bool(self.flags & 0x04)

    @property
    def raw_stream_on(self) -> bool:
        return bool(self.flags & 0x08)


@dataclass(frozen=True)
class Event:
    """Event packet (0x02): a detected kick / pass / trap / ground impact."""

    event_class: int  # see EventClass; unknown values are kept, not rejected
    sequence_number: int
    t_ms: int
    peak_accel_centi_g: int  # hundredths of a g (1523 = 15.23 g)
    peak_gyro_dps: int
    confidence: int  # 0-100
    flags: int

    @property
    def event_class_name(self) -> str:
        return EVENT_CLASS_NAMES.get(self.event_class, f"CLASS_{self.event_class}")

    @property
    def peak_accel_g(self) -> float:
        return self.peak_accel_centi_g / 100.0


@dataclass(frozen=True)
class Telemetry:
    """Telemetry packet (0x03): GPS position and speed."""

    fix_quality: int
    sequence_number: int
    t_ms: int
    lat_e7: int  # degrees x 10^7
    lon_e7: int  # degrees x 10^7
    speed_cm_s: int
    satellites: int
    hdop_x10: int

    @property
    def has_fix(self) -> bool:
        """fix_quality 0 means no fix: latitude/longitude are not meaningful."""
        return self.fix_quality > 0

    @property
    def fix_name(self) -> str:
        return FIX_NAMES.get(self.fix_quality, f"FIX_{self.fix_quality}")

    @property
    def latitude(self) -> float:
        return self.lat_e7 / 1e7

    @property
    def longitude(self) -> float:
        return self.lon_e7 / 1e7

    @property
    def speed_m_s(self) -> float:
        return self.speed_cm_s / 100.0

    @property
    def hdop(self) -> float:
        return self.hdop_x10 / 10.0


@dataclass(frozen=True)
class RawSample:
    """One IMU sample: accel x/y/z, gyro x/y/z, temp (all signed 16-bit, raw units)."""

    ax: int
    ay: int
    az: int
    gx: int
    gy: int
    gz: int
    temp: int


@dataclass(frozen=True)
class RawBatch:
    """Raw packet (0x04): a variable-length batch of samples (length depends on BLE MTU)."""

    sequence_number: int
    t_ms: int  # timestamp of the FIRST sample in the batch
    samples: Tuple[RawSample, ...]


Packet = Union[Status, Event, Telemetry, RawBatch]


@dataclass
class Session:
    """Everything received since the session started. Single source of truth."""

    started_at: datetime = field(default_factory=datetime.now)
    latest_status: Optional[Status] = None
    battery_level: Optional[int] = None  # standard Battery Level characteristic, 0-100
    events: List[Event] = field(default_factory=list)
    telemetry: List[Telemetry] = field(default_factory=list)
    raw_batches: List[RawBatch] = field(default_factory=list)
    # Per stream: list of (first_missing_seq, last_missing_seq), inclusive; may wrap past 65535.
    sequence_gaps: Dict[Stream, List[Tuple[int, int]]] = field(
        default_factory=lambda: {s: [] for s in Stream}
    )
    missed_by_stream: Dict[Stream, int] = field(default_factory=lambda: {s: 0 for s in Stream})
    parse_error_count: int = 0
    # Device time (ms since the recording started) up to which this session has been RECORDING.
    # The dashboard's timer reads this, so it stops when the session stops and starts from 0 for a
    # new session. Status packets that arrive after Stop do not move it.
    recording_ms: int = 0

    @property
    def missed_packet_count(self) -> int:
        return sum(self.missed_by_stream.values())