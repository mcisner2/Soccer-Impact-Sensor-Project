// calibration.cpp
// Placeholder only. Edward's version adds the spec 5.1 stillness and range checks.

#include "calibration.h"
#include "config.h"

namespace
{
    constexpr uint32_t calSamples = config::imuOdrHz * 4;  // spec 5.1: 4 s

    CalState currentState = CalState::idle;
    CalResult lastResult;
    uint32_t count = 0;
    double gyroSum[3];
    double accelSum[3];
    double tempSum;
}

namespace calibration
{
    void start()
    {
        currentState = CalState::collecting;
        lastResult = CalResult();
        count = 0;
        tempSum = 0.0;
        for (int axis = 0; axis < 3; axis++)
        {
            gyroSum[axis] = 0.0;
            accelSum[axis] = 0.0;
        }
    }

    void feed(const ImuSample& sample)
    {
        if (currentState != CalState::collecting)
        {
            return;
        }

        for (int axis = 0; axis < 3; axis++)
        {
            gyroSum[axis] += sample.gyroDps[axis];
            accelSum[axis] += sample.accelG[axis];
        }
        tempSum += sample.tempC;

        if (++count < calSamples)
        {
            return;
        }

        for (int axis = 0; axis < 3; axis++)
        {
            lastResult.gyroBiasDps[axis] = (float)(gyroSum[axis] / count);
            lastResult.gravityG[axis] = (float)(accelSum[axis] / count);
        }
        lastResult.tempAtCalC = (float)(tempSum / count);
        lastResult.ok = true;
        currentState = CalState::done;
    }

    CalState state()
    {
        return currentState;
    }

    CalResult result()
    {
        return lastResult;
    }
}
