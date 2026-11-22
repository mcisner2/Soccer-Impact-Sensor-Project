// detector.h
// Peak detector (spec 5.2). Streaming C++ port of detect_events() in peak_detection_prototype.py.
// Feed it one calibrated sample at a time (gyro bias already removed, see calibration::apply()).
// Fixed memory, no heap, no Arduino calls, so it also builds on a laptop for host tests.
//
// Classes (decision table, a = peak dynamic acceleration, g = largest gyro magnitude from 150 ms before to 50 ms after):
//                     g >= kick gyro   pass gyro <= g < kick gyro   g < pass gyro
//   a >= 5 g          KICK             PASS                         HARD_IMPACT
//   soft floor <= a   PASS             PASS                         TRAP
//   < 5 g
// With classifyPassTrap off it is the original rule: g >= kick gyro = KICK, otherwise HARD_IMPACT, floor 5 g.
//
// How it differs from the Python on purpose:
//  - The threshold is causal: each 1 s block uses the 5 s that ended before the block started. (The prototype's
//    batch version also looked at the block itself. On the synthetic sessions both gave identical events.)
//  - An event comes out about one refractory period (150 ms) after the impact, not 50 ms as spec 5.2 says,
//    because "highest peak within the refractory period wins" cannot be decided any sooner.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sampler.h"
#include "config.h"

enum class EventClass : uint8_t
{
    unknown = 0,
    kick = 1,        // spec 6.3 codes
    hardImpact = 2,
    pass = 3,        // controlled strike: swing between the pass and kick gyro limits, or any swing with a soft contact
    trap = 4         // soft contact with almost no swing: the leg absorbing a ball
};

struct DetectedEvent
{
    EventClass eventClass = EventClass::unknown;
    uint32_t tMs = 0;          // timestamp of the impact sample
    float peakAccelG = 0.0f;   // peak dynamic acceleration, | |a| - 1 g |
    float peakGyroDps = 0.0f;  // largest |gyro| in the window around the impact
    float confidence = 0.0f;   // 0 to 1. Multiply by 100 for the spec's u8
    float thresholdG = 0.0f;   // threshold that was in force at the impact
};

struct DetectorParams
{
    uint32_t sampleRateHz;
    float kMad;
    float minPeakG;
    float kickGyroDps;
    bool classifyPassTrap;   // false = only KICK / HARD_IMPACT, same as the Python prototype
    float softMinPeakG;      // threshold floor used instead of minPeakG when classifyPassTrap is on (never above minPeakG)
    float passGyroDps;
    uint32_t refractoryMs;
    uint32_t gyroBeforeMs;
    uint32_t gyroAfterMs;
    uint32_t baselineWindowMs;
    uint32_t baselineHopMs;

    static DetectorParams fromConfig();  // everything from config.h, sample rate = config::imuOdrHz
};

class Detector
{
    public:
        // Capacities. The constructor clamps the parameters to these.
        static constexpr size_t maxBaselineSamples = 1280;  // 5 s at up to 256 Hz
        static constexpr size_t historySamples = 512;       // gyro/time history, 2.3 s at 220 Hz
        static constexpr size_t maxClusterPeaks = 16;       // peaks closer together than the refractory period
        static constexpr size_t eventQueueSize = 16;

        explicit Detector(const DetectorParams& params = DetectorParams::fromConfig());

        void reset();                    // new session: forgets everything, including the threshold history
        void feed(const ImuSample& s);   // once per sample, in order. Events appear via popEvent()
        void flush();                    // end of session: decide what is still pending, using the data seen so far

        bool popEvent(DetectedEvent& out);
        size_t eventsWaiting() const { return queueCount; }

        uint32_t droppedEvents() const { return dropped; }          // queue was full, caller is not draining it
        uint32_t forcedResolves() const { return forced; }          // a run of peaks got too long and was decided early
        float currentThresholdG() const { return thrCur; }

    private:
        struct Peak
        {
            uint32_t index;
            float height;
            float threshold;
        };

        void updateThreshold(uint32_t idx);
        float thresholdAt(uint32_t peakIndex) const;
        void onPeak(uint32_t peakIndex, float height);
        void resolveCluster(uint32_t lastIdx);
        void pushEvent(const DetectedEvent& e);

        DetectorParams cfg;
        float floorG;  // lowest threshold the detector will use
        uint32_t refractorySamples, gyroBeforeSamples, gyroAfterSamples, windowSamples, hopSamples;

        uint32_t count = 0;  // samples fed since reset(), index of the next sample

        // causal threshold
        float baseline[maxBaselineSamples];
        float scratch[maxBaselineSamples];
        float thrCur = 0.0f, thrPrev = 0.0f;
        bool thrFromData = false, thrPrevFromData = false;  // false until a threshold has been computed from real samples (the first second)
        uint32_t curBlockStart = 0;

        // history for the gyro window and timestamps, indexed by sample number modulo historySamples
        float gyroMag[historySamples];
        uint32_t timeMs[historySamples];

        // local-maximum tracking (same rules as scipy's find_peaks, including flat tops)
        float prev = 0.0f;
        bool canPeak = false;
        uint32_t leftIdx = 0;

        // peaks above the threshold that are still close enough together to compete
        Peak cluster[maxClusterPeaks];
        size_t clusterCount = 0;

        DetectedEvent queue[eventQueueSize];
        size_t queueHead = 0, queueCount = 0;
        uint32_t dropped = 0, forced = 0;
};
