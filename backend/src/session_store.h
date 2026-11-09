// session_store.h
// Session Store: owns the current Session and detects missing packets.
//
// Event, Telemetry and Raw each have their OWN sequence counter on the firmware, so each
// gets its own tracker here (one shared counter would report false gaps). Status has no
// sequence number, so it is never tracked.

#pragma once
#include <array>
#include <functional>
#include <vector>

#include "models.h"
#include "packet_parser.h"
#include "sequence_tracker.h"

namespace session
{
    class SessionStore
    {
    public:
        // --- reading ---
        const Session& session() const { return session_; }

        void subscribe(std::function<void(const Session&)> callback);

        // --- writing ---

        // Add one parsed packet; record a gap if its stream's sequence numbers skipped.
        void ingest(const parser::Packet& packet);

        // Forget the last-seen sequence numbers (keeps all data). Called when a recording starts.
        void resetSequenceTracking();

        // Standard Battery Level characteristic (one byte, 0-100).
        void setBatteryLevel(int level);

        // Called when the parser rejected a payload; session data is untouched.
        void recordParseError();

        // Start a fresh session (e.g. a new drill).
        void reset();

    private:
        void notify();

        Session session_;
        std::array<SequenceTracker, streamCount> trackers_;
        std::vector<std::function<void(const Session&)>> subscribers_;
    };
}