// packet_parser.h
// Decodes the BLE packets sent by the sensor (interface spec section 6)
// into plain C++ structs. This is the reverse of firmware/packets.cpp:
// bytes in, values out.

#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace parser
{
    // pkt_type, byte 0 of every packet (same values as firmware/packets.h)
    constexpr uint8_t typeStatus = 0x01;
    constexpr uint8_t typeEvent = 0x02;
    constexpr uint8_t typeTelemetry = 0x03;
    constexpr uint8_t typeRaw = 0x04;

    constexpr size_t statusSize = 12;
    constexpr size_t eventSize = 14;
    constexpr size_t telemetrySize = 20;
    constexpr size_t rawHeaderSize = 8;
    constexpr size_t rawSampleSize = 14;

    struct StatusPacket
    {
        uint8_t state;
        uint8_t errorCode;
        uint8_t flags;
        uint16_t batteryMv;
        int16_t tempDc;  // deci-degrees C, so 253 means 25.3 C
        uint32_t tMs;
    };

    struct EventPacket
    {
        uint8_t eventClass;  // 0 unknown, 1 kick, 2 hard impact
        uint16_t seq;
        uint32_t tMs;
        uint16_t peakAccelCg;  // centi-g, so 3000 means 30 g
        uint16_t peakGyroDps;
        uint8_t confidence;  // 0-100
        uint8_t flags;
    };

    struct TelemetryPacket
    {
        uint8_t fixQuality;
        uint16_t seq;
        uint32_t tMs;
        int32_t lat1e7;  // degrees x 1e7
        int32_t lon1e7;
        uint16_t speedCms;  // cm/s
        uint8_t satellites;
        uint8_t hdopX10;
    };

    struct RawSample
    {
        int16_t accel[3];  // raw LSB
        int16_t gyro[3];   // raw LSB
        int16_t tempRaw;
    };

    struct RawPacket
    {
        uint16_t seq;
        uint32_t firstTMs;  // t_ms of the first sample in the batch
        std::vector<RawSample> samples;
    };

    using Packet = std::variant<StatusPacket, EventPacket, TelemetryPacket, RawPacket>;

    enum class ParseError
    {
        None,
        Empty,        // no bytes at all
        UnknownType,  // first byte isn't 0x01-0x04
        BadLength     // wrong number of bytes for that packet type
    };

    struct ParseResult
    {
        ParseError error = ParseError::None;
        std::optional<Packet> packet;  // only has a value when error == None
    };

    // Decode one notification's worth of bytes. Never throws and never reads
    // past `length`, so garbage or truncated input just returns an error.
    ParseResult parse(const uint8_t* data, size_t length);
}