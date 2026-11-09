// packet_parser.cpp

#include "packet_parser.h"

namespace
{
    // little-endian: low byte first, as the spec requires
    uint16_t getU16(const uint8_t* in)
    {
        return (uint16_t)(in[0] | (in[1] << 8));
    }

    uint32_t getU32(const uint8_t* in)
    {
        return (uint32_t)in[0]
             | ((uint32_t)in[1] << 8)
             | ((uint32_t)in[2] << 16)
             | ((uint32_t)in[3] << 24);
    }

    // The firmware sends signed values as the same bits stored in an
    // unsigned field, so reading them back is a reinterpretation.
    int16_t getI16(const uint8_t* in)
    {
        return static_cast<int16_t>(getU16(in));
    }

    int32_t getI32(const uint8_t* in)
    {
        return static_cast<int32_t>(getU32(in));
    }

    parser::ParseResult fail(parser::ParseError error)
    {
        parser::ParseResult result;
        result.error = error;
        return result;
    }

    parser::ParseResult ok(parser::Packet packet)
    {
        parser::ParseResult result;
        result.packet = std::move(packet);
        return result;
    }

    // Offsets below match firmware/packets.cpp and spec section 6.
    parser::StatusPacket parseStatus(const uint8_t* d)
    {
        parser::StatusPacket p;
        p.state = d[1];
        p.errorCode = d[2];
        p.flags = d[3];
        p.batteryMv = getU16(d + 4);
        p.tempDc = getI16(d + 6);
        p.tMs = getU32(d + 8);
        return p;
    }

    parser::EventPacket parseEvent(const uint8_t* d)
    {
        parser::EventPacket p;
        p.eventClass = d[1];
        p.seq = getU16(d + 2);
        p.tMs = getU32(d + 4);
        p.peakAccelCg = getU16(d + 8);
        p.peakGyroDps = getU16(d + 10);
        p.confidence = d[12];
        p.flags = d[13];
        return p;
    }

    parser::TelemetryPacket parseTelemetry(const uint8_t* d)
    {
        parser::TelemetryPacket p;
        p.fixQuality = d[1];
        p.seq = getU16(d + 2);
        p.tMs = getU32(d + 4);
        p.lat1e7 = getI32(d + 8);
        p.lon1e7 = getI32(d + 12);
        p.speedCms = getU16(d + 16);
        p.satellites = d[18];
        p.hdopX10 = d[19];
        return p;
    }

    parser::RawPacket parseRaw(const uint8_t* d, size_t sampleCount)
    {
        parser::RawPacket p;
        p.seq = getU16(d + 2);
        p.firstTMs = getU32(d + 4);
        p.samples.reserve(sampleCount);
        for (size_t i = 0; i < sampleCount; i++)
        {
            const uint8_t* s = d + parser::rawHeaderSize + i * parser::rawSampleSize;
            parser::RawSample sample;
            for (int axis = 0; axis < 3; axis++)
            {
                sample.accel[axis] = getI16(s + axis * 2);
                sample.gyro[axis] = getI16(s + 6 + axis * 2);
            }
            sample.tempRaw = getI16(s + 12);
            p.samples.push_back(sample);
        }
        return p;
    }
}

namespace parser
{
    ParseResult parse(const uint8_t* data, size_t length)
    {
        if (data == nullptr || length == 0)
        {
            return fail(ParseError::Empty);
        }

        // Each packet type has an exact size, so a length mismatch means
        // the packet was corrupted or cut short. Rejecting it is safer
        // than decoding garbage and saving it to the database.
        switch (data[0])
        {
            case typeStatus:
                if (length != statusSize) return fail(ParseError::BadLength);
                return ok(parseStatus(data));

            case typeEvent:
                if (length != eventSize) return fail(ParseError::BadLength);
                return ok(parseEvent(data));

            case typeTelemetry:
                if (length != telemetrySize) return fail(ParseError::BadLength);
                return ok(parseTelemetry(data));

            case typeRaw:
            {
                if (length < rawHeaderSize) return fail(ParseError::BadLength);
                const size_t sampleCount = data[1];
                if (length != rawHeaderSize + sampleCount * rawSampleSize)
                {
                    return fail(ParseError::BadLength);
                }
                return ok(parseRaw(data, sampleCount));
            }

            default:
                return fail(ParseError::UnknownType);
        }
    }
}