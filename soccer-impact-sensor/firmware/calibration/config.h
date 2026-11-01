// config.h
// Build switch and spec numbers. Every [ASSUMPTION] value lives here.

#pragma once
#include <stdint.h>
#include <stddef.h>

#define USE_MOCK_IMU 0      // 1 = fake data, 0 = real ICM-20649
#define USE_MOCK_BATTERY 1  // 1 = simulated discharge, 0 = read the divider on pins::batteryAdc
#define USE_MOCK_GPS 1      // 1 = fake NMEA text through the real parser, 0 = Serial1

namespace config
{
    // 200 Hz (spec 3.1) isn't a rate the chip can hit: gyro runs at 1100/(1+div) Hz and accel at
    // 1125/(1+div) Hz (Adafruit_ICM20X notes). div 4 gives 220 Hz gyro, 225 Hz accel. [ASSUMPTION, check datasheet]
    constexpr uint32_t imuOdrHz = 220;
    constexpr uint8_t gyroRateDivisor = 4;
    constexpr uint16_t accelRateDivisor = 4;
    constexpr uint8_t imuI2cAddress = 0x68;  // Adafruit 4464 default
    constexpr uint32_t i2cClockHz = 400000;  // fast mode: a 14-byte read takes about 0.5 ms instead of about 2 ms
    constexpr float accelLsbPerG = 1024.0f;  // +-30 g, datasheet Table 2
    constexpr float gyroLsbPerDps = 8.2f;    // +-4000 dps, datasheet Table 1
    constexpr float tempLsbPerC = 333.87f;   // datasheet, 0 counts = 21 C
    constexpr float tempZeroC = 21.0f;

    // 256 samples = 1.28 s at 200 Hz, enough to ride out a BLE stall [ASSUMPTION, measure on hardware]
    constexpr size_t rawBufferSamples = 256;

    // Max samples pulled per drain. 64 = 320 ms of slack before timestamps go stale
    constexpr size_t drainBatchMax = 64;

    // Raw packet cap. 16 samples is what an MTU of 247 fits: (247 - 3 - 8) / 14
    constexpr size_t rawMaxSamplesPerPacket = 16;

    // Battery. Thresholds are pack voltage, spec 3.3 [ASSUMPTION] until the bench discharge curve.
    // Handoff had 4600 / 4400 instead; team still needs to pick one set.
    constexpr uint32_t batteryWarnMv = 4400;
    constexpr uint32_t batteryCriticalMv = 4000;
    constexpr uint32_t batteryHysteresisMv = 100;  // warning clears only once the pack recovers this far
    constexpr uint8_t batteryConfirmReads = 3;     // readings in a row before a level changes

    constexpr float dividerRatio = 3.0f;  // 200k (two 100k in series) over 100k: pin sees a third, so 6.0 V lands at 2.0 V, inside the accurate ADC range
    constexpr float batteryCalFactor = 1.0f;  // set from one multimeter reading: dmmMv / (adcMv * dividerRatio)
    constexpr uint8_t batteryAveragedReads = 16;  // spec 3.3 asks for at least 16

    // Percent is a straight line between these, rough on purpose: NiMH voltage is flat for most of the discharge
    constexpr uint32_t batteryEmptyMv = 4000;
    constexpr uint32_t batteryFullMv = 5600;

    // mock only: 10 mV/s reaches critical (and deep sleep) about 160 s after boot. Set 0 for long bench tests.
    constexpr uint32_t mockBatteryDrainMvPerS = 0;

    // GPS. The MTK3339 computes at most 5 fixes a second (Adafruit_GPS notes), and SBAS/WAAS only
    // works at 1 Hz. Spec 3.2 asks for 10 Hz plus WAAS, which can't both happen. [OPEN, team decision]
    constexpr uint32_t gpsFixHz = 5;            // 1 turns SBAS/WAAS on, anything faster leaves it off
    constexpr uint32_t gpsBootBaud = 9600;      // what the module wakes up at with no coin cell
    constexpr uint32_t gpsFastBaud = 57600;     // spec 3.2 [ASSUMPTION]
    constexpr uint32_t gpsProbeMs = 1500;       // how long to listen at one baud before trying the other
    constexpr uint32_t gpsCommandGapMs = 100;   // spacing between config commands so none get dropped
    constexpr uint32_t gpsNoFixTimeoutMs = 90000;  // spec 8, WARN_GPS_NO_FIX
    constexpr uint8_t gpsMaxGoodHdopX10 = 20;      // HDOP above 2.0 = degraded, spec 8 [ASSUMPTION]

    // Calibration, spec 5.1. Every number is an [ASSUMPTION] until it has been checked on the bench.
    constexpr uint32_t calSeconds = 4;              // stand-still window at the start of a session
    constexpr float calStillAccelMeanTolG = 0.10f;  // mean |a| must be within 1 g +- this
    constexpr float calStillAccelStdG = 0.05f;      // spread of |a| allowed while "still"
    constexpr float calStillGyroStdDps = 0.5f;      // per-axis gyro spread allowed while "still". Depends on the chip's
                                                    // low-pass filter, which isn't configured yet: print the at-rest std and retune
    constexpr float calGyroBiasMaxDps = 5.0f;       // datasheet initial zero-rate tolerance, a bigger bias means a fault or motion
    constexpr float calGravityTolG = 0.15f;         // |mean accel vector| must be within 1 g +- this
    constexpr float calRefreshTempDeltaC = 5.0f;    // die temp moved this far since calibration = stale (spec 5.1)
    constexpr uint32_t calRezeroStillMs = 2000;     // stillness needed before the bias is touched mid-session (spec 5.1)
    constexpr float calRezeroBlend = 0.25f;         // share of a new still-window estimate blended into the bias
    constexpr float calRezeroMaxStepDps = 1.0f;     // bigger jump than this in one blend = probably slow rotation, ignored
}
