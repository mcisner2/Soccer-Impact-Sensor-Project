# Shot Meter dashboard (Group 3 Soccer Impact Sensor)

Two folders:

- `backend/`  Python. Talks Bluetooth to the sensor (or a fake one), computes the numbers, serves the dashboard.
- `frontend/` Your Figma Make design, now showing live data. A ready-to-run build is already in `frontend/dist`.

## Run it (demo, no hardware)

```
cd backend
pip install -r requirements.txt
python server.py --mock --manual
```

Open http://localhost:8765. Press Calibrate, wait about 3 seconds, press Start session. Kicks, speed and distance start moving.
`python server.py --mock` skips the buttons and starts already recording.

## Run it with the real sensor

```
python server.py --real
```

Bluetooth on, sensor powered, keep it still for the 4 second calibration. The Bluetooth code has not been run against
real hardware yet (no sensor yet), so expect a first-connection fix or two.

## Change the front end

```
cd frontend
pnpm install          # or npm install
pnpm dev              # hot reload on http://localhost:5173, while server.py runs in another terminal
pnpm build            # rebuilds frontend/dist
```

Screens: Home (live), Sessions (summary), Data (charts, timeline, gear icon = device diagnostics).
Live-data code: `frontend/src/useSensor.ts` and `frontend/src/App.tsx`.

## Tests

```
cd backend && python -m pytest        # 104 tests
```
