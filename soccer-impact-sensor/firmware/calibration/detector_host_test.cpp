// detector_host_test.cpp
// Tests detector.cpp on a laptop, no ESP32 needed. From the firmware folder:
//   g++ -std=c++17 -Wall -Wextra -I. detector.cpp detector_host_test.cpp -o det_test && ./det_test
// Tests 1 to 14 run in legacy mode (no PASS/TRAP), tests 15 onward check the PASS and TRAP rules with the default settings.
// Samples are built so the dynamic acceleration equals the number you ask for: accel = (0, 0, 1 + g) gives | |a| - 1 | = g.

#include <stdio.h>
#include <math.h>
#include <vector>
#include "detector.h"

namespace
{
    int failures = 0;

    #define CHECK(cond, msg) \
        do { if (cond) { printf("ok:   %s\n", msg); } else { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

    constexpr uint32_t fs = config::imuOdrHz;
    constexpr uint32_t refractory = config::detRefractoryMs * fs / 1000;  // samples

    struct Spike
    {
        uint32_t index;
        float accelG;
        float gyroDps;
        uint32_t gyroOffset;  // gyro spike sits this many samples BEFORE the impact, like a swing
    };

    // Quiet baseline plus spikes. Returns the number of samples fed.
    std::vector<DetectedEvent> run(Detector& det, uint32_t samples, const std::vector<Spike>& spikes, float baseG = 0.0f, bool flush = true)
    {
        std::vector<float> acc(samples, baseG), gyro(samples, 0.0f);
        for (const Spike& s : spikes)
        {
            acc[s.index] = s.accelG;
            gyro[s.index - s.gyroOffset] = s.gyroDps;
        }

        std::vector<DetectedEvent> out;
        for (uint32_t i = 0; i < samples; i++)
        {
            ImuSample sample = {};
            sample.tMs = i * 1000 / fs;
            sample.accelG[2] = 1.0f + acc[i];
            sample.gyroDps[1] = gyro[i];
            det.feed(sample);
            DetectedEvent e;
            while (det.popEvent(e)) out.push_back(e);
        }
        if (flush)
        {
            det.flush();
            DetectedEvent e;
            while (det.popEvent(e)) out.push_back(e);
        }
        return out;
    }
}

int main()
{
    const uint32_t kickAt = 6 * fs;

    // tests 1 to 14 check the original KICK / HARD_IMPACT rule, the same one the Python prototype has
    DetectorParams legacy = DetectorParams::fromConfig();
    legacy.classifyPassTrap = false;
    DetectorParams soft = DetectorParams::fromConfig();  // PASS/TRAP on with the soft floor lowered, so TRAP can happen at all
    soft.softMinPeakG = 2.5f;

    // 1. a clean kick and a clean hard impact
    {
        Detector det(legacy);
        auto ev = run(det, 10 * fs, { { kickAt, 12.0f, 1200.0f, 10 }, { 8 * fs, 7.0f, 100.0f, 10 } });
        CHECK(ev.size() == 2, "two spikes give two events");
        CHECK(ev.size() == 2 && ev[0].eventClass == EventClass::kick && ev[1].eventClass == EventClass::hardImpact,
              "big gyro swing = KICK, small = HARD_IMPACT");
        CHECK(ev.size() == 2 && ev[0].tMs == kickAt * 1000 / fs, "event time is the impact sample's time");
        CHECK(ev.size() == 2 && fabsf(ev[0].peakAccelG - 12.0f) < 1e-3f && fabsf(ev[0].peakGyroDps - 1200.0f) < 1e-2f,
              "peak accel and gyro reported");
        CHECK(ev.size() == 2 && ev[0].confidence > 0.0f && ev[0].confidence <= 1.0f && ev[1].confidence >= 0.0f, "confidence in 0..1");
    }

    // 2. gyro gate edges: 649 dps is not a kick, 650 is (>=)
    {
        Detector det(legacy);
        auto ev = run(det, 10 * fs, { { kickAt, 12.0f, 649.0f, 10 }, { 8 * fs, 12.0f, 650.0f, 10 } });
        CHECK(ev.size() == 2 && ev[0].eventClass == EventClass::hardImpact && ev[1].eventClass == EventClass::kick, "gate is gyro >= 650");
    }

    // 3. gyro window: the swing counts from 150 ms before the impact to 50 ms after, not outside
    {
        Detector det(legacy);
        uint32_t before = config::detGyroBeforeMs * fs / 1000;
        auto ev = run(det, 10 * fs, { { kickAt, 12.0f, 1200.0f, before }, { 8 * fs, 12.0f, 1200.0f, before + 1 } });
        CHECK(ev.size() == 2 && ev[0].eventClass == EventClass::kick && ev[1].eventClass == EventClass::hardImpact,
              "swing exactly at the window start counts, one sample earlier does not");
    }

    // 4. below the threshold is ignored
    {
        Detector det(legacy);
        auto ev = run(det, 10 * fs, { { kickAt, 4.9f, 1200.0f, 10 } });
        CHECK(ev.empty(), "4.9 g peak under the 5 g floor gives no event");
        Detector det2(legacy);
        auto ev2 = run(det2, 10 * fs, { { kickAt, 5.0f, 1200.0f, 10 } });
        CHECK(ev2.size() == 1, "a peak exactly at the threshold counts (height >= threshold)");
    }

    // 5. refractory period: the taller of two close peaks wins, far apart peaks both count
    {
        Detector det(legacy);
        auto ev = run(det, 10 * fs, { { kickAt, 12.0f, 1200.0f, 10 }, { kickAt + refractory - 1, 15.0f, 1200.0f, 10 } });
        CHECK(ev.size() == 1 && fabsf(ev[0].peakAccelG - 15.0f) < 1e-3f, "two peaks closer than the refractory period: only the taller");
        Detector det2(legacy);
        auto ev2 = run(det2, 10 * fs, { { kickAt, 12.0f, 1200.0f, 10 }, { kickAt + refractory, 15.0f, 1200.0f, 10 } });
        CHECK(ev2.size() == 2, "exactly one refractory period apart: both kept (like scipy)");
        Detector det3(legacy);
        auto ev3 = run(det3, 10 * fs, { { kickAt, 15.0f, 1200.0f, 10 }, { kickAt + refractory - 1, 12.0f, 1200.0f, 10 } });
        CHECK(ev3.size() == 1 && fabsf(ev3[0].peakAccelG - 15.0f) < 1e-3f, "the taller wins whichever comes first");
    }

    // 6. a chain 5.5, 6.5, 7.5 each 26 samples apart (less than the 33-sample refractory period):
    //    scipy keeps the tallest, removes the middle one, and the first survives because it is 52 samples from the tallest
    {
        Detector det(legacy);
        uint32_t step = refractory * 8 / 10;
        auto ev = run(det, 10 * fs, { { kickAt, 5.5f, 100.0f, 5 }, { kickAt + step, 6.5f, 100.0f, 5 }, { kickAt + 2 * step, 7.5f, 100.0f, 5 } });
        CHECK(ev.size() == 2 && fabsf(ev[0].peakAccelG - 5.5f) < 1e-3f && fabsf(ev[1].peakAccelG - 7.5f) < 1e-3f,
              "chain: tallest kept, its neighbour removed, the far end survives");
    }

    // 7. flat top: three equal samples count once, at the middle
    {
        Detector det(legacy);
        auto ev = run(det, 10 * fs, { { kickAt, 8.0f, 100.0f, 5 }, { kickAt + 1, 8.0f, 100.0f, 5 }, { kickAt + 2, 8.0f, 100.0f, 5 } });
        CHECK(ev.size() == 1 && ev[0].tMs == (kickAt + 1) * 1000 / fs, "flat top = one event at its middle sample");
    }

    // 8. first and last sample can never be a peak
    {
        Detector det(legacy);
        auto ev = run(det, 10 * fs, { { 0, 9.0f, 0.0f, 0 }, { 10 * fs - 1, 9.0f, 0.0f, 0 } });
        CHECK(ev.empty(), "a high first or last sample is not a peak");
        Detector det2(legacy);
        auto ev2 = run(det2, 10 * fs, { { 1, 9.0f, 0.0f, 1 }, { 10 * fs - 2, 9.0f, 0.0f, 1 } });
        CHECK(ev2.size() == 2, "samples 1 and n-2 can be peaks (flush decides the last one)");
    }

    // 9. when does the event appear? One refractory period after the impact, never earlier, and flush() forces it sooner
    {
        Detector det(legacy);
        std::vector<float> dummy;
        uint32_t appeared = 0;
        for (uint32_t i = 0; i < 10 * fs && appeared == 0; i++)
        {
            ImuSample s = {};
            s.tMs = i * 1000 / fs;
            s.accelG[2] = 1.0f + (i == kickAt ? 12.0f : 0.0f);
            det.feed(s);
            if (det.eventsWaiting() > 0) appeared = i;
        }
        CHECK(appeared == kickAt + refractory, "event appears exactly one refractory period (150 ms) after the impact sample");

        Detector det2(legacy);
        for (uint32_t i = 0; i <= kickAt + 5; i++)
        {
            ImuSample s = {};
            s.accelG[2] = 1.0f + (i == kickAt ? 12.0f : 0.0f);
            det2.feed(s);
        }
        CHECK(det2.eventsWaiting() == 0, "not decided yet 5 samples after the impact");
        det2.flush();
        CHECK(det2.eventsWaiting() == 1, "flush() decides it with the data available");
    }

    // 10. the adaptive threshold: a steady 2 g vibration with some spread raises it above 5 g, and then a 6 g peak is ignored
    {
        Detector det(legacy);
        std::vector<Spike> none;
        uint32_t n = 12 * fs;
        std::vector<DetectedEvent> ev;
        // alternating 0.5 g / 3.5 g: median 2 g, every deviation 1.5 g, so MAD = 1.4826 * 1.5 and the threshold is about 2 + 8 * 2.2 = 20 g
        for (uint32_t i = 0; i < n; i++)
        {
            ImuSample s = {};
            s.tMs = i * 1000 / fs;
            float dyn = (i % 2 == 0) ? 0.5f : 3.5f;
            if (i == 8 * fs + 1) dyn = 6.0f;
            s.accelG[2] = 1.0f + dyn;
            det.feed(s);
        }
        det.flush();
        DetectedEvent e;
        int events = 0;
        while (det.popEvent(e)) events++;
        CHECK(det.currentThresholdG() > 10.0f, "threshold rises above the 5 g floor in a vibrating baseline");
        CHECK(events == 0, "so a 6 g peak in that baseline is not an event");
    }

    // 11. the threshold is causal: a loud first second cannot raise its own threshold, only the blocks after it
    {
        Detector det(legacy);
        for (uint32_t i = 0; i < fs; i++)  // exactly the first 1 s block (samples 0 to fs-1), loud chatter
        {
            ImuSample s = {};
            s.accelG[2] = 1.0f + ((i % 2 == 0) ? 0.5f : 3.5f);
            det.feed(s);
        }
        CHECK(det.currentThresholdG() == config::detMinPeakG, "first block: no history yet, threshold stays at the floor");
        ImuSample s = {};
        s.accelG[2] = 1.0f;
        det.feed(s);  // first sample of block 2: now the loud block counts
        CHECK(det.currentThresholdG() > 10.0f, "second block: threshold built from the loud first block");
    }

    // 12. reset() starts clean
    {
        Detector det(legacy);
        run(det, 4 * fs, { { 2 * fs, 12.0f, 1200.0f, 10 } }, 2.0f, false);
        det.reset();
        CHECK(det.currentThresholdG() == config::detMinPeakG && det.eventsWaiting() == 0 && det.droppedEvents() == 0,
              "reset() clears threshold, pending events and counters");
        auto ev = run(det, 10 * fs, { { kickAt, 12.0f, 1200.0f, 10 } });
        CHECK(ev.size() == 1, "works normally after reset()");
    }

    // 13. a full queue drops and counts instead of overwriting or crashing
    {
        Detector det(legacy);
        std::vector<Spike> many;
        for (uint32_t k = 0; k < 20; k++) many.push_back({ 2 * fs + k * fs / 2, 9.0f, 1000.0f, 5 });
        std::vector<float> acc(14 * fs, 0.0f);
        for (const Spike& s : many) acc[s.index] = s.accelG;
        for (uint32_t i = 0; i < acc.size(); i++)
        {
            ImuSample s = {};
            s.accelG[2] = 1.0f + acc[i];
            det.feed(s);  // never popped
        }
        det.flush();
        CHECK(det.eventsWaiting() == Detector::eventQueueSize && det.droppedEvents() == 20 - Detector::eventQueueSize,
              "20 events with nobody reading: 16 kept, 4 counted as dropped");
    }

    // 14. a long run of peaks every 10 samples (vibration) never crashes and is decided early rather than growing forever
    {
        Detector det(legacy);
        uint32_t n = 20 * fs;
        uint32_t events = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            ImuSample s = {};
            s.tMs = i * 1000 / fs;
            s.accelG[2] = 1.0f + ((i % 10 == 0) ? 8.0f + (float)(i % 7) * 0.1f : 0.0f);
            det.feed(s);
            DetectedEvent e;
            while (det.popEvent(e)) events++;
        }
        CHECK(events > 0 && det.forcedResolves() > 0, "dense peaks: events still come out, run is decided early");
    }


    // 15. the PASS / TRAP decision table. a = 12 g is a strong contact, 3.5 g a soft one. Gyro 1200 = hard swing, 400 = medium, 100 = almost none
    {
        struct Cell { float a; float g; EventClass expect; const char* name; };
        const Cell cells[] = {
            { 12.0f, 1200.0f, EventClass::kick,       "strong contact + hard swing = KICK" },
            { 12.0f,  400.0f, EventClass::pass,       "strong contact + medium swing = PASS" },
            { 12.0f,  100.0f, EventClass::hardImpact, "strong contact + no swing = HARD_IMPACT" },
            {  3.5f, 1200.0f, EventClass::pass,       "soft contact + hard swing = PASS" },
            {  3.5f,  400.0f, EventClass::pass,       "soft contact + medium swing = PASS" },
            {  3.5f,  100.0f, EventClass::trap,       "soft contact + no swing = TRAP" },
        };
        for (const Cell& cell : cells)
        {
            Detector det(soft);
            auto ev = run(det, 10 * fs, { { kickAt, cell.a, cell.g, 10 } });
            CHECK(ev.size() == 1 && ev[0].eventClass == cell.expect, cell.name);
        }
    }

    // 16. the edges of that table
    {
        auto classify = [&](float a, float g, bool& found) {
            Detector det(soft);
            auto ev = run(det, 10 * fs, { { kickAt, a, g, 10 } });
            found = ev.size() == 1;
            return found ? ev[0].eventClass : EventClass::unknown;
        };
        bool f = false;
        CHECK(classify(12.0f, 649.0f, f) == EventClass::pass && classify(12.0f, 650.0f, f) == EventClass::kick, "kick gyro limit is >= 650");
        CHECK(classify(12.0f, 249.0f, f) == EventClass::hardImpact && classify(12.0f, 250.0f, f) == EventClass::pass, "pass gyro limit is >= 250");
        CHECK(classify(4.99f, 100.0f, f) == EventClass::trap && classify(5.0f, 100.0f, f) == EventClass::hardImpact, "5 g is where a trap becomes a hard impact");
        CHECK(classify(3.5f, 249.0f, f) == EventClass::trap && classify(3.5f, 250.0f, f) == EventClass::pass, "soft contact: pass gyro limit is >= 250 there too");
        classify(2.5f, 100.0f, f);
        CHECK(f, "a 2.5 g contact exactly at the soft floor is detected");
        classify(2.49f, 100.0f, f);
        CHECK(!f, "2.49 g is below the soft floor: no event");
    }

    // 16b. with the shipped settings the soft floor is off: a 3.5 g contact is ignored, strong contacts still split into KICK / PASS / HARD_IMPACT
    {
        Detector det;
        auto ev = run(det, 10 * fs, { { kickAt, 3.5f, 100.0f, 10 }, { 8 * fs, 12.0f, 400.0f, 10 } });
        CHECK(ev.size() == 1 && ev[0].eventClass == EventClass::pass, "default settings: 3.5 g ignored (no TRAP), 12 g with a medium swing is a PASS");
    }

    // 17. legacy mode really is the old behaviour
    {
        Detector det(legacy);
        auto ev = run(det, 10 * fs, { { kickAt, 3.5f, 100.0f, 10 }, { 8 * fs, 12.0f, 400.0f, 10 } });
        CHECK(ev.size() == 1 && ev[0].eventClass == EventClass::hardImpact, "legacy: a 3.5 g contact is ignored, a medium swing is just HARD_IMPACT");
    }

    // 18. kick and hard-impact confidence do not change when the soft floor is on
    {
        Detector newMode;
        Detector oldMode(legacy);
        auto a = run(newMode, 10 * fs, { { kickAt, 12.0f, 1200.0f, 10 }, { 8 * fs, 9.0f, 100.0f, 10 } });
        auto b = run(oldMode, 10 * fs, { { kickAt, 12.0f, 1200.0f, 10 }, { 8 * fs, 9.0f, 100.0f, 10 } });
        CHECK(a.size() == 2 && b.size() == 2 && fabsf(a[0].confidence - b[0].confidence) < 1e-6f && fabsf(a[1].confidence - b[1].confidence) < 1e-6f,
              "KICK and HARD_IMPACT confidence are the same with the soft floor on or off");
    }

    // 19. confidence for the new classes: 0 at the soft floor, rising above it, never outside 0..1
    {
        Detector det(soft);
        auto ev = run(det, 10 * fs, { { kickAt, 2.5f, 100.0f, 10 }, { 8 * fs, 4.5f, 100.0f, 10 } });
        CHECK(ev.size() == 2 && ev[0].confidence == 0.0f && ev[1].confidence > ev[0].confidence && ev[1].confidence <= 1.0f, "trap confidence is 0 at the floor and grows with the peak");
        Detector det2(soft);
        auto ev2 = run(det2, 10 * fs, { { kickAt, 12.0f, 250.0f, 10 }, { 8 * fs, 12.0f, 600.0f, 10 } });
        CHECK(ev2.size() == 2 && ev2[1].confidence > ev2[0].confidence && ev2[1].confidence <= 1.0f, "pass confidence grows with the swing");
    }

    // 20. a loud baseline still blocks soft contacts: the adaptive threshold is above the soft floor
    {
        Detector det(soft);
        uint32_t n = 12 * fs;
        for (uint32_t i = 0; i < n; i++)
        {
            ImuSample s = {};
            s.tMs = i * 1000 / fs;
            float dyn = (i % 2 == 0) ? 0.5f : 3.5f;
            if (i == 8 * fs + 1) dyn = 6.0f;
            s.accelG[2] = 1.0f + dyn;
            det.feed(s);
        }
        det.flush();
        CHECK(det.currentThresholdG() > 10.0f && det.eventsWaiting() == 0, "vibrating baseline: threshold well above the soft floor, no TRAP from a 6 g blip");
    }

    // 21. a soft peak right before a kick is the same event (the tallest wins), a soft peak 300 ms earlier is its own event
    {
        Detector det(soft);
        auto ev = run(det, 10 * fs, { { kickAt - 20, 3.0f, 100.0f, 5 }, { kickAt, 12.0f, 1200.0f, 10 } });
        CHECK(ev.size() == 1 && ev[0].eventClass == EventClass::kick, "a 3 g bump 90 ms before a kick is absorbed by it");
        Detector det2(soft);
        uint32_t gap = 300 * fs / 1000;
        auto ev2 = run(det2, 10 * fs, { { kickAt - gap, 3.0f, 100.0f, 5 }, { kickAt, 12.0f, 1200.0f, 10 } });
        CHECK(ev2.size() == 2 && ev2[0].eventClass == EventClass::trap && ev2[1].eventClass == EventClass::kick, "300 ms apart: a TRAP, then a KICK, in time order");
    }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
