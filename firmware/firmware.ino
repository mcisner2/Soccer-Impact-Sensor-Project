// firmware.ino
// Step 7: the session state machine runs everything. Calibration is a placeholder and the
// kick check is a stand-in, both replaced by Edward's modules.

#include "config.h"
#include "imu.h"
#include "sampler.h"
#include "packets.h"
#include "sensorBle.h"
#include "batteryMonitor.h"
#include "gpsModule.h"
#include "sessionController.h"
#include "calibration.h"

Sampler sampler;
BatteryMonitor battery;
GpsModule gps;
SessionController session;
float lastTempC = 0.0f;
uint16_t telemetrySeq = 0;
uint8_t lastPercentSent = 255;  // impossible value so the first reading always goes out

bool rawStreamOn = false;
uint16_t eventSeq = 0;
uint16_t rawSeq = 0;

// stand-in kick tracking: rise above threshold, follow to the peak, emit on the way down
constexpr float standInThresholdG = 5.0f;
bool inSpike = false;
bool eventPending = false;
packets::EventFields pendingEvent;

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
    if (!session.recording())
    {
        inSpike = false;
        return;
    }

    float magnitude = sqrtf(sample.accelG[0] * sample.accelG[0] + sample.accelG[1] * sample.accelG[1] + sample.accelG[2] * sample.accelG[2]);
    float dynamicG = fabsf(magnitude - 1.0f);  // spec 2: orientation-independent
    float gyroMag = sqrtf(sample.gyroDps[0] * sample.gyroDps[0] + sample.gyroDps[1] * sample.gyroDps[1] + sample.gyroDps[2] * sample.gyroDps[2]);

    if (dynamicG > standInThresholdG)
    {
        if (!inSpike)
        {
            inSpike = true;
            pendingEvent = {};
            pendingEvent.eventClass = 1;  // KICK
            pendingEvent.confidence = 50;  // meaningless until the real detector exists
        }

        uint16_t accelCg = (uint16_t)min(dynamicG * 100.0f, 3000.0f);  // spec caps at 30 g
        if (accelCg > pendingEvent.peakAccelCg)
        {
            pendingEvent.peakAccelCg = accelCg;
            pendingEvent.peakGyroDps = (uint16_t)gyroMag;
            pendingEvent.tMs = sample.tMs;
        }
    }
    else if (inSpike)
    {
        inSpike = false;
        pendingEvent.seq = eventSeq++;
        eventPending = true;  // loop() sends it, the hook stays fast
    }
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

    if (eventPending)
    {
        uint8_t buffer[packets::eventSize];
        packets::encodeEvent(pendingEvent, buffer);
        if (sensorBle::sendEvent(buffer, sizeof(buffer)))
        {
            eventsSent++;
        }
        eventPending = false;  // a missed event shows up as a seq gap on the webpage
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
    }

    // 1 ms, not 10: the polling driver has to be called at least 220 times a second or it misses samples.
    // Once the FIFO driver exists, this can go back up.
    delay(1);
}
