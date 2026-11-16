// detector_parity.cpp
// Reads a CSV of calibrated samples (t_ms,ax,ay,az,gx,gy,gz), runs Detector, prints one line per event:
//   t_ms,class,peak_accel_g,peak_gyro_dps,confidence,threshold_g
// Used by parity_check.py to compare against the Python prototype.
//   g++ -O2 -std=c++17 -I. detector.cpp detector_parity.cpp -o det_parity && ./det_parity samples.csv 220

#include <stdio.h>
#include <stdlib.h>
#include "detector.h"

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s samples.csv sample_rate_hz\n", argv[0]);
        return 2;
    }

    FILE* f = fopen(argv[1], "r");
    if (!f)
    {
        perror("open");
        return 2;
    }

    DetectorParams params = DetectorParams::fromConfig();
    params.classifyPassTrap = false;  // the Python prototype only has KICK and HARD_IMPACT, so compare that rule
    params.sampleRateHz = (uint32_t)atoi(argv[2]);
    Detector det(params);

    char header[256];
    if (!fgets(header, sizeof(header), f))
    {
        return 2;
    }

    auto drain = [&]()
    {
        DetectedEvent e;
        while (det.popEvent(e))
        {
            printf("%u,%u,%.6f,%.4f,%.6f,%.6f\n", e.tMs, (unsigned)e.eventClass, e.peakAccelG, e.peakGyroDps, e.confidence, e.thresholdG);
        }
    };

    unsigned t;
    float ax, ay, az, gx, gy, gz;
    while (fscanf(f, "%u,%f,%f,%f,%f,%f,%f", &t, &ax, &ay, &az, &gx, &gy, &gz) == 7)
    {
        ImuSample s = {};
        s.tMs = t;
        s.accelG[0] = ax; s.accelG[1] = ay; s.accelG[2] = az;
        s.gyroDps[0] = gx; s.gyroDps[1] = gy; s.gyroDps[2] = gz;
        det.feed(s);
        drain();
    }
    det.flush();
    drain();

    fprintf(stderr, "dropped=%u forced=%u\n", det.droppedEvents(), det.forcedResolves());
    fclose(f);
    return 0;
}
