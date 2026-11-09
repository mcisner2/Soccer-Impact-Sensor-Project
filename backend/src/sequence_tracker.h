// sequence_tracker.h
// Tracks one stream's uint16 sequence counter (wraparound-safe).
// Event, Telemetry and Raw each have their OWN counter on the firmware, so each
// stream gets its own tracker (one shared counter would report false gaps).
// Status has no sequence number, so it is never tracked.

#pragma once
#include <cstdint>
#include <optional>
#include <utility>

namespace session
{
    // sequence numbers are uint16 and wrap at 65535
    constexpr uint32_t seqMod = 0x10000;

    struct SequenceCheck
    {
        // false -> duplicate or late packet, drop it
        bool accepted;
        // (first, last) inclusive, set when packets were skipped
        std::optional<std::pair<uint16_t, uint16_t>> gap;
        // how many packets were skipped
        uint32_t missed;
    };

    class SequenceTracker
    {
    public:
        SequenceCheck check(uint16_t seq);

    private:
        std::optional<uint16_t> last_;
    };
}