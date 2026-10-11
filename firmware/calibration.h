// calibration.h
// PLACEHOLDER for Edward's calibration module (spec 5.1). Same shape as his API so it drops in.
// It averages bias and gravity over 4 s but does NOT run the stillness checks yet, so it always passes.

#pragma once
#include <stdint.h>
#include "sampler.h"

enum class CalState : uint8_t
{
    idle,
    collecting,
    done,
    failed
};

struct CalResult
{
    bool ok = false;
    uint8_t errorCode = 0;     // spec 8, 0x01 to 0x04 when a check fails
    float gyroBiasDps[3] = {};
    float gravityG[3] = {};
    float tempAtCalC = 0.0f;
};

namespace calibration
{
    void start();
    void feed(const ImuSample& sample);  // once per sample while collecting
    CalState state();
    CalResult result();
}
