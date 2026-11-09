import { useCallback, useEffect, useRef, useState } from "react";

/** What the Python back end (server.py) sends. Mirrors ble_central/dashboard_state.py. */
export type Zone = "standing" | "walking" | "running";

export interface SensorEvent {
  seq: number;
  t_s: number;
  class: number;
  name: string;
  peak_g: number;
  peak_dps: number;
  confidence: number;
}

export interface Snapshot {
  connection: { state: string; message: string; connected: boolean };
  device: {
    state: string | null;
    recording: boolean;
    error_code: number;
    error_name: string;
    error_message: string | null;
    battery_pct: number | null;
    battery_mv: number | null;
    low_battery: boolean;
    temperature_c: number | null;
    calibration_stale: boolean;
    gps_degraded: boolean;
    raw_stream_on: boolean;
  };
  session: {
    started_at: string;
    duration_s: number;
    kicks: number;
    events_total: number;
    kicks_per_min: number | null;
    peak_g: number | null;
    peak_event: SensorEvent | null;
    latest_event: SensorEvent | null;
    events: SensorEvent[];
  };
  gps: {
    has_fix: boolean;
    fix_name: string | null;
    satellites: number | null;
    hdop: number | null;
    speed_m_s: number | null;
    speed_mph: number | null;
    zone: Zone | null;
  };
  movement: {
    distance_m: number;
    distance_km: number;
    avg_speed_mph: number;
    max_speed_mph: number;
    max_speed_t_s: number | null;
    seconds: Record<Zone, number>;
    percent: Record<Zone, number>;
    series: Array<{ t_s: number; mph: number }>;
    estimated_from_gps_speed: boolean;
  };
  quality: {
    missed_packets: number;
    bad_packets: number;
    missed_by_stream: Record<string, number>;
  };
}

export type Command =
  | "connect"
  | "disconnect"
  | "calibrate"
  | "recalibrate"
  | "start_session"
  | "stop_session"
  | "new_session";

/** "open" = talking to the Python server, "connecting" = trying, "closed" = server not reachable. */
export type ServerLink = "connecting" | "open" | "closed";

export interface Sensor {
  snap: Snapshot | null;
  link: ServerLink;
  notice: string | null;
  send: (cmd: Command) => void;
}

function serverUrl(): string {
  const override = import.meta.env.VITE_SENSOR_WS as string | undefined;
  if (override) return override;
  const scheme = window.location.protocol === "https:" ? "wss" : "ws";
  // `npm run dev` serves the page on its own port; the Python server is always on 8765.
  const host = import.meta.env.DEV
    ? `${window.location.hostname}:8765`
    : window.location.host;
  return `${scheme}://${host}/ws`;
}

const RETRY_MS = 2000;
const NOTICE_MS = 6000;

export function useSensor(): Sensor {
  const [snap, setSnap] = useState<Snapshot | null>(null);
  const [link, setLink] = useState<ServerLink>("connecting");
  const [notice, setNotice] = useState<string | null>(null);
  const socket = useRef<WebSocket | null>(null);
  const noticeTimer = useRef<number | undefined>(undefined);

  const showNotice = useCallback((message: string) => {
    setNotice(message);
    window.clearTimeout(noticeTimer.current);
    noticeTimer.current = window.setTimeout(() => setNotice(null), NOTICE_MS);
  }, []);

  useEffect(() => {
    let stopped = false;
    let retry: number | undefined;

    const open = () => {
      setLink("connecting");
      const ws = new WebSocket(serverUrl());
      socket.current = ws;
      ws.onopen = () => setLink("open");
      ws.onmessage = (e) => {
        try {
          const msg = JSON.parse(e.data as string);
          if (msg.type === "state") setSnap(msg.data as Snapshot);
          else if (msg.type === "notice") showNotice(String(msg.message));
        } catch {
          // ignore a malformed message rather than crash the page
        }
      };
      ws.onclose = () => {
        if (socket.current === ws) socket.current = null;
        if (stopped) return;
        setLink("closed");
        retry = window.setTimeout(open, RETRY_MS);
      };
      ws.onerror = () => ws.close();
    };

    open();
    return () => {
      stopped = true;
      window.clearTimeout(retry);
      window.clearTimeout(noticeTimer.current);
      socket.current?.close();
    };
  }, [showNotice]);

  const send = useCallback(
    (cmd: Command) => {
      const ws = socket.current;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ cmd }));
      } else {
        showNotice("Can't reach the Shot Meter server. Is server.py running?");
      }
    },
    [showNotice],
  );

  return { snap, link, notice, send };
}
