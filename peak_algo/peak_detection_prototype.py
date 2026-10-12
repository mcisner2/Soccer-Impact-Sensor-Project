"""
Soccer Impact Sensor - peak detection + calibration prototype (Python)

Purpose: design and tune the algorithm logic BEFORE porting to C/C++ on the ESP32-S3.
Everything here is written so it can be ported: fixed-size windows, hop-based
baseline updates, no library calls in the core detector other than find_peaks
(which can be replaced by a simple local-max + refractory check in C).

IMPORTANT: the synthetic data below is a stand-in. Kick/stride shapes and
amplitudes are ASSUMPTIONS. Replace generate_session() with real logged data
(same array layout) as soon as hardware arrives, then re-tune the thresholds.

Sensor noise/bias numbers come from the ICM-20649 datasheet (DS-000192 rev 1.0):
  gyro noise density 0.0175 dps/sqrt(Hz), accel noise density 285 ug/sqrt(Hz),
  initial gyro ZRO +/-5 dps, gyro ZRO drift +/-0.05 dps/degC,
  initial accel offset +/-65 mg.

Run:  python3 peak_detection_prototype.py
"""
import numpy as np
from scipy.signal import find_peaks
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ----------------------------------------------------------------------------
# Configuration (all tunable; these become constants in firmware)
# ----------------------------------------------------------------------------
FS = 200                      # Hz, IMU sample rate (ASSUMPTION - see spec)
CAL_SECONDS = 4.0             # stand-still calibration window
STILL_ACC_TOL_G = 0.10        # |a| must be within 1 g +/- this
STILL_GYRO_STD_DPS = 0.5      # gyro std must be below this
GYRO_BIAS_MAX_DPS = 5.0       # datasheet initial ZRO tolerance
GRAVITY_TOL_G = 0.15

BASELINE_WINDOW_S = 5.0       # rolling window for adaptive threshold
BASELINE_HOP_S = 1.0          # threshold updated once per hop
K_MAD = 8.0                   # threshold = median + K * scaled MAD
MIN_PEAK_G = 5.0              # absolute floor on peak height (dynamic g)
REFRACTORY_S = 0.15           # min time between events (kills ringing)
                               # tuned via grid search: 0.30 merged kicks <0.35s apart
KICK_GYRO_DPS = 650.0         # gyro peak needed near impact to call it a kick
                               # tuned via grid search (see refine_and_test.py)
GYRO_WIN_BEFORE_S = 0.15      # swing precedes ball contact
GYRO_WIN_AFTER_S = 0.05
MATCH_TOL_S = 0.06            # detection counts as correct if within this of truth

rng = np.random.default_rng(7)


# ----------------------------------------------------------------------------
# Synthetic data (stand-in for real logs)
# ----------------------------------------------------------------------------
def _pulse(n, center, width_s, amp):
    t = np.arange(n) / FS
    return amp * np.exp(-0.5 * ((t - center) / width_s) ** 2)


def generate_session(duration_s=180.0, n_kicks=25, n_plants=15, temp_rise_c=8.0):
    """Returns dict with acc (g), gyro (dps), temp (C), truth kick times (s)."""
    n = int(duration_s * FS)
    t = np.arange(n) / FS
    acc = np.zeros((n, 3))
    gyro = np.zeros((n, 3))

    # Player stands still first (calibration window), then runs
    move_start = CAL_SECONDS + 2.0
    moving = t >= move_start

    # Running: stride ~2.5 Hz -> heel strikes (accel) + shin swing (gyro)
    stride_f = 2.5
    for k in np.arange(move_start, duration_s, 1.0 / stride_f):
        c = k + rng.normal(0, 0.02)
        amp = rng.uniform(2.0, 4.0)
        acc[:, 2] += _pulse(n, c, 0.025, amp)
        acc[:, 0] += _pulse(n, c, 0.03, amp * 0.4)
        gyro[:, 1] += _pulse(n, c - 0.08, 0.06, rng.uniform(150, 350))

    # Kicks: short high-g impact + big gyro swing just before contact
    margin = 8.0
    kick_times = np.sort(rng.uniform(move_start + margin, duration_s - margin, n_kicks))
    for c in kick_times:
        acc[:, 0] += _pulse(n, c, 0.008, rng.uniform(10, 22))
        acc[:, 2] += _pulse(n, c, 0.010, rng.uniform(4, 9))
        gyro[:, 1] += _pulse(n, c - 0.05, 0.035, rng.uniform(900, 1800))

    # Distractors: hard foot plants / cuts -> big accel, little gyro
    for c in rng.uniform(move_start + margin, duration_s - margin, n_plants):
        acc[:, 2] += _pulse(n, c, 0.015, rng.uniform(6, 9))
        gyro[:, 1] += _pulse(n, c - 0.05, 0.05, rng.uniform(80, 200))

    # --- sensor imperfections from the datasheet ---
    bw = FS / 2.0
    acc_noise_g = 285e-6 * np.sqrt(bw)
    gyro_noise_dps = 0.0175 * np.sqrt(bw)
    acc_offset = rng.uniform(-0.065, 0.065, 3)          # +/-65 mg
    gyro_bias = rng.uniform(-5.0, 5.0, 3)               # +/-5 dps
    temp = 22.0 + temp_rise_c * (1 - np.exp(-t / 600.0))  # warms up over session
    gyro_drift = 0.05 * (temp - temp[0])[:, None] * np.array([1, -1, 0.5])

    acc = acc + np.array([0, 0, 1.0]) + acc_offset + rng.normal(0, acc_noise_g, (n, 3))
    gyro = gyro + gyro_bias + gyro_drift + rng.normal(0, gyro_noise_dps, (n, 3))
    acc = np.clip(acc, -30, 30)          # ICM-20649 +/-30 g range
    gyro = np.clip(gyro, -4000, 4000)    # +/-4000 dps range

    return dict(acc=acc, gyro=gyro, temp=temp, t=t, kick_times=kick_times,
                true_gyro_bias=gyro_bias, true_acc_offset=acc_offset)


# ----------------------------------------------------------------------------
# Calibration
# ----------------------------------------------------------------------------
def calibrate(acc, gyro, temp_c):
    """Stand-still calibration over the first CAL_SECONDS.
    Returns dict: ok, error, gyro_bias, gravity_vec, temp_at_cal."""
    n = int(CAL_SECONDS * FS)
    a, g = acc[:n], gyro[:n]
    a_mag = np.linalg.norm(a, axis=1)
    if np.abs(a_mag.mean() - 1.0) > STILL_ACC_TOL_G or a_mag.std() > 0.05:
        return dict(ok=False, error="NOT_STILL_ACCEL")
    if g.std(axis=0).max() > STILL_GYRO_STD_DPS:
        return dict(ok=False, error="NOT_STILL_GYRO")
    bias = g.mean(axis=0)
    if np.abs(bias).max() > GYRO_BIAS_MAX_DPS:
        return dict(ok=False, error="GYRO_BIAS_OUT_OF_RANGE")
    grav = a.mean(axis=0)
    if abs(np.linalg.norm(grav) - 1.0) > GRAVITY_TOL_G:
        return dict(ok=False, error="GRAVITY_OUT_OF_RANGE")
    return dict(ok=True, error=None, gyro_bias=bias, gravity_vec=grav,
                temp_at_cal=float(temp_c[:n].mean()))


# ----------------------------------------------------------------------------
# Detection
# ----------------------------------------------------------------------------
def dynamic_accel_g(acc):
    """Magnitude of acceleration minus 1 g (orientation-independent)."""
    return np.abs(np.linalg.norm(acc, axis=1) - 1.0)


def adaptive_threshold(m):
    """Piecewise-constant threshold: median + K*scaled-MAD over a trailing
    window, refreshed once per hop. Ports directly to a ring buffer in C."""
    n = len(m)
    win = int(BASELINE_WINDOW_S * FS)
    hop = int(BASELINE_HOP_S * FS)
    thr = np.full(n, MIN_PEAK_G)
    for i in range(0, n, hop):
        seg = m[max(0, i - win):i + hop]
        med = np.median(seg)
        mad = 1.4826 * np.median(np.abs(seg - med))
        thr[i:i + hop] = max(MIN_PEAK_G, med + K_MAD * mad)
    return thr


def detect_events(acc, gyro, cal=None):
    """Returns list of event dicts (the 'detected event' data contract)."""
    if cal is not None and cal.get("ok"):
        gyro = gyro - cal["gyro_bias"]
    m = dynamic_accel_g(acc)
    thr = adaptive_threshold(m)
    peaks, props = find_peaks(m, height=thr, distance=int(REFRACTORY_S * FS))
    gmag = np.linalg.norm(gyro, axis=1)
    pre, post = int(GYRO_WIN_BEFORE_S * FS), int(GYRO_WIN_AFTER_S * FS)
    events = []
    for i, p in enumerate(peaks):
        gpk = gmag[max(0, p - pre):p + post + 1].max()
        cls = "KICK" if gpk >= KICK_GYRO_DPS else "HARD_IMPACT"
        # crude confidence: how far above threshold and above the gyro gate
        conf = float(np.clip(0.5 * (props["peak_heights"][i] / thr[p] - 1) +
                             0.5 * (gpk / KICK_GYRO_DPS - 1) * (cls == "KICK"), 0, 1))
        events.append(dict(t_s=p / FS, peak_accel_g=float(m[p]),
                           peak_gyro_dps=float(gpk), event_class=cls,
                           confidence=conf, threshold_g=float(thr[p])))
    return events


# ----------------------------------------------------------------------------
# Evaluation
# ----------------------------------------------------------------------------
def evaluate(events, kick_times):
    """Greedy match KICK detections to ground-truth kicks."""
    det = [e["t_s"] for e in events if e["event_class"] == "KICK"]
    used, tp = set(), 0
    for kt in kick_times:
        best = None
        for j, dt in enumerate(det):
            if j in used:
                continue
            if abs(dt - kt) <= MATCH_TOL_S and (best is None or abs(dt - kt) < abs(det[best] - kt)):
                best = j
        if best is not None:
            used.add(best)
            tp += 1
    fp = len(det) - tp
    fn = len(kick_times) - tp
    prec = tp / (tp + fp) if tp + fp else 0.0
    rec = tp / (tp + fn) if tp + fn else 0.0
    return dict(tp=tp, fp=fp, fn=fn, precision=prec, recall=rec)


def plot_session(data, events, thr_m, path):
    t = data["t"]
    fig, ax = plt.subplots(2, 1, figsize=(12, 6), sharex=True)
    ax[0].plot(t, thr_m["m"], lw=0.5, label="dynamic accel (g)")
    ax[0].plot(t, thr_m["thr"], "r--", lw=1, label="adaptive threshold")
    for e in events:
        c = "g" if e["event_class"] == "KICK" else "orange"
        ax[0].axvline(e["t_s"], color=c, alpha=0.4)
    for kt in data["kick_times"]:
        ax[0].plot(kt, -1, "k^", ms=4)
    ax[0].set_ylabel("g"); ax[0].legend(loc="upper right")
    ax[0].set_title("green = detected KICK, orange = HARD_IMPACT, black triangles = true kicks")
    ax[1].plot(t, np.linalg.norm(data["gyro"], axis=1), lw=0.5)
    ax[1].axhline(KICK_GYRO_DPS, color="r", ls="--", lw=1)
    ax[1].set_ylabel("|gyro| dps"); ax[1].set_xlabel("time (s)")
    fig.tight_layout(); fig.savefig(path, dpi=110); plt.close(fig)


if __name__ == "__main__":
    data = generate_session()
    cal = calibrate(data["acc"], data["gyro"], data["temp"])
    print("Calibration:", {k: (np.round(v, 3) if isinstance(v, np.ndarray) else v) for k, v in cal.items()})
    print("True gyro bias:", np.round(data["true_gyro_bias"], 3))

    events = detect_events(data["acc"], data["gyro"], cal)
    res = evaluate(events, data["kick_times"])
    print(f"\nEvents detected: {len(events)} "
          f"({sum(e['event_class']=='KICK' for e in events)} KICK, "
          f"{sum(e['event_class']=='HARD_IMPACT' for e in events)} HARD_IMPACT)")
    print("Kick detection vs ground truth:", {k: (round(v, 3) if isinstance(v, float) else v) for k, v in res.items()})

    # Ablation: what does calibration actually buy the PEAK detector?
    res_nocal = evaluate(detect_events(data["acc"], data["gyro"], None), data["kick_times"])
    print("Without calibration:          ", {k: (round(v, 3) if isinstance(v, float) else v) for k, v in res_nocal.items()})

    # Robustness sweep: repeat over many random sessions
    rs = [evaluate(detect_events(d["acc"], d["gyro"], calibrate(d["acc"], d["gyro"], d["temp"])),
                   d["kick_times"]) for d in (generate_session() for _ in range(30))]
    print(f"\n30-session sweep: mean precision {np.mean([r['precision'] for r in rs]):.3f}, "
          f"mean recall {np.mean([r['recall'] for r in rs]):.3f}")

    m = dynamic_accel_g(data["acc"])
    plot_session(data, events, dict(m=m, thr=adaptive_threshold(m)),
                 "/mnt/user-data/outputs/peak_detection_demo.png")
    print("Saved plot: peak_detection_demo.png")
