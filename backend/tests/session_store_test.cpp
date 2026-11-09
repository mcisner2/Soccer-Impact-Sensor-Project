// session_store_test.cpp
// Written from the behavior in session_store.py. Compare against the Python
// tests/test_session_store.py once we port those too.

#include <gtest/gtest.h>
#include "session_store.h"

using namespace session;

namespace
{
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

TEST(SessionStore, RecordsGapOnlyForTheStreamThatSkipped)
{
    SessionStore store;
    store.ingest(makeEvent(1));
    store.ingest(makeEvent(4));  // 2 and 3 never arrived
    store.ingest(makeTelemetry(1));
    store.ingest(makeTelemetry(2));

    const Session& s = store.session();
    ASSERT_EQ(s.sequenceGaps[streamIndex(Stream::Event)].size(), 1u);
    EXPECT_EQ(s.sequenceGaps[streamIndex(Stream::Event)][0].first, 2);
    EXPECT_EQ(s.sequenceGaps[streamIndex(Stream::Event)][0].second, 3);
    EXPECT_EQ(s.missedByStream[streamIndex(Stream::Event)], 2u);
    EXPECT_TRUE(s.sequenceGaps[streamIndex(Stream::Telemetry)].empty());
    EXPECT_EQ(s.missedPacketCount(), 2u);
    EXPECT_EQ(s.events.size(), 2u);  // the late-arriving packet after a gap is still stored
}

TEST(SessionStore, StreamsKeepIndependentCounters)
{
    // One shared counter would report false gaps here.
    SessionStore store;
    store.ingest(makeEvent(5));
    store.ingest(makeTelemetry(1));
    store.ingest(makeRaw(9));
    store.ingest(makeEvent(6));
    store.ingest(makeTelemetry(2));
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, DropsDuplicatePackets)
{
    SessionStore store;
    store.ingest(makeEvent(5));
    store.ingest(makeEvent(5));
    EXPECT_EQ(store.session().events.size(), 1u);
}

TEST(SessionStore, StartOfRecordingResetsSequenceTracking)
{
    SessionStore store;
    store.ingest(makeEvent(100));
    store.ingest(makeStatus(stateRecording));  // firmware restarts its counters
    store.ingest(makeEvent(0));
    EXPECT_EQ(store.session().events.size(), 2u);  // not dropped as "behind"
    EXPECT_EQ(store.session().missedPacketCount(), 0u);
}

TEST(SessionStore, StatusAlreadyRecordingDoesNotResetTracking)
{
    SessionStore store;
    store.ingest(makeStatus(stateRecording));
    store.ingest(makeEvent(100));
    store.ingest(makeStatus(stateRecording));  // still recording, so no reset
    store.ingest(makeEvent(0));                // behind the counter, so dropped
    EXPECT_EQ(store.session().events.size(), 1u);
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

TEST(SessionStore, BatteryLevelOutOfRangeCountsAsParseError)
{
    SessionStore store;
    store.setBatteryLevel(50);
    store.setBatteryLevel(101);
    store.setBatteryLevel(-1);
    EXPECT_EQ(store.session().batteryLevel, 50);  // unchanged
    EXPECT_EQ(store.session().parseErrorCount, 2u);
}

TEST(SessionStore, RecordParseErrorCountsWithoutTouchingData)
{
    SessionStore store;
    store.ingest(makeEvent(1));
    store.recordParseError();
    EXPECT_EQ(store.session().parseErrorCount, 1u);
    EXPECT_EQ(store.session().events.size(), 1u);
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

TEST(SessionStore, SubscribersAreNotifiedOnEveryChangeButNotOnDrops)
{
    SessionStore store;
    int calls = 0;
    store.subscribe([&calls](const Session&) { calls++; });

    store.ingest(makeStatus(3));   // 1
    store.ingest(makeEvent(1));    // 2
    store.ingest(makeEvent(1));    // duplicate, dropped, no notification
    store.setBatteryLevel(80);     // 3
    store.recordParseError();      // 4
    store.reset();                 // 5
    EXPECT_EQ(calls, 5);
}