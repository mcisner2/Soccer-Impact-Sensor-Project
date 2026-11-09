// session_store.cpp

#include "session_store.h"

namespace session
{
    void SessionStore::subscribe(std::function<void(const Session&)> callback)
    {
        subscribers_.push_back(std::move(callback));
    }

    void SessionStore::ingest(const parser::Packet& packet)
    {
        if (const auto* status = std::get_if<parser::StatusPacket>(&packet))
        {
            // A new recording may restart the firmware's sequence counters. Without this, a counter
            // that restarts at 0 would look "behind" and every event would be dropped as a duplicate.
            const std::optional<parser::StatusPacket>& previous = session_.latestStatus;
            if (status->state == stateRecording && (!previous.has_value() || previous->state != stateRecording))
            {
                resetSequenceTracking();
            }
            session_.latestStatus = *status;  // no sequence number on Status
            notify();
            return;
        }

        // Returns false for a duplicate or late packet (drop it); records any gap otherwise.
        auto accept = [this](Stream stream, uint16_t seq) -> bool
        {
            const size_t i = streamIndex(stream);
            const SequenceCheck result = trackers_[i].check(seq);
            if (!result.accepted) return false;
            if (result.gap.has_value())
            {
                session_.sequenceGaps[i].push_back(*result.gap);
                session_.missedByStream[i] += result.missed;
            }
            return true;
        };

        if (const auto* event = std::get_if<parser::EventPacket>(&packet))
        {
            if (!accept(Stream::Event, event->seq)) return;
            session_.events.push_back(*event);
        }
        else if (const auto* telemetry = std::get_if<parser::TelemetryPacket>(&packet))
        {
            if (!accept(Stream::Telemetry, telemetry->seq)) return;
            session_.telemetry.push_back(*telemetry);
        }
        else if (const auto* raw = std::get_if<parser::RawPacket>(&packet))
        {
            if (!accept(Stream::Raw, raw->seq)) return;
            session_.rawBatches.push_back(*raw);
        }
        notify();
    }

    void SessionStore::resetSequenceTracking()
    {
        trackers_ = {};
    }

    void SessionStore::setBatteryLevel(int level)
    {
        if (level < 0 || level > 100)
        {
            recordParseError();
            return;
        }
        session_.batteryLevel = level;
        notify();
    }

    void SessionStore::recordParseError()
    {
        session_.parseErrorCount++;
        notify();
    }

    void SessionStore::reset()
    {
        session_ = Session();  // startedAt is set to "now" by the default member initializer
        trackers_ = {};
        notify();
    }

    void SessionStore::notify()
    {
        // Copy first, like Python's list(self._subscribers), so a callback can safely subscribe.
        const auto callbacks = subscribers_;
        for (const auto& callback : callbacks) callback(session_);
    }
}