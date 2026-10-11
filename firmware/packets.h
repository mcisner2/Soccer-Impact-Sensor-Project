// packets.h
// Byte-exact encoders for spec section 6. Fields are written one byte at a time, never by copying a struct,
// so compiler padding can't sneak into the wire format.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "imu.h"

namespace packets
{
    // pkt_type, byte 0 of every packet
    constexpr uint8_t typeStatus = 0x01;
    constexpr uint8_t typeEvent = 0x02;
    constexpr uint8_t typeTelemetry = 0x03;
    constexpr uint8_t typeRaw = 0x04;

    constexpr size_t statusSize = 12;
    constexpr size_t eventSize = 14;
    constexpr size_t telemetrySize = 20;
    constexpr size_t rawHeaderSize = 8;
    constexpr size_t rawSampleSize = 14;

    // Control commands (spec 6.1), webpage -> device
    constexpr uint8_t cmdStartCal = 0x01;
    constexpr uint8_t cmdStartSession = 0x02;
    constexpr uint8_t cmdStopSession = 0x03;
    constexpr uint8_t cmdRecal = 0x04;
    constexpr uint8_t cmdRawStreamOn = 0x05;
    constexpr uint8_t cmdRawStreamOff = 0x06;
    constexpr size_t controlSize = 2;

    // Status flags byte (spec 6.2)
    constexpr uint8_t flagCalStale = 0x01;
    constexpr uint8_t flagGpsDegraded = 0x02;
    constexpr uint8_t flagLowBattery = 0x04;
    constexpr uint8_t flagRawStreamOn = 0x08;

    struct StatusFields
    {
        uint8_t state;
        uint8_t errorCode;
        uint8_t flags;
        uint16_t batteryMv;
        int16_t tempDc;
        uint32_t tMs;
    };

    struct EventFields
    {
        uint8_t eventClass;
        uint16_t seq;
        uint32_t tMs;
        uint16_t peakAccelCg;
        uint16_t peakGyroDps;
        uint8_t confidence;
        uint8_t flags;
    };

    struct TelemetryFields
    {
        uint8_t fixQuality;
        uint16_t seq;
        uint32_t tMs;
        int32_t lat1e7;
        int32_t lon1e7;
        uint16_t speedCms;
        uint8_t satellites;
        uint8_t hdopX10;
    };

    // Each returns bytes written. Caller owns a big enough buffer.
    size_t encodeStatus(const StatusFields& in, uint8_t* out);
    size_t encodeEvent(const EventFields& in, uint8_t* out);
    size_t encodeTelemetry(const TelemetryFields& in, uint8_t* out);
    size_t encodeRawHeader(uint8_t sampleCount, uint16_t seq, uint32_t firstTMs, uint8_t* out);
    size_t encodeRawSample(const RawImuSample& sample, uint8_t* out);
}
