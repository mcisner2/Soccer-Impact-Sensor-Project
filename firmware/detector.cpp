// detector.cpp
// Streaming port of detect_events() from peak_detection_prototype.py. See detector.h for the two deliberate differences.
//
// The Python calls scipy.signal.find_peaks(height=threshold, distance=refractory). That does three things in order:
//  1. finds local maxima (a flat top counts once, at its middle sample; the first and last sample never count)
//  2. drops maxima below the threshold at their own position
//  3. among what is left, keeps the tallest peak and removes everything closer than `distance` samples to it,
//     then repeats with the next tallest that is still standing
// Step 3 needs to see a whole run of close peaks before it can decide, so peaks are collected into a "cluster"
// (each peak less than `distance` after the previous one) and the cluster is decided once nothing new can join it.
// Peaks in different clusters are at least `distance` apart, so they never affect each other.

#include <math.h>
#include <algorithm>
#include "detector.h"

namespace
{
    // numpy.median: the middle value, or the mean of the two middle values. Reorders the array.
    float medianOf(float* values, size_t n)
    {
        if (n == 0)
        {
            return 0.0f;
        }

        size_t mid = n / 2;
        std::nth_element(values, values + mid, values + n);
        float upper = values[mid];
        if (n % 2 == 1)
        {
            return upper;
        }

        float lower = *std::max_element(values, values + mid);
        return 0.5f * (lower + upper);
    }
}

DetectorParams DetectorParams::fromConfig()
{
    DetectorParams p;
    p.sampleRateHz = config::imuOdrHz;
    p.kMad = config::detKMad;
    p.minPeakG = config::detMinPeakG;
    p.kickGyroDps = config::detKickGyroDps;
    p.classifyPassTrap = config::detClassifyPassTrap;
    p.softMinPeakG = config::detSoftMinPeakG;
    p.passGyroDps = config::detPassGyroDps;
    p.refractoryMs = config::detRefractoryMs;
    p.gyroBeforeMs = config::detGyroBeforeMs;
    p.gyroAfterMs = config::detGyroAfterMs;
    p.baselineWindowMs = config::detBaselineWindowMs;
    p.baselineHopMs = config::detBaselineHopMs;
    return p;
}

Detector::Detector(const DetectorParams& params) : cfg(params)
{
    // whole samples, rounded down, same as int(seconds * FS) in the Python
    refractorySamples = cfg.refractoryMs * cfg.sampleRateHz / 1000;
    gyroBeforeSamples = cfg.gyroBeforeMs * cfg.sampleRateHz / 1000;
    gyroAfterSamples = cfg.gyroAfterMs * cfg.sampleRateHz / 1000;
    windowSamples = cfg.baselineWindowMs * cfg.sampleRateHz / 1000;
    hopSamples = cfg.baselineHopMs * cfg.sampleRateHz / 1000;

    // the lowest threshold in use: the soft floor when passes and traps are on, otherwise the 5 g floor
    floorG = cfg.classifyPassTrap && cfg.softMinPeakG < cfg.minPeakG ? cfg.softMinPeakG : cfg.minPeakG;

    // keep everything inside the fixed buffers
    if (refractorySamples < 1) refractorySamples = 1;
    if (hopSamples < 1) hopSamples = 1;
    if (windowSamples > maxBaselineSamples) windowSamples = maxBaselineSamples;
    if (windowSamples < hopSamples) windowSamples = hopSamples;
    // a peak is decided one refractory period after it, so the gyro window after it can't be longer than that
    if (gyroAfterSamples > refractorySamples) gyroAfterSamples = refractorySamples;
    // history must hold the window before the oldest peak that can still be waiting (see resolveCluster)
    if (gyroBeforeSamples > historySamples / 4) gyroBeforeSamples = historySamples / 4;

    reset();
}

void Detector::reset()
{
    count = 0;
    thrCur = floorG;
    thrPrev = floorG;
    thrFromData = false;
    thrPrevFromData = false;
    curBlockStart = 0;
    prev = 0.0f;
    canPeak = false;
    leftIdx = 0;
    clusterCount = 0;
    queueHead = 0;
    queueCount = 0;
    dropped = 0;
    forced = 0;
}

bool Detector::popEvent(DetectedEvent& out)
{
    if (queueCount == 0)
    {
        return false;
    }

    out = queue[queueHead];
    queueHead = (queueHead + 1) % eventQueueSize;
    queueCount--;
    return true;
}

void Detector::pushEvent(const DetectedEvent& e)
{
    if (queueCount == eventQueueSize)
    {
        dropped++;
        return;
    }

    queue[(queueHead + queueCount) % eventQueueSize] = e;
    queueCount++;
}

// New threshold for the block that starts at sample `idx`, from the samples before it.
// Same as adaptive_threshold(m, causal=True) in the Python.
void Detector::updateThreshold(uint32_t idx)
{
    size_t n = idx < windowSamples ? idx : windowSamples;
    for (size_t i = 0; i < n; i++)
    {
        scratch[i] = baseline[(idx - n + i) % maxBaselineSamples];
    }

    float med = medianOf(scratch, n);
    for (size_t i = 0; i < n; i++)
    {
        scratch[i] = baseline[(idx - n + i) % maxBaselineSamples] - med;
        if (scratch[i] < 0.0f) scratch[i] = -scratch[i];
    }
    float mad = 1.4826f * medianOf(scratch, n);

    thrPrev = thrCur;
    thrPrevFromData = thrFromData;
    thrCur = fmaxf(floorG, med + cfg.kMad * mad);
    if (n > 0) thrFromData = true;
    curBlockStart = idx;
}

float Detector::thresholdAt(uint32_t peakIndex) const
{
    // a peak is only recognized after the sample following it, so it can sit in the previous block
    return peakIndex >= curBlockStart ? thrCur : thrPrev;
}

void Detector::onPeak(uint32_t peakIndex, float height)
{
    float thr = thresholdAt(peakIndex);
    if (height < thr)
    {
        return;  // find_peaks keeps height >= threshold
    }
    bool fromData = peakIndex >= curBlockStart ? thrFromData : thrPrevFromData;
    if (!fromData && height < cfg.minPeakG)
    {
        return;  // warm-up: with no baseline yet, the soft floor could fire on ordinary vibration, so only strong peaks count
    }

    if (clusterCount > 0 && peakIndex - cluster[clusterCount - 1].index >= refractorySamples)
    {
        // too far from the run so far to compete with it: decide that run first.
        // (count is the index of the sample being processed, which is already stored)
        resolveCluster(count);
    }
    else if (clusterCount == maxClusterPeaks)
    {
        forced++;
        resolveCluster(count);
    }

    cluster[clusterCount++] = { peakIndex, height, thr };
}

// Decide the current cluster and emit its surviving peaks, oldest first. lastIdx = newest sample available.
void Detector::resolveCluster(uint32_t lastIdx)
{
    if (clusterCount == 0)
    {
        return;
    }

    // scipy's rule: go through the peaks from the tallest down, and each one still standing removes
    // every peak closer than `distance` samples
    size_t order[maxClusterPeaks];
    bool keep[maxClusterPeaks];
    for (size_t i = 0; i < clusterCount; i++)
    {
        order[i] = i;
        keep[i] = true;
    }
    for (size_t i = 1; i < clusterCount; i++)  // insertion sort by height, ascending
    {
        size_t value = order[i];
        size_t j = i;
        while (j > 0 && cluster[order[j - 1]].height > cluster[value].height)
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = value;
    }
    for (size_t r = clusterCount; r-- > 0;)
    {
        size_t j = order[r];
        if (!keep[j])
        {
            continue;
        }
        for (size_t k = 0; k < clusterCount; k++)
        {
            if (k == j)
            {
                continue;
            }
            uint32_t gap = cluster[j].index > cluster[k].index ? cluster[j].index - cluster[k].index : cluster[k].index - cluster[j].index;
            if (gap < refractorySamples)
            {
                keep[k] = false;
            }
        }
    }

    for (size_t i = 0; i < clusterCount; i++)
    {
        if (!keep[i])
        {
            continue;
        }

        const Peak& peak = cluster[i];

        // largest |gyro| from gyroBefore samples before to gyroAfter samples after the impact, clipped to what exists
        uint32_t first = peak.index > gyroBeforeSamples ? peak.index - gyroBeforeSamples : 0;
        uint32_t oldestKept = lastIdx >= historySamples ? lastIdx - (uint32_t)historySamples + 1 : 0;
        if (first < oldestKept) first = oldestKept;
        uint32_t last = peak.index + gyroAfterSamples;
        if (last > lastIdx) last = lastIdx;

        float gyroPeak = 0.0f;
        for (uint32_t j = first; j <= last; j++)
        {
            gyroPeak = fmaxf(gyroPeak, gyroMag[j % historySamples]);
        }

        DetectedEvent e;
        float peakG = peak.height;
        bool strong = peakG >= cfg.minPeakG;  // 5 g or more: a real strike or knock, not just a soft contact
        EventClass cls;
        if (!cfg.classifyPassTrap)
        {
            cls = gyroPeak >= cfg.kickGyroDps ? EventClass::kick : EventClass::hardImpact;
        }
        else if (strong)
        {
            cls = gyroPeak >= cfg.kickGyroDps ? EventClass::kick : (gyroPeak >= cfg.passGyroDps ? EventClass::pass : EventClass::hardImpact);
        }
        else
        {
            cls = gyroPeak >= cfg.passGyroDps ? EventClass::pass : EventClass::trap;
        }

        e.eventClass = cls;
        e.tMs = timeMs[peak.index % historySamples];
        e.peakAccelG = peakG;
        e.peakGyroDps = gyroPeak;
        e.thresholdG = peak.threshold;

        // how far above the reference level, plus how far above the gyro gate for classes that have one, clipped to 0..1.
        // KICK and HARD_IMPACT keep the 5 g reference they always had, so their confidence does not change with the soft floor.
        float ref = strong ? fmaxf(peak.threshold, cfg.minPeakG) : peak.threshold;
        float conf = 0.5f * (peakG / ref - 1.0f);
        if (cls == EventClass::kick)
        {
            conf += 0.5f * (gyroPeak / cfg.kickGyroDps - 1.0f);
        }
        else if (cls == EventClass::pass)
        {
            conf += 0.5f * (gyroPeak / cfg.passGyroDps - 1.0f);
        }
        e.confidence = fminf(fmaxf(conf, 0.0f), 1.0f);

        pushEvent(e);
    }

    clusterCount = 0;
}

void Detector::feed(const ImuSample& s)
{
    uint32_t idx = count;

    // the block that starts here uses the samples before this one, so update before storing this sample
    if (idx > 0 && idx % hopSamples == 0)
    {
        updateThreshold(idx);
    }

    float a2 = s.accelG[0] * s.accelG[0] + s.accelG[1] * s.accelG[1] + s.accelG[2] * s.accelG[2];
    float dynamicG = fabsf(sqrtf(a2) - 1.0f);  // orientation-independent, spec 2
    float g2 = s.gyroDps[0] * s.gyroDps[0] + s.gyroDps[1] * s.gyroDps[1] + s.gyroDps[2] * s.gyroDps[2];

    baseline[idx % maxBaselineSamples] = dynamicG;
    gyroMag[idx % historySamples] = sqrtf(g2);
    timeMs[idx % historySamples] = s.tMs;

    // local maxima, same rules as find_peaks: a rise starts a candidate, a flat top keeps it,
    // the first drop after it confirms it at the middle of the flat top
    if (idx > 0)
    {
        if (dynamicG > prev)
        {
            canPeak = true;
            leftIdx = idx;
        }
        else if (dynamicG < prev)
        {
            if (canPeak)
            {
                onPeak((leftIdx + (idx - 1)) / 2, prev);
            }
            canPeak = false;
        }
    }
    prev = dynamicG;
    count = idx + 1;

    // Nothing new can join the run once a refractory period has passed since its last peak, and no
    // candidate that is still rising or flat could land inside that period.
    if (clusterCount > 0)
    {
        uint32_t lastPeak = cluster[clusterCount - 1].index;
        bool candidateCouldJoin = canPeak && (leftIdx - lastPeak < refractorySamples);
        bool tooOld = idx - cluster[0].index >= historySamples - gyroBeforeSamples - refractorySamples;
        if (idx - lastPeak >= refractorySamples && !candidateCouldJoin)
        {
            resolveCluster(idx);
        }
        else if (tooOld)
        {
            // a long unbroken run of peaks (vibration, saturation): decide early so the history never runs out
            forced++;
            resolveCluster(idx);
        }
    }
}

void Detector::flush()
{
    if (count > 0)
    {
        resolveCluster(count - 1);
    }
}
