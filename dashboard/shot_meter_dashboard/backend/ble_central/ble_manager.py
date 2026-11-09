"""BLE Manager: owns the Bluetooth connection and its 9-state state machine.

The ONLY module that talks to a Bluetooth library (bleak). Everything else sees:
  - connection-state changes
  - raw (bytes, arrival_time) payloads from Status / Event / Telemetry / RawStream
  - the standard Battery Level (0-100)
and can send commands to the Control characteristic.

Subscriptions: Status, Event, Telemetry as soon as connected. RawStream ONLY while the
raw stream is turned on (subscribed just before RAW_STREAM_ON is written).
"""
from __future__ import annotations

import asyncio
from datetime import datetime
from typing import Callable, List, Optional, Protocol

from .commands import Command, encode_command
from .constants import (
    BATTERY_LEVEL_UUID,
    CONTROL_UUID,
    DEVICE_NAME,
    EVENT_UUID,
    MAX_RECONNECT_ATTEMPTS,
    RAW_STREAM_UUID,
    RECONNECT_DELAY_S,
    REQUIRED_MTU,
    SERVICE_UUID,
    STATUS_UUID,
    TELEMETRY_UUID,
)
from .models import ConnectionState

StateListener = Callable[[ConnectionState], None]
BytesListener = Callable[[bytes, datetime], None]
BatteryListener = Callable[[int], None]


class NotConnectedError(Exception):
    """A command was sent while not subscribed to the sensor."""


class BLEManaging(Protocol):
    """What the rest of the app is allowed to ask of a BLE manager (arch doc, Section 6)."""

    @property
    def state(self) -> ConnectionState: ...
    def on_state_change(self, listener: StateListener) -> None: ...
    def on_raw_packet(self, listener: BytesListener) -> None: ...
    def on_battery_level(self, listener: BatteryListener) -> None: ...
    async def start_scanning(self) -> None: ...
    async def stop_scanning(self) -> None: ...
    async def disconnect(self) -> None: ...
    async def send_command(self, command: Command, arg: int = 0) -> None: ...


class BaseBLEManager:
    """State + listener plumbing shared by the real and the mock manager."""

    def __init__(self) -> None:
        self._state = ConnectionState.IDLE
        self._state_listeners: List[StateListener] = []
        self._bytes_listeners: List[BytesListener] = []
        self._battery_listeners: List[BatteryListener] = []

    @property
    def state(self) -> ConnectionState:
        return self._state

    def on_state_change(self, listener: StateListener) -> None:
        self._state_listeners.append(listener)

    def on_raw_packet(self, listener: BytesListener) -> None:
        self._bytes_listeners.append(listener)

    def on_battery_level(self, listener: BatteryListener) -> None:
        self._battery_listeners.append(listener)

    def _set_state(self, new_state: ConnectionState) -> None:
        if new_state == self._state:
            return
        self._state = new_state
        for cb in list(self._state_listeners):
            cb(new_state)

    def _emit_bytes(self, payload: bytes) -> None:
        now = datetime.now()
        for cb in list(self._bytes_listeners):
            cb(payload, now)

    def _emit_battery(self, level: int) -> None:
        for cb in list(self._battery_listeners):
            cb(level)


def _classify_error(exc: Exception) -> Optional[ConnectionState]:
    """Best-effort: map a bleak error message to bluetoothOff / unauthorized."""
    msg = str(exc).lower()
    if any(k in msg for k in ("not authorized", "permission", "denied", "unauthorized")):
        return ConnectionState.UNAUTHORIZED
    if any(k in msg for k in ("powered off", "turned off", "no bluetooth adapter",
                              "adapter", "bluetooth device is turned off")):
        return ConnectionState.BLUETOOTH_OFF
    return None


class BleakBLEManager(BaseBLEManager):
    """Real Bluetooth, via the `bleak` library (works on Windows, macOS, Linux)."""

    def __init__(
        self,
        max_attempts: int = MAX_RECONNECT_ATTEMPTS,
        reconnect_delay_s: float = RECONNECT_DELAY_S,
        scan_timeout_s: float = 10.0,
    ) -> None:
        super().__init__()
        self._max_attempts = max_attempts
        self._reconnect_delay_s = reconnect_delay_s
        self._scan_timeout_s = scan_timeout_s
        self._task: Optional[asyncio.Task] = None
        self._user_stopped = False
        self._lost = asyncio.Event()
        self._client = None
        self._raw_subscribed = False
        self.mtu: Optional[int] = None

    @property
    def mtu_ok(self) -> bool:
        """False if the negotiated MTU is below the spec's 185 (packets might not fit in one notify)."""
        return self.mtu is None or self.mtu >= REQUIRED_MTU

    async def start_scanning(self) -> None:
        if self._task and not self._task.done():
            return
        self._user_stopped = False
        self._task = asyncio.create_task(self._run())

    async def stop_scanning(self) -> None:
        await self.disconnect()

    async def disconnect(self) -> None:
        self._user_stopped = True
        self._lost.set()
        if self._task:
            try:
                await asyncio.wait_for(self._task, timeout=self._scan_timeout_s + 5)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                self._task.cancel()
        self._set_state(ConnectionState.IDLE)

    # --- commands (Control characteristic) ---
    async def send_command(self, command: Command, arg: int = 0) -> None:
        """Write a 2-byte command to Control. Raises NotConnectedError if not subscribed."""
        payload = encode_command(command, arg)  # validates the argument too
        client = self._client
        if self._state != ConnectionState.SUBSCRIBED or client is None:
            raise NotConnectedError(f"cannot send {Command(command).name}: not connected to sensor")

        subscribed_now = False
        if command == Command.RAW_STREAM_ON and not self._raw_subscribed:
            # Subscribe first so the very first raw packets are not missed.
            await client.start_notify(RAW_STREAM_UUID, self._on_notify)
            self._raw_subscribed = subscribed_now = True
        try:
            await client.write_gatt_char(CONTROL_UUID, payload, response=True)
        except Exception:
            if subscribed_now:
                await self._unsubscribe_raw(client)
            raise
        if command == Command.RAW_STREAM_OFF and self._raw_subscribed:
            await self._unsubscribe_raw(client)

    async def _unsubscribe_raw(self, client) -> None:
        try:
            await client.stop_notify(RAW_STREAM_UUID)
        finally:
            self._raw_subscribed = False

    def _on_notify(self, _characteristic, data) -> None:
        self._emit_bytes(bytes(data))

    # --- the state machine loop ---
    async def _run(self) -> None:
        from bleak import BleakClient, BleakScanner  # imported here so tests/mock don't need bleak
        from bleak.exc import BleakError

        attempts = 0
        while not self._user_stopped:
            # idle/reconnecting -> scanning
            self._set_state(ConnectionState.SCANNING)
            try:
                device = await BleakScanner.find_device_by_filter(
                    lambda d, adv: (SERVICE_UUID.lower() in [u.lower() for u in (adv.service_uuids or [])])
                    or (d.name == DEVICE_NAME or adv.local_name == DEVICE_NAME),
                    timeout=self._scan_timeout_s,
                )
            except (BleakError, OSError) as exc:
                special = _classify_error(exc)
                if special == ConnectionState.UNAUTHORIZED:
                    self._set_state(ConnectionState.UNAUTHORIZED)
                    return  # retrying cannot fix a permission problem
                if special == ConnectionState.BLUETOOTH_OFF:
                    self._set_state(ConnectionState.BLUETOOTH_OFF)
                    await asyncio.sleep(2.0)  # auto-resume once Bluetooth is back on
                    continue
                raise
            if device is None:
                continue  # not found yet: keep scanning

            # scanning -> connecting -> discoveringServices -> subscribed
            self._set_state(ConnectionState.CONNECTING)
            self._lost = asyncio.Event()
            self._raw_subscribed = False
            client = BleakClient(device, disconnected_callback=lambda _c: self._lost.set())
            self._client = client
            try:
                await client.connect()
                self._set_state(ConnectionState.DISCOVERING_SERVICES)
                await self._subscribe_all(client, BleakError)
                self._set_state(ConnectionState.SUBSCRIBED)
                attempts = 0
                await self._lost.wait()  # stay subscribed until dropped or user stops
            except (BleakError, OSError, asyncio.TimeoutError):
                pass
            finally:
                self._client = None
                self._raw_subscribed = False
                try:
                    if client.is_connected:
                        await client.disconnect()
                except Exception:
                    pass

            if self._user_stopped:
                break

            # dropped / failed to connect -> disconnected -> reconnecting (or give up -> idle)
            self._set_state(ConnectionState.DISCONNECTED)
            attempts += 1
            if attempts > self._max_attempts:
                self._set_state(ConnectionState.IDLE)  # give up; user must retry manually
                return
            self._set_state(ConnectionState.RECONNECTING)
            await asyncio.sleep(self._reconnect_delay_s)

        self._set_state(ConnectionState.IDLE)

    async def _subscribe_all(self, client, BleakError) -> None:
        """Everything done after connecting: check the service, subscribe, read Status once."""
        for uuid in (CONTROL_UUID, STATUS_UUID, EVENT_UUID, TELEMETRY_UUID):
            if client.services.get_characteristic(uuid) is None:
                raise BleakError(f"sensor characteristic {uuid} not found")
        for uuid in (STATUS_UUID, EVENT_UUID, TELEMETRY_UUID):  # RawStream only on demand
            await client.start_notify(uuid, self._on_notify)
        # Status is Read + Notify at 1 Hz: read it once so the device state shows up immediately.
        try:
            self._emit_bytes(bytes(await client.read_gatt_char(STATUS_UUID)))
        except BleakError:
            pass
        await self._setup_battery(client, BleakError)
        self.mtu = getattr(client, "mtu_size", None)  # spec wants >= 185; the OS negotiates it

    async def _setup_battery(self, client, BleakError) -> None:
        """Standard Battery Level: read once, then subscribe if the sensor supports notify."""
        try:
            level = await client.read_gatt_char(BATTERY_LEVEL_UUID)
            if level:
                self._emit_battery(level[0])
            await client.start_notify(BATTERY_LEVEL_UUID, lambda _c, d: self._emit_battery(d[0]))
        except (BleakError, KeyError):
            pass  # battery service is optional; Status still reports battery voltage
