// sampler.cpp

#include <esp_timer.h>
#include "sampler.h"

void Sampler::begin(Imu& imuToUse, SampleHook hook)
{
    imu = &imuToUse;
    onSample = hook;
    samplePeriodUs = 1000000UL / config::imuOdrHz;
    sessionStartUs = esp_timer_get_time();  // before a session starts, time counts from boot
}

void Sampler::startSession()
{
    sessionStartUs = esp_timer_get_time();
}

void Sampler::service()
{
    size_t count = imu->readSamples(batch, config::drainBatchMax);
    if (count == 0)
    {
        return;
    }

    if (count == config::drainBatchMax)
    {
        lateDrains++;  // more may be waiting, so "newest = now" below is off for this batch
    }

    // 64-bit clock: micros() wraps at ~71 min, shorter than a 90 min match
    int64_t nowUs = esp_timer_get_time();

    for (size_t i = 0; i < count; i++)
    {
        // Spec 9: newest sample is "now", each earlier one steps back one period
        int64_t sampleUs = nowUs - (int64_t)(count - 1 - i) * samplePeriodUs;
        int64_t sinceStartUs = sampleUs - sessionStartUs;

        // samples back-dated to just before START_SESSION would go negative
        uint32_t tMs = sinceStartUs > 0 ? (uint32_t)(sinceStartUs / 1000) : 0;

        StampedRawSample stamped = { tMs, batch[i] };
        if (!rawBuffer.push(stamped))
        {
            droppedSamples++;  // newest gets dropped, the raw packet seq gap will show it
        }

        if (onSample)
        {
            const RawImuSample& raw = batch[i];
            ImuSample sample;
            sample.tMs = tMs;
            for (int axis = 0; axis < 3; axis++)
            {
                sample.accelG[axis] = raw.accel[axis] / config::accelLsbPerG;
                sample.gyroDps[axis] = raw.gyro[axis] / config::gyroLsbPerDps;
            }
            sample.tempC = raw.tempRaw / config::tempLsbPerC + config::tempZeroC;
            onSample(sample);
        }
    }
}

bool Sampler::popRaw(StampedRawSample& out)
{
    return rawBuffer.pop(out);
}

size_t Sampler::rawWaiting() const
{
    return rawBuffer.count();
}
