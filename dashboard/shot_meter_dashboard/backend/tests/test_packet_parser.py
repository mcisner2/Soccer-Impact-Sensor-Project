import struct

import pytest

from ble_central.models import EventClass
from ble_central.packet_parser import (
    InvalidValue, TooLong, TooShort, UnknownPacketType,
    encode_event, encode_raw, encode_status, encode_telemetry, parse,
)


def hx(s: str) -> bytes:
    return bytes.fromhex(s.replace(" ", ""))


# ---- byte-exact firmware test vectors (from the feedback) ----

def test_firmware_vector_status():
    p = parse(hx("01 03 00 08 C0 12 CC FF 40 E2 01 00"))
    assert p.state == 3 and p.state_name == "READY"
    assert p.error_code == 0
    assert p.raw_stream_on is True
    assert not p.calibration_stale and not p.gps_degraded and not p.low_battery
    assert p.battery_mv == 4800
    assert p.temperature_c == pytest.approx(-5.2)
    assert p.t_ms == 123456


def test_firmware_vector_event():
    p = parse(hx("02 01 07 00 CD 81 01 00 F3 05 9C 04 32 00"))
    assert p.event_class == EventClass.KICK and p.event_class_name == "KICK"
    assert p.sequence_number == 7
    assert p.t_ms == 98765
    assert p.peak_accel_g == pytest.approx(15.23)
    assert p.peak_accel_centi_g == 1523  # integer hundredths of a g, not a float
    assert p.peak_gyro_dps == 1180
    assert p.confidence == 50


# ---- lengths ----

def test_fixed_lengths_match_spec():
    assert len(encode_status(3, 0, 0, 4000, 200, 1)) == 12
    assert len(encode_event(1, 1, 1, 1000, 100, 50)) == 14
    assert len(encode_telemetry(1, 1, 1, 0, 0, 0, 0, 0)) == 20


# ---- status flags ----

@pytest.mark.parametrize("flags,attr", [
    (0x01, "calibration_stale"), (0x02, "gps_degraded"), (0x04, "low_battery"), (0x08, "raw_stream_on"),
])
def test_each_status_flag_bit(flags, attr):
    p = parse(encode_status(3, 0, flags, 4000, 0, 0))
    for name in ("calibration_stale", "gps_degraded", "low_battery", "raw_stream_on"):
        assert getattr(p, name) is (name == attr)


@pytest.mark.parametrize("code,name", [
    (0, "BOOT"), (1, "SENSOR_INIT"), (2, "CALIBRATING"), (3, "READY"),
    (4, "RECORDING"), (5, "STOPPED"), (6, "ERROR"),
])
def test_all_device_states_from_spec(code, name):
    assert parse(encode_status(code, 0, 0, 4000, 0, 0)).state_name == name


def test_unknown_state_code_still_parses_with_generic_name():
    assert parse(encode_status(9, 0, 0, 4000, 0, 0)).state_name == "STATE_9"


@pytest.mark.parametrize("code,name", [
    (0x01, "NOT_STILL_ACCEL"), (0x02, "NOT_STILL_GYRO"), (0x03, "GYRO_BIAS_OUT_OF_RANGE"),
    (0x04, "GRAVITY_OUT_OF_RANGE"), (0x10, "ERR_CAL_FAILED"), (0x11, "ERR_NOT_CALIBRATED"),
    (0x20, "ERR_IMU_COMM"), (0x21, "ERR_IMU_FIFO_OVERFLOW"), (0x30, "WARN_GPS_NO_FIX"),
    (0x31, "WARN_GPS_POOR_ACCURACY"), (0x40, "WARN_CAL_STALE"), (0x50, "WARN_LOW_BATTERY"),
    (0x51, "ERR_BATTERY_CRITICAL"), (0x60, "WARN_NO_EVENTS"),
])
def test_every_error_code_in_spec_has_a_name_and_a_dashboard_message(code, name):
    p = parse(encode_status(6, code, 0, 4000, 0, 0))
    assert p.error_name == name and p.error_message


def test_no_error_and_unknown_error():
    assert parse(encode_status(3, 0, 0, 4000, 0, 0)).error_message is None
    assert parse(encode_status(3, 0x77, 0, 4000, 0, 0)).error_name == "ERROR_0x77"


# ---- event ----

@pytest.mark.parametrize("cls,name", [(0, "UNKNOWN"), (1, "KICK"), (2, "PASS"), (3, "TRAP"), (4, "GROUND_IMPACT")])
def test_event_class_names(cls, name):
    p = parse(encode_event(cls, 1, 1, 100, 10, 10))
    assert p.event_class == cls and p.event_class_name == name


def test_class_0_unknown_is_valid_per_spec_and_not_dropped():
    # Interface Specification 6.3: event_class 0 = UNKNOWN is a legal value.
    assert parse(encode_event(0, 1, 1, 100, 10, 10)).event_class_name == "UNKNOWN"


def test_unrecognised_event_class_is_kept_not_dropped():
    # firmware may add classes (e.g. spec's 2=HARD_IMPACT vs. the firmware owner's 2=pass);
    # a real event must never be lost over a label
    p = parse(encode_event(9, 1, 1, 100, 10, 10))
    assert p.event_class == 9 and p.event_class_name == "CLASS_9"


def test_confidence_above_100_rejected():
    with pytest.raises(InvalidValue):
        parse(encode_event(1, 1, 1, 100, 10, 101))
    assert parse(encode_event(1, 1, 1, 100, 10, 100)).confidence == 100


# ---- telemetry ----

def test_fix_quality_names_and_no_fix_flag():
    for q, name, fix in ((0, "none", False), (1, "GPS", True), (2, "DGPS/WAAS", True)):
        p = parse(encode_telemetry(q, 1, 1, 0, 0, 0, 0, 0))
        assert p.fix_name == name and p.has_fix is fix


def test_telemetry_roundtrip_with_negative_coordinates():
    raw = encode_telemetry(fix_quality=2, seq=300, t_ms=5000, lat_e7=-337000000, lon_e7=1512000000,
                           speed_cm_s=450, satellites=9, hdop_x10=15)
    p = parse(raw)
    assert (p.fix_quality, p.sequence_number, p.t_ms) == (2, 300, 5000)
    assert p.latitude == pytest.approx(-33.7)
    assert p.longitude == pytest.approx(151.2)
    assert p.speed_m_s == pytest.approx(4.5)
    assert (p.satellites, p.hdop) == (9, 1.5)


def test_telemetry_is_little_endian_byte_exact():
    raw = bytes([0x03, 0x01, 0x02, 0x01, 0x01, 0x02, 0x03, 0x04]) \
        + struct.pack("<i", 1) + struct.pack("<i", -1) + bytes([0x34, 0x12, 5, 7])
    p = parse(raw)
    assert p.sequence_number == 0x0102 and p.t_ms == 0x04030201
    assert p.lat_e7 == 1 and p.lon_e7 == -1 and p.speed_cm_s == 0x1234


# ---- raw (variable length) ----

def test_raw_header_is_8_bytes_and_samples_14_bytes_each():
    assert len(encode_raw(1, 1, [])) == 8
    one = (1, -2, 3, -4, 5, -6, 7)
    assert len(encode_raw(1, 1, [one])) == 8 + 14
    assert len(encode_raw(1, 1, [one] * 10)) == 8 + 140


@pytest.mark.parametrize("n", [0, 1, 5, 17])
def test_raw_length_follows_sample_count(n):
    samples = [(i, -i, 2 * i, 3 * i, -3 * i, 7, 100 + i) for i in range(n)]
    p = parse(encode_raw(seq=42, t_ms=1000, samples=samples))
    assert p.sequence_number == 42 and p.t_ms == 1000
    assert len(p.samples) == n
    for i, s in enumerate(p.samples):
        assert (s.ax, s.ay, s.az, s.gx, s.gy, s.gz, s.temp) == samples[i]


def test_raw_samples_are_signed_16_bit():
    p = parse(encode_raw(1, 1, [(-32768, 32767, -1, 0, 1, -2, 3)]))
    s = p.samples[0]
    assert (s.ax, s.ay, s.az) == (-32768, 32767, -1)


def test_raw_truncated_or_padded_rejected():
    good = encode_raw(1, 1, [(1, 2, 3, 4, 5, 6, 7)] * 3)
    with pytest.raises(TooShort) as e:
        parse(good[:-1])
    assert (e.value.expected, e.value.got) == (8 + 42, 8 + 42 - 1)
    with pytest.raises(TooLong):
        parse(good + b"\x00")
    with pytest.raises(TooShort):
        parse(good[:5])  # not even a full header


# ---- generic rejection ----

def test_empty_and_truncated_rejected_not_guessed():
    with pytest.raises(TooShort):
        parse(b"")
    full = encode_event(1, 1, 1, 1000, 100, 50)
    with pytest.raises(TooShort) as e:
        parse(full[:-1])
    assert (e.value.expected, e.value.got) == (14, 13)


def test_extra_bytes_rejected():
    with pytest.raises(TooLong):
        parse(encode_status(3, 0, 0, 4000, 0, 0) + b"\x00")


def test_unknown_packet_type():
    with pytest.raises(UnknownPacketType):
        parse(bytes([0x7F] * 12))
    with pytest.raises(UnknownPacketType):
        parse(bytes([0x00] * 12))
