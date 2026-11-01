// calibration_host_test.cpp
// Tests calibration.cpp on a laptop, no ESP32 needed. From the firmware folder:
//   g++ -std=c++17 -Wall -Wextra -I. calibration.cpp calibration_host_test.cpp -o cal_test && ./cal_test
// The noise levels follow the ICM-20649 datasheet (gyro 0.0175 dps/sqrt(Hz), accel 285 ug/sqrt(Hz)) over ~110 Hz.

#include <stdio.h>
#include <math.h>
#include <random>
#include "calibration.h"
#include "config.h"

namespace
{
    int failures = 0;

    #define CHECK(cond, msg) \
        do { if (cond) { printf("ok:   %s\n", msg); } else { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

    std::mt19937 gen(42);
    std::normal_distribution<float> unit(0.0f, 1.0f);

    constexpr uint32_t windowSamples = config::imuOdrHz * config::calSeconds;

    struct Scenario
    {
        float gravity[3] = { -0.046f, 0.007f, 0.956f };
        float gyroBias[3] = { -4.6f, 4.4f, -1.6f };
        float gyroNoise = 0.18f;
        float accelNoise = 0.003f;
        float shakeG = 0.0f;  // 7 Hz shake along the gravity axis (z), in g. A sideways shake barely changes |a|, see the note in main()
        float tempC = 22.0f;
    };

    ImuSample makeSample(const Scenario& s, uint32_t i)
    {
        ImuSample out;
        out.tMs = i * 1000 / config::imuOdrHz;
        float shake = s.shakeG * sinf(2.0f * 3.14159265f * 7.0f * (float)i / (float)config::imuOdrHz);
        for (int axis = 0; axis < 3; axis++)
        {
            out.accelG[axis] = s.gravity[axis] + s.accelNoise * unit(gen);
            out.gyroDps[axis] = s.gyroBias[axis] + s.gyroNoise * unit(gen);
        }
        out.accelG[2] += shake;
        out.tempC = s.tempC;
        return out;
    }

    CalResult runCalibration(const Scenario& s)
    {
        calibration::start();
        for (uint32_t i = 0; i < windowSamples; i++)
        {
            calibration::feed(makeSample(s, i));
        }
        return calibration::result();
    }

    void feedSeconds(const Scenario& s, float seconds)
    {
        uint32_t count = (uint32_t)(seconds * config::imuOdrHz);
        for (uint32_t i = 0; i < count; i++)
        {
            calibration::opportunisticUpdate(makeSample(s, i));
        }
    }
}

int main()
{
    // Note: the stillness check looks at |a|, so a purely sideways shake of up to ~0.5 g still passes it. A real hand shake
    // also rotates the sensor, which the gyro check catches, and gyro bias is unaffected by pure translation.

    // 1. a still device passes and the bias comes out right
    Scenario still;
    CalResult r = runCalibration(still);
    CHECK(calibration::state() == CalState::done && r.ok && r.errorCode == 0, "still device passes");
    CHECK(fabsf(r.gyroBiasDps[0] - still.gyroBias[0]) < 0.05f && fabsf(r.gyroBiasDps[1] - still.gyroBias[1]) < 0.05f
          && fabsf(r.gyroBiasDps[2] - still.gyroBias[2]) < 0.05f, "gyro bias within 0.05 dps of the truth");
    CHECK(fabsf(r.gravityG[2] - still.gravity[2]) < 0.005f, "gravity vector recovered");
    CHECK(fabsf(r.tempAtCalC - 22.0f) < 0.01f, "calibration temperature recorded");
    CHECK(fabsf(r.gyroStdDps[0] - 0.18f) < 0.02f && fabsf(r.gyroStdDps[2] - 0.18f) < 0.02f, "gyro spread reported (about the 0.18 dps simulated)");
    CHECK(r.accelMagStdG > 0.001f && r.accelMagStdG < 0.01f, "accel spread reported (a few mg)");

    // 2. it does not finish early or late
    calibration::start();
    for (uint32_t i = 0; i < windowSamples - 1; i++)
    {
        calibration::feed(makeSample(still, i));
    }
    CHECK(calibration::state() == CalState::collecting, "still collecting one sample before the window ends");
    calibration::feed(makeSample(still, windowSamples));
    CHECK(calibration::state() == CalState::done, "done on the last sample");

    // 3. each failure comes back with its spec 8 code
    Scenario shaken = still;
    shaken.shakeG = 0.5f;
    r = runCalibration(shaken);
    CHECK(calibration::state() == CalState::failed && !r.ok && r.errorCode == 0x01, "shaking -> NOT_STILL_ACCEL (0x01)");

    Scenario noisyGyro = still;
    noisyGyro.gyroNoise = 1.0f;
    r = runCalibration(noisyGyro);
    CHECK(r.errorCode == 0x02, "noisy gyro -> NOT_STILL_GYRO (0x02)");

    Scenario bigBias = still;
    bigBias.gyroBias[0] = 7.0f;
    r = runCalibration(bigBias);
    CHECK(r.errorCode == 0x03, "7 dps bias -> GYRO_BIAS_OUT_OF_RANGE (0x03)");
    CHECK(fabsf(r.gyroBiasDps[0] - 7.0f) < 0.1f, "failed result still reports what was measured");

    Scenario wrongGravity = still;
    wrongGravity.gravity[0] = 0.0f;
    wrongGravity.gravity[1] = 0.0f;
    wrongGravity.gravity[2] = 1.12f;
    r = runCalibration(wrongGravity);
    CHECK(r.errorCode == 0x01, "|a| = 1.12 g is rejected");

    // 4. a failed calibration can be retried
    r = runCalibration(still);
    CHECK(r.ok && calibration::state() == CalState::done, "start() after a failure runs a clean calibration");

    // 5. apply()
    ImuSample raw = makeSample(still, 0);
    ImuSample fixed;
    calibration::apply(raw, fixed);
    CHECK(fabsf(fixed.gyroDps[0] - (raw.gyroDps[0] - r.gyroBiasDps[0])) < 1e-4f, "apply() subtracts the gyro bias");
    CHECK(fixed.accelG[2] == raw.accelG[2] && fixed.tMs == raw.tMs, "apply() leaves accel and time alone");
    CHECK(fabsf(fixed.gyroDps[0]) < 1.0f, "a still sample comes out near zero after apply()");

    calibration::start();  // collecting, nothing valid yet
    calibration::apply(raw, fixed);
    CHECK(fixed.gyroDps[0] == raw.gyroDps[0], "apply() passes data through before a calibration has succeeded");

    // 6. needsRefresh()
    CHECK(!calibration::needsRefresh(40.0f), "no stale flag without a valid calibration");
    r = runCalibration(still);
    CHECK(!calibration::needsRefresh(26.9f), "+4.9 C is not stale");
    CHECK(calibration::needsRefresh(27.1f), "+5.1 C is stale");
    CHECK(calibration::needsRefresh(16.9f), "-5.1 C is stale too");

    // 7. opportunistic re-zero
    r = runCalibration(still);
    float before = r.gyroBiasDps[0];

    Scenario moving = still;
    moving.shakeG = 0.5f;
    moving.gyroBias[0] += 0.5f;
    feedSeconds(moving, 6.0f);
    CHECK(calibration::result().gyroBiasDps[0] == before, "bias untouched while the device is moving");

    Scenario slightlyOff = still;
    slightlyOff.gyroBias[0] += 0.5f;
    slightlyOff.tempC = 30.0f;
    feedSeconds(slightlyOff, 1.5f);
    CHECK(calibration::result().gyroBiasDps[0] == before, "bias untouched before 2 s of stillness");

    feedSeconds(slightlyOff, 0.6f);
    float step = calibration::result().gyroBiasDps[0] - before;
    CHECK(step > 0.09f && step < 0.16f, "after 2 s of stillness the bias moves about a quarter of the way (0.125 dps)");
    CHECK(fabsf(calibration::result().tempAtCalC - 24.0f) < 0.1f, "reference temperature moves the same quarter (22 -> 24 C)");

    feedSeconds(slightlyOff, 8.0f);
    CHECK(fabsf(calibration::result().gyroBiasDps[0] - (still.gyroBias[0] + 0.5f)) < 0.15f, "keeps converging on the new bias over repeated windows");

    r = runCalibration(still);
    before = r.gyroBiasDps[0];
    Scenario rotating = still;
    rotating.gyroBias[0] += 3.0f;
    feedSeconds(rotating, 6.0f);
    CHECK(calibration::result().gyroBiasDps[0] == before, "a 3 dps jump in a 'still' window is ignored as slow rotation");

    calibration::start();
    feedSeconds(slightlyOff, 4.0f);
    CHECK(!calibration::result().ok && calibration::result().gyroBiasDps[0] == 0.0f, "re-zero does nothing before a calibration exists");

    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
