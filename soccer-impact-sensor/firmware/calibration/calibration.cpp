// calibration.cpp
// Spec 5.1: stand-still bias calibration with stillness and range checks, bias removal,
// the temperature "stale" check, and the opportunistic re-zero.
// Only depends on the ImuSample struct, so it also builds on a laptop for host tests.
// Same logic as calibrate() in peak_detection_prototype.py.

#include <math.h>
#include "calibration.h"
#include "config.h"

namespace
{
    constexpr uint32_t calSamples = config::imuOdrHz * config::calSeconds;
    constexpr uint32_t rezeroSamples = config::imuOdrHz * config::calRezeroStillMs / 1000;

    // Welford's method: running mean and variance in one pass, no big sums to lose precision.
    // Population std (divide by n), same as numpy's default, so results match the Python prototype.
    struct RunningStat
    {
        uint32_t n = 0;
        double mean = 0.0;
        double m2 = 0.0;

        void reset()
        {
            n = 0;
            mean = 0.0;
            m2 = 0.0;
        }

        void add(double x)
        {
            n++;
            double delta = x - mean;
            mean += delta / n;
            m2 += delta * (x - mean);
        }

        double stddev() const
        {
            return n > 0 ? sqrt(m2 / n) : 0.0;
        }
    };

    // Everything the stillness checks need, collected over one window of samples
    struct StillWindow
    {
        RunningStat accelMag;
        RunningStat accel[3];
        RunningStat gyro[3];
        RunningStat temp;

        void reset()
        {
            accelMag.reset();
            temp.reset();
            for (int axis = 0; axis < 3; axis++)
            {
                accel[axis].reset();
                gyro[axis].reset();
            }
        }

        uint32_t count() const
        {
            return accelMag.n;
        }

        void add(const ImuSample& s)
        {
            accelMag.add(sqrtf(s.accelG[0] * s.accelG[0] + s.accelG[1] * s.accelG[1] + s.accelG[2] * s.accelG[2]));
            for (int axis = 0; axis < 3; axis++)
            {
                accel[axis].add(s.accelG[axis]);
                gyro[axis].add(s.gyroDps[axis]);
            }
            temp.add(s.tempC);
        }

        // 0 = the device looked still and sane, otherwise the spec 8 code of the first check that failed
        uint8_t check() const
        {
            if (fabs(accelMag.mean - 1.0) > config::calStillAccelMeanTolG || accelMag.stddev() > config::calStillAccelStdG)
            {
                return calibration::errNotStillAccel;
            }

            for (int axis = 0; axis < 3; axis++)
            {
                if (gyro[axis].stddev() > config::calStillGyroStdDps)
                {
                    return calibration::errNotStillGyro;
                }
            }

            for (int axis = 0; axis < 3; axis++)
            {
                if (fabs(gyro[axis].mean) > config::calGyroBiasMaxDps)
                {
                    return calibration::errBiasOutOfRange;
                }
            }

            double gravity = sqrt(accel[0].mean * accel[0].mean + accel[1].mean * accel[1].mean + accel[2].mean * accel[2].mean);
            if (fabs(gravity - 1.0) > config::calGravityTolG)
            {
                return calibration::errGravityOutOfRange;
            }

            return 0;
        }
    };

    CalState currentState = CalState::idle;
    CalResult lastResult;
    StillWindow calWindow;     // the start-of-session window
    StillWindow rezeroWindow;  // mid-session window for the opportunistic re-zero

    void finishCalibration()
    {
        uint8_t error = calWindow.check();

        // measured values are kept even when a check failed, so a serial print shows what the sensor saw
        lastResult = CalResult();
        for (int axis = 0; axis < 3; axis++)
        {
            lastResult.gyroBiasDps[axis] = (float)calWindow.gyro[axis].mean;
            lastResult.gravityG[axis] = (float)calWindow.accel[axis].mean;
        }
        lastResult.tempAtCalC = (float)calWindow.temp.mean;
        for (int axis = 0; axis < 3; axis++)
        {
            lastResult.gyroStdDps[axis] = (float)calWindow.gyro[axis].stddev();
        }
        lastResult.accelMagStdG = (float)calWindow.accelMag.stddev();

        if (error != 0)
        {
            lastResult.errorCode = error;
            currentState = CalState::failed;
            return;
        }

        lastResult.ok = true;
        rezeroWindow.reset();
        currentState = CalState::done;
    }
}

namespace calibration
{
    void start()
    {
        currentState = CalState::collecting;
        lastResult = CalResult();
        calWindow.reset();
        rezeroWindow.reset();
    }

    void feed(const ImuSample& sample)
    {
        if (currentState != CalState::collecting)
        {
            return;
        }

        calWindow.add(sample);
        if (calWindow.count() >= calSamples)
        {
            finishCalibration();
        }
    }

    CalState state()
    {
        return currentState;
    }

    CalResult result()
    {
        return lastResult;
    }

    void apply(const ImuSample& in, ImuSample& out)
    {
        out = in;
        if (!lastResult.ok)
        {
            return;
        }

        for (int axis = 0; axis < 3; axis++)
        {
            out.gyroDps[axis] = in.gyroDps[axis] - lastResult.gyroBiasDps[axis];
        }
    }

    bool needsRefresh(float tempNowC)
    {
        return lastResult.ok && fabsf(tempNowC - lastResult.tempAtCalC) > config::calRefreshTempDeltaC;
    }

    void opportunisticUpdate(const ImuSample& sample)
    {
        if (!lastResult.ok)
        {
            return;
        }

        rezeroWindow.add(sample);
        if (rezeroWindow.count() < rezeroSamples)
        {
            return;
        }

        // A full window is in. Only a window that was still the whole way through is allowed to change anything,
        // and every window starts over afterwards, so one noisy still stretch can't drag the bias far.
        if (rezeroWindow.check() == 0)
        {
            float newBias[3];
            float biggestStep = 0.0f;
            for (int axis = 0; axis < 3; axis++)
            {
                newBias[axis] = (float)rezeroWindow.gyro[axis].mean;
                biggestStep = fmaxf(biggestStep, fabsf(newBias[axis] - lastResult.gyroBiasDps[axis]));
            }

            // a still window whose gyro average sits far from the old bias is more likely a slow, steady
            // rotation than real drift (drift over a match is a fraction of a dps)
            if (biggestStep <= config::calRezeroMaxStepDps)
            {
                const float blend = config::calRezeroBlend;
                for (int axis = 0; axis < 3; axis++)
                {
                    lastResult.gyroBiasDps[axis] += blend * (newBias[axis] - lastResult.gyroBiasDps[axis]);
                    lastResult.gravityG[axis] += blend * ((float)rezeroWindow.accel[axis].mean - lastResult.gravityG[axis]);
                }
                // the bias now describes a temperature closer to the current one, which clears the stale flag
                lastResult.tempAtCalC += blend * ((float)rezeroWindow.temp.mean - lastResult.tempAtCalC);
            }
        }

        rezeroWindow.reset();
    }
}
