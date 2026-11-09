"""Packet Parser: pure, stateless decoder (plus matching encoders for tests/mock).

Wire format (Ed's interface spec). Everything is little-endian; byte 0 is the type.

  Status    0x01  12 bytes  type, state, error, flags, battery_mV u16, temp_tenthsC i16, t_ms u32
  Event     0x02  14 bytes  type, class, seq u16, t_ms u32, peak_accel_centi_g u16,
                            peak_gyro_dps u16, confidence u8 (0-100), flags u8
  Telemetry 0x03  20 bytes  type, fix_quality, seq u16, t_ms u32, lat_e7 i32, lon_e7 i32,
                            speed_cm_s u16, satellites u8, hdop_x10 u8
  Raw       0x04  variable  header 8 bytes: type, sample_count u8, seq u16, t_ms u32 (first sample)
                            then sample_count x 14 bytes: accel x/y/z, gyro x/y/z, temp (all i16)

Raw length depends on the BLE MTU, so its length is checked against sample_count in the
header -- never against a fixed size. Knows nothing about Bluetooth or the UI.
"""
from __future__ import annotations

import struct
from typing import Sequence

from .models import Event, Packet, RawBatch, RawSample, Status, Telemetry

TYPE_STATUS = 0x01
TYPE_EVENT = 0x02
TYPE_TELEMETRY = 0x03
TYPE_RAW = 0x04

_STATUS = struct.Struct("<BBBBHhI")      # 12 bytes
_EVENT = struct.Struct("<BBHIHHBB")      # 14 bytes
_TELEMETRY = struct.Struct("<BBHIiiHBB")  # 20 bytes
_RAW_HEADER = struct.Struct("<BBHI")     # 8 bytes
_RAW_SAMPLE = struct.Struct("<7h")       # 14 bytes

_FIXED = {
    TYPE_STATUS: _STATUS.size,
    TYPE_EVENT: _EVENT.size,
    TYPE_TELEMETRY: _TELEMETRY.size,
}


class PacketParseError(Exception):
    """Base class: the payload could not be turned into a Packet."""


class TooShort(PacketParseError):
    def __init__(self, expected: int, got: int):
        super().__init__(f"packet too short: expected {expected} bytes, got {got}")
        self.expected, self.got = expected, got


class TooLong(PacketParseError):
    def __init__(self, expected: int, got: int):
        super().__init__(f"packet too long: expected {expected} bytes, got {got}")
        self.expected, self.got = expected, got


class UnknownPacketType(PacketParseError):
    def __init__(self, packet_type: int):
        super().__init__(f"unknown packet type 0x{packet_type:02x}")
        self.packet_type = packet_type


class InvalidValue(PacketParseError):
    """A field had a value outside what the spec allows (e.g. confidence 150)."""


def parse(payload: bytes) -> Packet:
    """Decode one BLE notification. Raises PacketParseError on any problem.

    Never guesses: a bad packet is rejected whole, not partially decoded.
    """
    if len(payload) < 1:
        raise TooShort(1, 0)
    packet_type = payload[0]

    if packet_type == TYPE_RAW:
        return _parse_raw(payload)

    expected = _FIXED.get(packet_type)
    if expected is None:
        raise UnknownPacketType(packet_type)
    if len(payload) < expected:
        raise TooShort(expected, len(payload))
    if len(payload) > expected:
        raise TooLong(expected, len(payload))

    if packet_type == TYPE_STATUS:
        _t, state, error, flags, mv, temp, t_ms = _STATUS.unpack(payload)
        return Status(state, error, flags, mv, temp, t_ms)

    if packet_type == TYPE_EVENT:
        _t, cls, seq, t_ms, accel, gyro, conf, flags = _EVENT.unpack(payload)
        if conf > 100:
            raise InvalidValue(f"confidence {conf} is outside 0-100")
        # Unknown class values are kept (shown as CLASS_<n>) rather than dropping a real event.
        return Event(cls, seq, t_ms, accel, gyro, conf, flags)

    # TYPE_TELEMETRY
    _t, fix, seq, t_ms, lat, lon, speed, sats, hdop = _TELEMETRY.unpack(payload)
    return Telemetry(fix, seq, t_ms, lat, lon, speed, sats, hdop)


def _parse_raw(payload: bytes) -> RawBatch:
    if len(payload) < _RAW_HEADER.size:
        raise TooShort(_RAW_HEADER.size, len(payload))
    _t, count, seq, t_ms = _RAW_HEADER.unpack_from(payload, 0)
    expected = _RAW_HEADER.size + count * _RAW_SAMPLE.size
    if len(payload) < expected:
        raise TooShort(expected, len(payload))
    if len(payload) > expected:
        raise TooLong(expected, len(payload))
    samples = tuple(
        RawSample(*_RAW_SAMPLE.unpack_from(payload, _RAW_HEADER.size + i * _RAW_SAMPLE.size))
        for i in range(count)
    )
    return RawBatch(seq, t_ms, samples)


# --- Encoders: used by the mock sensor and the tests (the real ESP32 does this in C++) ---

def encode_status(state: int, error_code: int, flags: int, battery_mv: int,
                  temperature_tenths_c: int, t_ms: int) -> bytes:
    return _STATUS.pack(TYPE_STATUS, state, error_code, flags, battery_mv, temperature_tenths_c, t_ms)


def encode_event(event_class: int, seq: int, t_ms: int, peak_accel_centi_g: int,
                 peak_gyro_dps: int, confidence: int, flags: int = 0) -> bytes:
    return _EVENT.pack(TYPE_EVENT, int(event_class), seq & 0xFFFF, t_ms & 0xFFFFFFFF,
                       peak_accel_centi_g, peak_gyro_dps, confidence, flags)


def encode_telemetry(fix_quality: int, seq: int, t_ms: int, lat_e7: int, lon_e7: int,
                     speed_cm_s: int, satellites: int, hdop_x10: int) -> bytes:
    return _TELEMETRY.pack(TYPE_TELEMETRY, fix_quality, seq & 0xFFFF, t_ms & 0xFFFFFFFF,
                           lat_e7, lon_e7, speed_cm_s, satellites, hdop_x10)


def encode_raw(seq: int, t_ms: int, samples: Sequence[Sequence[int]]) -> bytes:
    out = _RAW_HEADER.pack(TYPE_RAW, len(samples), seq & 0xFFFF, t_ms & 0xFFFFFFFF)
    for s in samples:
        out += _RAW_SAMPLE.pack(*s)
    return out
