"""View Model: turns connection state + session data into display-ready values.

The dashboard (whatever UI the team builds -- terminal, desktop, web) only needs to read
the properties below and call the action methods. It never sees Bluetooth, bytes or packets.
"""
from __future__ import annotations

from datetime import datetime
from typing import Callable, List, Optional

from .ble_manager import BLEManaging
from .commands import Command
from .models import ConnectionState, Event, Session, Status, Telemetry
from .packet_parser import PacketParseError, parse
from .session_store import SessionStore

_STATUS_MESSAGES = {
    ConnectionState.IDLE: "Not connected",
    ConnectionState.SCANNING: "Searching for sensor...",
    ConnectionState.CONNECTING: "Connecting...",
    ConnectionState.DISCOVERING_SERVICES: "Setting up connection...",
    ConnectionState.SUBSCRIBED: "Connected to sensor",
    ConnectionState.DISCONNECTED: "Connection lost",
    ConnectionState.RECONNECTING: "Reconnecting...",
    ConnectionState.BLUETOOTH_OFF: "Turn on Bluetooth",
    ConnectionState.UNAUTHORIZED: "Bluetooth permission denied - allow it in system settings",
}


class DashboardViewModel:
    """Wires BLE manager -> parser -> session store, and exposes what the screen needs."""

    def __init__(self, ble_manager: BLEManaging, store: Optional[SessionStore] = None) -> None:
        self._ble = ble_manager
        self._store = store or SessionStore()
        self._subscribers: List[Callable[[], None]] = []

        self._ble.on_state_change(lambda _s: self._notify())
        self._ble.on_raw_packet(self._handle_raw)
        self._ble.on_battery_level(self._store.set_battery_level)
        self._store.subscribe(lambda _sess: self._notify())

    # --- pipeline: raw bytes -> parsed packet -> session ---
    def _handle_raw(self, payload: bytes, _received_at: datetime) -> None:
        try:
            packet = parse(payload)
        except PacketParseError:
            self._store.record_parse_error()  # drop the packet; never guess
            return
        self._store.ingest(packet)

    # --- connection ---
    @property
    def connection_state(self) -> ConnectionState:
        return self._ble.state

    @property
    def status_message(self) -> str:
        return _STATUS_MESSAGES[self._ble.state]

    # --- device status (from the Status packet) ---
    @property
    def session(self) -> Session:
        return self._store.session

    @property
    def latest_status(self) -> Optional[Status]:
        return self._store.session.latest_status

    @property
    def device_state_name(self) -> str:
        s = self.latest_status
        return s.state_name if s else "--"

    @property
    def error_message(self) -> Optional[str]:
        """What the dashboard should tell the user about the sensor's current error/warning, if any."""
        s = self.latest_status
        return s.error_message if s else None

    @property
    def battery_mv(self) -> Optional[int]:
        s = self.latest_status
        return s.battery_mv if s else None

    @property
    def temperature_c(self) -> Optional[float]:
        s = self.latest_status
        return s.temperature_c if s else None

    @property
    def calibration_stale(self) -> bool:
        s = self.latest_status
        return bool(s and s.calibration_stale)

    @property
    def gps_degraded(self) -> bool:
        s = self.latest_status
        return bool(s and s.gps_degraded)

    @property
    def low_battery(self) -> bool:
        s = self.latest_status
        return bool(s and s.low_battery)

    @property
    def raw_stream_on(self) -> bool:
        s = self.latest_status
        return bool(s and s.raw_stream_on)

    # --- battery (standard Battery Level characteristic) ---
    @property
    def battery_percentage(self) -> Optional[int]:
        return self._store.session.battery_level

    # --- events / GPS ---
    @property
    def recent_events(self) -> List[Event]:
        return self._store.session.events[-5:]

    @property
    def latest_telemetry(self) -> Optional[Telemetry]:
        t = self._store.session.telemetry
        return t[-1] if t else None

    # --- data quality ---
    @property
    def missed_packet_count(self) -> int:
        return self._store.session.missed_packet_count

    @property
    def parse_error_count(self) -> int:
        return self._store.session.parse_error_count

    # --- actions ---
    async def connect_to_sensor(self) -> None:
        await self._ble.start_scanning()

    async def disconnect_from_sensor(self) -> None:
        await self._ble.disconnect()

    async def start_calibration(self) -> None:
        await self._ble.send_command(Command.START_CAL)

    async def start_session(self) -> None:
        # The firmware may restart its sequence counters for a new recording; forget the old ones
        # BEFORE asking, so the first events of the new session are not mistaken for duplicates.
        self._store.reset_sequence_tracking()
        await self._ble.send_command(Command.START_SESSION)

    async def stop_session(self) -> None:
        await self._ble.send_command(Command.STOP_SESSION)

    async def recalibrate(self) -> None:
        await self._ble.send_command(Command.RECAL)

    async def set_raw_stream(self, enabled: bool) -> None:
        await self._ble.send_command(Command.RAW_STREAM_ON if enabled else Command.RAW_STREAM_OFF)

    def new_session(self) -> None:
        """Clear the app-side session data (does not command the sensor)."""
        self._store.reset()

    # --- change notification for the UI ---
    def subscribe(self, callback: Callable[[], None]) -> None:
        self._subscribers.append(callback)

    def _notify(self) -> None:
        for cb in list(self._subscribers):
            cb()
