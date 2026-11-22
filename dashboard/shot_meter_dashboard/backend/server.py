"""Live dashboard server: BLE back end  ->  WebSocket  ->  Shot Meter front end.

  python server.py --mock                 # fake sensor, data flows immediately (best for demos)
  python server.py --mock --manual        # fake sensor, you press Calibrate then Start in the dashboard
  python server.py --real                 # real Group3Sensor over Bluetooth
  python server.py --mock --static ../frontend/dist   # serve the built front end (found automatically if it is next door)
  python server.py --mock --no-save       # do not keep sessions on disk
  python server.py --real --live-test     # live-test mode: a full live event log at /live, with marking

Then open http://localhost:8765 . The front end only ever talks to this server; it never sees
Bluetooth, bytes or packets. The server listens on localhost only unless you pass --host 0.0.0.0
(anyone who can reach the port can then send commands to the sensor).

Sessions are saved automatically when the player presses Stop, to a SQLite file (sessions.db next
to this file, or --db PATH). The front end can list them and discard one; see ble_central/session_db.py.

Live-test mode (--live-test) serves live_test.html at /live: every event with its times, a button to
mark an event as wrongly flagged, and a CSV download. The marks are saved with the session on Stop.
"""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
from typing import Any, Optional
from urllib.parse import urlparse

from aiohttp import WSMsgType, web

from ble_central import BleakBLEManager, DashboardViewModel, MockBLEManager, NotConnectedError
from ble_central.dashboard_state import build_snapshot
from ble_central.live_events import EventLabels, build_live_events
from ble_central.session_db import SessionDatabase

BLE_KEY = web.AppKey("ble")
VM_KEY = web.AppKey("vm", DashboardViewModel)
ARCHIVE_KEY = web.AppKey("archive", SessionDatabase)
LIVE_KEY = web.AppKey("live_test", bool)
LABELS_KEY = web.AppKey("labels", EventLabels)

PUSH_INTERVAL_S = 0.2
STATE_READY, STATE_STOPPED = "READY", "STOPPED"

# command name sent by the front end -> what to do
COMMANDS = {"connect", "disconnect", "calibrate", "recalibrate", "start_session", "stop_session", "new_session",
            "discard_session", "flag_event", "unflag_event"}


def _origin_allowed(request: web.Request) -> bool:
    """Block other websites from driving the sensor through the user's browser."""
    origin = request.headers.get("Origin")
    if not origin:
        return True  # non-browser clients (scripts, tests)
    host = urlparse(origin).hostname
    return host in ("localhost", "127.0.0.1", "::1") or origin.split("//", 1)[-1] == request.host


def _save_finished_session(vm: DashboardViewModel, archive: Optional[SessionDatabase],
                           live_test: bool = False, labels: Optional[EventLabels] = None) -> Optional[str]:
    """Save the current session if it has data and was not saved yet. Returns a message for the user."""
    if archive is None:
        return None
    wrong = set(labels.wrong_set(vm.session)) if labels is not None else None
    saved_id = archive.save_once(vm.session, build_snapshot(vm), live_test=live_test, wrong_events=wrong)
    return "Session saved" if saved_id is not None else None


async def run_command(vm: DashboardViewModel, name: str, archive: Optional[SessionDatabase] = None,
                      arg: Any = None, live_test: bool = False,
                      labels: Optional[EventLabels] = None) -> Optional[str]:
    """Execute one front-end command. Returns a message for the user (an error or a short
    confirmation), or None."""
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
            try:
                await vm.stop_session()
            except NotConnectedError:
                # The sensor is gone, but whatever was recorded so far is still worth keeping.
                return _save_finished_session(vm, archive, live_test, labels) or "Not connected to the sensor yet"
            # Stopping saves the session automatically; it can be discarded from the Sessions page.
            return _save_finished_session(vm, archive, live_test, labels)
        elif name == "new_session":
            vm.new_session()
        elif name == "discard_session":
            if archive is None:
                return "Saving is turned off on this server"
            if isinstance(arg, bool) or not isinstance(arg, int):
                return "Pick a saved session to discard"
            return "Session discarded" if archive.delete_session(arg) else "That session was already gone"
        elif name in ("flag_event", "unflag_event"):
            # Live-test mode only: mark an event of the current session as wrongly flagged (or undo it).
            if not live_test or labels is None:
                return "Start the server with --live-test to mark events"
            if isinstance(arg, bool) or not isinstance(arg, int):
                return "Pick an event to mark"
            if not labels.set_wrong(vm.session, arg, name == "flag_event"):
                return "That event is no longer in this session"
    except NotConnectedError:
        return "Not connected to the sensor yet"
    return None


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    if not _origin_allowed(request):
        raise web.HTTPForbidden(text="origin not allowed")
    vm: DashboardViewModel = request.app[VM_KEY]
    archive: Optional[SessionDatabase] = request.app.get(ARCHIVE_KEY)
    live_test: bool = bool(request.app.get(LIVE_KEY, False))
    labels: Optional[EventLabels] = request.app.get(LABELS_KEY)
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(request)

    async def pusher() -> None:
        last = None
        last_sessions_version = None
        last_live_key = None
        while not ws.closed:
            payload = json.dumps({"type": "state", "data": build_snapshot(vm)}, separators=(",", ":"))
            if payload != last:
                await ws.send_str(payload)
                last = payload
            # The saved-sessions list is sent on connect and again whenever one is saved or discarded.
            if archive is not None and archive.version != last_sessions_version:
                await ws.send_str(json.dumps({"type": "sessions", "data": archive.list_sessions()},
                                             separators=(",", ":")))
                last_sessions_version = archive.version
            # Live-test mode: the full event log, sent again only when an event arrives or a mark changes.
            if live_test and labels is not None:
                session = vm.session
                wrong = labels.wrong_set(session)
                live_key = (id(session), len(session.events), frozenset(wrong))
                if live_key != last_live_key:
                    await ws.send_str(json.dumps({"type": "live_events", "data": build_live_events(session, wrong)},
                                                 separators=(",", ":")))
                    last_live_key = live_key
            await asyncio.sleep(PUSH_INTERVAL_S)

    push_task = asyncio.create_task(pusher())
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            try:
                data = json.loads(msg.data)
                cmd = data.get("cmd")
                arg = data.get("id")
            except (ValueError, AttributeError):
                cmd, arg = None, None
            if cmd not in COMMANDS:
                await ws.send_json({"type": "notice", "message": f"Unknown command: {cmd!r}"})
                continue
            message = await run_command(vm, cmd, archive, arg, live_test, labels)
            if message:
                await ws.send_json({"type": "notice", "message": message})
    finally:
        push_task.cancel()
    return ws


def make_app(ble, static_dir: Optional[Path] = None, auto_connect: bool = True,
             archive: Optional[SessionDatabase] = None, live_test: bool = False) -> web.Application:
    app = web.Application()
    app[BLE_KEY] = ble
    app[VM_KEY] = DashboardViewModel(ble)
    if archive is not None:
        app[ARCHIVE_KEY] = archive
    app[LIVE_KEY] = live_test
    if live_test:
        app[LABELS_KEY] = EventLabels()
    app.router.add_get("/ws", ws_handler)

    async def health(_request: web.Request) -> web.Response:
        return web.json_response({"ok": True})

    app.router.add_get("/api/health", health)

    if live_test:
        live_page = Path(__file__).resolve().parent / "live_test.html"

        async def live(_request: web.Request) -> web.FileResponse:
            return web.FileResponse(live_page)

        app.router.add_get("/live", live)

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
    ap.add_argument("--db", type=Path, default=None, help="file to keep saved sessions in (default: sessions.db next to this file)")
    ap.add_argument("--no-save", action="store_true", help="do not save sessions to disk")
    ap.add_argument("--live-test", action="store_true",
                    help="live-test mode: serve the live event log at /live, with marking of wrongly flagged events")
    args = ap.parse_args()

    here = Path(__file__).resolve().parent
    static = args.static or next((p for p in (here / "dist", here.parent / "frontend" / "dist", here.parent / "shot_meter_app" / "dist") if p.exists()), None)

    if args.mock:
        ble = MockBLEManager(interval_s=0.2, auto_start=not args.manual, cal_duration_s=3.0,
                             drop_every=args.drop_every, event_every=15, telemetry_every=5)
    else:
        ble = BleakBLEManager()

    db_path = args.db or here / "sessions.db"
    archive = None if args.no_save else SessionDatabase(db_path)

    app = make_app(ble, static, archive=archive, live_test=args.live_test)
    shown_host = 'localhost' if args.host == '127.0.0.1' else args.host
    print(f"Dashboard: http://{shown_host}:{args.port}"
          f"   (sensor: {'MOCK' if args.mock else 'REAL'}; front end: {static or 'not found - run the Vite dev server'})")
    print(f"Saved sessions: {'OFF' if archive is None else db_path}")
    if args.live_test:
        print(f"Live test page: http://{shown_host}:{args.port}/live")
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()