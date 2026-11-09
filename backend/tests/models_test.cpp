// models_test.cpp

#include <gtest/gtest.h>
#include "models.h"

using namespace session;

TEST(Models, StateNames)
{
    EXPECT_EQ(stateName(0), "BOOT");
    EXPECT_EQ(stateName(4), "RECORDING");
    EXPECT_EQ(stateName(6), "ERROR");
    EXPECT_EQ(stateName(99), "STATE_99");
}

TEST(Models, ErrorNameAndMessage)
{
    EXPECT_EQ(errorName(0), "NONE");
    EXPECT_FALSE(errorMessage(0).has_value());

    EXPECT_EQ(errorName(0x10), "ERR_CAL_FAILED");
    ASSERT_TRUE(errorMessage(0x10).has_value());
    EXPECT_EQ(*errorMessage(0x10), "Calibration failed - try recalibrating");

    EXPECT_EQ(errorName(0x7F), "ERROR_0x7f");
    ASSERT_TRUE(errorMessage(0x7F).has_value());
    EXPECT_EQ(*errorMessage(0x7F), "Sensor reported error 0x7f");
}

TEST(Models, EventClassNamesKeepUnknownValues)
{
    EXPECT_EQ(eventClassName(1), "KICK");
    EXPECT_EQ(eventClassName(4), "GROUND_IMPACT");
    EXPECT_EQ(eventClassName(42), "CLASS_42");
}

TEST(Models, StatusFlagsAndTemperature)
{
    parser::StatusPacket s{};
    s.flags = 0x05;  // calibration stale + low battery
    s.tempDc = -105;
    EXPECT_TRUE(calibrationStale(s));
    EXPECT_FALSE(gpsDegraded(s));
    EXPECT_TRUE(lowBattery(s));
    EXPECT_FALSE(rawStreamOn(s));
    EXPECT_NEAR(temperatureC(s), -10.5, 1e-9);
}

TEST(Models, TelemetryConversions)
{
    parser::TelemetryPacket t{};
    t.fixQuality = 1;
    t.lat1e7 = 411234567;
    t.lon1e7 = -862345678;
    t.speedCms = 750;
    t.hdopX10 = 18;
    EXPECT_TRUE(hasFix(t));
    EXPECT_NEAR(latitude(t), 41.1234567, 1e-9);
    EXPECT_NEAR(longitude(t), -86.2345678, 1e-9);
    EXPECT_NEAR(speedMS(t), 7.5, 1e-9);
    EXPECT_NEAR(hdop(t), 1.8, 1e-9);
    EXPECT_EQ(fixName(2), "DGPS/WAAS");

    t.fixQuality = 0;
    EXPECT_FALSE(hasFix(t));
}

TEST(Models, PeakAccelConversion)
{
    parser::EventPacket e{};
    e.peakAccelCg = 1523;
    EXPECT_NEAR(peakAccelG(e), 15.23, 1e-9);
}