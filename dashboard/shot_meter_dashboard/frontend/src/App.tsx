import {
  createContext,
  useContext,
  useEffect,
  useState,
  type CSSProperties,
  type ReactNode,
} from "react";
import {
  useSensor,
  type Command,
  type Sensor,
  type SensorEvent,
  type Snapshot,
  type Zone,
} from "./useSensor";

type Screen = "home" | "sessions" | "data";

const screenPaths: Record<Screen, string> = {
  home: "/",
  sessions: "/sessions",
  data: "/data",
};

// The Figma design had a sample athlete here. The sensor does not know who is wearing it,
// so this is a plain project label. Change these three lines to show a real player.
const PROFILE = { initials: "G3", name: "Group 3", role: "Soccer Impact Sensor" };

function getScreenFromPath(pathname: string): Screen {
  if (pathname.startsWith("/sessions")) return "sessions";
  if (pathname.startsWith("/data")) return "data";
  return "home";
}

type IconName =
  | "activity"
  | "bluetooth"
  | "chevron"
  | "clock"
  | "distance"
  | "foot"
  | "gear"
  | "gps"
  | "home"
  | "imu"
  | "kick"
  | "route"
  | "sensors"
  | "sessions"
  | "trend";

function Icon({ name, size = 20 }: { name: IconName; size?: number }) {
  const paths: Record<IconName, ReactNode> = {
    activity: <path d="M3 12h4l2.2-6 4.2 12 2.4-6H21" />,
    bluetooth: (
      <>
        <path d="m7 7 10 10-5 4V3l5 4L7 17" />
      </>
    ),
    chevron: <path d="m9 18 6-6-6-6" />,
    clock: (
      <>
        <circle cx="12" cy="12" r="9" />
        <path d="M12 7v5l3 2" />
      </>
    ),
    distance: (
      <>
        <path d="M5 7h14M5 17h14" />
        <path d="m8 4-3 3 3 3m8 4 3 3-3 3" />
      </>
    ),
    foot: (
      <>
        <path d="M15.5 7.5c1.2 2.1.4 5.3-1.8 7.1-2 1.7-4.2 3.9-5.9 2.4-1.7-1.5.3-3.4 1.6-5.3 1.6-2.3 1.9-5.4 3.5-5.8 1-.3 2 .7 2.6 1.6Z" />
        <circle cx="14.5" cy="3.6" r="1" />
        <circle cx="17.6" cy="4.7" r=".8" />
      </>
    ),
    gear: (
      <>
        <circle cx="12" cy="12" r="3" />
        <path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1-2.8 2.8-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.6v.2h-4V21a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1L4.2 17l.1-.1a1.7 1.7 0 0 0 .3-1.9A1.7 1.7 0 0 0 3 14H2.8v-4H3a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9L4.2 7 7 4.2l.1.1A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-1.6v-.2h4V3a1.7 1.7 0 0 0 1 1.6 1.7 1.7 0 0 0 1.9-.3l.1-.1L19.8 7l-.1.1a1.7 1.7 0 0 0-.3 1.9 1.7 1.7 0 0 0 1.6 1h.2v4H21a1.7 1.7 0 0 0-1.6 1Z" />
      </>
    ),
    gps: (
      <>
        <circle cx="12" cy="12" r="3" />
        <path d="M12 2v3m0 14v3M2 12h3m14 0h3" />
      </>
    ),
    home: (
      <>
        <path d="m3 11 9-7 9 7" />
        <path d="M5 10v10h14V10M9 20v-6h6v6" />
      </>
    ),
    imu: (
      <>
        <rect x="5" y="5" width="14" height="14" rx="3" />
        <path d="M9 2v3m6-3v3M9 19v3m6-3v3M2 9h3m-3 6h3m14-6h3m-3 6h3" />
      </>
    ),
    kick: (
      <>
        <path d="M7 4c3 4 3 7 1 10l6 4c2 1 4 .2 5-1l1-1" />
        <path d="M8 14 4 17c-1.5 1-1 3 1 3h10" />
      </>
    ),
    route: (
      <>
        <circle cx="6" cy="18" r="2" />
        <circle cx="18" cy="6" r="2" />
        <path d="M7.5 16.5c5-1 1-7 5-8 2-.5 3 0 4-1" />
      </>
    ),
    sensors: (
      <>
        <circle cx="12" cy="12" r="2" />
        <path d="M8.5 8.5a5 5 0 0 0 0 7m7-7a5 5 0 0 1 0 7M5.5 5.5a9 9 0 0 0 0 13m13-13a9 9 0 0 1 0 13" />
      </>
    ),
    sessions: (
      <>
        <rect x="4" y="3" width="16" height="18" rx="3" />
        <path d="M8 8h8m-8 4h8m-8 4h5" />
      </>
    ),
    trend: (
      <>
        <path d="m4 16 5-5 4 3 7-8" />
        <path d="M15 6h5v5" />
      </>
    ),
  };

  return (
    <svg
      aria-hidden="true"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.8"
      strokeLinecap="round"
      strokeLinejoin="round"
    >
      {paths[name]}
    </svg>
  );
}

/* ---------- live sensor data shared by every screen ---------- */

const SensorContext = createContext<Sensor>({
  snap: null,
  link: "connecting",
  notice: null,
  send: () => {},
});

function useSensorData() {
  return useContext(SensorContext);
}

/* ---------- formatting helpers ---------- */

function clock(totalSeconds: number): string {
  const s = Math.max(0, Math.round(totalSeconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const two = (n: number) => String(n).padStart(2, "0");
  return h > 0 ? `${h}:${two(m)}:${two(sec)}` : `${m}:${two(sec)}`;
}

function sessionDate(iso: string | undefined): string {
  if (!iso) return "NO SESSION YET";
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "THIS SESSION";
  const day = d.toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" });
  const time = d.toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
  return `${day} · ${time}`.toUpperCase();
}

function prettyName(name: string): string {
  const t = name.toLowerCase().replace(/_/g, " ");
  return t.charAt(0).toUpperCase() + t.slice(1);
}

function fixed(value: number | null | undefined, digits: number, empty = "--"): string {
  return value === null || value === undefined ? empty : value.toFixed(digits);
}

function distanceText(snap: Snapshot | null): string {
  if (!snap || snap.movement.series.length === 0) return "--";
  const km = snap.movement.distance_km;
  return km < 10 ? km.toFixed(2) : km.toFixed(1);
}

const ZONE_LABEL: Record<Zone, string> = {
  standing: "Standing",
  walking: "Walking",
  running: "Running",
};

/* ---------- what the buttons do, based on the sensor's state ---------- */

interface ControlButton {
  label: string;
  cmd?: Command;
  tone?: "primary" | "secondary" | "danger";
  disabled?: boolean;
}

interface Controls {
  title: string;
  hint: string;
  badge: string;
  live: boolean;
  buttons: ControlButton[];
}

const CONNECTING_STATES = new Set(["scanning", "connecting", "discoveringServices", "reconnecting"]);

function getControls({ snap, link }: Sensor): Controls {
  if (link !== "open" || !snap) {
    return {
      title: link === "connecting" ? "Reaching the server" : "Server offline",
      hint: "Start the Python back end: python server.py --mock",
      badge: link === "connecting" ? "CONNECTING" : "OFFLINE",
      live: false,
      buttons: [],
    };
  }
  if (!snap.connection.connected) {
    if (CONNECTING_STATES.has(snap.connection.state)) {
      return {
        title: snap.connection.message,
        hint: "Looking for the shin guard (Group3Sensor).",
        badge: "CONNECTING",
        live: false,
        buttons: [{ label: "Connecting...", disabled: true }],
      };
    }
    return {
      title: snap.connection.message,
      hint: "Turn on the shin guard, then connect.",
      badge: "NOT CONNECTED",
      live: false,
      buttons: [{ label: "Connect to sensor", cmd: "connect" }],
    };
  }
  switch (snap.device.state) {
    case "CALIBRATING":
      return {
        title: "Calibrating",
        hint: "Stand still and keep the shin guard motionless.",
        badge: "CALIBRATING",
        live: false,
        buttons: [{ label: "Calibrating...", disabled: true }],
      };
    case "READY":
    case "STOPPED":
      return {
        title: snap.device.state === "READY" ? "Ready to record" : "Session stopped",
        hint:
          snap.device.state === "READY"
            ? "Calibration is done. Start when you are ready."
            : "Start again for a fresh session, or recalibrate first.",
        badge: snap.device.state.replace("_", " "),
        live: false,
        buttons: [
          { label: "Start session", cmd: "start_session" },
          { label: "Recalibrate", cmd: "recalibrate", tone: "secondary" },
        ],
      };
    case "RECORDING":
      return {
        title: "Recording",
        hint: "Kicks and movement are being measured live.",
        badge: "LIVE",
        live: true,
        buttons: [{ label: "Stop session", cmd: "stop_session", tone: "danger" }],
      };
    case "ERROR":
      return {
        title: "Sensor error",
        hint: snap.device.error_message ?? "Recalibrate, or restart the shin guard.",
        badge: "ERROR",
        live: false,
        buttons: [{ label: "Recalibrate", cmd: "recalibrate" }],
      };
    default:
      return {
        title: "Needs calibration",
        hint: "Place the shin guard on a flat surface, keep it still, then calibrate.",
        badge: snap.device.state ? snap.device.state.replace("_", " ") : "CONNECTED",
        live: false,
        buttons: [{ label: "Calibrate", cmd: "calibrate" }],
      };
  }
}

function ControlRow({ controls }: { controls: Controls }) {
  const { send } = useSensorData();
  if (controls.buttons.length === 0) return null;
  return (
    <div className="control-row">
      {controls.buttons.map((b) => (
        <button
          key={b.label}
          className={`control-button ${b.tone ?? "primary"}`}
          disabled={b.disabled || !b.cmd}
          onClick={() => b.cmd && send(b.cmd)}
        >
          {b.label}
        </button>
      ))}
    </div>
  );
}

/* ---------- warnings and errors ---------- */

function Alerts() {
  const { snap, link, notice } = useSensorData();
  const items: Array<{ tone: "error" | "warn" | "info"; text: string }> = [];
  if (link === "closed") {
    items.push({ tone: "error", text: "Cannot reach the Shot Meter server. Start it with: python server.py --mock" });
  }
  if (notice) items.push({ tone: "info", text: notice });
  if (snap) {
    const d = snap.device;
    if (d.error_code !== 0 && d.error_message) {
      items.push({ tone: d.state === "ERROR" ? "error" : "warn", text: d.error_message });
    }
    if (d.low_battery) items.push({ tone: "warn", text: "Shin guard battery is low. Charge it soon." });
    if (d.calibration_stale) items.push({ tone: "warn", text: "Calibration is out of date. Recalibrate when convenient." });
    if (d.gps_degraded && !(d.error_code !== 0 && d.error_message)) {
      items.push({ tone: "warn", text: "GPS signal is weak. Distance and speed may be off." });
    }
    if (snap.quality.missed_packets > 0) {
      items.push({ tone: "info", text: `${snap.quality.missed_packets} data packets were lost over Bluetooth.` });
    }
  }
  if (items.length === 0) return null;
  return (
    <div className="alert-stack" role="status">
      {items.map((a) => (
        <div className={`alert-banner ${a.tone}`} key={a.text}>
          {a.text}
        </div>
      ))}
    </div>
  );
}

/* ---------- shared pieces ---------- */

function ConnectionChip() {
  const { snap, link } = useSensorData();
  let label = "Not connected";
  let tone = "off";
  if (link !== "open") {
    label = link === "connecting" ? "Reaching server" : "Server offline";
  } else if (snap?.connection.connected) {
    label = "Connected";
    tone = "on";
  } else if (snap && CONNECTING_STATES.has(snap.connection.state)) {
    label = "Connecting";
    tone = "wait";
  }
  return (
    <div className="device-chip">
      <span className={`connection-dot is-${tone}`} />
      <Icon name="bluetooth" size={14} />
      <span>{label}</span>
    </div>
  );
}

function AppHeader({
  eyebrow,
  title,
  action,
}: {
  eyebrow?: string;
  title: string;
  action?: ReactNode;
}) {
  return (
    <header className="app-header">
      <div>
        {eyebrow && <div className="eyebrow">{eyebrow}</div>}
        <div className="page-title">{title}</div>
      </div>
      {action ?? <ConnectionChip />}
    </header>
  );
}

function Metric({
  icon,
  label,
  value,
  unit,
}: {
  icon: IconName;
  label: string;
  value: string;
  unit?: string;
}) {
  return (
    <div className="metric-card">
      <div className="metric-top">
        <span className="metric-icon">
          <Icon name={icon} size={17} />
        </span>
        <span className="metric-label">{label}</span>
      </div>
      <div className="metric-value">
        {value}
        {unit && value !== "--" && <span>{unit}</span>}
      </div>
    </div>
  );
}

/* ---------- Home ---------- */

function LiveDashboard() {
  const sensor = useSensorData();
  const { snap } = sensor;
  const controls = getControls(sensor);

  let activity = "WAITING";
  if (!snap || !snap.connection.connected) activity = "NOT CONNECTED";
  else if (snap.gps.zone) activity = snap.gps.zone.toUpperCase();
  else if (snap.device.recording) activity = "NO GPS FIX";
  else if (snap.device.state === "BOOT" || snap.device.state === "SENSOR_INIT") activity = "NOT CALIBRATED";
  else if (snap.device.state) activity = snap.device.state.replace("_", " ");

  const peak = snap?.session.peak_event ?? null;
  const ringPct = peak ? Math.min(100, (peak.peak_g / 60) * 100) : 0;

  return (
    <div className="screen-content home-screen">
      <AppHeader eyebrow="LIVE TRAINING" title="Shot Meter" />
      <Alerts />

      <div className="home-hero-grid">
        <section className="activity-hero">
          <div className="hero-orbit hero-orbit-one" />
          <div className="hero-orbit hero-orbit-two" />
          <div className="activity-copy">
            <span className="live-label">
              <span className="pulse-dot" />
              Movement (estimated from GPS)
            </span>
            <div className={activity.length > 9 ? "activity-name is-long" : "activity-name"}>{activity}</div>
            <div className="speed-reading">
              <span>{fixed(snap?.gps.speed_mph, 1)}</span> mph
            </div>
          </div>
          <div className="runner-mark">
            <Icon name="activity" size={28} />
          </div>
        </section>

        <section className="card session-focus">
          <div className="section-heading">
            <div>
              <div className="section-kicker">SESSION</div>
              <div className="section-title">{controls.title}</div>
            </div>
            <span className={`focus-badge ${controls.live ? "is-live" : ""}`}>{controls.badge}</span>
          </div>
          <div className="shot-score">
            <div className="score-ring" style={{ "--pct": `${ringPct}%` } as CSSProperties}>
              <span>
                {peak ? peak.peak_g.toFixed(0) : "--"}
                <small>peak g</small>
              </span>
            </div>
            <div className="score-copy">
              <strong>
                {peak ? `Hardest impact: ${prettyName(peak.name)}` : "No impacts yet"}
              </strong>
              <span>{controls.hint}</span>
            </div>
          </div>
          <ControlRow controls={controls} />
        </section>
      </div>

      <div className="metric-grid">
        <Metric icon="clock" label="Session" value={clock(snap?.session.duration_s ?? 0)} />
        <Metric icon="distance" label="Distance" value={distanceText(snap)} unit="km" />
        <Metric icon="kick" label="Kicks" value={String(snap?.session.kicks ?? 0)} />
        <Metric
          icon="activity"
          label="Fastest"
          value={snap && snap.movement.series.length > 0 ? snap.movement.max_speed_mph.toFixed(1) : "--"}
          unit="mph"
        />
      </div>
    </div>
  );
}

/* ---------- Sessions ---------- */

const ZONES: Zone[] = ["running", "walking", "standing"];

function SessionSummary({ onViewData }: { onViewData: () => void }) {
  const { snap } = useSensorData();
  const movement = snap?.movement;
  const totalMoved = movement ? ZONES.reduce((n, z) => n + movement.seconds[z], 0) : 0;
  const hasMovement = totalMoved > 0;
  const stateLabel = snap?.device.state ?? "NO DATA";

  return (
    <div className="screen-content session-screen">
      <AppHeader eyebrow={sessionDate(snap?.session.started_at)} title="Session Summary" />
      <Alerts />

      <section className="summary-hero card">
        <div className="summary-icon">
          <Icon name="route" size={24} />
        </div>
        <div>
          <span>Training session</span>
          <strong>{clock(snap?.session.duration_s ?? 0)}</strong>
          <small>Total duration</small>
        </div>
        <div className="complete-chip">{stateLabel.replace("_", " ")}</div>
      </section>

      <div className="metric-grid summary-metrics">
        <Metric icon="distance" label="Distance" value={distanceText(snap)} unit="km" />
        <Metric
          icon="activity"
          label="Avg. speed"
          value={snap && snap.movement.series.length > 0 ? snap.movement.avg_speed_mph.toFixed(1) : "--"}
          unit="mph"
        />
        <Metric icon="kick" label="Kicks" value={String(snap?.session.kicks ?? 0)} />
        <Metric icon="imu" label="Peak impact" value={fixed(snap?.session.peak_g, 1)} unit="g" />
      </div>

      <section className="card movement-card">
        <div className="section-title">Movement Breakdown</div>
        {movement && hasMovement ? (
          <>
            <div
              className="stacked-bar"
              aria-label={ZONES.map((z) => `${ZONE_LABEL[z]} ${Math.round(movement.percent[z])}%`).join(", ")}
            >
              {ZONES.filter((z) => movement.percent[z] > 0).map((z) => (
                <span className={`bar-${z}`} style={{ width: `${movement.percent[z]}%` }} key={z} />
              ))}
            </div>
            <div className="movement-legend">
              {ZONES.map((z) => (
                <div key={z}>
                  <i className={`legend-${z}`} />
                  <span>{ZONE_LABEL[z]}</span>
                  <strong>{Math.round(movement.percent[z])}%</strong>
                </div>
              ))}
            </div>
          </>
        ) : (
          <div className="empty-note">Waiting for GPS speed data. Movement shows up once the sensor has a fix.</div>
        )}
      </section>

      <button className="primary-button" onClick={onViewData}>
        <Icon name="trend" size={19} />
        View Performance Data
        <Icon name="chevron" size={17} />
      </button>

      <div className="classification-note">
        <span>
          <Icon name="activity" size={18} />
        </span>
        <div>
          <strong>Movement is estimated</strong>
          <small>
            {"Standing, walking and running are worked out from GPS speed. The sensor itself only reports kicks and impacts."}
          </small>
        </div>
      </div>
    </div>
  );
}

/* ---------- Data ---------- */

function SpeedChart({ series }: { series: Array<{ t_s: number; mph: number }> }) {
  const W = 294;
  const H = 92;
  const hasData = series.length >= 2;
  let line = "";
  let area = "";
  let labels = ["0:00", "", ""];

  if (hasData) {
    const t0 = series[0].t_s;
    const t1 = series[series.length - 1].t_s;
    const span = Math.max(t1 - t0, 1e-6);
    const yMax = Math.max(...series.map((p) => p.mph), 1) * 1.15;
    const pts = series.map((p) => [((p.t_s - t0) / span) * W, H - 4 - (p.mph / yMax) * (H - 12)]);
    line = pts.map(([x, y], i) => `${i === 0 ? "M" : "L"}${x.toFixed(1)} ${y.toFixed(1)}`).join(" ");
    area = `${line} L${W} ${H} L0 ${H}Z`;
    labels = [clock(t0), clock((t0 + t1) / 2), clock(t1)];
  }

  return (
    <div className="speed-chart">
      <div className="speed-grid">
        <span />
        <span />
        <span />
      </div>
      {hasData ? (
        <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none">
          <defs>
            <linearGradient id="speed-fill" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stopColor="var(--accent)" stopOpacity=".24" />
              <stop offset="1" stopColor="var(--accent)" stopOpacity="0" />
            </linearGradient>
          </defs>
          <path d={area} fill="url(#speed-fill)" />
          <path
            d={line}
            className="line-primary"
            fill="none"
            strokeWidth="2.5"
            vectorEffect="non-scaling-stroke"
          />
        </svg>
      ) : (
        <div className="chart-empty">Waiting for GPS speed data</div>
      )}
      <div className="speed-axis">
        <span>{labels[0]}</span>
        <span>{labels[1]}</span>
        <span>{labels[2]}</span>
      </div>
    </div>
  );
}

function eventIcon(e: SensorEvent): IconName {
  return e.name === "KICK" ? "kick" : "imu";
}

function DeviceDiagnostics({ onClose }: { onClose: () => void }) {
  const sensor = useSensorData();
  const { snap, link, send } = sensor;
  const d = snap?.device;
  const connected = !!snap?.connection.connected;

  const batteryStatus =
    d?.battery_pct != null ? `${d.battery_pct}%` : d?.battery_mv != null ? `${(d.battery_mv / 1000).toFixed(2)} V` : "--";
  const batteryDetail =
    d?.battery_mv != null
      ? `${(d.battery_mv / 1000).toFixed(2)} V${d.low_battery ? " · low" : ""}`
      : "Waiting for the sensor";

  const rows: Array<[IconName, string, string, string]> = [
    [
      "bluetooth",
      "Bluetooth",
      connected ? "Connected" : "Not connected",
      link === "open" ? (snap?.connection.message ?? "") : "Server offline",
    ],
    [
      "imu",
      "Motion sensor",
      d?.state ? prettyName(d.state) : "--",
      d ? `Raw stream ${d.raw_stream_on ? "on" : "off"}${d.error_code ? " · " + d.error_name : ""}` : "Waiting for the sensor",
    ],
    [
      "gps",
      "GPS",
      snap?.gps.fix_name ? snap.gps.fix_name.replace("_", " ") : "--",
      snap?.gps.satellites != null
        ? `${snap.gps.satellites} satellites · HDOP ${fixed(snap.gps.hdop, 1)}`
        : "No GPS data yet",
    ],
    ["activity", "Shin guard battery", batteryStatus, batteryDetail],
    [
      "sensors",
      "Temperature",
      d?.temperature_c != null ? `${d.temperature_c.toFixed(1)} °C` : "--",
      "Inside the shin guard",
    ],
  ];

  const controls = getControls(sensor);

  return (
    <div className="screen-content diagnostics-screen">
      <AppHeader
        eyebrow="SETTINGS"
        title="Device & Diagnostics"
        action={
          <button className="text-button" onClick={onClose}>
            Done
          </button>
        }
      />
      <Alerts />
      <section className="device-overview card">
        <div className="device-illustration">
          <Icon name="sensors" size={28} />
        </div>
        <div>
          <span>SHOT METER SHIN GUARD</span>
          <strong>{controls.title}</strong>
          <small>Group3Sensor · ESP32</small>
        </div>
        <i />
      </section>
      <div className="settings-label">DEVICE STATUS</div>
      <section className="card diagnostics-list">
        {rows.map(([icon, title, status, detail]) => (
          <div className="diagnostic-row" key={title}>
            <span className="sensor-icon">
              <Icon name={icon} size={17} />
            </span>
            <div>
              <strong>{title}</strong>
              <small>{detail}</small>
            </div>
            <span className="diagnostic-status">{status}</span>
          </div>
        ))}
      </section>
      {link === "open" && (
        <div className="control-row diagnostics-actions">
          {connected ? (
            <>
              <button className="control-button secondary" onClick={() => send("recalibrate")} disabled={!!d?.recording}>
                Recalibrate
              </button>
              <button className="control-button secondary" onClick={() => send("disconnect")}>
                Disconnect
              </button>
            </>
          ) : (
            <button
              className="control-button primary"
              onClick={() => send("connect")}
              disabled={!!snap && CONNECTING_STATES.has(snap.connection.state)}
            >
              Connect to sensor
            </button>
          )}
        </div>
      )}
      <div className="diagnostics-note">
        Technical information is provided for setup and troubleshooting. Your
        performance data is automatically processed before it appears in the app.
      </div>
    </div>
  );
}

function PerformanceData() {
  const { snap } = useSensorData();
  const [showDiagnostics, setShowDiagnostics] = useState(false);
  if (showDiagnostics) {
    return <DeviceDiagnostics onClose={() => setShowDiagnostics(false)} />;
  }

  const movement = snap?.movement;
  const session = snap?.session;
  const totalMoved = movement ? ZONES.reduce((n, z) => n + movement.seconds[z], 0) : 0;
  const hasMovement = !!movement && totalMoved > 0;

  // donut: running, then walking, then standing, as a conic gradient
  const run = hasMovement ? movement.percent.running : 0;
  const walk = hasMovement ? movement.percent.walking : 0;
  const donutStyle: CSSProperties = hasMovement
    ? {
        background: `conic-gradient(var(--accent) 0 ${run}%, var(--gold) ${run}% ${run + walk}%, #e7e5e1 ${run + walk}% 100%)`,
      }
    : { background: "var(--line)" };
  const dominant: Zone | null = hasMovement
    ? ZONES.reduce((best, z) => (movement.percent[z] > movement.percent[best] ? z : best), ZONES[0])
    : null;

  const timeline: Array<{ key: string; icon: IconName; kick: boolean; title: string; meta: string; time: string }> = [];
  if (movement && movement.max_speed_t_s != null && movement.max_speed_mph > 0) {
    timeline.push({
      key: "top-speed",
      icon: "activity",
      kick: false,
      title: "Top speed",
      meta: `${movement.max_speed_mph.toFixed(1)} mph`,
      time: clock(movement.max_speed_t_s),
    });
  }
  (session?.events ?? []).slice(0, 6).forEach((e) => {
    timeline.push({
      key: `ev-${e.seq}-${e.t_s}`,
      icon: eventIcon(e),
      kick: e.name === "KICK",
      title: prettyName(e.name),
      meta: `${e.peak_g.toFixed(1)} g · ${e.peak_dps.toFixed(0)} °/s · ${e.confidence}% sure`,
      time: clock(e.t_s),
    });
  });

  const lost = snap?.quality.missed_packets ?? 0;
  const bad = snap?.quality.bad_packets ?? 0;

  return (
    <div className="screen-content data-screen">
      <AppHeader
        eyebrow={`THIS SESSION · ${sessionDate(session?.started_at)}`}
        title="Performance"
        action={
          <button
            className="icon-button"
            onClick={() => setShowDiagnostics(true)}
            aria-label="Open device settings"
          >
            <Icon name="gear" size={19} />
          </button>
        }
      />
      <Alerts />

      <section className="data-hero">
        <div>
          <span>PEAK IMPACT</span>
          <strong>{fixed(session?.peak_g, 1)}</strong>
          <small>{session?.peak_event ? `${prettyName(session.peak_event.name)} · g-force` : "No impacts yet"}</small>
        </div>
        <div className="data-hero-stats">
          <span><strong>{distanceText(snap)}</strong> km</span>
          <span><strong>{session?.kicks ?? 0}</strong> kicks</span>
          <span>
            <strong>{snap && snap.movement.series.length > 0 ? snap.movement.avg_speed_mph.toFixed(1) : "--"}</strong> avg mph
          </span>
        </div>
      </section>

      <section className="card insight-card">
        <div className="section-heading">
          <div>
            <div className="section-title">Movement Breakdown</div>
            <div className="section-subtitle">Estimated from GPS speed</div>
          </div>
          <span className="insight-total">{clock(session?.duration_s ?? 0)}</span>
        </div>
        <div className="movement-donut">
          <div className="donut-ring" style={donutStyle}>
            <span>
              {dominant ? `${Math.round(movement!.percent[dominant])}%` : "--"}
              <small>{dominant ?? "no data"}</small>
            </span>
          </div>
          <div className="compact-legend">
            {ZONES.map((z) => (
              <div key={z}>
                <i className={`legend-${z}`} />
                <span>{ZONE_LABEL[z]}</span>
                <strong>{movement ? clock(movement.seconds[z]) : "0:00"}</strong>
              </div>
            ))}
            <div>
              <i className="legend-kicking" />
              <span>Kicking</span>
              <strong>{session?.kicks ?? 0} {session?.kicks === 1 ? "event" : "events"}</strong>
            </div>
          </div>
        </div>
      </section>

      <section className="card insight-card">
        <div className="section-heading">
          <div>
            <div className="section-title">Speed</div>
            <div className="section-subtitle">Throughout your session</div>
          </div>
          <div className="peak-speed">
            <strong>{snap && snap.movement.series.length > 0 ? snap.movement.max_speed_mph.toFixed(1) : "--"}</strong> mph peak
          </div>
        </div>
        <SpeedChart series={movement?.series ?? []} />
      </section>

      <section className="card insight-card">
        <div className="section-heading">
          <div>
            <div className="section-title">Activity Timeline</div>
            <div className="section-subtitle">Latest moments from your session</div>
          </div>
          <span className="event-count">
            {session?.kicks_per_min != null ? `${session.kicks_per_min.toFixed(1)} kicks/min` : "-- kicks/min"}
          </span>
        </div>
        <div className="event-list">
          {timeline.length === 0 && <div className="empty-note">No kicks or impacts recorded yet.</div>}
          {timeline.map((row) => (
            <div className="event-row" key={row.key}>
              <span className={`event-icon ${row.kick ? "kick-event" : ""}`}>
                <Icon name={row.icon} size={17} />
              </span>
              <div>
                <strong>{row.title}</strong>
                <small>{row.meta}</small>
              </div>
              <time>{row.time}</time>
            </div>
          ))}
        </div>
      </section>

      <section className="card trend-card">
        <div>
          <span className="trend-icon"><Icon name="bluetooth" size={18} /></span>
          <div>
            <div className="section-title">Data quality</div>
            <div className="section-subtitle">
              {lost} lost over Bluetooth · {bad} unreadable
            </div>
          </div>
        </div>
        <strong style={lost + bad > 0 ? { color: "#9b6805" } : undefined}>{lost + bad === 0 ? "Clean" : "Gaps"}</strong>
      </section>
    </div>
  );
}

/* ---------- App shell ---------- */

function BottomNav({
  screen,
  onNavigate,
}: {
  screen: Screen;
  onNavigate: (screen: Screen) => void;
}) {
  const items: Array<[Screen, IconName, string]> = [
    ["home", "home", "Home"],
    ["sessions", "sessions", "Sessions"],
    ["data", "trend", "Data"],
  ];
  return (
    <nav className="bottom-nav" aria-label="Main navigation">
      {items.map(([id, icon, label]) => (
        <button
          className={screen === id ? "nav-item active" : "nav-item"}
          onClick={() => onNavigate(id)}
          aria-current={screen === id ? "page" : undefined}
          key={id}
        >
          <span className="nav-icon">
            <Icon name={icon} size={21} />
          </span>
          <span>{label}</span>
        </button>
      ))}
    </nav>
  );
}

export default function App() {
  const sensor = useSensor();
  const { snap, link } = sensor;
  const [screen, setScreen] = useState<Screen>(() =>
    getScreenFromPath(window.location.pathname),
  );

  useEffect(() => {
    const handlePopState = () => {
      setScreen(getScreenFromPath(window.location.pathname));
    };

    window.addEventListener("popstate", handlePopState);
    return () => window.removeEventListener("popstate", handlePopState);
  }, []);

  useEffect(() => {
    const pageNames: Record<Screen, string> = {
      home: "Live Dashboard",
      sessions: "Sessions",
      data: "Performance Data",
    };
    document.title = `${pageNames[screen]} | Shot Meter`;
  }, [screen]);

  const navigate = (nextScreen: Screen) => {
    const nextPath = screenPaths[nextScreen];
    if (window.location.pathname !== nextPath) {
      window.history.pushState({}, "", nextPath);
    }
    setScreen(nextScreen);
  };

  const connected = link === "open" && !!snap?.connection.connected;

  return (
    <SensorContext.Provider value={sensor}>
      <main className="app-shell">
        <div className="dashboard-app">
          <aside className="site-sidebar">
            <div className="brand">
              <span className="brand-mark">
                <Icon name="kick" size={23} />
              </span>
              <span className="brand-name">Shot Meter</span>
            </div>
            <BottomNav screen={screen} onNavigate={navigate} />
            <div className={connected ? "sidebar-device" : "sidebar-device is-off"}>
              <div className="device-visual"><Icon name="sensors" size={24} /></div>
              <div>
                <strong>Shin guard</strong>
                <span><i /> {connected ? "Connected" : link === "open" ? "Not connected" : "Server offline"}</span>
              </div>
            </div>
          </aside>
          <section className="workspace">
            <div className="desktop-topbar">
              <div>
                <span>LIVE SESSION</span>
                <strong>{snap ? sessionDate(snap.session.started_at) : "Waiting for data"}</strong>
              </div>
              <div className="athlete-profile">
                <span className="avatar">{PROFILE.initials}</span>
                <span><strong>{PROFILE.name}</strong><small>{PROFILE.role}</small></span>
              </div>
            </div>
            <div className="screen">
              {screen === "home" && <LiveDashboard />}
              {screen === "sessions" && (
                <SessionSummary onViewData={() => navigate("data")} />
              )}
              {screen === "data" && <PerformanceData />}
            </div>
          </section>
        </div>
      </main>
    </SensorContext.Provider>
  );
}
