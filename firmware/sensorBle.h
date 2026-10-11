// sensorBle.h
// GATT server from spec section 6. BLE callbacks run in NimBLE's own task, so they only record
// what happened. loop() reads that state and does the real work.

#pragma once
#include <stdint.h>
#include <stddef.h>

struct ControlCommand
{
    uint8_t cmd;
    uint8_t arg;
};

namespace sensorBle
{
    bool begin();

    bool isConnected();
    uint16_t mtu();
    size_t maxRawSamplesPerPacket();  // 0 means not even one sample fits at this MTU

    bool popCommand(ControlCommand& out);  // commands written by the webpage, oldest first
    uint32_t badWriteCount();              // control writes with the wrong length

    // Each returns false if not connected, not subscribed, or the stack is out of buffers
    bool sendStatus(const uint8_t* data, size_t length);
    bool sendEvent(const uint8_t* data, size_t length);
    bool sendRaw(const uint8_t* data, size_t length);
    bool sendTelemetry(const uint8_t* data, size_t length);

    void setBatteryPercent(uint8_t percent);
}
