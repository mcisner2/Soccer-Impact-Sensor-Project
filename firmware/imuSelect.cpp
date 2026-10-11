// imuSelect.cpp
// The one place that picks mock vs real.

#include "config.h"
#include "imu.h"

#if USE_MOCK_IMU
    #include "mockImu.h"
    static MockImu imuInstance;
#else
    #include "icm20649Imu.h"
    static Icm20649Imu imuInstance;
#endif

Imu& getImu()
{
    return imuInstance;
}
