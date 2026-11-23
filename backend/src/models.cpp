// models.cpp

#include "models.h"

#include <cstdio>

namespace session
{
    namespace
    {
        // Error / warning codes (Interface Specification, section 8): code -> (name, dashboard message)
        struct ErrorEntry
        {
            uint8_t code;
            const char* name;
            const char* message;
        };

        const ErrorEntry errorTable[] = {
            {0x01, "NOT_STILL_ACCEL", "Stand still while the sensor calibrates"},
            {0x02, "NOT_STILL_GYRO", "Stand still while the sensor calibrates"},
            {0x03, "GYRO_BIAS_OUT_OF_RANGE", "Sensor fault"},
            {0x04, "GRAVITY_OUT_OF_RANGE", "Sensor fault"},
            {0x10, "ERR_CAL_FAILED", "Calibration failed - try recalibrating"},
            {0x11, "ERR_NOT_CALIBRATED", "Calibrate the sensor before starting a session"},
            {0x20, "ERR_IMU_COMM", "Sensor communication problem - data may have gaps"},
            {0x21, "ERR_IMU_FIFO_OVERFLOW", "Sensor data gap in the timeline"},
            {0x30, "WARN_GPS_NO_FIX", "No GPS fix - distance and speed are low-confidence"},
            {0x31, "WARN_GPS_POOR_ACCURACY", "GPS accuracy is poor - distance and speed are low-confidence"},
            {0x40, "WARN_CAL_STALE", "Sensor has drifted - recalibration suggested"},
            {0x50, "WARN_LOW_BATTERY", "Low battery"},
            {0x51, "ERR_BATTERY_CRITICAL", "Session ended - battery critical"},
            {0x60, "WARN_NO_EVENTS", "No events detected for 10 minutes - check sensor mounting"},
        };

        const ErrorEntry* findError(uint8_t code)
        {
            for (const ErrorEntry& entry : errorTable)
            {
                if (entry.code == code) return &entry;
            }
            return nullptr;
        }

        std::string hexCode(const char* prefix, uint8_t code)
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s0x%02x", prefix, static_cast<unsigned>(code));
            return buf;
        }
    }

    std::string stateName(uint8_t state)
    {
        switch (state)
        {
            case 0: return "BOOT";
            case 1: return "SENSOR_INIT";
            case 2: return "CALIBRATING";
            case 3: return "READY";
            case 4: return "RECORDING";
            case 5: return "STOPPED";
            case 6: return "ERROR";
            default: return "STATE_" + std::to_string(state);
        }
    }

    std::string errorName(uint8_t errorCode)
    {
        if (errorCode == 0) return "NONE";
        if (const ErrorEntry* entry = findError(errorCode)) return entry->name;
        return hexCode("ERROR_", errorCode);
    }

    std::optional<std::string> errorMessage(uint8_t errorCode)
    {
        if (errorCode == 0) return std::nullopt;
        if (const ErrorEntry* entry = findError(errorCode)) return std::string(entry->message);
        return hexCode("Sensor reported error ", errorCode);
    }

    std::string eventClassName(uint8_t eventClass)
    {
        switch (eventClass)
        {
            case 0: return "UNKNOWN";
            case 1: return "KICK";
            case 2: return "PASS";
            case 3: return "TRAP";
            case 4: return "GROUND_IMPACT";
            default: return "CLASS_" + std::to_string(eventClass);
        }
    }

    std::string fixName(uint8_t fixQuality)
    {
        switch (fixQuality)
        {
            case 0: return "none";
            case 1: return "GPS";
            case 2: return "DGPS/WAAS";
            default: return "FIX_" + std::to_string(fixQuality);
        }
    }

    double temperatureC(const parser::StatusPacket& s) { return s.tempDc / 10.0; }
    bool calibrationStale(const parser::StatusPacket& s) { return (s.flags & 0x01) != 0; }
    bool gpsDegraded(const parser::StatusPacket& s) { return (s.flags & 0x02) != 0; }
    bool lowBattery(const parser::StatusPacket& s) { return (s.flags & 0x04) != 0; }
    bool rawStreamOn(const parser::StatusPacket& s) { return (s.flags & 0x08) != 0; }

    double peakAccelG(const parser::EventPacket& e) { return e.peakAccelCg / 100.0; }

    bool hasFix(const parser::TelemetryPacket& t) { return t.fixQuality > 0; }
    double latitude(const parser::TelemetryPacket& t) { return t.lat1e7 / 1e7; }
    double longitude(const parser::TelemetryPacket& t) { return t.lon1e7 / 1e7; }
    double speedMS(const parser::TelemetryPacket& t) { return t.speedCms / 100.0; }
    double hdop(const parser::TelemetryPacket& t) { return t.hdopX10 / 10.0; }

    uint32_t Session::missedPacketCount() const
    {
        uint32_t total = 0;
        for (uint32_t missed : missedByStream) total += missed;
        return total;
    }
}
