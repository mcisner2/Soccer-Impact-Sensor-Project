// packet_parser_test.cpp
// Each test feeds hand-written bytes to the parser and checks the decoded
// values. The bytes are written out in little-endian order on purpose, so
// the tests check the spec and not just the parser's own logic.

#include <gtest/gtest.h>
#include "packet_parser.h"

using namespace parser;

TEST(PacketParser, DecodesStatus)
{
    const std::vector<uint8_t> bytes = {
        0x01,        // type: status
        0x04,        // state: RECORDING
        0x00,        // error code: none
        0x04,        // flags: low battery
        0x10, 0x12,  // battery_mv = 0x1210 = 4624
        0xF6, 0xFF,  // temp_dc = 0xFFF6 = -10 (negative, tests signed)
        0x88, 0x13, 0x00, 0x00  // t_ms = 0x1388 = 5000
    };
    ParseResult r = parse(bytes.data(), bytes.size());
    ASSERT_EQ(r.error, ParseError::None);
    ASSERT_TRUE(r.packet.has_value());
    const StatusPacket* s = std::get_if<StatusPacket>(&*r.packet);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->state, 4);
    EXPECT_EQ(s->errorCode, 0);
    EXPECT_EQ(s->flags, 0x04);
    EXPECT_EQ(s->batteryMv, 4624);
    EXPECT_EQ(s->tempDc, -10);
    EXPECT_EQ(s->tMs, 5000u);
}

TEST(PacketParser, DecodesEvent)
{
    const std::vector<uint8_t> bytes = {
        0x02,        // type: event
        0x01,        // class: KICK
        0x07, 0x00,  // seq = 7
        0xE8, 0x03, 0x00, 0x00,  // t_ms = 1000
        0xB8, 0x0B,  // peak_accel_cg = 0x0BB8 = 3000 (30 g)
        0xF4, 0x01,  // peak_gyro_dps = 0x01F4 = 500
        0x5A,        // confidence = 90
        0x00         // flags
    };
    ParseResult r = parse(bytes.data(), bytes.size());
    ASSERT_EQ(r.error, ParseError::None);
    const EventPacket* e = std::get_if<EventPacket>(&*r.packet);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->eventClass, 1);
    EXPECT_EQ(e->seq, 7);
    EXPECT_EQ(e->tMs, 1000u);
    EXPECT_EQ(e->peakAccelCg, 3000);
    EXPECT_EQ(e->peakGyroDps, 500);
    EXPECT_EQ(e->confidence, 90);
    EXPECT_EQ(e->flags, 0);
}

TEST(PacketParser, DecodesTelemetryWithNegativeLongitude)
{
    const std::vector<uint8_t> bytes = {
        0x03,        // type: telemetry
        0x02,        // fix quality: DGPS
        0x09, 0x00,  // seq = 9
        0x10, 0x27, 0x00, 0x00,  // t_ms = 10000
        0x00, 0xE1, 0xF5, 0x05,  // lat = 0x05F5E100 = 100000000 (10.0 deg)
        0x00, 0x1F, 0x0A, 0xFA,  // lon = 0xFA0A1F00 = -100000000 (-10.0 deg)
        0xDC, 0x05,  // speed_cms = 1500
        0x08,        // satellites
        0x12         // hdop_x10 = 18
    };
    ParseResult r = parse(bytes.data(), bytes.size());
    ASSERT_EQ(r.error, ParseError::None);
    const TelemetryPacket* t = std::get_if<TelemetryPacket>(&*r.packet);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->fixQuality, 2);
    EXPECT_EQ(t->seq, 9);
    EXPECT_EQ(t->tMs, 10000u);
    EXPECT_EQ(t->lat1e7, 100000000);
    EXPECT_EQ(t->lon1e7, -100000000);
    EXPECT_EQ(t->speedCms, 1500);
    EXPECT_EQ(t->satellites, 8);
    EXPECT_EQ(t->hdopX10, 18);
}

TEST(PacketParser, DecodesRawBatchOfTwoSamples)
{
    const std::vector<uint8_t> bytes = {
        // header
        0x04,        // type: raw
        0x02,        // n_samples = 2
        0x03, 0x00,  // seq = 3
        0x64, 0x00, 0x00, 0x00,  // first t_ms = 100
        // sample 1: accel (1, -1, 1024), gyro (0, 0, -2), temp 25
        0x01, 0x00, 0xFF, 0xFF, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF,
        0x19, 0x00,
        // sample 2: accel (2, 3, 4), gyro (5, 6, 7), temp 256
        0x02, 0x00, 0x03, 0x00, 0x04, 0x00,
        0x05, 0x00, 0x06, 0x00, 0x07, 0x00,
        0x00, 0x01
    };
    ParseResult r = parse(bytes.data(), bytes.size());
    ASSERT_EQ(r.error, ParseError::None);
    const RawPacket* raw = std::get_if<RawPacket>(&*r.packet);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->seq, 3);
    EXPECT_EQ(raw->firstTMs, 100u);
    ASSERT_EQ(raw->samples.size(), 2u);
    EXPECT_EQ(raw->samples[0].accel[1], -1);
    EXPECT_EQ(raw->samples[0].accel[2], 1024);
    EXPECT_EQ(raw->samples[0].gyro[2], -2);
    EXPECT_EQ(raw->samples[0].tempRaw, 25);
    EXPECT_EQ(raw->samples[1].accel[0], 2);
    EXPECT_EQ(raw->samples[1].gyro[2], 7);
    EXPECT_EQ(raw->samples[1].tempRaw, 256);
}

TEST(PacketParser, AcceptsRawBatchWithZeroSamples)
{
    const std::vector<uint8_t> bytes = {0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    ParseResult r = parse(bytes.data(), bytes.size());
    ASSERT_EQ(r.error, ParseError::None);
    const RawPacket* raw = std::get_if<RawPacket>(&*r.packet);
    ASSERT_NE(raw, nullptr);
    EXPECT_TRUE(raw->samples.empty());
}

TEST(PacketParser, RejectsEmptyInput)
{
    EXPECT_EQ(parse(nullptr, 0).error, ParseError::Empty);
    const std::vector<uint8_t> none;
    EXPECT_EQ(parse(none.data(), none.size()).error, ParseError::Empty);
}

TEST(PacketParser, RejectsUnknownType)
{
    const std::vector<uint8_t> bytes = {0x7F, 0x00, 0x00};
    EXPECT_EQ(parse(bytes.data(), bytes.size()).error, ParseError::UnknownType);
}

TEST(PacketParser, RejectsTruncatedStatus)
{
    const std::vector<uint8_t> bytes = {0x01, 0x04, 0x00, 0x04, 0x10, 0x12, 0xF6, 0xFF, 0x88, 0x13, 0x00};  // 11 bytes, needs 12
    ParseResult r = parse(bytes.data(), bytes.size());
    EXPECT_EQ(r.error, ParseError::BadLength);
    EXPECT_FALSE(r.packet.has_value());
}

TEST(PacketParser, RejectsRawWhenSampleCountDoesNotMatchLength)
{
    // header claims 2 samples but only 1 sample's bytes follow
    std::vector<uint8_t> bytes = {0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    bytes.insert(bytes.end(), 14, 0x00);
    EXPECT_EQ(parse(bytes.data(), bytes.size()).error, ParseError::BadLength);
}

TEST(PacketParser, RejectsRawShorterThanHeader)
{
    const std::vector<uint8_t> bytes = {0x04, 0x01, 0x00};
    EXPECT_EQ(parse(bytes.data(), bytes.size()).error, ParseError::BadLength);
}