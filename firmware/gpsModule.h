// gpsModule.h
// Brings the GPS up on every boot (no coin cell, so it forgets everything), then reads fixes.
// Never blocks: each step waits by checking the clock on the next call, not with delay().

#pragma once
#include <stdint.h>
#include "gpsParser.h"

class GpsModule
{
    public:
        void begin();
        void service();  // call every loop

        bool takeNewFix();  // true once per fix, then resets
        const GpsFix& fix() const
        {
            return currentFix;
        }

        bool degraded() const;  // spec gps_degraded: no fix yet past the timeout, or HDOP too high
        const char* stateName() const;

    private:
        enum class State : uint8_t
        {
            probeFast,    // maybe only the ESP32 rebooted and the GPS is still at 57600
            probeSlow,    // fresh power-up, GPS is at 9600
            configuring,  // sending PMTK commands one at a time
            running,
            missing       // nothing answered at either speed, retry later
        };

        State state = State::probeFast;
        uint32_t stateStartMs = 0;
        uint32_t bootMs = 0;
        uint8_t commandIndex = 0;
        uint32_t nextCommandMs = 0;

        GpsFix currentFix;
        bool newFix = false;
        bool heardValidLine = false;

        char lineBuffer[100];
        size_t lineLength = 0;

        void enterState(State next);
        void readSerial();
        void handleLine(const char* line);
        void sendBody(const char* body);
        bool sendNextConfigCommand();
        void serviceMock();
};
