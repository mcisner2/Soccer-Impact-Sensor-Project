// sessionController.cpp

#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "sessionController.h"
#include "calibration.h"
#include "packets.h"
#include "pins.h"

namespace
{
    constexpr uint8_t maxCalAttempts = 3;        // spec 5.1
    constexpr uint32_t calRetryDelayMs = 1000;   // pause so the dashboard can say "stand still" [ASSUMPTION]
    constexpr uint32_t shutdownDelayMs = 2000;   // time for the final Status packet to reach the dashboard

    // spec 8
    constexpr uint8_t errCalFailed = 0x10;
    constexpr uint8_t errNotCalibrated = 0x11;
    constexpr uint8_t errImuComm = 0x20;
    constexpr uint8_t errBatteryCritical = 0x51;
    constexpr uint8_t errBadState = 0x12;  // NOT IN SPEC: command not allowed in this state, proposed addition
}

void SessionController::begin(Sampler& samplerToUse, bool imuOk)
{
    sampler = &samplerToUse;
    enter(SessionState::sensorInit);

    if (!imuOk)
    {
        lastErrorCode = errImuComm;
        enter(SessionState::error);
        return;
    }

    // spec 7 goes straight on to calibration; GPS keeps acquiring in parallel
    startCalibration(true);
}

void SessionController::enter(SessionState next)
{
    if (next != currentState)
    {
        currentState = next;
        changed = true;
    }
}

bool SessionController::takeStateChanged()
{
    bool was = changed;
    changed = false;
    return was;
}

void SessionController::startCalibration(bool resetAttempts)
{
    if (resetAttempts)
    {
        calAttempts = 0;
    }
    calAttempts++;
    retryPending = false;
    calibration::start();
    enter(SessionState::calibrating);
}

void SessionController::onImuSample(const ImuSample& sample)
{
    if (currentState == SessionState::calibrating)
    {
        calibration::feed(sample);
    }
}

uint8_t SessionController::handleCommand(uint8_t cmd)
{
    if (shuttingDown)
    {
        return errBatteryCritical;
    }

    uint8_t result = 0;

    switch (cmd)
    {
        case packets::cmdStartCal:
        case packets::cmdRecal:
            // never mid-session: recalibrating while the player moves would ruin the bias
            if (currentState == SessionState::recording || currentState == SessionState::calibrating)
            {
                result = errBadState;
            }
            else
            {
                lastErrorCode = 0;
                startCalibration(true);
            }
            break;

        case packets::cmdStartSession:
            if (currentState == SessionState::ready)
            {
                lastErrorCode = 0;
                sampler->startSession();
                enter(SessionState::recording);
            }
            else if (currentState != SessionState::recording)
            {
                result = errNotCalibrated;  // includes STOPPED: calibration is redone every session (spec 5.1)
            }
            break;

        case packets::cmdStopSession:
            if (currentState == SessionState::recording)
            {
                enter(SessionState::stopped);
            }
            else
            {
                result = errBadState;
            }
            break;

        default:
            break;  // raw stream on/off is handled outside, it's allowed in any state
    }

    if (result != 0)
    {
        lastErrorCode = result;
        changed = true;  // push a Status now so the dashboard sees the rejection
    }
    else
    {
        lastErrorCode = 0;  // an accepted command clears the last rejection, or it sticks around forever
    }
    return result;
}

void SessionController::service(BatteryLevel batteryLevel)
{
    uint32_t nowMs = millis();

    // spec 8: critical battery ends everything, whatever state we're in
    if (batteryLevel == BatteryLevel::critical && !shuttingDown)
    {
        shuttingDown = true;
        shutdownAtMs = nowMs + shutdownDelayMs;
        lastErrorCode = errBatteryCritical;
        enter(SessionState::error);
    }

    if (shuttingDown)
    {
        if ((int32_t)(nowMs - shutdownAtMs) >= 0)
        {
            shutDown();
        }
        return;
    }

    if (currentState == SessionState::calibrating)
    {
        CalState cal = calibration::state();

        if (cal == CalState::done)
        {
            lastErrorCode = 0;
            enter(SessionState::ready);
        }
        else if (cal == CalState::failed && !retryPending)
        {
            lastErrorCode = calibration::result().errorCode;  // 0x01 to 0x04, dashboard prompts "stand still"
            changed = true;

            if (calAttempts >= maxCalAttempts)
            {
                lastErrorCode = errCalFailed;
                enter(SessionState::error);  // waits for RECAL, spec 7
            }
            else
            {
                retryPending = true;
                retryAtMs = nowMs + calRetryDelayMs;
            }
        }

        if (retryPending && (int32_t)(nowMs - retryAtMs) >= 0)
        {
            startCalibration(false);
        }
    }
}

void SessionController::shutDown()
{
    // Turn off what we can, then deep sleep. Holds keep the pins low while asleep,
    // otherwise the GPS breakout's own pull-up on EN would switch it back on.
    digitalWrite(pins::gpsEnable, LOW);
    gpio_hold_en((gpio_num_t)pins::gpsEnable);
    digitalWrite(PIN_I2C_POWER, LOW);
    gpio_hold_en((gpio_num_t)PIN_I2C_POWER);
    gpio_deep_sleep_hold_en();

    // no wake source on purpose: only the reset button or a power cycle brings it back
    esp_deep_sleep_start();
}
