// sampler.h
// Drains the IMU, timestamps each sample, feeds the detector hook and the raw ring buffer.

#pragma once
#include <atomic>
#include "imu.h"
#include "ringBuffer.h"
#include "config.h"

// Physical units, same fields as spec imu_sample_t. Calibration not applied yet.
struct ImuSample
{
    uint32_t tMs;
    float accelG[3];
    float gyroDps[3];
    float tempC;
};

// Raw counts plus time, what the raw stream (spec 6.5) sends
struct StampedRawSample
{
    uint32_t tMs;
    RawImuSample raw;
};

// Where calibration + peak detection plug in. Runs once per sample, so it must stay fast.
using SampleHook = void (*)(const ImuSample& sample);

class Sampler
{
    public:
        void begin(Imu& imuToUse, SampleHook hook);
        void startSession();  // tMs counts from here
        void service();       // call often from loop(), cheap when nothing is waiting

        bool popRaw(StampedRawSample& out);
        size_t rawWaiting() const;

        uint32_t droppedCount() const
        {
            return droppedSamples.load();
        }

        uint32_t lateDrainCount() const
        {
            return lateDrains.load();
        }

    private:
        Imu* imu = nullptr;
        SampleHook onSample = nullptr;
        int64_t sessionStartUs = 0;
        uint32_t samplePeriodUs = 0;

        RawImuSample batch[config::drainBatchMax];  // member, not local, to keep it off the loop task's stack
        RingBuffer<StampedRawSample, config::rawBufferSamples> rawBuffer;

        std::atomic<uint32_t> droppedSamples { 0 };  // ring buffer was full
        std::atomic<uint32_t> lateDrains { 0 };      // hit drainBatchMax, timestamps for that batch are suspect
};
