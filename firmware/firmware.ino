// firmware.ino
// Step 7: the session state machine runs everything. Calibration is still a placeholder, replaced
// by Edward's module later. The peak detector is Edward's (detector.h / detector.cpp).

#include "config.h"
#include "imu.h"
#include "sampler.h"
#include "packets.h"
#include "sensorBle.h"
#include "batteryMonitor.h"
#include "gpsModule.h"
#include "sessionController.h"
#include "calibration.h"
#include "detector.h"

Sampler sampler;
BatteryMonitor battery;
GpsModule gps;
SessionController session;
Detector detector;  // peak detector (spec 5.2): about 15 KB of fixed memory, so it lives here and not on the stack
float lastTempC = 0.0f;
uint16_t telemetrySeq = 0;
uint8_t lastPercentSent = 255;  // impossible value so the first reading always goes out

bool rawStreamOn = false;
uint16_t eventSeq = 0;
uint16_t rawSeq = 0;

// recording edges for the detector: reset() when a recording starts, flush() when it ends
bool wasRecording = false;

// raw packet being built; kept until a send succeeds so a full BLE buffer loses nothing
uint8_t rawPacket[packets::rawHeaderSize + config::rawMaxSamplesPerPacket * packets::rawSampleSize];
size_t rawCount = 0;
uint32_t rawFirstMs = 0;

uint32_t lastStatusMs = 0;
uint32_t eventsSent = 0;
uint32_t rawPacketsSent = 0;
uint32_t rawSendRetries = 0;

void onImuSample(const ImuSample& sample)
{
    lastTempC = sample.tempC;
    session.onImuSample(sample);

    // detector only runs while recording, spec 7
    bool recording = session.recording();
    if (recording && !wasRecording)
    {
        detector.reset();  // new recording: forget the last session's threshold history
    }
    else if (!recording && wasRecording)
    {
        detector.flush();  // recording just ended: decide the peaks still pending, loop() sends them
    }
    wasRecording = recording;

    if (!recording)
    {
        return;
    }

    // Calibration is still the placeholder, so the gyro bias is not removed yet. When Edward's calibration
    // goes in, the sample goes through calibration::apply() first and the detector gets the result.
    detector.feed(sample);  // the hook stays fast: events come out through popEvent() in loop()
}

void handleCommands()
{
    ControlCommand command;
    while (sensorBle::popCommand(command))
    {
        uint8_t result = 0;

        if (command.cmd == packets::cmdRawStreamOn)
        {
            rawStreamOn = true;
        }
        else if (command.cmd == packets::cmdRawStreamOff)
        {
            rawStreamOn = false;
        }
        else
        {
            result = session.handleCommand(command.cmd);
        }

        Serial.printf("command 0x%02X -> %s 0x%02X\n", command.cmd, result ? "rejected" : "ok", result);
    }
}

void serviceRawStream()
{
    size_t maxSamples = min(sensorBle::maxRawSamplesPerPacket(), config::rawMaxSamplesPerPacket);

    if (!rawStreamOn || maxSamples == 0 || !sensorBle::isConnected())
    {
        // nobody is listening, so throw samples away instead of letting the buffer report drops
        StampedRawSample discard;
        while (sampler.popRaw(discard))
        {
        }
        rawCount = 0;
        return;
    }

    // fill until full; a packet already full is waiting on a retry
    StampedRawSample stamped;
    while (rawCount < maxSamples && sampler.popRaw(stamped))
    {
        if (rawCount == 0)
        {
            rawFirstMs = stamped.tMs;
        }
        packets::encodeRawSample(stamped.raw, rawPacket + packets::rawHeaderSize + rawCount * packets::rawSampleSize);
        rawCount++;
    }

    if (rawCount < maxSamples)
    {
        return;
    }

    packets::encodeRawHeader((uint8_t)rawCount, rawSeq, rawFirstMs, rawPacket);
    size_t length = packets::rawHeaderSize + rawCount * packets::rawSampleSize;

    if (sensorBle::sendRaw(rawPacket, length))
    {
        rawSeq++;
        rawPacketsSent++;
        rawCount = 0;
    }
    else
    {
        rawSendRetries++;  // try the same packet next loop
    }
}

void sendStatus(uint32_t nowMs)
{
    packets::StatusFields status = {};
    status.state = (uint8_t)session.state();
    status.batteryMv = (uint16_t)battery.packMv();
    status.tempDc = (int16_t)(lastTempC * 10.0f);
    status.tMs = nowMs;

    status.flags = rawStreamOn ? packets::flagRawStreamOn : 0;
    if (gps.degraded())
    {
        status.flags |= packets::flagGpsDegraded;
    }
    if (battery.level() != BatteryLevel::ok)
    {
        status.flags |= packets::flagLowBattery;
    }

    // one code slot, so the most serious thing wins: session error, then battery, then GPS
    status.errorCode = session.errorCode();
    if (status.errorCode == 0 && battery.level() == BatteryLevel::warning)
    {
        status.errorCode = 0x50;  // WARN_LOW_BATTERY
    }
    if (status.errorCode == 0 && gps.degraded())
    {
        status.errorCode = 0x30;  // WARN_GPS_NO_FIX
    }

    uint8_t buffer[packets::statusSize];
    packets::encodeStatus(status, buffer);
    sensorBle::sendStatus(buffer, sizeof(buffer));
}

void setup()
{
    Serial.begin(115200);

    // wait for USB monitor, but don't hang on battery
    uint32_t waitStartMs = millis();
    while (!Serial && millis() - waitStartMs < 2000)
    {
    }

    bool imuOk = getImu().begin();
    if (!sensorBle::begin())
    {
        Serial.println("ble begin failed");
    }

    battery.begin();
    gps.begin();
    sampler.begin(getImu(), onImuSample);
    session.begin(sampler, imuOk);  // BOOT -> SENSOR_INIT -> CALIBRATING
    Serial.println("advertising as Group3Sensor");
}

void loop()
{
    sampler.service();
    handleCommands();
    serviceRawStream();
    gps.service();
    session.service(battery.level());

    // telemetry only while recording (spec 7); before that, GPS state reaches the dashboard via Status flags
    if (gps.takeNewFix() && session.recording())
    {
        const GpsFix& fix = gps.fix();
        packets::TelemetryFields telemetry = {};
        telemetry.fixQuality = fix.rmcValid ? fix.fixQuality : 0;
        telemetry.seq = telemetrySeq++;
        telemetry.tMs = millis();  // ESP32 clock until PPS alignment exists (spec 9)
        telemetry.lat1e7 = fix.lat1e7;
        telemetry.lon1e7 = fix.lon1e7;
        telemetry.speedCms = fix.speedCms;
        telemetry.satellites = fix.satellites;
        telemetry.hdopX10 = fix.hdopX10;

        uint8_t buffer[packets::telemetrySize];
        packets::encodeTelemetry(telemetry, buffer);
        sensorBle::sendTelemetry(buffer, sizeof(buffer));
    }

    // The detector settles each event about 150 ms after the impact and queues it (up to 16), so drain the queue here
    DetectedEvent detected;
    while (detector.popEvent(detected))
    {
        packets::EventFields eventFields = {};
        eventFields.eventClass = (uint8_t)detected.eventClass;
        eventFields.seq = eventSeq++;
        eventFields.tMs = detected.tMs;
        eventFields.peakAccelCg = (uint16_t)min(detected.peakAccelG * 100.0f + 0.5f, 3000.0f);  // spec caps at 30 g
        eventFields.peakGyroDps = (uint16_t)min(detected.peakGyroDps + 0.5f, 65535.0f);
        eventFields.confidence = (uint8_t)min(detected.confidence * 100.0f + 0.5f, 100.0f);  // detector gives 0 to 1, the packet wants 0 to 100

        uint8_t buffer[packets::eventSize];
        packets::encodeEvent(eventFields, buffer);
        if (sensorBle::sendEvent(buffer, sizeof(buffer)))
        {
            eventsSent++;
        }
        // a missed event shows up as a seq gap on the webpage

        Serial.printf("event #%u class %u | %.1f g | %.0f dps | conf %u | t %lu ms\n",
            (unsigned)eventFields.seq,
            (unsigned)eventFields.eventClass,
            detected.peakAccelG,
            detected.peakGyroDps,
            (unsigned)eventFields.confidence,
            (unsigned long)eventFields.tMs);
    }

    uint32_t nowMs = millis();

    // spec 6.1: Status at 1 Hz and immediately on any state change
    if (session.takeStateChanged())
    {
        sendStatus(nowMs);
        Serial.printf("session state %u, code 0x%02X\n", (unsigned)session.state(), session.errorCode());

        // sanity check on real data: at rest, gravity should be about 1 g and gyro bias within +-5 dps
        if (session.state() == SessionState::ready)
        {
            CalResult cal = calibration::result();
            float g = sqrtf(cal.gravityG[0] * cal.gravityG[0] + cal.gravityG[1] * cal.gravityG[1] + cal.gravityG[2] * cal.gravityG[2]);
            Serial.printf("cal gravity %.3f %.3f %.3f g (|g| %.3f) | gyro bias %.2f %.2f %.2f dps | temp %.1f C\n",
                cal.gravityG[0], cal.gravityG[1], cal.gravityG[2], g,
                cal.gyroBiasDps[0], cal.gyroBiasDps[1], cal.gyroBiasDps[2], cal.tempAtCalC);
        }
    }

    if (nowMs - lastStatusMs >= 1000)
    {
        lastStatusMs = nowMs;
        battery.update();

        // Battery Level is a standard characteristic, so only notify when the number actually changes
        if (battery.percent() != lastPercentSent)
        {
            lastPercentSent = battery.percent();
            sensorBle::setBatteryPercent(lastPercentSent);
        }

        sendStatus(nowMs);
        Serial.printf("gps %s q%u sats %u hdop %u lat %ld lon %ld %u cm/s\n",
            gps.stateName(), gps.fix().fixQuality, gps.fix().satellites, gps.fix().hdopX10,
            (long)gps.fix().lat1e7, (long)gps.fix().lon1e7, gps.fix().speedCms);
        Serial.printf("batt %lu mV %u%% lvl %d | conn %d | mtu %u | raw fits %u | events %lu | raw pkts %lu | retries %lu | dropped %lu | imu missed %lu | bad writes %lu\n",
            (unsigned long)battery.packMv(),
            (unsigned)battery.percent(),
            (int)battery.level(),
            sensorBle::isConnected(),
            (unsigned)sensorBle::mtu(),
            (unsigned)sensorBle::maxRawSamplesPerPacket(),
            (unsigned long)eventsSent,
            (unsigned long)rawPacketsSent,
            (unsigned long)rawSendRetries,
            (unsigned long)sampler.droppedCount(),
            (unsigned long)getImu().missedSamples(),
            (unsigned long)sensorBle::badWriteCount());
        Serial.printf("detector threshold %.1f g | queue dropped %lu | forced %lu\n",
            detector.currentThresholdG(),
            (unsigned long)detector.droppedEvents(),
            (unsigned long)detector.forcedResolves());
    }

    // 1 ms, not 10: the polling driver has to be called at least 220 times a second or it misses samples.
    // Once the FIFO driver exists, this can go back up.
    delay(1);
}