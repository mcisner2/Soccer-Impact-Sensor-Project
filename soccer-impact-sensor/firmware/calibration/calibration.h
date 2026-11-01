// calibration.h
// Calibration module (spec 5.1). Same start/feed/state/result shape as the placeholder it replaces,
// so sessionController and firmware.ino keep working, plus apply(), needsRefresh() and opportunisticUpdate().

#pragma once
#include <stdint.h>
#include "sampler.h"

enum class CalState : uint8_t
{
    idle,
    collecting,
    done,
    failed
};

struct CalResult
{
    bool ok = false;
    uint8_t errorCode = 0;     // spec 8, 0x01 to 0x04 when a check fails
    float gyroBiasDps[3] = {};
    float gravityG[3] = {};
    float tempAtCalC = 0.0f;
    float gyroStdDps[3] = {};  // spread over the window, what the stillness limits are compared against
    float accelMagStdG = 0.0f;
};

namespace calibration
{
    // spec 8 codes
    constexpr uint8_t errNotStillAccel = 0x01;
    constexpr uint8_t errNotStillGyro = 0x02;
    constexpr uint8_t errBiasOutOfRange = 0x03;
    constexpr uint8_t errGravityOutOfRange = 0x04;

    void start();
    void feed(const ImuSample& sample);  // once per sample while collecting. Raw, uncalibrated samples.
    CalState state();
    CalResult result();                  // measured values are filled in even on a failure, to help debugging

    // Copy of the sample with the gyro bias removed. Accel passes through (peak detection uses |a| - 1 g).
    // Passes the sample through unchanged until a calibration has succeeded.
    void apply(const ImuSample& in, ImuSample& out);

    // True once die temperature has moved calRefreshTempDeltaC from the calibration temperature (spec 5.1)
    bool needsRefresh(float tempNowC);

    // Call once per RAW sample while recording. Only after calRezeroStillMs of continuous stillness
    // does it nudge the bias toward the new estimate, so it never changes anything mid-play.
    void opportunisticUpdate(const ImuSample& sample);
}
