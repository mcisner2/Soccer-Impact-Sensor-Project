// session_store_test.cpp
// Ported from the Python tests in tests/test_session_store.py, plus a few
// extra cases written from the logic in session_store.py.

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "session_store.h"

using namespace session;

namespace
{
    using GapList = std::vector<std::pair<uint16_t, uint16_t>>;

    parser::Packet makeStatus(uint8_t state)
    {
        parser::StatusPacket p{};
        p.state = state;
        return p;
    }

    parser::Packet makeEvent(uint16_t seq)
    {
        parser::EventPacket p{};
        p.eventClass = 1;
        p.seq = seq;
        return p;
    }

    parser::Packet makeTelemetry(uint16_t seq)
    {
        parser::TelemetryPacket p{};
        p.seq = seq;
        return p;
    }

    parser::Packet makeRaw(uint16_t seq)
    {
        parser::RawPacket p{};
        p.seq = seq;
        return p;
    }
}

TEST(SessionStore, StartsEmpty)
{
    SessionStore store;
    const Session& s = store.session();
    EXPECT_FALSE(s.latestStatus.has_value());
    EXPECT_FALSE(s.batteryLevel.has_value());
    EXPECT_TRUE(s.events.empty());
    EXPECT_TRUE(s.telemetry.empty());
    EXPECT_TRUE(s.rawBatches.empty());
    EXPECT_EQ(s.missedPacketCount(), 0u);
    EXPECT_EQ(s.parseErrorCount, 0u);
}

TEST(SessionStore, StoresEachPacketTypeInItsOwnList)
{
    SessionStore store;
    store.ingest(makeEvent(1));
    store.ingest(makeTelemetry(1));
    store.ingest(makeRaw(1));
    store.ingest(makeEvent(2));
    EXPECT_EQ(store.session().events.size(), 2u);
    EXPECT_EQ(store.session().telemetry.size(), 1u);
    EXPECT_EQ(store.session().rawBatches.size(), 1u);
}

TEST(SessionStore, StatusReplacesLatestStatus)
{
    SessionStore store;
    store.ingest(makeStatus(3));
    store.ingest(makeStatus(5));
    ASSERT_TRUE(store.session().latestStatus.has_value());
    EXPECT_EQ(store.session().latestStatus->state, 5);
}

TEST(SessionStore, InOrderPacketsHaveNoGaps)
{
    SessionStore store;
    for (uint16_t i = 1; i <= 5; i++)
    {
        store.ingest(makeEvent(i));
    }
    EXPECT_EQ(store.session().events.size(), 5u);
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, StreamsHaveIndependentCountersNoFalseGaps)
{
    // The old single-counter bug: interleaved streams with different counters looked like gaps.
    // event seq 1,2,3 ; telemetry seq 100,101,102 ; raw seq 5000,5001 -- interleaved
    SessionStore store;
    store.ingest(makeEvent(1));
    store.ingest(makeTelemetry(100));
    store.ingest(makeRaw(5000));
    store.ingest(makeEvent(2));
    store.ingest(makeTelemetry(101));
    store.ingest(makeRaw(5001));
    store.ingest(makeEvent(3));
    store.ingest(makeTelemetry(102));

    const Session& s = store.session();
    EXPECT_EQ(s.missedPacketCount(), 0u);
    for (size_t i = 0; i < streamCount; i++)
    {
        EXPECT_TRUE(s.sequenceGaps[i].empty());
    }
    EXPECT_EQ(s.events.size(), 3u);
    EXPECT_EQ(s.telemetry.size(), 3u);
    EXPECT_EQ(s.rawBatches.size(), 2u);
}

TEST(SessionStore, GapInOneStreamOnlyAffectsThatStream)
{
    SessionStore store;
    for (uint16_t seq : {1, 2, 6})  // events 3,4,5 missing
    {
        store.ingest(makeEvent(seq));
    }
    for (uint16_t seq : {1, 2, 3})
    {
        store.ingest(makeTelemetry(seq));
    }

    const Session& s = store.session();
    const GapList expected = {{3, 5}};
    EXPECT_EQ(s.sequenceGaps[streamIndex(Stream::Event)], expected);
    EXPECT_EQ(s.missedByStream[streamIndex(Stream::Event)], 3u);
    EXPECT_EQ(s.missedByStream[streamIndex(Stream::Telemetry)], 0u);
    EXPECT_EQ(s.missedByStream[streamIndex(Stream::Raw)], 0u);
    EXPECT_EQ(s.missedPacketCount(), 3u);
    EXPECT_EQ(s.events.size(), 3u);  // processing continues normally
}

TEST(SessionStore, GapAcrossUint16WraparoundForEveryStream)
{
    struct StreamCase
    {
        Stream stream;
        parser::Packet (*make)(uint16_t);
    };
    const StreamCase cases[] = {
        {Stream::Event, makeEvent},
        {Stream::Telemetry, makeTelemetry},
        {Stream::Raw, makeRaw},
    };

    for (const StreamCase& c : cases)
    {
        SessionStore store;
        store.ingest(c.make(65534));
        store.ingest(c.make(1));  // 65535 and 0 missing

        const GapList expected = {{65535, 0}};
        EXPECT_EQ(store.session().sequenceGaps[streamIndex(c.stream)], expected);
        EXPECT_EQ(store.session().missedByStream[streamIndex(c.stream)], 2u);
    }
}

TEST(SessionStore, NormalWraparoundIsNotAGap)
{
    SessionStore store;
    store.ingest(makeEvent(65535));
    store.ingest(makeEvent(0));
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, DuplicatePacketDropped)
{
    SessionStore store;
    store.ingest(makeEvent(1));
    store.ingest(makeEvent(2));
    store.ingest(makeEvent(2));
    EXPECT_EQ(store.session().events.size(), 2u);
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, StatusHasNoSequenceAndIsNeverAGap)
{
    SessionStore store;
    for (int i = 0; i < 5; i++)
    {
        store.ingest(makeStatus(3));
    }
    EXPECT_TRUE(store.session().latestStatus.has_value());
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, BatteryLevelInRangeIsStored)
{
    SessionStore store;
    store.setBatteryLevel(0);
    EXPECT_EQ(store.session().batteryLevel, 0);
    store.setBatteryLevel(100);
    EXPECT_EQ(store.session().batteryLevel, 100);
    EXPECT_EQ(store.session().parseErrorCount, 0u);
}

TEST(SessionStore, BatteryLevelOutOfRangeIsIgnoredAndCountedAsBadPacket)
{
    SessionStore store;
    store.setBatteryLevel(87);
    EXPECT_EQ(store.session().batteryLevel, 87);
    store.setBatteryLevel(150);  // out of 0-100: ignored and counted as a bad packet
    store.setBatteryLevel(-1);
    EXPECT_EQ(store.session().batteryLevel, 87);
    EXPECT_EQ(store.session().parseErrorCount, 2u);
}

TEST(SessionStore, ParseErrorCountedAndSessionUntouched)
{
    SessionStore store;
    store.ingest(makeEvent(1));
    store.recordParseError();
    EXPECT_EQ(store.session().parseErrorCount, 1u);
    EXPECT_EQ(store.session().events.size(), 1u);
}

TEST(SessionStore, SubscribersGetTheCurrentSessionAndResetClearsEverything)
{
    SessionStore store;
    std::vector<size_t> calls;
    store.subscribe([&calls](const Session& s) { calls.push_back(s.events.size()); });

    store.ingest(makeEvent(1));
    store.reset();
    const std::vector<size_t> expected = {1, 0};
    EXPECT_EQ(calls, expected);
    EXPECT_TRUE(store.session().events.empty());

    store.ingest(makeEvent(500));  // after reset the first packet never counts as a gap
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, SubscribersAreNotNotifiedWhenAPacketIsDropped)
{
    SessionStore store;
    int calls = 0;
    store.subscribe([&calls](const Session&) { calls++; });

    store.ingest(makeStatus(3));  // 1
    store.ingest(makeEvent(1));   // 2
    store.ingest(makeEvent(1));   // duplicate, dropped, no notification
    store.setBatteryLevel(80);    // 3
    store.recordParseError();     // 4
    store.reset();                // 5
    EXPECT_EQ(calls, 5);
}

TEST(SessionStore, ResetClearsDataAndTracking)
{
    SessionStore store;
    store.ingest(makeEvent(100));
    store.ingest(makeStatus(3));
    store.recordParseError();
    store.reset();

    EXPECT_TRUE(store.session().events.empty());
    EXPECT_FALSE(store.session().latestStatus.has_value());
    EXPECT_EQ(store.session().parseErrorCount, 0u);

    store.ingest(makeEvent(0));  // would be "behind" 100 if tracking had survived
    EXPECT_EQ(store.session().events.size(), 1u);
}

TEST(SessionStore, TrackerRunsPerStream)
{
    SequenceTracker a;
    SequenceTracker b;

    SequenceCheck r = a.check(10);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
    EXPECT_EQ(r.missed, 0u);

    r = b.check(500);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
    EXPECT_EQ(r.missed, 0u);

    r = a.check(11);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
    EXPECT_EQ(r.missed, 0u);

    r = b.check(503);
    EXPECT_TRUE(r.accepted);
    ASSERT_TRUE(r.gap.has_value());
    EXPECT_EQ(r.gap->first, 501);
    EXPECT_EQ(r.gap->second, 502);
    EXPECT_EQ(r.missed, 2u);
}

// ---- new recording: firmware may restart its sequence counters ----

TEST(SessionStore, RestartedCounterAfterResetIsNotDroppedAsDuplicate)
{
    SessionStore store;
    for (uint16_t q = 1; q <= 500; q++)
    {
        store.ingest(makeEvent(q));
    }
    const size_t before = store.session().events.size();
    store.resetSequenceTracking();  // what start_session() does
    for (uint16_t q = 0; q < 10; q++)
    {
        store.ingest(makeEvent(q));
    }
    EXPECT_EQ(store.session().events.size() - before, 10u);  // all accepted
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
    EXPECT_EQ(store.session().events.size(), 510u);  // earlier data kept
}

TEST(SessionStore, StatusEnteringRecordingResetsTracking)
{
    SessionStore store;
    store.ingest(makeStatus(3));  // READY
    for (uint16_t q = 1; q <= 300; q++)
    {
        store.ingest(makeEvent(q));
    }
    store.ingest(makeStatus(stateRecording));  // -> RECORDING
    store.ingest(makeEvent(0));
    store.ingest(makeEvent(1));
    EXPECT_EQ(store.session().events.size(), 302u);
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, StatusAlreadyRecordingDoesNotResetTracking)
{
    SessionStore store;
    store.ingest(makeStatus(stateRecording));  // first status: RECORDING
    store.ingest(makeEvent(1));
    store.ingest(makeEvent(2));
    store.ingest(makeStatus(stateRecording));  // 1 Hz status while still recording: no reset
    store.ingest(makeEvent(2));                // duplicate must still be dropped
    store.ingest(makeEvent(5));                // real gap must still be seen
    EXPECT_EQ(store.session().events.size(), 3u);
    const GapList expected = {{3, 4}};
    EXPECT_EQ(store.session().sequenceGaps[streamIndex(Stream::Event)], expected);
}