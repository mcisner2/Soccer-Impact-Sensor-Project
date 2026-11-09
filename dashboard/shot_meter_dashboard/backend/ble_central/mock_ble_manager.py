"""Mock BLE Manager: a fake sensor so the app can be built and tested without hardware.

Walks through the same connection states as the real manager, and behaves like the firmware
described in the Interface Specification:

  device state:  1 SENSOR_INIT -> (START_CAL) 2 CALIBRATING -> 3 READY -> (START_SESSION)
                 4 RECORDING -> (STOP_SESSION) 5 STOPPED
  START_SESSION before calibration passed -> Status error_code 0x11 (ERR_NOT_CALIBRATED)
  Event + Telemetry are only sent while RECORDING; Raw only after RAW_STREAM_ON
  each stream has its OWN sequence counter; counters restart at 0 for each new recording
  (restart_seq_on_session=True) -- the spec does not say what the firmware does, so both
  behaviours can be tested

auto_start=True (default) skips the setup and begins already RECORDING, so simple demos and
tests get data right away. auto_start=False gives the full calibrate-then-record flow.

Test hooks:
  drop_every=N        silently lose every Nth Event/Telemetry/Raw packet (-> sequence gaps)
  inject(bytes)       push any raw bytes (e.g. a corrupted packet) into the pipeline
  commands_sent       every (Command, arg) the app wrote to "Control"
  simulate_drop()     pretend the connection dropped (exercises the reconnect path)
  simulate_bluetooth_off() / simulate_unauthorized()
"""
from __future__ import annotations

import asyncio
import math
from typing import List, Optional, Tuple

from .ble_manager import BaseBLEManager, NotConnectedError
from .commands import Command, encode_command
from .models import ConnectionState, EventClass, Stream
from .packet_parser import encode_event, encode_raw, encode_status, encode_telemetry

# device states (spec section 6.2)
SENSOR_INIT, CALIBRATING, READY, RECORDING, STOPPED = 1, 2, 3, 4, 5
ERR_NOT_CALIBRATED = 0x11
_FLAG_LOW_BATTERY = 0x04
_FLAG_RAW_ON = 0x08
_LOW_BATTERY_MV = 4400  # spec: low-battery warning at <= 4.4 V


class MockBLEManager(BaseBLEManager):
    def __init__(
        self,
        interval_s: float = 0.2,
        step_delay_s: float = 0.05,
        drop_every: int = 0,
        status_every: int = 10,
        event_every: int = 7,
        telemetry_every: int = 5,
        raw_samples_per_batch: int = 5,
        max_attempts: int = 5,
        reconnect_delay_s: float = 0.1,
        auto_start: bool = True,
        cal_duration_s: float = 0.3,
        restart_seq_on_session: bool = True,
    ) -> None:
        super().__init__()
        self.interval_s = interval_s
        self.step_delay_s = step_delay_s
        self.drop_every = drop_every
        self.status_every = status_every
        self.event_every = event_every
        self.telemetry_every = telemetry_every
        self.raw_samples_per_batch = raw_samples_per_batch
        self.max_attempts = max_attempts
        self.reconnect_delay_s = reconnect_delay_s
        self.cal_ticks = max(1, math.ceil(cal_duration_s / interval_s))
        self.restart_seq_on_session = restart_seq_on_session

        self.commands_sent: List[Tuple[Command, int]] = []
        self._task: Optional[asyncio.Task] = None
        self._tick = 0
        self._seq = {s: 0 for s in Stream}   # one counter per stream, like the firmware
        self._sent = {s: 0 for s in Stream}
        self._battery_mv = 4800
        self._raw_on = False
        self._drop_requested = False

        self._calibrated = auto_start
        self._dev_state = RECORDING if auto_start else SENSOR_INIT
        self._error = 0
        self._cal_left = 0

    # --- BLEManaging ---
    async def start_scanning(self) -> None:
        if self._task and not self._task.done():
            return
        self._task = asyncio.create_task(self._run())

    async def stop_scanning(self) -> None:
        await self.disconnect()

    async def disconnect(self) -> None:
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        self._raw_on = False
        self._set_state(ConnectionState.IDLE)

    async def send_command(self, command: Command, arg: int = 0) -> None:
        encode_command(command, arg)  # validates like the real manager
        if self._state != ConnectionState.SUBSCRIBED:
            raise NotConnectedError(f"cannot send {Command(command).name}: not connected to sensor")
        command = Command(command)
        self.commands_sent.append((command, arg))

        if command in (Command.START_CAL, Command.RECAL):
            if self._dev_state != RECORDING:
                self._dev_state, self._cal_left, self._error = CALIBRATING, self.cal_ticks, 0
                self._calibrated = False
        elif command == Command.START_SESSION:
            if not self._calibrated:
                self._error = ERR_NOT_CALIBRATED          # rejected, state unchanged
            elif self._dev_state in (READY, STOPPED):
                self._dev_state, self._error = RECORDING, 0
                if self.restart_seq_on_session:
                    self._seq = {s: 0 for s in Stream}
                    self._sent = {s: 0 for s in Stream}
        elif command == Command.STOP_SESSION:
            if self._dev_state == RECORDING:
                self._dev_state, self._error = STOPPED, 0
        elif command == Command.RAW_STREAM_ON:
            self._raw_on = True
        elif command == Command.RAW_STREAM_OFF:
            self._raw_on = False
        self._emit_status()  # Status is sent "on state change" too

    # --- test hooks ---
    def inject(self, payload: bytes) -> None:
        self._emit_bytes(payload)

    def simulate_drop(self) -> None:
        self._drop_requested = True

    def simulate_bluetooth_off(self) -> None:
        self._set_state(ConnectionState.BLUETOOTH_OFF)

    def simulate_unauthorized(self) -> None:
        self._set_state(ConnectionState.UNAUTHORIZED)

    # --- internals ---
    async def _connect_sequence(self) -> None:
        for state in (
            ConnectionState.SCANNING,
            ConnectionState.CONNECTING,
            ConnectionState.DISCOVERING_SERVICES,
            ConnectionState.SUBSCRIBED,
        ):
            self._set_state(state)
            await asyncio.sleep(self.step_delay_s)
        self._emit_status()      # like the real manager's one-time Status read on connect
        self._emit_battery(100)  # standard Battery Level characteristic

    async def _run(self) -> None:
        attempts = 0
        while True:
            await self._connect_sequence()
            attempts = 0
            while not self._drop_requested:
                await asyncio.sleep(self.interval_s)
                self._tick_once()
            # connection lost (the device itself keeps its state)
            self._drop_requested = False
            self._raw_on = False
            self._set_state(ConnectionState.DISCONNECTED)
            attempts += 1
            if attempts > self.max_attempts:
                self._set_state(ConnectionState.IDLE)
                return
            self._set_state(ConnectionState.RECONNECTING)
            await asyncio.sleep(self.reconnect_delay_s)

    def _now_ms(self) -> int:
        return self._tick * int(self.interval_s * 1000)

    def _emit_status(self) -> None:
        flags = (_FLAG_RAW_ON if self._raw_on else 0) | (
            _FLAG_LOW_BATTERY if self._battery_mv <= _LOW_BATTERY_MV else 0)
        self._emit_bytes(encode_status(self._dev_state, self._error, flags, self._battery_mv, 215, self._now_ms()))

    def _send(self, stream: Stream, payload_for_seq) -> None:
        """Advance this stream's own counter; maybe 'lose' the packet in the air."""
        self._seq[stream] = (self._seq[stream] + 1) & 0xFFFF
        self._sent[stream] += 1
        if self.drop_every and self._sent[stream] % self.drop_every == 0:
            return  # lost: the sequence number advanced but nothing arrives
        self._emit_bytes(payload_for_seq(self._seq[stream]))

    def _tick_once(self) -> None:
        self._tick += 1
        t = self._tick
        ts = self._now_ms()

        if self._dev_state == CALIBRATING:
            self._cal_left -= 1
            if self._cal_left <= 0:
                self._dev_state, self._calibrated = READY, True
                self._emit_status()

        if self.status_every and t % self.status_every == 0:
            self._battery_mv = max(3000, self._battery_mv - 5)
            self._emit_status()

        if self._dev_state == RECORDING:
            if self.telemetry_every and t % self.telemetry_every == 0:
                self._send(Stream.TELEMETRY, lambda seq: encode_telemetry(
                    fix_quality=1, seq=seq, t_ms=ts,
                    lat_e7=300000000 + t * 100, lon_e7=-970000000 - t * 100,
                    speed_cm_s=200 + (t % 9) * 50, satellites=8, hdop_x10=12))

            if self.event_every and t % self.event_every == 0:
                cls = [c for c in EventClass if c != EventClass.UNKNOWN][(t // self.event_every) % 4]
                self._send(Stream.EVENT, lambda seq: encode_event(
                    cls, seq, ts, peak_accel_centi_g=1500 + (t % 7) * 50,
                    peak_gyro_dps=900 + (t % 5) * 40, confidence=50 + (t % 50)))

        if self._raw_on:
            n = self.raw_samples_per_batch
            samples = [(i, -i, 1000 + i, 10 * i, -10 * i, 5, 215) for i in range(n)]
            self._send(Stream.RAW, lambda seq: encode_raw(seq, ts, samples))
