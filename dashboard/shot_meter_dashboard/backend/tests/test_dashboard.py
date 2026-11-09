"""Dashboard snapshot maths + the WebSocket server (mock sensor, no hardware)."""
import asyncio
import json

import pytest
from aiohttp import ClientSession, WSMsgType
from aiohttp.test_utils import TestClient, TestServer

from ble_central import DashboardViewModel, MockBLEManager
from ble_central.dashboard_state import build_snapshot, movement_zone
from ble_central.packet_parser import encode_event, encode_status, encode_telemetry
from server import make_app, run_command


def run(coro):
    return asyncio.run(coro)


def feed(vm, payloads):
    for p in payloads:
        vm._handle_raw(p, None)


def tel(seq, t_ms, speed_cm_s, fix=1):
    return encode_telemetry(fix, seq, t_ms, 400000000, -740000000, speed_cm_s, 8, 12)


# ---------------- snapshot ----------------

def fresh_vm():
    return DashboardViewModel(MockBLEManager())


def test_empty_snapshot_has_no_numbers_but_does_not_crash():
    s = build_snapshot(fresh_vm())
    assert s["connection"]["connected"] is False
    assert s["device"]["state"] is None and s["gps"]["has_fix"] is False
    assert s["session"]["kicks"] == 0 and s["session"]["peak_g"] is None
    assert s["session"]["kicks_per_min"] is None
    assert s["movement"]["distance_m"] == 0 and s["movement"]["series"] == []
    json.dumps(s)  # must be JSON-serialisable


def test_zones():
    assert movement_zone(0.0) == "standing"
    assert movement_zone(1.0) == "walking"
    assert movement_zone(2.2) == "running"
    assert movement_zone(7.0) == "running"


def test_distance_zones_and_speeds():
    vm = fresh_vm()
    # 1 m/s for 10 s, then 4 m/s for 5 s, then 0 for 5 s
    pts = [(t, 100) for t in range(0, 10)] + [(t, 400) for t in range(10, 15)] + [(t, 0) for t in range(15, 21)]
    feed(vm, [tel(i, t * 1000, v) for i, (t, v) in enumerate(pts)])
    m = build_snapshot(vm)["movement"]
    assert m["distance_m"] == pytest.approx(10 * 1.0 + 5 * 4.0)
    assert m["seconds"] == {"standing": 5.0, "walking": 10.0, "running": 5.0}
    assert m["percent"]["walking"] == pytest.approx(50.0)
    assert m["max_speed_mph"] == pytest.approx(4.0 * 2.2369362920544)
    assert m["max_speed_t_s"] == 10.0


def test_no_fix_samples_and_long_gaps_are_ignored():
    vm = fresh_vm()
    feed(vm, [tel(0, 0, 300), tel(1, 1_000, 999, fix=0), tel(2, 2_000, 300), tel(3, 60_000, 300)])
    s = build_snapshot(vm)
    # the no-fix sample is dropped; the 58 s gap is skipped; only the 0->2 s span counts
    assert s["movement"]["distance_m"] == pytest.approx(3.0 * 2.0)
    assert s["gps"]["has_fix"] is True


def test_no_fix_hides_speed():
    vm = fresh_vm()
    feed(vm, [tel(0, 0, 300, fix=0)])
    g = build_snapshot(vm)["gps"]
    assert g["has_fix"] is False and g["speed_mph"] is None and g["zone"] is None


def test_series_is_downsampled_and_keeps_ends():
    vm = fresh_vm()
    feed(vm, [tel(i, i * 1000, 100 + i) for i in range(200)])
    series = build_snapshot(vm)["movement"]["series"]
    assert len(series) == 60
    assert series[0]["t_s"] == 0 and series[-1]["t_s"] == 199


def test_only_kicks_count_as_kicks_and_peak_is_highest():
    vm = fresh_vm()
    feed(vm, [
        encode_event(1, 0, 10_000, 1500, 900, 80),
        encode_event(1, 1, 20_000, 2500, 1000, 90),
        encode_event(4, 2, 30_000, 4000, 100, 70),   # ground impact: not a kick
        encode_status(4, 0, 0x08, 4800, 250, 60_000),
    ])
    s = build_snapshot(vm)["session"]
    assert s["kicks"] == 2 and s["events_total"] == 3
    assert s["peak_g"] == pytest.approx(40.0)
    assert s["events"][0]["seq"] == 2                   # newest first
    assert s["duration_s"] == 60.0
    assert s["kicks_per_min"] == pytest.approx(2.0)


def test_kicks_per_minute_hidden_for_very_short_sessions():
    vm = fresh_vm()
    feed(vm, [encode_event(1, 0, 5_000, 1500, 900, 80)])
    assert build_snapshot(vm)["session"]["kicks_per_min"] is None


def test_events_sent_are_capped():
    vm = fresh_vm()
    feed(vm, [encode_event(1, i, i * 1000, 1500, 900, 80) for i in range(100)])
    s = build_snapshot(vm)["session"]
    assert len(s["events"]) == 30 and s["kicks"] == 100


def test_device_error_message_reaches_snapshot():
    vm = fresh_vm()
    feed(vm, [encode_status(3, 0x11, 0, 4800, 250, 1000)])
    d = build_snapshot(vm)["device"]
    assert d["state"] == "READY" and d["error_code"] == 0x11 and d["error_message"]


# ---------------- server ----------------

def fast_mock(**kw):
    return MockBLEManager(interval_s=0.02, step_delay_s=0.005, status_every=3,
                          event_every=2, telemetry_every=2, cal_duration_s=0.1, **kw)


async def next_state(ws, until=lambda s: True, timeout=3.0):
    end = asyncio.get_event_loop().time() + timeout
    while True:
        msg = await asyncio.wait_for(ws.receive(), timeout=max(0.01, end - asyncio.get_event_loop().time()))
        if msg.type != WSMsgType.TEXT:
            raise AssertionError(f"unexpected {msg.type}")
        data = json.loads(msg.data)
        if data["type"] == "state" and until(data["data"]):
            return data["data"]


def test_websocket_streams_live_state_from_mock():
    async def go():
        async with TestClient(TestServer(make_app(fast_mock()))) as client:
            async with client.ws_connect("/ws") as ws:
                return await next_state(ws, lambda s: s["session"]["events_total"] > 0
                                        and s["gps"]["has_fix"] and s["connection"]["connected"])

    s = run(go())
    assert s["device"]["state"] == "RECORDING" and s["quality"]["missed_packets"] == 0


def test_manual_flow_calibrate_then_start_then_stop():
    async def go():
        ble = fast_mock(auto_start=False)
        async with TestClient(TestServer(make_app(ble))) as client:
            async with client.ws_connect("/ws") as ws:
                await next_state(ws, lambda s: s["connection"]["connected"])
                await ws.send_json({"cmd": "calibrate"})
                await next_state(ws, lambda s: s["device"]["state"] == "READY")
                await ws.send_json({"cmd": "start_session"})
                await next_state(ws, lambda s: s["device"]["recording"])
                await ws.send_json({"cmd": "stop_session"})
                return await next_state(ws, lambda s: s["device"]["state"] == "STOPPED")

    assert run(go())["device"]["recording"] is False


def test_start_before_calibration_shows_the_sensors_error():
    async def go():
        async with TestClient(TestServer(make_app(fast_mock(auto_start=False)))) as client:
            async with client.ws_connect("/ws") as ws:
                await next_state(ws, lambda s: s["connection"]["connected"])
                await ws.send_json({"cmd": "start_session"})
                return await next_state(ws, lambda s: s["device"]["error_code"] == 0x11)

    assert "alibrate" in run(go())["device"]["error_message"]


def test_unknown_command_gets_a_notice():
    async def go():
        async with TestClient(TestServer(make_app(fast_mock()))) as client:
            async with client.ws_connect("/ws") as ws:
                await ws.send_json({"cmd": "format_disk"})
                while True:
                    msg = json.loads((await asyncio.wait_for(ws.receive(), 3)).data)
                    if msg["type"] == "notice":
                        return msg

    assert "Unknown command" in run(go())["message"]


def test_command_while_disconnected_gives_friendly_notice():
    async def go():
        vm = DashboardViewModel(fast_mock())
        return await run_command(vm, "calibrate")

    assert run(go()) == "Not connected to the sensor yet"


def test_foreign_website_origin_is_rejected():
    async def go():
        async with TestClient(TestServer(make_app(fast_mock()))) as client:
            bad = await client.get("/ws", headers={"Origin": "https://evil.example"})
            ok = await client.ws_connect("/ws", headers={"Origin": "http://localhost:5173"})
            await ok.close()
            return bad.status

    assert run(go()) == 403


def test_health_endpoint():
    async def go():
        async with TestClient(TestServer(make_app(fast_mock(), auto_connect=False))) as client:
            r = await client.get("/api/health")
            return r.status, await r.json()

    assert run(go()) == (200, {"ok": True})
