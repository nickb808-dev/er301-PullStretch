// Host-test stub of od/audio/Sample.h
#pragma once
#include <cstddef>
namespace od {
struct Sample {
    size_t mSampleCount  = 0;
    int    mChannelCount = 1;
    float *mpData        = nullptr;
};
} // namespace od
