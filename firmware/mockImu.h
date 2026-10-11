// mockImu.h
// Fake ICM-20649 so the firmware runs with no hardware.

#pragma once
#include "imu.h"

class MockImu : public Imu
{
    public:
        bool begin() override;
        size_t readSamples(RawImuSample* out, size_t maxCount) override;

    private:
        uint32_t samplePeriodUs = 0;
        uint32_t lastSampleUs = 0;
        uint32_t samplesUntilKick = 0;
        uint32_t kickIndex = 0;  // 0 = no kick running

        void makeSample(RawImuSample& sample);
        void scheduleNextKick();
};
