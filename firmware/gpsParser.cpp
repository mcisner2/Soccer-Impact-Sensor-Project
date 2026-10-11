// gpsParser.cpp

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "gpsParser.h"

namespace
{
    constexpr size_t maxFields = 20;
    constexpr size_t maxLine = 96;  // NMEA caps sentences at 82 characters

    uint8_t xorBetween(const char* start, const char* end)
    {
        uint8_t sum = 0;
        for (const char* p = start; p < end; p++)
        {
            sum ^= (uint8_t)*p;
        }
        return sum;
    }

    // "4141.1234" + 'N' -> 416852056 (41.6852056 deg). Integer math the whole way to keep all 7 decimals.
    bool parseCoord(const char* value, const char* hemi, int32_t& out)
    {
        const char* dot = strchr(value, '.');
        if (!dot || dot - value < 3)
        {
            return false;
        }

        int degDigits = (int)(dot - value) - 2;  // the last two digits before the dot are minutes
        int64_t degrees = 0;
        for (int i = 0; i < degDigits; i++)
        {
            degrees = degrees * 10 + (value[i] - '0');
        }

        int64_t minutesScaled = (value[degDigits] - '0') * 10 + (value[degDigits + 1] - '0');
        int64_t scale = 1;
        for (const char* p = dot + 1; *p >= '0' && *p <= '9' && scale < 1000000; p++)
        {
            minutesScaled = minutesScaled * 10 + (*p - '0');
            scale *= 10;
        }

        int64_t result = degrees * 10000000LL + minutesScaled * 10000000LL / (60 * scale);
        if (hemi[0] == 'S' || hemi[0] == 'W')
        {
            result = -result;
        }
        out = (int32_t)result;
        return true;
    }
}

namespace gpsParser
{
    bool checksumOk(const char* line)
    {
        const char* star = strchr(line, '*');
        if (line[0] != '$' || !star || strlen(star) < 3)
        {
            return false;
        }
        return xorBetween(line + 1, star) == (uint8_t)strtol(star + 1, nullptr, 16);
    }

    bool parseLine(const char* line, GpsFix& fix)
    {
        if (!checksumOk(line) || strlen(line) >= maxLine)
        {
            return false;
        }

        // copy and cut at '*' so fields can be split in place
        char buffer[maxLine];
        strcpy(buffer, line);
        *strchr(buffer, '*') = '\0';

        // strtok would skip empty fields, and NMEA uses empty fields for "no data", so split by hand
        const char* fields[maxFields];
        size_t count = 0;
        fields[count++] = buffer;
        for (char* p = buffer; *p && count < maxFields; p++)
        {
            if (*p == ',')
            {
                *p = '\0';
                fields[count++] = p + 1;
            }
        }

        // talker can be GP or GN, so match on the last three letters
        const char* type = fields[0] + 3;

        if (strcmp(type, "RMC") == 0 && count >= 8)
        {
            fix.rmcValid = fields[2][0] == 'A';
            if (fix.rmcValid)
            {
                parseCoord(fields[3], fields[4], fix.lat1e7);
                parseCoord(fields[5], fields[6], fix.lon1e7);
                fix.speedCms = (uint16_t)(atof(fields[7]) * 51.4444f + 0.5f);  // knots to cm/s
            }
            return true;
        }

        if (strcmp(type, "GGA") == 0 && count >= 9)
        {
            fix.fixQuality = (uint8_t)atoi(fields[6]);
            fix.satellites = (uint8_t)atoi(fields[7]);
            fix.hdopX10 = fields[8][0] ? (uint8_t)(atof(fields[8]) * 10.0f + 0.5f) : 255;
        }
        return false;
    }

    size_t buildSentence(const char* body, char* out, size_t outSize)
    {
        uint8_t sum = xorBetween(body, body + strlen(body));
        int written = snprintf(out, outSize, "$%s*%02X\r\n", body, sum);
        return (written > 0 && (size_t)written < outSize) ? (size_t)written : 0;
    }
}
