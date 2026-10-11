// icm20649Imu.h
// Real ICM-20649 driver, polling version. Adafruit_ICM20X sets the chip up; we read raw registers ourselves.
// FIFO + INT1 (spec 3.1) comes later and needs the register details from the datasheet.

#pragma once
#include "config.h"

// only built for real hardware, so mock builds don't need the Adafruit library installed
#if !USE_MOCK_IMU

#include <Adafruit_ICM20649.h>
#include "imu.h"

class Icm20649Imu : public Imu
{
    public:
        bool begin() override;
        size_t readSamples(RawImuSample* out, size_t maxCount) override;

        uint32_t missedSamples() const override
        {
            return missed;
        }

        uint32_t readErrors() const
        {
            return errors;
        }

    private:
        Adafruit_ICM20649 chip;
        uint32_t samplePeriodUs = 0;
        uint32_t lastSampleUs = 0;
        uint32_t missed = 0;
        uint32_t errors = 0;

        bool readRegisters(RawImuSample& sample);
};

#endif
