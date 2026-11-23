// models.h
// Data models: what the rest of the app works with (matches Ed's interface spec).
// The packet structs themselves (StatusPacket, EventPacket, TelemetryPacket,
// RawPacket) live in packet_parser.h; this file adds the Session that holds
// them plus the plain-language helpers the dashboard needs.

#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "packet_parser.h"

namespace session
{
    // Streams that carry their own sequence counter. (Status has no sequence number.)
    enum class Stream
    {
        Event = 0,
        Telemetry = 1,
        Raw = 2
    };
    constexpr size_t streamCount = 3;

    inline size_t streamIndex(Stream s)
    {
        return static_cast<size_t>(s);
    }

    // Known event classes.
    //
    // CONFLICT TO RESOLVE: the firmware owner's list is 1 kick, 2 pass, 3 trap, 4 ground impact
    // (used here). Ed's Interface Specification v0.1 says 0 UNKNOWN, 1 KICK, 2 HARD_IMPACT and
    // treats pass/trap as a later refinement. Class 2 therefore means different things in the two
    // documents. The parser accepts ANY class value (never drops an event over it); only the
    // display names below depend on this table.
    enum class EventClass : uint8_t
    {
        Unknown = 0,
        Kick = 1,
        Pass = 2,
        Trap = 3,
        GroundImpact = 4
    };

    // Device state code from the Status packet (Interface Specification, section 6.2 / 7).
    constexpr uint8_t stateRecording = 4;

    // --- plain-language helpers (these were properties on the Python dataclasses) ---

    // "BOOT", "SENSOR_INIT", ... or "STATE_<n>" for an unknown code
    std::string stateName(uint8_t state);

    // "NONE" when there is no error, the spec name (e.g. "ERR_CAL_FAILED"), or "ERROR_0x<hex>"
    std::string errorName(uint8_t errorCode);

    // Plain-language message for the dashboard, or nullopt when there is no error.
    std::optional<std::string> errorMessage(uint8_t errorCode);

    // "UNKNOWN", "KICK", ... or "CLASS_<n>"; unknown values are kept, not rejected
    std::string eventClassName(uint8_t eventClass);

    // "none", "GPS", "DGPS/WAAS", or "FIX_<n>"
    std::string fixName(uint8_t fixQuality);

    // Status
    double temperatureC(const parser::StatusPacket& s);
    bool calibrationStale(const parser::StatusPacket& s);
    bool gpsDegraded(const parser::StatusPacket& s);
    bool lowBattery(const parser::StatusPacket& s);
    bool rawStreamOn(const parser::StatusPacket& s);

    // Event: hundredths of a g (1523 = 15.23 g)
    double peakAccelG(const parser::EventPacket& e);

    // Telemetry
    // fix_quality 0 means no fix: latitude/longitude are not meaningful.
    bool hasFix(const parser::TelemetryPacket& t);
    double latitude(const parser::TelemetryPacket& t);
    double longitude(const parser::TelemetryPacket& t);
    double speedMS(const parser::TelemetryPacket& t);
    double hdop(const parser::TelemetryPacket& t);

    // Everything received since the session started. Single source of truth.
    struct Session
    {
        std::chrono::system_clock::time_point startedAt = std::chrono::system_clock::now();
        std::optional<parser::StatusPacket> latestStatus;
        // standard Battery Level characteristic, 0-100
        std::optional<int> batteryLevel;
        std::vector<parser::EventPacket> events;
        std::vector<parser::TelemetryPacket> telemetry;
        std::vector<parser::RawPacket> rawBatches;
        // Per stream: list of (first_missing_seq, last_missing_seq), inclusive; may wrap past 65535.
        std::array<std::vector<std::pair<uint16_t, uint16_t>>, streamCount> sequenceGaps;
        std::array<uint32_t, streamCount> missedByStream{};
        uint32_t parseErrorCount = 0;

        uint32_t missedPacketCount() const;
    };
}