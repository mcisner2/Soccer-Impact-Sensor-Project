// mockImu.cpp
// Quiet baseline plus a kick-like spike every few seconds.
// Kick shape and size are made up, not from real data.

#include <Arduino.h>
#include "mockImu.h"
#include "config.h"

namespace
{
    constexpr uint32_t kickLengthSamples = 4;  // about 18 ms at 220 Hz
    constexpr float kickPeakG = 15.0f;
    constexpr float kickPeakDps = 1200.0f;
    constexpr float fakeTempC = 30.0f;

    // Inside the datasheet's +-5 dps tolerance, gives calibration something to remove
    constexpr float gyroBiasDps[3] = { 2.0f, -1.5f, 0.8f };

    // Three uniforms in [-a, a] sum to std dev a, close enough to Gaussian
    int16_t noise(int16_t sd)
    {
        return (int16_t)(random(-sd, sd + 1) + random(-sd, sd + 1) + random(-sd, sd + 1));
    }
}

bool MockImu::begin()
{
    samplePeriodUs = 1000000UL / config::imuOdrHz;
    lastSampleUs = micros();
    scheduleNextKick();
    return true;
}

size_t MockImu::readSamples(RawImuSample* out, size_t maxCount)
{
    size_t count = 0;

    // One sample per elapsed period. Leftovers wait for next call, like the chip (but never overflow)
    while (count < maxCount && (int32_t)(micros() - lastSampleUs) >= (int32_t)samplePeriodUs)
    {
        lastSampleUs += samplePeriodUs;
        makeSample(out[count]);
        count++;
    }

    return count;
}

void MockImu::makeSample(RawImuSample& sample)
{
    // ~3 mg accel and ~0.2 dps gyro noise, from datasheet densities at ~100 Hz bandwidth
    sample.accel[0] = noise(3);
    sample.accel[1] = noise(3);
    sample.accel[2] = (int16_t)config::accelLsbPerG + noise(3);  // gravity on z, real axis still [OPEN]

    for (int axis = 0; axis < 3; axis++)
    {
        sample.gyro[axis] = (int16_t)(gyroBiasDps[axis] * config::gyroLsbPerDps) + noise(2);
    }

    sample.tempRaw = (int16_t)((fakeTempC - config::tempZeroC) * config::tempLsbPerC);

    if (kickIndex == 0 && --samplesUntilKick == 0)
    {
        kickIndex = 1;
    }

    if (kickIndex > 0)
    {
        // half-sine: simplest pulse with one clear peak
        float shape = sinf(PI * (kickIndex - 0.5f) / kickLengthSamples);
        sample.accel[0] += (int16_t)(kickPeakG * config::accelLsbPerG * shape);
        sample.gyro[1] += (int16_t)(kickPeakDps * config::gyroLsbPerDps * shape);

        if (++kickIndex > kickLengthSamples)
        {
            kickIndex = 0;
            scheduleNextKick();
        }
    }
}

void MockImu::scheduleNextKick()
{
    // 2 to 6 s gap
    samplesUntilKick = random(2 * config::imuOdrHz, 6 * config::imuOdrHz + 1);
}
