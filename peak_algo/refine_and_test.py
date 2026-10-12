"""
Refinement and additional testing for the peak detector.
Builds on peak_detection_prototype.py (grid search + targeted stress tests).
"""
import numpy as np
import peak_detection_prototype as p

rng = np.random.default_rng(42)

# ----------------------------------------------------------------------------
# 1. Grid search over detector parameters
# ----------------------------------------------------------------------------
def run_grid_search(n_sessions=15):
    k_mad_opts = [5.0, 6.0, 8.0, 10.0, 12.0]
    refractory_opts = [0.15, 0.20, 0.30, 0.40]
    gyro_gate_opts = [300, 400, 500, 650]

    sessions = [p.generate_session() for _ in range(n_sessions)]
    cals = [p.calibrate(d["acc"], d["gyro"], d["temp"]) for d in sessions]

    results = []
    for k in k_mad_opts:
        for r in refractory_opts:
            for gg in gyro_gate_opts:
                p.K_MAD, p.REFRACTORY_S, p.KICK_GYRO_DPS = k, r, gg
                precs, recs = [], []
                for d, c in zip(sessions, cals):
                    ev = p.detect_events(d["acc"], d["gyro"], c)
                    res = p.evaluate(ev, d["kick_times"])
                    precs.append(res["precision"]); recs.append(res["recall"])
                results.append((k, r, gg, np.mean(precs), np.mean(recs)))

    # restore defaults
    p.K_MAD, p.REFRACTORY_S, p.KICK_GYRO_DPS = 8.0, 0.30, 500.0

    results.sort(key=lambda x: (-x[4], -x[3]))  # sort by recall, then precision
    print(f"{'K_MAD':>6} {'REFRAC':>7} {'GYRO_GATE':>10} {'PREC':>6} {'RECALL':>7}")
    print("Top 5 by recall (precision tiebreak):")
    for k, r, gg, pr, rc in results[:5]:
        print(f"{k:6.1f} {r:7.2f} {gg:10.0f} {pr:6.3f} {rc:7.3f}")
    print("\nCurrent default (K=8.0, R=0.30, GYRO=500):")
    for k, r, gg, pr, rc in results:
        if k == 8.0 and r == 0.30 and gg == 500:
            print(f"{k:6.1f} {r:7.2f} {gg:10.0f} {pr:6.3f} {rc:7.3f}")
            break
    return results


# ----------------------------------------------------------------------------
# 2. Targeted stress tests (adversarial scenarios, not random)
# ----------------------------------------------------------------------------
def stress_close_kicks(gap_s_list=(0.08, 0.12, 0.15, 0.20, 0.25, 0.35)):
    """Back-to-back kicks: how close can two kicks be before refractory
    period merges them into one detection?"""
    print("\n--- Stress test: back-to-back kicks ---")
    print(f"{'gap(s)':>7} {'detected_as':>12}")
    for gap in gap_s_list:
        n = int(10 * p.FS)
        acc = np.tile([0, 0, 1.0], (n, 1)).astype(float)
        gyro = np.zeros((n, 3))
        c1, c2 = 4.0, 4.0 + gap
        for c in (c1, c2):
            acc[:, 0] += p._pulse(n, c, 0.008, 16.0)
            acc[:, 2] += p._pulse(n, c, 0.010, 6.0)
            gyro[:, 1] += p._pulse(n, c - 0.05, 0.035, 1300.0)
        acc += rng.normal(0, 0.01, (n, 3))
        gyro += rng.normal(0, 0.5, (n, 3))
        events = p.detect_events(acc, gyro, None)
        n_kicks = sum(e["event_class"] == "KICK" for e in events)
        print(f"{gap:7.2f} {n_kicks:>12} event(s)  (refractory={p.REFRACTORY_S}s)")


def stress_weak_kick(gyro_peaks=(200, 300, 400, 500, 600, 800)):
    """Soft pushes / weak kicks: at what gyro peak does the KICK gate miss them?"""
    print("\n--- Stress test: weak kick gyro gate ---")
    print(f"{'gyro_peak':>10} {'classified_as':>15}")
    for gpk in gyro_peaks:
        n = int(6 * p.FS)
        acc = np.tile([0, 0, 1.0], (n, 1)).astype(float)
        gyro = np.zeros((n, 3))
        c = 3.0
        acc[:, 0] += p._pulse(n, c, 0.008, 14.0)
        acc[:, 2] += p._pulse(n, c, 0.010, 5.0)
        gyro[:, 1] += p._pulse(n, c - 0.05, 0.035, gpk)
        acc += rng.normal(0, 0.01, (n, 3))
        gyro += rng.normal(0, 0.5, (n, 3))
        events = p.detect_events(acc, gyro, None)
        cls = events[0]["event_class"] if events else "MISSED"
        print(f"{gpk:10.0f} {cls:>15}")


def stress_stale_calibration(temp_shift_c=(0, 3, 5, 8, 12)):
    """How much does an un-refreshed calibration (device warmed up) hurt
    gyro bias correction, in dps of residual error?"""
    print("\n--- Stress test: stale calibration (temp drift) ---")
    print(f"{'temp_shift_C':>13} {'residual_bias_dps':>18}")
    for dt in temp_shift_c:
        true_bias = np.array([3.0, -2.0, 1.0])
        drift_rate = np.array([0.05, -0.05, 0.025])  # dps/°C, datasheet value
        residual = np.linalg.norm(drift_rate * dt)
        print(f"{dt:13.0f} {residual:18.3f}")


if __name__ == "__main__":
    print("=== 1. Grid search ===")
    run_grid_search()
    print("\n=== 2. Stress tests ===")
    stress_close_kicks()
    stress_weak_kick()
    stress_stale_calibration()
