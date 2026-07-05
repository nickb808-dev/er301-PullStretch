// Host-test stub of od/objects/heads/Head.h
#pragma once
#include <od/objects/Object.h>
#include <od/audio/Sample.h>
namespace od {
struct Head : Object {
    Sample *mpSample      = nullptr;
    int     mCurrentIndex = 0;
    int     mEndIndex     = 0;
    virtual void setSample(Sample *s) { mpSample = s; }
    Sample *getSample() { return mpSample; }
    int getPosition() { return mCurrentIndex; }
    void attach() {}
    void release() {}
};
} // namespace od
