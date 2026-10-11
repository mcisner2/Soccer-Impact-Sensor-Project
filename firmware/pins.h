// pins.h
// Every GPIO the firmware touches, in one place, so wiring and code can't drift apart.
// Numbers come from the core's Feather ESP32-S3 pin map (variants/adafruit_feather_esp32s3/pins_arduino.h).

#pragma once
#include <Arduino.h>

namespace pins
{
    // A5 = GPIO8 = ADC1 channel 7. A0 to A4 are GPIO 18 to 14, which are ADC2, so they're skipped on purpose.
    constexpr uint8_t batteryAdc = A5;

    // GPS on Serial1. Fixed by the board's RX/TX labels.
    constexpr uint8_t gpsRx = RX;  // GPIO38, receives from GPS TX
    constexpr uint8_t gpsTx = TX;  // GPIO39, sends to GPS RX

    // PROPOSED in the wiring handoff, change here if the team picks different pins
    constexpr uint8_t gpsEnable = 10;  // breakout pulls EN high itself, low turns the GPS off
    constexpr uint8_t gpsPps = 11;     // not used yet, timestamp alignment comes later
    constexpr uint8_t imuInt = 12;     // used by the real IMU driver in step 8

    // Already taken by the board, don't reuse:
    // SDA 3, SCL 4 (STEMMA QT), I2C_POWER 7, RX 38 / TX 39 (GPS on Serial1), NeoPixel 33, NeoPixel power 21
}
