// gpsParser.h
// Minimal NMEA reader for the two sentences we enable (RMC, GGA), plus the PMTK command builder.
// Pure text in, numbers out, no hardware, so it can be tested on a laptop.

#pragma once
#include <stdint.h>
#include <stddef.h>

// Kept in the same units the telemetry packet sends (spec 6.4), so nothing is converted twice
struct GpsFix
{
    int32_t lat1e7 = 0;      // degrees x 1e7, float can't hold this precisely
    int32_t lon1e7 = 0;
    uint16_t speedCms = 0;
    uint8_t fixQuality = 0;  // GGA: 0 none, 1 GPS, 2 DGPS/WAAS
    uint8_t satellites = 0;
    uint8_t hdopX10 = 255;   // 255 = not reported yet
    bool rmcValid = false;   // RMC status A = receiver trusts this fix
};

namespace gpsParser
{
    bool checksumOk(const char* line);

    // Updates only the fields its sentence carries. Returns true for a parsed RMC (one per fix).
    bool parseLine(const char* line, GpsFix& fix);

    // Wraps a body like "PMTK220,200" into "$PMTK220,200*2C\r\n". Returns length, 0 if it didn't fit.
    size_t buildSentence(const char* body, char* out, size_t outSize);
}
