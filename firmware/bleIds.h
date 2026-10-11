// bleIds.h
// Our own UUIDs. Base was generated fresh with uuid4; only the first block changes per characteristic
// so they're easy to tell apart in nRF Connect. Never reuse the public example UUIDs.

#pragma once

namespace bleIds
{
    constexpr const char* service   = "1d390000-dcea-4626-9762-d6dc5fedaa02";
    constexpr const char* control   = "1d390001-dcea-4626-9762-d6dc5fedaa02";
    constexpr const char* status    = "1d390002-dcea-4626-9762-d6dc5fedaa02";
    constexpr const char* event     = "1d390003-dcea-4626-9762-d6dc5fedaa02";
    constexpr const char* telemetry = "1d390004-dcea-4626-9762-d6dc5fedaa02";
    constexpr const char* rawStream = "1d390005-dcea-4626-9762-d6dc5fedaa02";

    // Bluetooth SIG standard Battery Service and Battery Level
    constexpr const char* batteryService = "180F";
    constexpr const char* batteryLevel   = "2A19";
}
