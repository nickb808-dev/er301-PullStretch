// PullStretch host verification.  Build with -Dprivate=public for member access.
// Modes:
//   ident <out.raw>  deterministic moderate-level render → raw floats (baseline cmp)
//   rms              output RMS at scatter 0 and 1 vs input RMS (level flatness)
//   lockstep         pipeline/commit invariant + stretch sweep {1,4,100,10000}
//   freeze           FreezeLock=Static → output exactly hop-periodic
//   nan              NaN burst into input (incl. while freezing) → output finite
#include "PullStretch.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

using pullstretch::PullStretch;
using namespace pullstretch;

static void fill(od::Port &p, float v) { for (int i = 0; i < FRAMELENGTH; ++i) p.buffer()[i] = v; }

static void setup(PullStretch &d, float stretch, float scatter, float sprd,
                  float freeze, float level)
{
    fill(d.mStretchIn, stretch);
    fill(d.mScatterIn, scatter);
    fill(d.mSprdIn,    sprd);
    fill(d.mFreezeIn,  freeze);
    fill(d.mLevelIn,   level);
}

static void feed(PullStretch &d, long t0, float amp, bool nanBurst = false)
{
    for (int s = 0; s < FRAMELENGTH; ++s) {
        const long t = t0 + s;
        float v = amp * (0.6f * sinf(2.f * 3.14159265f * 220.f * t / 48000.f)
                       + 0.4f * sinf(2.f * 3.14159265f * 333.f * t / 48000.f));
        if (nanBurst && (s & 7) == 0) v = 0.0f / 0.0f * v + 1.0f / 0.0f;  // NaN/Inf
        d.mIn.buffer()[s] = v;
    }
}

// Window option (1-based on v0.4.0; harmless no-op member on 0.3.1 baseline).
static void setWindow(PullStretch &d, int oneBased)
{
#ifdef HAS_WINDOW_OPT
    d.mWindowOpt.set(oneBased);
#else
    (void)d; (void)oneBased;
#endif
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "mode?\n"); return 2; }

#ifdef HAS_WINDOW_OPT
    if (!strcmp(argv[1], "sizes")) {
        // Per-size: RMS flatness at scatter endpoints + non-finite + peak.
        for (int w = 1; w <= 3; ++w) {
            for (float sc : {0.0f, 1.0f}) {
                PullStretch d;
                setWindow(d, w);
                setup(d, 6.0f, sc, 0.75f, 0.0f, 1.0f);
                double ei = 0, eo = 0; long n = 0, nf = 0; float peak = 0;
                for (int b = 0; b < 6000; ++b) {
                    feed(d, long(b) * FRAMELENGTH, 0.4f);
                    d.process();
                    if (b < 2000) continue;
                    for (int s = 0; s < FRAMELENGTH; ++s) {
                        const float y = d.mOutL.buffer()[s];
                        if (!std::isfinite(y)) nf++;
                        else if (fabsf(y) > peak) peak = fabsf(y);
                        ei += double(d.mIn.buffer()[s]) * d.mIn.buffer()[s];
                        eo += double(y) * y;
                        n++;
                    }
                }
                printf("size=%d scatter=%.0f: ratio=%+.2f dB peak=%.3f nonFinite=%ld\n",
                       w, sc, 10.0 * log10(eo / ei), peak, nf);
            }
        }
        return 0;
    }

    if (!strcmp(argv[1], "switch")) {
        // Live window switching mid-run: bounded, finite, re-primes cleanly.
        // v0.5.1: a switch while the Freeze gate is HELD must re-arm the
        // snapshot at the new size (refroze), not replay a stale spectrum.
        PullStretch d;
        setWindow(d, 2);
        setup(d, 10.0f, 1.0f, 0.75f, 0.0f, 1.0f);
        long nf = 0; float peak = 0;
        bool refroze = false;
        for (int b = 0; b < 9000; ++b) {
            if (b == 2000) setWindow(d, 3);   // normal → deep
            if (b == 4500) setWindow(d, 1);   // deep → short (WHILE FROZEN)
            if (b == 7000) setWindow(d, 2);   // short → normal
            if (b == 3000) fill(d.mFreezeIn, 1.0f);   // freeze across a switch
            if (b == 5000) fill(d.mFreezeIn, 0.0f);
            feed(d, long(b) * FRAMELENGTH, 0.4f);
            d.process();
            if (b == 4900) refroze = d.mFrozenActive;  // snapshot re-armed?
            for (int s = 0; s < FRAMELENGTH; ++s) {
                const float y = d.mOutL.buffer()[s];
                if (!std::isfinite(y)) nf++;
                else if (fabsf(y) > peak) peak = fabsf(y);
            }
        }
        printf("switch: nonFinite=%ld peak=%.3f refrozeAfterSwitch=%d\n",
               nf, peak, refroze ? 1 : 0);
        return (nf || !refroze) ? 1 : 0;
    }
#endif

    if (!strcmp(argv[1], "ident")) {
        FILE *f = fopen(argv[2], "wb");
        // Moderate level (peaks « 0.9) so the new soft limiter is transparent
        // and the render must be BIT-IDENTICAL to baseline.
        PullStretch d;
        setup(d, 8.0f, 0.7f, 0.6f, 0.0f, 1.0f);
        for (int b = 0; b < 3000; ++b) {
            feed(d, long(b) * FRAMELENGTH, 0.25f);
            if (b == 1500) fill(d.mFreezeIn, 1.0f);   // freeze mid-run
            if (b == 2200) fill(d.mFreezeIn, 0.0f);   // release
            d.process();
            fwrite(d.mOutL.buffer(), 4, FRAMELENGTH, f);
            fwrite(d.mOutR.buffer(), 4, FRAMELENGTH, f);
        }
        fclose(f);
        printf("ident done\n");
        return 0;
    }

    if (!strcmp(argv[1], "rms")) {
        for (float sc : {0.0f, 1.0f}) {
            PullStretch d;
            setup(d, 6.0f, sc, 0.75f, 0.0f, 1.0f);
            double ei = 0, eo = 0; long n = 0;
            for (int b = 0; b < 4000; ++b) {
                feed(d, long(b) * FRAMELENGTH, 0.4f);
                d.process();
                if (b < 1000) continue;   // settle
                for (int s = 0; s < FRAMELENGTH; ++s) {
                    ei += double(d.mIn.buffer()[s]) * d.mIn.buffer()[s];
                    eo += double(d.mOutL.buffer()[s]) * d.mOutL.buffer()[s];
                    n++;
                }
            }
            printf("rms scatter=%.0f: in=%.4f out=%.4f ratio=%.2f dB\n",
                   sc, sqrt(ei / n), sqrt(eo / n),
                   10.0 * log10(eo / ei));
        }
        return 0;
    }

    if (!strcmp(argv[1], "lockstep")) {
        for (float st : {1.0f, 4.0f, 100.0f, 10000.0f}) {
            PullStretch d;
            setup(d, st, 1.0f, 0.75f, 0.0f, 1.0f);
            long bad = 0, nonFinite = 0; float peak = 0;
            bool started = false;
            for (int b = 0; b < 6000; ++b) {
                feed(d, long(b) * FRAMELENGTH, 0.5f);
                d.process();
                if (d.mFirstFrameDone) started = true;
                if (started) {
                    // invariant: after the drain, stage == drainedBlocks − 1
                    const int drained = d.mOutPos / FRAMELENGTH;
                    if (d.mGenStage != drained - 1 && d.mGenStage != 8) bad++;
#ifdef HAS_WINDOW_OPT
                    if (d.mOutPos > d.mHopSize) bad++;
#else
                    if (d.mOutPos > kHopSize) bad++;
#endif
                }
                for (int s = 0; s < FRAMELENGTH; ++s) {
                    const float y = d.mOutL.buffer()[s];
                    if (!std::isfinite(y)) nonFinite++;
                    else if (fabsf(y) > peak) peak = fabsf(y);
                }
            }
            printf("lockstep stretch=%-7.0f badInvariant=%ld nonFinite=%ld peak=%.3f\n",
                   st, bad, nonFinite, peak);
        }
        return 0;
    }

    if (!strcmp(argv[1], "freeze")) {
        PullStretch d;
        setup(d, 10.0f, 1.0f, 0.75f, 0.0f, 1.0f);
#ifdef HAS_WINDOW_OPT
        d.mFreezeLockOpt.set(2);            // Static (1-based, v0.4.0)
#else
        d.mFreezeLockOpt.set(1);            // Static (0-based, v0.3.1)
#endif
        std::vector<float> out;
        for (int b = 0; b < 4000; ++b) {
            feed(d, long(b) * FRAMELENGTH, 0.4f);
            if (b == 800) fill(d.mFreezeIn, 1.0f);
            d.process();
            if (b >= 2000)
                out.insert(out.end(), d.mOutL.buffer(), d.mOutL.buffer() + FRAMELENGTH);
        }
        // Static freeze → identical frames → output exactly hop-periodic.
#ifdef HAS_WINDOW_OPT
        const size_t hop = size_t(d.mHopSize);
#else
        const size_t hop = size_t(kHopSize);
#endif
        float maxDiff = 0;
        for (size_t i = hop; i < out.size(); ++i)
            maxDiff = std::max(maxDiff, fabsf(out[i] - out[i - hop]));
        printf("freeze static: periodic maxDiff=%.3e (0 = fully static)\n", maxDiff);
        return 0;
    }

    if (!strcmp(argv[1], "nan")) {
        PullStretch d;
        setup(d, 10.0f, 1.0f, 0.75f, 0.0f, 1.0f);
        long nonFinite = 0; float peakAfter = 0;
        for (int b = 0; b < 5000; ++b) {
            const bool burst = (b >= 1000 && b < 1010);   // NaN/Inf burst
            feed(d, long(b) * FRAMELENGTH, 0.4f, burst);
            if (b == 1005) fill(d.mFreezeIn, 1.0f);       // freeze DURING the burst
            if (b == 3000) fill(d.mFreezeIn, 0.0f);
            d.process();
            for (int s = 0; s < FRAMELENGTH; ++s) {
                const float y = d.mOutL.buffer()[s];
                if (!std::isfinite(y)) nonFinite++;
                else if (b > 1010 && fabsf(y) > peakAfter) peakAfter = fabsf(y);
            }
        }
        printf("nan: nonFinite=%ld peakAfterBurst=%.3f (frozen through the burst)\n",
               nonFinite, peakAfter);
        return nonFinite ? 1 : 0;
    }

    fprintf(stderr, "unknown mode\n");
    return 2;
}
