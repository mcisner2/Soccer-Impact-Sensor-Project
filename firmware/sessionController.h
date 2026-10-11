// sessionController.h
// Spec section 7: BOOT -> SENSOR_INIT -> CALIBRATING -> READY -> RECORDING -> STOPPED, any of them -> ERROR.
// BLE connection is tracked separately on purpose: recording keeps going through a disconnect.

#pragma once
#include <stdint.h>
#include "sampler.h"
#include "batteryMonitor.h"

// numbers match the Status packet's state byte (spec 6.2)
enum class SessionState : uint8_t
{
    boot = 0,
    sensorInit = 1,
    calibrating = 2,
    ready = 3,
    recording = 4,
    stopped = 5,
    error = 6
};

class SessionController
{
    public:
        void begin(Sampler& samplerToUse, bool imuOk);
        void service(BatteryLevel batteryLevel);  // retries, timers, shutdown

        void onImuSample(const ImuSample& sample);  // from the sampler hook, feeds calibration
        uint8_t handleCommand(uint8_t cmd);         // returns spec 8 error code, 0 = accepted

        SessionState state() const
        {
            return currentState;
        }

        uint8_t errorCode() const
        {
            return lastErrorCode;
        }

        bool recording() const
        {
            return currentState == SessionState::recording;
        }

        bool takeStateChanged();  // true once per change, so Status goes out right away (spec 6.1)

    private:
        Sampler* sampler = nullptr;
        SessionState currentState = SessionState::boot;
        uint8_t lastErrorCode = 0;
        bool changed = true;

        uint8_t calAttempts = 0;
        uint32_t retryAtMs = 0;
        bool retryPending = false;
        uint32_t shutdownAtMs = 0;
        bool shuttingDown = false;

        void enter(SessionState next);
        void startCalibration(bool resetAttempts);
        void shutDown();
};
