// batteryMonitor.cpp

#include <Arduino.h>
#include "batteryMonitor.h"
#include "config.h"
#include "pins.h"

void BatteryMonitor::begin()
{
#if !USE_MOCK_BATTERY
    // 11 dB is the widest range. Espressif's guidance puts the accurate S3 range near 0 to 2900 mV at this setting.
    analogSetPinAttenuation(pins::batteryAdc, ADC_11db);
#endif
    lastPackMv = readPackMv();
}

uint32_t BatteryMonitor::readPackMv()
{
#if USE_MOCK_BATTERY
    // fake pack: starts full and drains at the configured rate, so warning and critical can be watched
    uint32_t drop = (millis() / 1000) * config::mockBatteryDrainMvPerS;
    return config::batteryFullMv > drop + 3800 ? config::batteryFullMv - drop : 3800;
#else
    // average several reads, a single ADC sample is noisy
    uint32_t sum = 0;
    for (uint8_t i = 0; i < config::batteryAveragedReads; i++)
    {
        sum += analogReadMilliVolts(pins::batteryAdc);  // already corrected with the chip's factory calibration
    }
    float pinMv = sum / (float)config::batteryAveragedReads;
    return (uint32_t)(pinMv * config::dividerRatio * config::batteryCalFactor);
#endif
}

void BatteryMonitor::update()
{
    lastPackMv = readPackMv();

    // where the reading says we should be, before the confirm count
    BatteryLevel target = currentLevel;

    if (lastPackMv <= config::batteryCriticalMv)
    {
        target = BatteryLevel::critical;
    }
    else if (currentLevel == BatteryLevel::ok && lastPackMv <= config::batteryWarnMv)
    {
        target = BatteryLevel::warning;
    }
    else if (currentLevel == BatteryLevel::warning && lastPackMv >= config::batteryWarnMv + config::batteryHysteresisMv)
    {
        target = BatteryLevel::ok;  // only after a real recovery, not a one-reading bounce
    }

    // critical never clears: once the pack is that low, the session ends
    if (currentLevel == BatteryLevel::critical || target == currentLevel)
    {
        readsPastLine = 0;
        return;
    }

    // load spikes (BLE transmit) sag the pack briefly, so a level only changes after several reads agree
    if (++readsPastLine >= config::batteryConfirmReads)
    {
        currentLevel = target;
        readsPastLine = 0;
    }
}

uint8_t BatteryMonitor::percent() const
{
    if (lastPackMv <= config::batteryEmptyMv)
    {
        return 0;
    }
    if (lastPackMv >= config::batteryFullMv)
    {
        return 100;
    }
    return (uint8_t)((lastPackMv - config::batteryEmptyMv) * 100 / (config::batteryFullMv - config::batteryEmptyMv));
}
