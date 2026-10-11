// packets.cpp

#include "packets.h"

namespace
{
    // little-endian: low byte first, as the spec requires
    void putU16(uint8_t* out, uint16_t value)
    {
        out[0] = (uint8_t)(value & 0xFF);
        out[1] = (uint8_t)(value >> 8);
    }

    void putU32(uint8_t* out, uint32_t value)
    {
        out[0] = (uint8_t)(value & 0xFF);
        out[1] = (uint8_t)(value >> 8);
        out[2] = (uint8_t)(value >> 16);
        out[3] = (uint8_t)(value >> 24);
    }
}

namespace packets
{
    size_t encodeStatus(const StatusFields& in, uint8_t* out)
    {
        out[0] = typeStatus;
        out[1] = in.state;
        out[2] = in.errorCode;
        out[3] = in.flags;
        putU16(out + 4, in.batteryMv);
        putU16(out + 6, (uint16_t)in.tempDc);  // same bits, reader treats it as signed
        putU32(out + 8, in.tMs);
        return statusSize;
    }

    size_t encodeEvent(const EventFields& in, uint8_t* out)
    {
        out[0] = typeEvent;
        out[1] = in.eventClass;
        putU16(out + 2, in.seq);
        putU32(out + 4, in.tMs);
        putU16(out + 8, in.peakAccelCg);
        putU16(out + 10, in.peakGyroDps);
        out[12] = in.confidence;
        out[13] = in.flags;
        return eventSize;
    }

    size_t encodeTelemetry(const TelemetryFields& in, uint8_t* out)
    {
        out[0] = typeTelemetry;
        out[1] = in.fixQuality;
        putU16(out + 2, in.seq);
        putU32(out + 4, in.tMs);
        putU32(out + 8, (uint32_t)in.lat1e7);  // same bits, reader treats them as signed
        putU32(out + 12, (uint32_t)in.lon1e7);
        putU16(out + 16, in.speedCms);
        out[18] = in.satellites;
        out[19] = in.hdopX10;
        return telemetrySize;
    }

    size_t encodeRawHeader(uint8_t sampleCount, uint16_t seq, uint32_t firstTMs, uint8_t* out)
    {
        out[0] = typeRaw;
        out[1] = sampleCount;
        putU16(out + 2, seq);
        putU32(out + 4, firstTMs);
        return rawHeaderSize;
    }

    size_t encodeRawSample(const RawImuSample& sample, uint8_t* out)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            putU16(out + axis * 2, (uint16_t)sample.accel[axis]);
            putU16(out + 6 + axis * 2, (uint16_t)sample.gyro[axis]);
        }
        putU16(out + 12, (uint16_t)sample.tempRaw);
        return rawSampleSize;
    }
}
