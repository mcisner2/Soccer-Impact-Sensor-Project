// sequence_tracker_test.cpp
// Ported from the Python SequenceTracker behavior in session_store.py.

#include <gtest/gtest.h>
#include "sequence_tracker.h"

using namespace session;

TEST(SequenceTracker, AcceptsFirstPacketWhateverItsNumber)
{
    SequenceTracker t;
    SequenceCheck r = t.check(500);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
    EXPECT_EQ(r.missed, 0u);
}

TEST(SequenceTracker, AcceptsConsecutivePacketsWithNoGap)
{
    SequenceTracker t;
    t.check(5);
    SequenceCheck r = t.check(6);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
    EXPECT_EQ(r.missed, 0u);
}

TEST(SequenceTracker, ReportsGapWhenPacketsAreSkipped)
{
    SequenceTracker t;
    t.check(5);
    SequenceCheck r = t.check(8);  // 6 and 7 never arrived
    EXPECT_TRUE(r.accepted);
    ASSERT_TRUE(r.gap.has_value());
    EXPECT_EQ(r.gap->first, 6);
    EXPECT_EQ(r.gap->second, 7);
    EXPECT_EQ(r.missed, 2u);
}

TEST(SequenceTracker, HandlesWraparoundWithNoGap)
{
    SequenceTracker t;
    t.check(65535);
    SequenceCheck r = t.check(0);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
    EXPECT_EQ(r.missed, 0u);
}

TEST(SequenceTracker, ReportsGapThatSpansTheWraparound)
{
    SequenceTracker t;
    t.check(65534);
    SequenceCheck r = t.check(1);  // 65535 and 0 never arrived
    EXPECT_TRUE(r.accepted);
    ASSERT_TRUE(r.gap.has_value());
    EXPECT_EQ(r.gap->first, 65535);
    EXPECT_EQ(r.gap->second, 0);
    EXPECT_EQ(r.missed, 2u);
}

TEST(SequenceTracker, RejectsDuplicatePacket)
{
    SequenceTracker t;
    t.check(5);
    SequenceCheck r = t.check(5);
    EXPECT_FALSE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
    EXPECT_EQ(r.missed, 0u);
}

TEST(SequenceTracker, RejectsLatePacketAndKeepsItsPlace)
{
    SequenceTracker t;
    t.check(10);
    EXPECT_FALSE(t.check(8).accepted);  // late, dropped
    // The rejected packet must not move the counter, so 11 is still "next".
    SequenceCheck r = t.check(11);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.gap.has_value());
}

TEST(SequenceTracker, HalfRangeIsTheBoundaryBetweenGapAndLate)
{
    {
        SequenceTracker t;
        t.check(0);
        // 32767 packets skipped: still counted as a gap
        SequenceCheck r = t.check(32768);
        EXPECT_TRUE(r.accepted);
        EXPECT_EQ(r.missed, 32767u);
    }
    {
        SequenceTracker t;
        t.check(0);
        // 32768 packets "skipped" is treated as behind us, so it is dropped
        EXPECT_FALSE(t.check(32769).accepted);
    }
}

TEST(SequenceTracker, FreshTrackerForgetsEverything)
{
    // Python resets tracking by making new trackers; a new object must accept
    // any first number, even one that would have been "behind" the old counter.
    SequenceTracker old;
    old.check(500);
    SequenceTracker fresh;
    EXPECT_TRUE(fresh.check(0).accepted);
}