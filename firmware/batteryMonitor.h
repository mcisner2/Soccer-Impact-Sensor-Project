// batteryMonitor.h
// Reads pack voltage through the divider and turns it into a level: ok, warning, or critical.

#pragma once
#include <stdint.h>

enum class BatteryLevel : uint8_t
{
    ok,
    warning,
    critical
};

class BatteryMonitor
{
    public:
        void begin();
        void update();  // call about once a second, a battery doesn't change faster than that

        uint32_t packMv() const
        {
            return lastPackMv;
        }

        uint8_t percent() const;

        BatteryLevel level() const
        {
            return currentLevel;
        }

    private:
        uint32_t lastPackMv = 0;
        BatteryLevel currentLevel = BatteryLevel::ok;
        uint8_t readsPastLine = 0;

        uint32_t readPackMv();
};
