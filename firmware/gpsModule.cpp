// gpsModule.cpp

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gpsModule.h"
#include "config.h"
#include "pins.h"

void GpsModule::begin()
{
    bootMs = millis();
#if USE_MOCK_GPS
    enterState(State::running);
#else
    pinMode(pins::gpsEnable, OUTPUT);
    digitalWrite(pins::gpsEnable, HIGH);

    Serial1.setRxBufferSize(512);  // must come before begin(); a burst of sentences can outrun the default
    Serial1.begin(config::gpsFastBaud, SERIAL_8N1, pins::gpsRx, pins::gpsTx);
    enterState(State::probeFast);
#endif
}

void GpsModule::enterState(State next)
{
    state = next;
    stateStartMs = millis();
    heardValidLine = false;
}

void GpsModule::service()
{
#if USE_MOCK_GPS
    serviceMock();
#else
    readSerial();
    uint32_t inState = millis() - stateStartMs;

    switch (state)
    {
        case State::probeFast:
            if (heardValidLine)
            {
                commandIndex = 0;
                enterState(State::configuring);  // already fast, still resend config: it may have rebooted too
            }
            else if (inState > config::gpsProbeMs)
            {
                Serial1.updateBaudRate(config::gpsBootBaud);
                enterState(State::probeSlow);
            }
            break;

        case State::probeSlow:
            if (heardValidLine)
            {
                // ask for the faster speed, then follow it. flush() waits until the bytes are actually out.
                char body[24];
                snprintf(body, sizeof(body), "PMTK251,%lu", (unsigned long)config::gpsFastBaud);
                sendBody(body);
                Serial1.flush();
                Serial1.updateBaudRate(config::gpsFastBaud);
                commandIndex = 0;
                enterState(State::configuring);
            }
            else if (inState > config::gpsProbeMs)
            {
                enterState(State::missing);
            }
            break;

        case State::configuring:
            if ((int32_t)(millis() - nextCommandMs) >= 0)
            {
                nextCommandMs = millis() + config::gpsCommandGapMs;
                if (!sendNextConfigCommand())
                {
                    enterState(State::running);
                }
            }
            break;

        case State::missing:
            // nothing wired, or EN held low. Try the whole sequence again every few seconds.
            if (inState > 5000)
            {
                Serial1.updateBaudRate(config::gpsFastBaud);
                enterState(State::probeFast);
            }
            break;

        case State::running:
            break;
    }
#endif
}

bool GpsModule::sendNextConfigCommand()
{
    uint32_t periodMs = 1000 / config::gpsFixHz;
    char body[48];

    switch (commandIndex++)
    {
        case 0:
            sendBody("PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");  // RMC + GGA only, spec 3.2
            return true;
        case 1:
            snprintf(body, sizeof(body), "PMTK220,%lu", (unsigned long)periodMs);  // how often sentences go out
            sendBody(body);
            return true;
        case 2:
            snprintf(body, sizeof(body), "PMTK300,%lu,0,0,0,0", (unsigned long)periodMs);  // how often it actually solves
            sendBody(body);
            return true;
        case 3:
            if (config::gpsFixHz == 1)
            {
                sendBody("PMTK313,1");  // search for SBAS satellites
                return true;
            }
            commandIndex = 5;  // skip the SBAS pair at faster rates
            return false;
        case 4:
            sendBody("PMTK301,2");  // use WAAS corrections
            return true;
        default:
            return false;
    }
}

void GpsModule::sendBody(const char* body)
{
    char sentence[64];
    size_t length = gpsParser::buildSentence(body, sentence, sizeof(sentence));
    if (length > 0)
    {
        Serial1.write((const uint8_t*)sentence, length);
    }
}

void GpsModule::readSerial()
{
    while (Serial1.available())
    {
        char c = (char)Serial1.read();

        if (c == '\n' || c == '\r')
        {
            if (lineLength > 0)
            {
                lineBuffer[lineLength] = '\0';
                handleLine(lineBuffer);
                lineLength = 0;
            }
        }
        else if (lineLength < sizeof(lineBuffer) - 1)
        {
            lineBuffer[lineLength++] = c;
        }
        else
        {
            lineLength = 0;  // overlong line is garbage, likely the wrong baud rate
        }
    }
}

void GpsModule::handleLine(const char* line)
{
    if (!gpsParser::checksumOk(line))
    {
        return;  // at the wrong baud rate every line fails here, which is how probing tells speeds apart
    }
    heardValidLine = true;

    if (gpsParser::parseLine(line, currentFix))
    {
        newFix = true;
    }
}

bool GpsModule::takeNewFix()
{
    bool had = newFix;
    newFix = false;
    return had;
}

bool GpsModule::degraded() const
{
    bool noFixYet = !currentFix.rmcValid && millis() - bootMs > config::gpsNoFixTimeoutMs;
    bool poorGeometry = currentFix.rmcValid && currentFix.hdopX10 > config::gpsMaxGoodHdopX10;
    return noFixYet || poorGeometry || state == State::missing;
}

const char* GpsModule::stateName() const
{
    switch (state)
    {
        case State::probeFast:
            return "probe57600";
        case State::probeSlow:
            return "probe9600";
        case State::configuring:
            return "config";
        case State::running:
            return currentFix.rmcValid ? "fix" : "searching";
        default:
            return "missing";
    }
}

void GpsModule::serviceMock()
{
    // Writes real NMEA text into the same handler the UART uses, so the parser gets exercised too.
    // About 34 s of no fix (the cold start figure), then a jog in a 20 m circle at 3 m/s. Location is made up.
    static uint32_t lastEpochMs = 0;
    uint32_t nowMs = millis();
    uint32_t periodMs = 1000 / config::gpsFixHz;
    if (nowMs - lastEpochMs < periodMs)
    {
        return;
    }
    lastEpochMs = nowMs;

    float secondsUp = (nowMs - bootMs) / 1000.0f;
    bool hasFix = secondsUp > 34.0f;

    const double centerLat = 41.6990, centerLon = -86.2340;
    const double radiusM = 20.0, speedMs = 3.0;
    double angle = speedMs / radiusM * secondsUp;
    double lat = centerLat + radiusM * sin(angle) / 111320.0;
    double lon = centerLon + radiusM * cos(angle) / (111320.0 * cos(centerLat * M_PI / 180.0));

    char latText[16], lonText[16];
    snprintf(latText, sizeof(latText), "%02d%07.4f", (int)lat, (lat - (int)lat) * 60.0);
    double absLon = fabs(lon);
    snprintf(lonText, sizeof(lonText), "%03d%07.4f", (int)absLon, (absLon - (int)absLon) * 60.0);

    char body[90], sentence[100];
    if (hasFix)
    {
        snprintf(body, sizeof(body), "GPGGA,120000.000,%s,N,%s,W,1,07,1.2,220.0,M,-33.0,M,,", latText, lonText);
        gpsParser::buildSentence(body, sentence, sizeof(sentence));
        handleLine(strtok(sentence, "\r"));

        snprintf(body, sizeof(body), "GPRMC,120000.000,A,%s,N,%s,W,%.2f,0.00,041026,,,A", latText, lonText, speedMs * 1.94384);
    }
    else
    {
        snprintf(body, sizeof(body), "GPGGA,120000.000,,,,,0,00,,,M,,M,,");
        gpsParser::buildSentence(body, sentence, sizeof(sentence));
        handleLine(strtok(sentence, "\r"));

        snprintf(body, sizeof(body), "GPRMC,120000.000,V,,,,,0.00,0.00,041026,,,N");
    }
    gpsParser::buildSentence(body, sentence, sizeof(sentence));
    handleLine(strtok(sentence, "\r"));
}
