# BLE Central App Skeleton (Python)

Matches the architecture document and the Soccer Impact Sensor Interface Specification (v0.1 draft):
BLE Manager -> Packet Parser -> Session Store -> View Model.

```
ble_central/
  constants.py         device name, service + 5 characteristic UUIDs, battery UUIDs, reconnect policy, MTU
  commands.py          the 6 Control commands (2 bytes each: code + argument)
  models.py            ConnectionState (9 states), Status / Event / Telemetry / RawBatch, Session,
                       device state names, error-code table (spec section 8), event class names
  packet_parser.py     decode/encode the byte formats  <-- the only file to edit if the packet spec changes
  session_store.py     keeps the session; one sequence tracker each for Event, Telemetry, Raw
  ble_manager.py       real Bluetooth (bleak) + connection state machine + send_command
  mock_ble_manager.py  fake sensor that follows the spec's calibrate -> record -> stop flow
  view_model.py        what the dashboard reads and the actions it can call
dashboard_state.py     (in ble_central/) turns the session into one JSON snapshot for the screen: distance, speeds, zones, kicks
server.py              web server: pushes that snapshot to the dashboard over a WebSocket and takes its button presses
main.py                tiny terminal dashboard to see it all working
tests/                 104 tests, including the firmware's byte-exact Status and Event vectors
```

## Run it

```
pip install -r requirements.txt
python -m pytest                              # run the tests
python main.py --mock                         # fake sensor, already recording
python main.py --mock --start-session         # full flow: START_CAL -> CALIBRATING -> READY -> START_SESSION -> RECORDING
python main.py --mock --start-session --raw   # ...and RAW_STREAM_ON
python main.py --mock --drop-every 3          # fake sensor that loses packets -> shows "missed"
python main.py --real --start-session         # real Group3Sensor (Bluetooth on, sensor powered, STAND STILL for the 4 s calibration)
```

Going from fake to real is one line in `main.py`.

## Run the dashboard (front end + back end)

```
python server.py --mock             # fake sensor, already recording: open http://localhost:8765
python server.py --mock --manual    # fake sensor you drive yourself: press Calibrate, then Start session
python server.py --real             # the real Group3Sensor
```

The built front end is picked up from `../frontend/dist`. To work on the front end with hot reload, run
`python server.py --mock` in one terminal and `pnpm dev` (or `npm run dev`) inside `../frontend` in another.
The server only listens on this computer (`127.0.0.1`) and refuses WebSocket connections from other websites.

What is real on the dashboard: connection state, device state, kicks/impacts (count, g-force, rate), GPS speed,
distance, battery voltage, temperature, GPS fix, data-quality counters, sensor error messages.
What is estimated: Standing / Walking / Running (from GPS speed; thresholds in `dashboard_state.py`).
What the sensor cannot provide, so it is not shown: steps, a performance score, Bluetooth signal strength,
firmware version, history across sessions.

## What the app does with the sensor

- Scans for `Group3Sensor` / service `1d390000-dcea-4626-9762-d6dc5fedaa02`.
- On connect: subscribes to **Status, Event, Telemetry**, reads Status once (so the device state shows immediately),
  reads/subscribes the standard **Battery Level** if the sensor has it. **RawStream** is subscribed only while the
  raw stream is on (just before RAW_STREAM_ON is written; dropped after RAW_STREAM_OFF).
- Writes commands to **Control**: START_CAL 01, START_SESSION 02, STOP_SESSION 03, RECAL 04, RAW_STREAM_ON 05,
  RAW_STREAM_OFF 06 (always exactly 2 bytes). START_SESSION is rejected by the sensor with error 0x11
  (ERR_NOT_CALIBRATED) until calibration passed; the app shows "Calibrate the sensor before starting a session".
- Device states: 0 BOOT, 1 SENSOR_INIT, 2 CALIBRATING, 3 READY, 4 RECORDING, 5 STOPPED, 6 ERROR.
  All 14 error/warning codes from spec section 8 are named and have a dashboard message (`vm.error_message`).
- Sequence tracking: Event, Telemetry and Raw each have their own counter (Status has none), so no false gaps.
  The trackers are cleared when a recording starts, because the firmware may restart its counters at 0 (without
  this, a restarted counter looks "behind" and every event of the new session would be dropped as a duplicate).
  They are NOT cleared on a Bluetooth reconnect: the spec says the device buffers and replays events, so a real gap
  after reconnect means data was lost.
- Telemetry with `fix_quality` 0 means no GPS fix; `has_fix` is False and lat/lon should not be used.

## Open items (need a decision from the team)

1. **Event classes conflict.** The firmware owner's list is 1 kick, 2 pass, 3 trap, 4 ground impact (used here).
   Ed's spec v0.1 says 0 UNKNOWN, 1 KICK, 2 HARD_IMPACT. Class 2 is different in the two. The parser accepts any
   class value, so no event is lost; only the display names change. Edit `EventClass` in `models.py` once decided.
2. **Sequence numbers on a new recording:** does the firmware restart them? The spec doesn't say. Handled either way.
3. **Battery:** the spec reports battery only as `battery_mv` in Status (NiMH pack, warning <= 4.4 V, critical <= 4.0 V)
   and doesn't mention the standard Battery Level characteristic. The app uses it if present and works without it.
4. **MTU:** the spec wants >= 185. bleak can't set it (the OS negotiates); the app records it in `manager.mtu` and
   `manager.mtu_ok` is False if it's too small.
5. Spec items marked [OPEN]/[ASSUMPTION] (telemetry rate, raw snippet in events, buffer depth, etc.) are not
   decided yet; nothing here depends on them.

## Known limits

- `BleakBLEManager` is **untested against real hardware** (no sensor yet). Command writing, the raw-stream
  subscribe order, and the after-connect subscriptions are tested with a fake client; scanning/connecting need a first run.
- No test vectors were given for Telemetry or Raw, so those are tested by round-trip and by byte layout against the spec.
- Raw batches are kept in memory for the whole session; a long raw stream will grow it.
- After a reconnect the raw stream is not re-subscribed; send RAW_STREAM_ON again.
- The spec's replay-after-reconnect (events buffered on the device) is not simulated by the mock.
