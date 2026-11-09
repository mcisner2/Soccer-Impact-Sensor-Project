"""End-to-end: mock sensor -> parser -> session store -> view model (no hardware)."""
import asyncio

import pytest

from ble_central import Command, ConnectionState, DashboardViewModel, MockBLEManager, NotConnectedError, Stream


def run(coro):
    return asyncio.run(coro)


def fast_mock(**kw):
    return MockBLEManager(interval_s=0.01, step_delay_s=0.005, reconnect_delay_s=0.02,
                          status_every=3, event_every=2, telemetry_every=2, **kw)


def test_connects_through_all_states_and_receives_everything():
    async def go():
        ble = fast_mock()
        states = []
        ble.on_state_change(states.append)
        vm = DashboardViewModel(ble)
        await vm.connect_to_sensor()
        await asyncio.sleep(0.4)
        await vm.disconnect_from_sensor()
        return vm, states

    vm, states = run(go())
    assert states[:4] == [
        ConnectionState.SCANNING, ConnectionState.CONNECTING,
        ConnectionState.DISCOVERING_SERVICES, ConnectionState.SUBSCRIBED,
    ]
    assert states[-1] == ConnectionState.IDLE
    assert vm.latest_status is not None and vm.device_state_name == "RECORDING"
    assert vm.battery_percentage == 100            # standard Battery Level characteristic
    assert vm.battery_mv and vm.temperature_c is not None
    assert vm.recent_events and vm.latest_telemetry
    # separate counters per stream => the healthy mock must show ZERO false gaps
    assert vm.missed_packet_count == 0 and vm.parse_error_count == 0
    assert vm.session.raw_batches == []            # raw stream is off until commanded


def test_raw_stream_only_arrives_after_raw_stream_on():
    async def go():
        ble = fast_mock()
        vm = DashboardViewModel(ble)
        await vm.connect_to_sensor()
        await asyncio.sleep(0.1)
        none_yet = len(vm.session.raw_batches)
        await vm.set_raw_stream(True)
        await asyncio.sleep(0.2)
        on_count = len(vm.session.raw_batches)
        raw_flag = vm.raw_stream_on
        await vm.set_raw_stream(False)
        await asyncio.sleep(0.05)
        after_off = len(vm.session.raw_batches)
        await asyncio.sleep(0.1)
        final = len(vm.session.raw_batches)
        await vm.disconnect_from_sensor()
        return vm, none_yet, on_count, raw_flag, after_off, final, ble

    vm, none_yet, on_count, raw_flag, after_off, final, ble = run(go())
    assert none_yet == 0 and on_count > 0
    assert raw_flag is True                       # Status flag bit 3 reported back
    assert final == after_off                     # stopped after RAW_STREAM_OFF
    assert vm.session.raw_batches[0].samples      # variable-length batch decoded
    assert vm.missed_packet_count == 0
    assert [c for c, _ in ble.commands_sent] == [Command.RAW_STREAM_ON, Command.RAW_STREAM_OFF]


def test_commands_reach_the_sensor_and_fail_cleanly_when_disconnected():
    async def go():
        ble = fast_mock()
        vm = DashboardViewModel(ble)
        with pytest.raises(NotConnectedError):
            await vm.start_session()
        await vm.connect_to_sensor()
        await asyncio.sleep(0.1)
        await vm.start_calibration()
        await vm.start_session()
        await vm.stop_session()
        await vm.recalibrate()
        await vm.disconnect_from_sensor()
        return ble

    ble = run(go())
    assert [c for c, _ in ble.commands_sent] == [
        Command.START_CAL, Command.START_SESSION, Command.STOP_SESSION, Command.RECAL]


def test_lost_packets_show_up_as_missed_per_stream():
    async def go():
        vm = DashboardViewModel(fast_mock(drop_every=3))
        await vm.connect_to_sensor()
        await asyncio.sleep(0.5)
        await vm.disconnect_from_sensor()
        return vm

    vm = run(go())
    assert vm.missed_packet_count > 0
    assert vm.session.sequence_gaps[Stream.EVENT] and vm.session.sequence_gaps[Stream.TELEMETRY]


def test_corrupt_packet_is_dropped_and_counted():
    async def go():
        ble = fast_mock()
        vm = DashboardViewModel(ble)
        await vm.connect_to_sensor()
        await asyncio.sleep(0.1)
        ble.inject(b"\x01\x03\x00")           # truncated Status
        ble.inject(bytes([0x7F] * 8))         # unknown type
        ble.inject(b"")                       # empty
        await vm.disconnect_from_sensor()
        return vm

    vm = run(go())
    assert vm.parse_error_count == 3
    assert vm.latest_status is not None       # good data kept, nothing crashed


def test_disconnect_keeps_data_and_reconnects():
    async def go():
        ble = fast_mock()
        states = []
        ble.on_state_change(states.append)
        vm = DashboardViewModel(ble)
        await vm.connect_to_sensor()
        await asyncio.sleep(0.15)
        count_before = len(vm.session.telemetry)
        ble.simulate_drop()
        await asyncio.sleep(0.4)
        await vm.disconnect_from_sensor()
        return vm, states, count_before

    vm, states, count_before = run(go())
    i = states.index(ConnectionState.DISCONNECTED)
    assert states[i + 1] == ConnectionState.RECONNECTING
    assert states.count(ConnectionState.SUBSCRIBED) >= 2
    assert len(vm.session.telemetry) > count_before


def test_bluetooth_off_and_unauthorized_status_messages():
    ble = fast_mock()
    vm = DashboardViewModel(ble)
    ble.simulate_bluetooth_off()
    assert vm.connection_state == ConnectionState.BLUETOOTH_OFF and "Bluetooth" in vm.status_message
    ble.simulate_unauthorized()
    assert vm.connection_state == ConnectionState.UNAUTHORIZED and "permission" in vm.status_message.lower()


def test_view_model_defaults_before_any_data():
    vm = DashboardViewModel(fast_mock())
    assert vm.latest_status is None and vm.device_state_name == "--"
    assert vm.battery_percentage is None and vm.battery_mv is None
    assert vm.recent_events == [] and vm.latest_telemetry is None
    assert not vm.raw_stream_on and not vm.calibration_stale


# ---- the spec's device state machine, via the mock ----

def test_start_session_before_calibration_is_rejected_with_a_dashboard_message():
    async def go():
        ble = fast_mock(auto_start=False)
        vm = DashboardViewModel(ble)
        await vm.connect_to_sensor()
        await asyncio.sleep(0.1)
        state_before = vm.device_state_name
        await vm.start_session()
        await asyncio.sleep(0.1)
        out = (state_before, vm.device_state_name, vm.latest_status.error_name,
               vm.error_message, len(vm.session.events))
        await vm.disconnect_from_sensor()
        return out

    before, after, err, msg, n_events = run(go())
    assert before == "SENSOR_INIT" and after == "SENSOR_INIT"
    assert err == "ERR_NOT_CALIBRATED" and "Calibrate" in msg
    assert n_events == 0                           # nothing recorded without calibration


def test_full_flow_calibrate_record_stop():
    async def go():
        ble = fast_mock(auto_start=False)
        vm = DashboardViewModel(ble)
        seen = []
        vm.subscribe(lambda: seen.append(vm.device_state_name))
        await vm.connect_to_sensor()
        await asyncio.sleep(0.1)
        await vm.start_calibration()
        await asyncio.sleep(0.1)
        calibrating = vm.device_state_name
        await asyncio.sleep(0.4)                   # calibration finishes -> READY
        ready = vm.device_state_name
        await vm.start_session()
        await asyncio.sleep(0.4)
        recording, n_rec = vm.device_state_name, len(vm.session.events)
        await vm.stop_session()
        await asyncio.sleep(0.1)
        stopped = vm.device_state_name
        n_after_stop = len(vm.session.events)
        await asyncio.sleep(0.2)
        n_later = len(vm.session.events)
        await vm.disconnect_from_sensor()
        return calibrating, ready, recording, n_rec, stopped, n_after_stop, n_later, vm

    calibrating, ready, recording, n_rec, stopped, n_after, n_later, vm = run(go())
    assert (calibrating, ready, recording, stopped) == ("CALIBRATING", "READY", "RECORDING", "STOPPED")
    assert n_rec > 0 and n_later == n_after        # events only while RECORDING
    assert vm.error_message is None
    assert vm.missed_packet_count == 0


def test_second_session_with_restarted_sequence_numbers_loses_nothing():
    """Regression: counters restart at 0 on a new recording; those events must not be dropped."""
    async def go():
        ble = fast_mock(auto_start=False, restart_seq_on_session=True)
        vm = DashboardViewModel(ble)
        await vm.connect_to_sensor()
        await asyncio.sleep(0.1)
        await vm.start_calibration(); await asyncio.sleep(0.4)
        await vm.start_session();     await asyncio.sleep(0.5)
        first = len(vm.session.events)
        await vm.stop_session();      await asyncio.sleep(0.1)
        await vm.start_session();     await asyncio.sleep(0.5)   # firmware counters back to 0
        second_total = len(vm.session.events)
        await vm.disconnect_from_sensor()
        return vm, first, second_total

    vm, first, total = run(go())
    assert first > 3 and total >= first + 3        # the new session's events arrived
    assert vm.missed_packet_count == 0 and vm.parse_error_count == 0


def test_low_battery_flag_follows_the_spec_threshold():
    async def go():
        ble = fast_mock()
        ble._battery_mv = 4400                     # spec: warning at <= 4.4 V
        vm = DashboardViewModel(ble)
        await vm.connect_to_sensor()
        await asyncio.sleep(0.1)
        low = vm.low_battery
        await vm.disconnect_from_sensor()
        return low

    assert run(go()) is True
