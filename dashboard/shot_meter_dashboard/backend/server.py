"""Live dashboard server: BLE back end  ->  WebSocket  ->  Shot Meter front end.

  python server.py --mock                 # fake sensor, data flows immediately (best for demos)
  python server.py --mock --manual        # fake sensor, you press Calibrate then Start in the dashboard
  python server.py --real                 # real Group3Sensor over Bluetooth
  python server.py --mock --static ../frontend/dist   # serve the built front end (found automatically if it is next door)

Then open http://localhost:8765 . The front end only ever talks to this server; it never sees
Bluetooth, bytes or packets. The server listens on localhost only unless you pass --host 0.0.0.0
(anyone who can reach the port can then send commands to the sensor).
"""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
from typing import Optional
from urllib.parse import urlparse

from aiohttp import WSMsgType, web

from ble_central import BleakBLEManager, DashboardViewModel, MockBLEManager, NotConnectedError
from ble_central.dashboard_state import build_snapshot

BLE_KEY = web.AppKey("ble")
VM_KEY = web.AppKey("vm", DashboardViewModel)

PUSH_INTERVAL_S = 0.2
STATE_READY, STATE_STOPPED = "READY", "STOPPED"

# command name sent by the front end -> what to do
COMMANDS = {"connect", "disconnect", "calibrate", "recalibrate", "start_session", "stop_session", "new_session"}


def _origin_allowed(request: web.Request) -> bool:
    """Block other websites from driving the sensor through the user's browser."""
    origin = request.headers.get("Origin")
    if not origin:
        return True  # non-browser clients (scripts, tests)
    host = urlparse(origin).hostname
    return host in ("localhost", "127.0.0.1", "::1") or origin.split("//", 1)[-1] == request.host


async def run_command(vm: DashboardViewModel, name: str) -> Optional[str]:
    """Execute one front-end command. Returns an error message for the user, or None."""
    try:
        if name == "connect":
            await vm.connect_to_sensor()
        elif name == "disconnect":
            await vm.disconnect_from_sensor()
        elif name == "calibrate":
            await vm.start_calibration()
        elif name == "recalibrate":
            await vm.recalibrate()
        elif name == "start_session":
            # A new recording restarts the device clock, so begin a fresh app-side session --
            # but only if the sensor is able to start (otherwise keep what is on screen).
            state = vm.device_state_name
            if state in (STATE_READY, STATE_STOPPED):
                vm.new_session()
            await vm.start_session()
        elif name == "stop_session":
            await vm.stop_session()
        elif name == "new_session":
            vm.new_session()
    except NotConnectedError:
        return "Not connected to the sensor yet"
    return None


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    if not _origin_allowed(request):
        raise web.HTTPForbidden(text="origin not allowed")
    vm: DashboardViewModel = request.app[VM_KEY]
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(request)

    async def pusher() -> None:
        last = None
        while not ws.closed:
            payload = json.dumps({"type": "state", "data": build_snapshot(vm)}, separators=(",", ":"))
            if payload != last:
                await ws.send_str(payload)
                last = payload
            await asyncio.sleep(PUSH_INTERVAL_S)

    push_task = asyncio.create_task(pusher())
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            try:
                cmd = json.loads(msg.data).get("cmd")
            except (ValueError, AttributeError):
                cmd = None
            if cmd not in COMMANDS:
                await ws.send_json({"type": "notice", "message": f"Unknown command: {cmd!r}"})
                continue
            error = await run_command(vm, cmd)
            if error:
                await ws.send_json({"type": "notice", "message": error})
    finally:
        push_task.cancel()
    return ws


def make_app(ble, static_dir: Optional[Path] = None, auto_connect: bool = True) -> web.Application:
    app = web.Application()
    app[BLE_KEY] = ble
    app[VM_KEY] = DashboardViewModel(ble)
    app.router.add_get("/ws", ws_handler)

    async def health(_request: web.Request) -> web.Response:
        return web.json_response({"ok": True})

    app.router.add_get("/api/health", health)

    if static_dir and (static_dir / "index.html").exists():
        index = static_dir / "index.html"
        if (static_dir / "assets").is_dir():
            app.router.add_static("/assets", static_dir / "assets")

        async def spa(_request: web.Request) -> web.FileResponse:
            # every screen (/, /sessions, /data) is the same single page
            return web.FileResponse(index)

        for path in ("/", "/sessions", "/data"):
            app.router.add_get(path, spa)

    async def on_startup(a: web.Application) -> None:
        if auto_connect:
            await a[VM_KEY].connect_to_sensor()

    async def on_cleanup(a: web.Application) -> None:
        await a[VM_KEY].disconnect_from_sensor()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    return app


def main() -> None:
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--mock", action="store_true", help="use the fake sensor")
    mode.add_argument("--real", action="store_true", help="use the real sensor over Bluetooth")
    ap.add_argument("--manual", action="store_true",
                    help="(mock) start un-calibrated so you press Calibrate, then Start, in the dashboard")
    ap.add_argument("--drop-every", type=int, default=0, help="(mock) lose every Nth packet")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--static", type=Path, default=None, help="folder with the built front end (dist)")
    args = ap.parse_args()

    here = Path(__file__).resolve().parent
    static = args.static or next((p for p in (here / "dist", here.parent / "frontend" / "dist", here.parent / "shot_meter_app" / "dist") if p.exists()), None)

    if args.mock:
        ble = MockBLEManager(interval_s=0.2, auto_start=not args.manual, cal_duration_s=3.0,
                             drop_every=args.drop_every, event_every=15, telemetry_every=5)
    else:
        ble = BleakBLEManager()

    app = make_app(ble, static)
    print(f"Dashboard: http://{'localhost' if args.host == '127.0.0.1' else args.host}:{args.port}"
          f"   (sensor: {'MOCK' if args.mock else 'REAL'}; front end: {static or 'not found - run the Vite dev server'})")
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
