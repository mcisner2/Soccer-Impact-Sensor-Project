// imu.h
// The only IMU type the rest of the firmware sees.

#pragma once
#include <stdint.h>
#include <stddef.h>

// Raw counts, converted to g/dps in one place later so raw stream and detector match
struct RawImuSample
{
    int16_t accel[3];
    int16_t gyro[3];
    int16_t tempRaw;
};

class Imu
{
    public:
        virtual ~Imu()
        {
        }

        virtual bool begin() = 0;

        // Works like the chip's FIFO: returns everything that piled up since the last call
        virtual size_t readSamples(RawImuSample* out, size_t maxCount) = 0;

        // samples the chip produced that we never read (polling fell behind). Mock never misses.
        virtual uint32_t missedSamples() const
        {
            return 0;
        }
};

// Mock or real, picked by USE_MOCK_IMU
Imu& getImu();
