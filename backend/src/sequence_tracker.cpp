// sequence_tracker.cpp

#include "sequence_tracker.h"

namespace session
{
    SequenceCheck SequenceTracker::check(uint16_t seq)
    {
        if (!last_.has_value())
        {
            last_ = seq;
            return {true, std::nullopt, 0};
        }

        // Arithmetic is done in uint16_t on purpose: it wraps at 65536 by itself,
        // which is the same as the Python "% 0x10000".
        const uint16_t expected = static_cast<uint16_t>(*last_ + 1);
        const uint16_t skipped = static_cast<uint16_t>(seq - expected);

        if (skipped == 0)
        {
            last_ = seq;
            return {true, std::nullopt, 0};
        }
        if (skipped >= seqMod / 2)
        {
            // "behind" what we expected: duplicate or out of order
            return {false, std::nullopt, 0};
        }

        last_ = seq;
        const uint16_t gapLast = static_cast<uint16_t>(seq - 1);
        return {true, std::make_pair(expected, gapLast), skipped};
    }
}