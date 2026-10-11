// sensorBle.cpp

#include <atomic>
#include <NimBLEDevice.h>
#include "sensorBle.h"
#include "bleIds.h"
#include "packets.h"
#include "ringBuffer.h"

namespace
{
    constexpr const char* deviceName = "Group3Sensor";
    constexpr uint16_t preferredMtu = 247;  // the most we'll accept; only the central can start the exchange
    constexpr uint16_t attHeaderBytes = 3;

    NimBLEServer* server = nullptr;
    NimBLECharacteristic* statusChar = nullptr;
    NimBLECharacteristic* eventChar = nullptr;
    NimBLECharacteristic* rawChar = nullptr;
    NimBLECharacteristic* telemetryChar = nullptr;
    NimBLECharacteristic* batteryChar = nullptr;

    // Written in the BLE task, read in loop(), so every shared value is atomic
    std::atomic<bool> connected { false };
    std::atomic<uint16_t> connHandle { 0 };
    std::atomic<uint16_t> currentMtu { 23 };
    std::atomic<bool> statusSubscribed { false };
    std::atomic<bool> eventSubscribed { false };
    std::atomic<bool> rawSubscribed { false };
    std::atomic<bool> telemetrySubscribed { false };
    std::atomic<uint32_t> badWrites { 0 };

    // BLE task is the only writer, loop() the only reader, same rule as the sample buffer
    RingBuffer<ControlCommand, 8> commandQueue;

    class ServerCallbacks : public NimBLEServerCallbacks
    {
        void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override
        {
            connHandle = connInfo.getConnHandle();
            currentMtu = connInfo.getMTU();
            connected = true;
        }

        void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override
        {
            // subscriptions don't survive a disconnect, the webpage must subscribe again
            connected = false;
            statusSubscribed = false;
            eventSubscribed = false;
            rawSubscribed = false;
            telemetrySubscribed = false;
            currentMtu = 23;
        }

        void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override
        {
            currentMtu = mtu;
        }
    };

    class ControlCallbacks : public NimBLECharacteristicCallbacks
    {
        void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override
        {
            NimBLEAttValue value = chr->getValue();
            if (value.length() != packets::controlSize)
            {
                badWrites++;
                return;
            }

            // just queue it, loop() decides what the command means
            ControlCommand command = { value.data()[0], value.data()[1] };
            commandQueue.push(command);
        }
    };

    class SubscribeCallbacks : public NimBLECharacteristicCallbacks
    {
        void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo&, uint16_t subValue) override
        {
            bool on = (subValue & 0x01) != 0;  // bit 0 = notifications enabled in the CCCD

            if (chr == statusChar)
            {
                statusSubscribed = on;
            }
            else if (chr == eventChar)
            {
                eventSubscribed = on;
            }
            else if (chr == rawChar)
            {
                rawSubscribed = on;
            }
            else if (chr == telemetryChar)
            {
                telemetrySubscribed = on;
            }
        }
    };

    ServerCallbacks serverCallbacks;
    ControlCallbacks controlCallbacks;
    SubscribeCallbacks subscribeCallbacks;

    bool sendTo(NimBLECharacteristic* chr, const std::atomic<bool>& subscribed, const uint8_t* data, size_t length)
    {
        // notify() with a value doesn't check the CCCD itself, so we do
        if (!connected || !subscribed)
        {
            return false;
        }
        return chr->notify(data, length, connHandle);
    }
}

namespace sensorBle
{
    bool begin()
    {
        if (!NimBLEDevice::init(deviceName))
        {
            return false;
        }
        NimBLEDevice::setMTU(preferredMtu);

        server = NimBLEDevice::createServer();
        server->setCallbacks(&serverCallbacks);
        server->advertiseOnDisconnect(true);  // off by default in NimBLE, without it the sensor vanishes after one disconnect

        NimBLEService* service = server->createService(bleIds::service);

        NimBLECharacteristic* controlChar = service->createCharacteristic(bleIds::control, NIMBLE_PROPERTY::WRITE);
        controlChar->setCallbacks(&controlCallbacks);

        statusChar = service->createCharacteristic(bleIds::status, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
        eventChar = service->createCharacteristic(bleIds::event, NIMBLE_PROPERTY::NOTIFY);
        rawChar = service->createCharacteristic(bleIds::rawStream, NIMBLE_PROPERTY::NOTIFY);
        telemetryChar = service->createCharacteristic(bleIds::telemetry, NIMBLE_PROPERTY::NOTIFY);

        statusChar->setCallbacks(&subscribeCallbacks);
        eventChar->setCallbacks(&subscribeCallbacks);
        rawChar->setCallbacks(&subscribeCallbacks);
        telemetryChar->setCallbacks(&subscribeCallbacks);

        NimBLEService* batteryService = server->createService(bleIds::batteryService);
        batteryChar = batteryService->createCharacteristic(bleIds::batteryLevel, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
        setBatteryPercent(100);  // placeholder until the battery monitor exists

        server->start();

        // UUID in the main packet so the browser's picker can filter on it, name in the scan response
        NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
        advertising->addServiceUUID(bleIds::service);
        advertising->enableScanResponse(true);  // must come before setName or the name goes in the main packet
        advertising->setName(deviceName);
        return advertising->start();
    }

    bool isConnected()
    {
        return connected;
    }

    uint16_t mtu()
    {
        return currentMtu;
    }

    size_t maxRawSamplesPerPacket()
    {
        uint16_t payload = currentMtu - attHeaderBytes;
        if (payload <= packets::rawHeaderSize)
        {
            return 0;
        }
        return (payload - packets::rawHeaderSize) / packets::rawSampleSize;
    }

    bool popCommand(ControlCommand& out)
    {
        return commandQueue.pop(out);
    }

    uint32_t badWriteCount()
    {
        return badWrites;
    }

    bool sendStatus(const uint8_t* data, size_t length)
    {
        statusChar->setValue(data, length);  // keep the readable copy fresh even with no subscriber
        return sendTo(statusChar, statusSubscribed, data, length);
    }

    bool sendEvent(const uint8_t* data, size_t length)
    {
        return sendTo(eventChar, eventSubscribed, data, length);
    }

    bool sendRaw(const uint8_t* data, size_t length)
    {
        return sendTo(rawChar, rawSubscribed, data, length);
    }

    bool sendTelemetry(const uint8_t* data, size_t length)
    {
        return sendTo(telemetryChar, telemetrySubscribed, data, length);
    }

    void setBatteryPercent(uint8_t percent)
    {
        batteryChar->setValue(&percent, 1);
        if (connected)
        {
            batteryChar->notify();  // no value given, so NimBLE only sends to subscribers
        }
    }
}
