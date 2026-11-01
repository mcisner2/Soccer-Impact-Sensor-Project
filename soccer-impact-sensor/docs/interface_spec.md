# Soccer Impact Sensor: Interface Specification

**Project:** CSE 40522 Capstone, Group 3
**Author:** Edward Hawkson (peak detection, calibration, interface spec)
**Status:** DRAFT v0.1 for team review
**Hardware basis:** Group 3 BOM (09/23/26): ESP32-S3 Feather (Adafruit 5323), ICM-20649 IMU (Adafruit 4464, I2C via STEMMA QT), Ultimate GPS Breakout / MTK3339 (Adafruit 746, UART), 4x AAA NiMH pack with ADC voltage divider.

Items marked **[OPEN]** need a team decision. Items marked **[ASSUMPTION]** are values I chose that must be confirmed with real hardware data.

---

## 1. Purpose and scope

This document defines the data contracts between the firmware modules and between the device and the dashboard, so teammates can build their parts in parallel.

Pipeline:

```
ICM-20649 (I2C) ─┐
                 ├─> Sensor Drivers ─> Calibration ─> Peak Detector ─> Event/Telemetry Encoder ─> BLE ─> Dashboard
MTK3339 (UART) ──┘                          ^                                                          |
Battery ADC ─────────────────────────────────┴──────────── Session Controller <──── Control commands ───┘
```

Out of scope for v0.1: dashboard UI design, enclosure design, and event classes beyond kick / hard impact (pass vs. shot vs. cross is a later refinement of the classifier, see §11).

## 2. Conventions

| Item | Convention |
|---|---|
| Byte order | Little-endian for all BLE packets |
| Time | `t_ms`: milliseconds since session start, `uint32` (rolls over after ~49 days, not a concern) |
| Acceleration | g (1 g = 9.80665 m/s²) in firmware structs; transmitted as centi-g (`int16`/`uint16`, 0.01 g resolution) |
| Angular rate | degrees per second (dps) |
| Temperature | °C in firmware; transmitted as deci-°C (`int16`) |
| Position | WGS-84 latitude/longitude as `int32` degrees × 1e-7 |
| Speed / distance | m/s in firmware; transmitted as cm/s (`uint16`) and meters (`uint32`) |
| Sensor axes | ICM-20649 native axes. **[OPEN]** Mounting orientation behind the shin guard must be fixed and documented (which axis points up the shin) before data collection |
| Dynamic acceleration | `abs(‖a‖ − 1 g)`, orientation-independent, used for peak detection |

## 3. Hardware interfaces

### 3.1 IMU (ICM-20649)

| Parameter | Value | Source |
|---|---|---|
| Bus | I2C, 400 kHz fast mode, via STEMMA QT | Datasheet §6.2 |
| Address | 0x68 default (0x69 if AD0 jumper set) | Adafruit guide |
| Supply / logic | 1.71–3.6 V (Feather 3.3 V) | Datasheet §3.3 |
| Accel range | ±30 g (ACCEL_FS_SEL = 3), 1024 LSB/g | Datasheet Table 2 |
| Gyro range | ±4000 dps (GYRO_FS_SEL = 3), 8.2 LSB/dps **[OPEN]**: ±2000 dps has better resolution and may still cover kicks; decide after first field data | Datasheet Table 1 |
| Output data rate | 200 Hz **[ASSUMPTION]** (kick impacts are short; if real data shows impact pulses under ~10 ms are being missed, raise to 400–1000 Hz) | Datasheet allows up to 1.125 kHz |
| Bandwidth / filter | Digital low-pass set near Nyquist/2 (~100 Hz at 200 Hz ODR) **[ASSUMPTION]** | Datasheet §8.3 |
| Read method | FIFO burst read triggered by watermark interrupt on INT1 (avoids missing samples during BLE activity) | Datasheet §4.15 |
| Clock | CLKSEL = 1 (auto PLL), required for full gyro performance | Datasheet §8.1 |

### 3.2 GPS (MTK3339)

| Parameter | Value | Source |
|---|---|---|
| Bus | UART, 3.3 V logic | Adafruit product page |
| Baud rate | 57600 **[ASSUMPTION]** (default is 9600, which cannot carry 10 Hz with multiple NMEA sentences; reconfigure at startup) | Adafruit guide |
| Update rate | 10 Hz | Spec: 1–10 Hz |
| NMEA sentences | RMC + GGA only | Reduces UART load |
| Augmentation | WAAS/SBAS enabled | Spec: DGPS/WAAS/EGNOS |
| Backup power | CR1220 coin cell for RTC (needed for warm starts) **[OPEN]**: not on the current BOM, recommend adding | Adafruit guide |
| Enable pin | Driven by an ESP32 GPIO so firmware can power-gate the module | Adafruit guide |
| Sync | PPS output wired to a GPIO interrupt for timestamp alignment with the IMU stream | Adafruit guide |

### 3.3 Battery monitor

The BOM specifies a 100 kΩ / 100 kΩ divider with a 0.1 µF filter capacitor feeding an ESP32 ADC pin, so the ADC sees half the pack voltage.

| Parameter | Value |
|---|---|
| Pack | 4 × NiMH AAA in series, 4.8 V nominal (≈5.8 V freshly charged) |
| Reported quantity | `battery_mv` = 2 × measured ADC voltage, averaged over ≥16 samples |
| Low-battery warning | ≤ 4.4 V **[ASSUMPTION]** |
| Critical / shutdown | ≤ 4.0 V **[ASSUMPTION]** (≈1.0 V/cell) |

NiMH voltage is flat for most of the discharge curve, so voltage alone is a poor remaining-capacity indicator. The bench discharge curve planned for the battery-safety standard will set these thresholds properly.

## 4. Data structures (firmware)

```c
typedef struct {            // one IMU sample after unit conversion
    uint32_t t_ms;          // ms since session start
    float    acc_g[3];      // g, native axes, calibration NOT applied
    float    gyro_dps[3];   // dps, native axes, calibration NOT applied
    float    temp_c;        // die temperature
} imu_sample_t;

typedef struct {            // result of the calibration routine
    bool     ok;
    uint8_t  error_code;    // see §8
    float    gyro_bias_dps[3];
    float    gravity_g[3];  // mean at-rest accel vector
    float    temp_at_cal_c;
    uint32_t t_ms_done;
} cal_result_t;

typedef struct {            // the "detected event" data contract
    uint16_t seq;           // increments per event, wraps at 65535
    uint32_t t_ms;          // time of accel peak
    uint8_t  event_class;   // 0=UNKNOWN 1=KICK 2=HARD_IMPACT
    float    peak_accel_g;  // peak dynamic acceleration
    float    peak_gyro_dps; // peak |gyro| in window around the peak
    uint8_t  confidence;    // 0-100
    uint8_t  flags;         // bit0 = calibration stale, bit1 = GPS degraded
    // optional raw snippet: ±150 ms of |a| and |gyro| for dashboard review [OPEN]
} event_t;

typedef struct {            // GPS fix
    uint32_t t_ms;
    double   lat_deg, lon_deg;
    float    speed_mps;
    uint8_t  fix_quality;   // GGA quality: 0 none, 1 GPS, 2 DGPS/WAAS
    uint8_t  satellites;
    float    hdop;
} gps_fix_t;
```

## 5. Module interfaces (firmware API)

All functions return `int` status: `0` = OK, negative = error code from §8.

### 5.1 Calibration module

```c
int  cal_start(void);                          // begin stand-still calibration (non-blocking)
int  cal_feed(const imu_sample_t *s);          // call once per IMU sample while calibrating
cal_state_t cal_state(void);                   // CAL_IDLE | CAL_COLLECTING | CAL_DONE | CAL_FAILED
int  cal_get_result(cal_result_t *out);        // valid when state is DONE or FAILED
int  cal_apply(const imu_sample_t *in, imu_sample_t *out);   // subtracts gyro bias; passes accel through
bool cal_needs_refresh(float temp_now_c);      // true if |temp_now - temp_at_cal| > 5 °C
int  cal_opportunistic_update(const imu_sample_t *s);        // call every sample; blends a new bias
                                                             // estimate only after >2 s of stillness
```

**Behavior** (matches `calibrate()` in `peak_detection_prototype.py`):

- Collects `CAL_SECONDS` = 4 s of samples. Rejects if: mean `‖a‖` is not within 1 g ± 0.10 g or its std exceeds 0.05 g (`NOT_STILL_ACCEL`); gyro std on any axis exceeds 0.5 dps (`NOT_STILL_GYRO`); any bias axis exceeds ±5 dps, the datasheet's initial-offset tolerance (`GYRO_BIAS_OUT_OF_RANGE`); or gravity magnitude is off by more than 0.15 g (`GRAVITY_OUT_OF_RANGE`). All thresholds are **[ASSUMPTION]** until tested on hardware.
- Calibration is **recomputed every session**. Results are logged (bias, temperature, timestamp) but not reused, so the multi-day drift test can compare sessions.
- The session controller retries up to 3 times, then blocks the session and reports `ERR_CAL_FAILED`.
- `cal_opportunistic_update` never runs during motion. It is a moving-average blend, not a replacement, so a single noisy still period cannot corrupt the estimate.

### 5.2 Peak detector

```c
int  det_init(const det_config_t *cfg);        // thresholds, window sizes (defaults in §5.3)
int  det_feed(const imu_sample_t *cal_sample,  // calibrated sample in
              event_t *out_event,              // filled when an event is emitted
              bool *event_ready);              // true if out_event is valid
void det_reset(void);
```

The detector is streaming: it keeps a ring buffer of `BASELINE_WINDOW_S` of dynamic-acceleration values, refreshes the threshold once per `BASELINE_HOP_S`, and emits an event when a local maximum exceeds the threshold and the refractory period has elapsed. An event is emitted with a fixed latency of `GYRO_WIN_AFTER_S` (50 ms) so the gyro window after the peak is complete, which is negligible for a dashboard.

### 5.3 Default detector parameters

These are the values used in the prototype. They come from synthetic data, so **treat them as starting points to retune on real kicks**.

| Parameter | Default | Meaning |
|---|---|---|
| `BASELINE_WINDOW_S` | 5.0 | Trailing window for baseline stats |
| `BASELINE_HOP_S` | 1.0 | Threshold refresh interval |
| `K_MAD` | 8.0 | Threshold = median + K × scaled MAD |
| `MIN_PEAK_G` | 5.0 | Absolute floor on threshold |
| `REFRACTORY_S` | 0.30 | Minimum spacing between events |
| `KICK_GYRO_DPS` | 500 | Gyro peak (window −150 ms / +50 ms) required to label KICK |

## 6. BLE interface (device ↔ dashboard)

Bluetooth LE 5.0, device is the GATT peripheral, dashboard (laptop) is the central. One custom service. UUIDs are placeholders **[OPEN]**: generate 128-bit UUIDs before implementation.

**Service:** `SoccerSensor` (UUID TBD). Request MTU 185 or higher at connection so every packet below fits in one notification.

| Characteristic | Properties | Payload | Rate |
|---|---|---|---|
| `Control` | Write | Command packet (§6.1) | On demand |
| `Status` | Read, Notify | Status packet (§6.2) | 1 Hz and on state change |
| `Event` | Notify | Event packet (§6.3) | On each detected event |
| `Telemetry` | Notify | Telemetry packet (§6.4) | 1–10 Hz **[OPEN]** |
| `RawStream` | Notify | Raw IMU packet (§6.5) | 200 Hz batched, only in data-collection mode |

### 6.1 Control packet (dashboard → device)

| Byte | Field | Notes |
|---|---|---|
| 0 | `cmd` | 0x01 START_CAL, 0x02 START_SESSION, 0x03 STOP_SESSION, 0x04 RECAL, 0x05 RAW_STREAM_ON, 0x06 RAW_STREAM_OFF |
| 1 | `arg` | Reserved (0) |

START_SESSION is rejected with `ERR_NOT_CALIBRATED` unless calibration passed.

### 6.2 Status packet (device → dashboard), 12 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `pkt_type` = 0x01 |
| 1 | u8 | `state` (0 BOOT, 1 SENSOR_INIT, 2 CALIBRATING, 3 READY, 4 RECORDING, 5 STOPPED, 6 ERROR) |
| 2 | u8 | `error_code` (§8, 0 = none) |
| 3 | u8 | `flags` (bit0 cal_stale, bit1 gps_degraded, bit2 low_battery, bit3 raw_stream_on) |
| 4 | u16 | `battery_mv` |
| 6 | i16 | `temp_dc` (deci-°C) |
| 8 | u32 | `t_ms` |

### 6.3 Event packet, 14 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `pkt_type` = 0x02 |
| 1 | u8 | `event_class` (0 UNKNOWN, 1 KICK, 2 HARD_IMPACT) |
| 2 | u16 | `seq` |
| 4 | u32 | `t_ms` |
| 8 | u16 | `peak_accel_cg` (centi-g, max 3000 for 30 g) |
| 10 | u16 | `peak_gyro_dps` |
| 12 | u8 | `confidence` (0–100) |
| 13 | u8 | `flags` |

The dashboard should detect dropped events from gaps in `seq`.

### 6.4 Telemetry packet, 20 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `pkt_type` = 0x03 |
| 1 | u8 | `fix_quality` |
| 2 | u16 | `seq` |
| 4 | u32 | `t_ms` |
| 8 | i32 | `lat_1e7` |
| 12 | i32 | `lon_1e7` |
| 16 | u16 | `speed_cms` |
| 18 | u8 | `satellites` |
| 19 | u8 | `hdop_x10` |

Total distance, average and maximum speed, and direction changes are computed **on the dashboard** from telemetry, so the firmware stays simple. **[OPEN]** team decision.

### 6.5 Raw IMU packet (data-collection mode only)

Header: `pkt_type` = 0x04 (u8), `n_samples` (u8), `seq` (u16), `t_ms` of first sample (u32). Then `n_samples` × 14 bytes: `i16 acc[3]` (raw LSB), `i16 gyro[3]` (raw LSB), `i16 temp_raw`. This mode exists so the team can capture labeled data for threshold tuning. It is not used during normal sessions. At 200 Hz, 14 bytes/sample is about 2.8 kB/s, well within BLE throughput.

## 7. Session state machine

```
BOOT → SENSOR_INIT → CALIBRATING → READY → RECORDING → STOPPED
            │              │                    │
            └──────────────┴────────────────────┴──> ERROR (reports code, waits for reset or RECAL)
```

- **SENSOR_INIT:** configure IMU registers, verify WHO_AM_I = 0xE1, power GPS, reconfigure GPS baud/rate/sentences.
- **CALIBRATING:** runs in parallel with GPS acquisition (a cold start takes about 34 s). The player stands still for 4 s, and the dashboard prompts for this.
- **READY:** calibration passed. GPS may still be acquiring, in which case the session can start with `gps_degraded` set.
- **RECORDING:** detector and telemetry running. Opportunistic re-zero active during stillness.
- **STOPPED:** session summary available; logs retained until next START.

## 8. Error codes and failure handling

| Code | Name | Cause | Device behavior | Dashboard behavior |
|---|---|---|---|---|
| 0x01 | `NOT_STILL_ACCEL` | Motion during calibration | Retry (max 3) | Prompt "stand still" |
| 0x02 | `NOT_STILL_GYRO` | Motion during calibration | Retry (max 3) | Prompt "stand still" |
| 0x03 | `GYRO_BIAS_OUT_OF_RANGE` | Bias beyond datasheet tolerance, likely a fault | Retry, then block session | Show "sensor fault" |
| 0x04 | `GRAVITY_OUT_OF_RANGE` | Gravity magnitude wrong | Retry, then block session | Show "sensor fault" |
| 0x10 | `ERR_CAL_FAILED` | 3 failed attempts | Block session, stay in ERROR | Offer RECAL |
| 0x11 | `ERR_NOT_CALIBRATED` | START_SESSION before cal | Reject command | Prompt calibration |
| 0x20 | `ERR_IMU_COMM` | I2C failure or WHO_AM_I mismatch | Retry init; if mid-session, flag and keep GPS logging | Show warning, mark data gap |
| 0x21 | `ERR_IMU_FIFO_OVERFLOW` | Missed samples | Reset FIFO, log gap, continue | Mark gap in timeline |
| 0x30 | `WARN_GPS_NO_FIX` | No fix after 90 s | Set `gps_degraded`, continue IMU-only | Mark distance/speed low-confidence |
| 0x31 | `WARN_GPS_POOR_ACCURACY` | Static scatter > 3 m or HDOP > 2 | Set `gps_degraded` | Same |
| 0x40 | `WARN_CAL_STALE` | Die temp moved > 5 °C from cal temp | Set `cal_stale`, recal at next stillness | Show hint |
| 0x50 | `WARN_LOW_BATTERY` | ≤ 4.4 V | Set `low_battery` | Show warning |
| 0x51 | `ERR_BATTERY_CRITICAL` | ≤ 4.0 V | Flush logs, stop session, shut down | Show "session ended, battery" |
| 0x60 | `WARN_NO_EVENTS` | No events for 10 min while moving | Set flag only | Show hint: check mounting |

BLE disconnect mid-session: the device keeps recording. Events and telemetry are buffered in RAM (ring buffer) and replayed on reconnect. **[OPEN]** buffer depth (suggest ≥ 10 minutes of events, and telemetry decimated to 1 Hz while disconnected).

## 9. Timing and synchronization

- IMU timestamps come from the ESP32 microsecond timer read at the FIFO-drain interrupt, back-computed per sample from the ODR (do not timestamp at BLE send time).
- GPS timestamps: use the PPS interrupt to anchor the ESP32 timer to GPS time once per second, then convert NMEA fixes to `t_ms`. Target alignment error under 20 ms **[ASSUMPTION]**.
- The ICM-20649's internal PLL frequency varies from part to part (datasheet §4.13); read `TIMEBASE_CORRECTION_PLL` at init and correct the sample interval accordingly.

## 10. Verification plan

| Requirement | Test | Pass criterion |
|---|---|---|
| Calibration removes gyro bias | Bench: still device, compare estimated bias to 60 s of stationary mean | Within 0.1 dps **[ASSUMPTION]** |
| Calibration rejects motion | Shake device during cal | Returns `NOT_STILL_*` |
| Kick detection | Field drill with known kick count (manual tally + video) | Precision and recall ≥ 90% **[ASSUMPTION]**; final target set by the team |
| Multi-day drift | Same procedure on 2–3 different days | Logged bias/temperature stable, kick metrics within 5% |
| BLE reliability | 30-minute session with events | Zero sequence gaps at ≤ 10 m |
| GPS accuracy | Tape-measured 20–40 m course, open pitch vs. near a structure | Report distance error % and static scatter |
| Battery warning | Bench discharge | Warning fires before shutdown threshold |

## 11. Open questions for the team

1. Sensor mounting orientation behind the shin guard, and which axis is "up the shin".
2. Gyro range: ±4000 dps or ±2000 dps for better resolution.
3. Should the event packet carry a raw snippet, or is metadata plus optional raw-stream mode enough?
4. Telemetry rate (1 Hz vs. 10 Hz) and where distance/speed/direction-change math lives.
5. Add a CR1220 coin cell to the BOM so GPS starts are warm.
6. Pass/shot/cross classification: requires labeled data and a second-stage classifier, planned after the KICK vs. HARD_IMPACT baseline works.
7. Proposal text (§3) names the ESP32-C3, but the BOM uses the ESP32-S3 Feather; confirm the S3 is final so the standards discussion cites the right datasheet.
8. Confirm the ICM-20649 order status (listed sold out on 9/18); it is a single point of failure.

## 12. References

- ICM-20649 Datasheet, TDK InvenSense DS-000192, Rev 1.0 (preliminary; check against the newer v1.1).
- Adafruit Ultimate GPS Breakout (product 746) learn guide and specifications.
- Group 3 BOM, 09/23/26.
- `peak_detection_prototype.py`: reference implementation of §5.1–5.3 logic.
