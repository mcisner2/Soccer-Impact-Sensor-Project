import asyncio

import pytest

from ble_central import BleakBLEManager, Command, ConnectionState, NotConnectedError, encode_command
from ble_central.constants import (
    BATTERY_LEVEL_UUID, CONTROL_UUID, EVENT_UUID, RAW_STREAM_UUID, STATUS_UUID, TELEMETRY_UUID,
)


def test_command_codes_match_spec():
    assert [(c.name, int(c)) for c in Command] == [
        ("START_CAL", 1), ("START_SESSION", 2), ("STOP_SESSION", 3),
        ("RECAL", 4), ("RAW_STREAM_ON", 5), ("RAW_STREAM_OFF", 6),
    ]


def test_every_command_is_exactly_two_bytes_with_zero_arg_by_default():
    for c in Command:
        b = encode_command(c)
        assert len(b) == 2 and b == bytes([int(c), 0])


def test_argument_goes_in_second_byte_and_is_range_checked():
    assert encode_command(Command.START_CAL, 0x7F) == b"\x01\x7f"
    with pytest.raises(ValueError):
        encode_command(Command.START_CAL, 256)
    with pytest.raises(ValueError):
        encode_command(Command.START_CAL, -1)


class FakeClient:
    """Stands in for bleak's BleakClient so we can check what the real manager writes."""

    def __init__(self, fail_write=False):
        self.calls = []
        self.fail_write = fail_write

    async def start_notify(self, uuid, cb):
        self.calls.append(("start_notify", uuid))

    async def stop_notify(self, uuid):
        self.calls.append(("stop_notify", uuid))

    async def write_gatt_char(self, uuid, data, response=False):
        if self.fail_write:
            raise OSError("write failed")
        self.calls.append(("write", uuid, bytes(data)))


def make_real(client):
    m = BleakBLEManager()
    m._client = client
    m._state = ConnectionState.SUBSCRIBED
    return m


def test_real_manager_writes_two_bytes_to_control():
    async def go():
        c = FakeClient()
        m = make_real(c)
        await m.send_command(Command.START_SESSION)
        return c.calls

    assert asyncio.run(go()) == [("write", CONTROL_UUID, b"\x02\x00")]


def test_real_manager_subscribes_raw_stream_only_around_raw_on_off():
    async def go():
        c = FakeClient()
        m = make_real(c)
        await m.send_command(Command.START_SESSION)       # no raw subscription
        await m.send_command(Command.RAW_STREAM_ON)       # subscribe THEN write
        await m.send_command(Command.RAW_STREAM_OFF)      # write THEN unsubscribe
        return c.calls

    assert asyncio.run(go()) == [
        ("write", CONTROL_UUID, b"\x02\x00"),
        ("start_notify", RAW_STREAM_UUID),
        ("write", CONTROL_UUID, b"\x05\x00"),
        ("write", CONTROL_UUID, b"\x06\x00"),
        ("stop_notify", RAW_STREAM_UUID),
    ]


def test_failed_raw_on_write_undoes_the_subscription():
    async def go():
        c = FakeClient(fail_write=True)
        m = make_real(c)
        with pytest.raises(OSError):
            await m.send_command(Command.RAW_STREAM_ON)
        return c.calls, m._raw_subscribed

    calls, still_subscribed = asyncio.run(go())
    assert calls == [("start_notify", RAW_STREAM_UUID), ("stop_notify", RAW_STREAM_UUID)]
    assert still_subscribed is False


def test_sending_while_not_subscribed_raises():
    async def go():
        m = BleakBLEManager()
        with pytest.raises(NotConnectedError):
            await m.send_command(Command.START_SESSION)

    asyncio.run(go())


# ---- what the real manager does right after connecting ----

class _Char:  # stands in for a bleak characteristic
    pass


class _Services:
    def __init__(self, uuids):
        self.uuids = set(uuids)

    def get_characteristic(self, uuid):
        return _Char() if uuid in self.uuids else None


class ConnectedFakeClient(FakeClient):
    def __init__(self, uuids, mtu=247):
        super().__init__()
        self.services = _Services(uuids)
        self.mtu_size = mtu

    async def read_gatt_char(self, uuid):
        self.calls.append(("read", uuid))
        if uuid == STATUS_UUID:
            return bytearray.fromhex("01 03 00 08 C0 12 CC FF 40 E2 01 00".replace(" ", ""))
        if uuid == BATTERY_LEVEL_UUID:
            return bytearray([87])
        raise KeyError(uuid)


def test_after_connect_subscribes_right_characteristics_and_reads_status_once():
    from bleak.exc import BleakError

    async def go():
        c = ConnectedFakeClient([CONTROL_UUID, STATUS_UUID, EVENT_UUID, TELEMETRY_UUID, RAW_STREAM_UUID,
                                 BATTERY_LEVEL_UUID])
        m = BleakBLEManager()
        got, battery = [], []
        m.on_raw_packet(lambda data, _t: got.append(data))
        m.on_battery_level(battery.append)
        await m._subscribe_all(c, BleakError)
        return c, m, got, battery

    c, m, got, battery = asyncio.run(go())
    subscribed = [u for kind, u, *_ in c.calls if kind == "start_notify"]
    assert STATUS_UUID in subscribed and EVENT_UUID in subscribed and TELEMETRY_UUID in subscribed
    assert RAW_STREAM_UUID not in subscribed       # only when the raw stream is turned on
    assert len(got) == 1 and got[0][0] == 0x01     # the one-time Status read was passed on
    assert battery == [87]
    assert m.mtu == 247 and m.mtu_ok is True


def test_missing_required_characteristic_is_an_error():
    from bleak.exc import BleakError

    async def go():
        c = ConnectedFakeClient([CONTROL_UUID, STATUS_UUID, EVENT_UUID])  # no Telemetry
        await BleakBLEManager()._subscribe_all(c, BleakError)

    with pytest.raises(BleakError):
        asyncio.run(go())


def test_small_mtu_is_flagged():
    m = BleakBLEManager()
    m.mtu = 23
    assert m.mtu_ok is False
