/* PullStretch.cpp — Paul's Extreme Time Stretch for ER-301 v0.7.0
 *
 * See PullStretch.h for algorithm, window-option, normalisation, and controls.
 *
 * v0.7.0 — MORPH REMOVED.  The freeze-morph / 7-slot spectral scanner (v0.5.0–
 * v0.6.0: Grab B, Morph, MorphWarp, and the optimal-transport interpolation)
 * is gone.  PullStretch is the pure extreme time-stretch + Freeze hold again.
 *
 * v0.4.0 — RUNTIME WINDOW SIZE (Short 2048 / Normal 4096 / Deep 8192)
 * ────────────────────────────────────────────────────────────────────
 * All buffers are max-size (8192) and all three FFT plans are made once in
 * the constructor (never destroyed — firmware plan-table corruption, see
 * header).  applyWindowSize() derives the per-size pipeline schedule and
 * re-primes; the OLA normalisation endpoints are computed NUMERICALLY from
 * each window at construction (window-agnostic — Nasca window ready).
 *
 * Pipeline schedule by size (hop = N/4 → 4/8/16 blocks available):
 *   Short : 0 frameStart · 1 phase(all) · 2 IFFT-L · 3 IFFT-R + window   (4 st)
 *   Normal: 0 frameStart · 1-3 phase    · 4 IFFT-L · 5 IFFT-R · 6 window (7 st)
 *   Deep  : 0 frameStart · 1-5 phase    · 6 IFFT-L · 7 IFFT-R · 8 window (9 st)
 * The frame commits at the hop boundary once mGenStage >= mStagesNeeded.
 *
 * STABILITY: RFFT_DESTROY NOT CALLED — see header.
 * v0.3.1 hardening retained: capture sanitize, output sanitize + softLimit
 * (pre-level), commit guard, mono window micro-opt. */

#include "PullStretch.h"
#include "trig_poly.h"   // libm-free ctor sin/cos (am335x package-trig fix)

#include <algorithm>
#include <cmath>
#include <cstring>  // memmove, memset, memcpy

namespace pullstretch {

// ── Finiteness guard (v0.3.1, pattern shared with Dirac/Planck) ──────────────
static inline float sanitize(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    return ((v.u & 0x7F800000u) == 0x7F800000u) ? 0.0f : x;
}

// ── Output soft limiter (v0.3.1) — transparent < ±0.9, asymptote ±1 ─────────
static inline float softLimit(float x)
{
    const float T  = 0.9f;
    const float ax = (x >= 0.0f) ? x : -x;
    if (ax <= T) return x;
    const float e   = ax - T;
    const float lim = T + (1.0f - T) * (e / (e + (1.0f - T)));
    return (x >= 0.0f) ? lim : -lim;
}

/* ── Constructor ─────────────────────────────────────────────────────────────── */

PullStretch::PullStretch()
{
    addInput(mIn);
    addInput(mStretchIn);
    addInput(mScatterIn);
    addInput(mSprdIn);
    addInput(mFreezeIn);
    addInput(mLevelIn);
    addOutput(mOutL);
    addOutput(mOutR);

    // Context-menu options — serialised by the ER-301 preset system and
    // exposed via OptionControl in PullStretch.lua's unit menu (v0.4.0).
    addOption(mWindowOpt);
    addOption(mFreezeLockOpt);

    // ── Allocate all buffers at MAX size ─────────────────────────────────
    // std::vector: heap allocator gives ≥ 8-byte (typically 16) alignment,
    // satisfying NE10 NEON requirements.  Nothing reallocates at runtime.
    mCapBuf.assign(kCapBufSize, 0.0f);
    mFFTIn.assign(kMaxFFTSize, 0.0f);
    mFFTOut.resize(kMaxNumBins);
    mPhasedL.resize(kMaxNumBins);
    mPhasedR.resize(kMaxNumBins);
    mFrozenSpectrum.resize(kMaxNumBins);
    mSynthL.assign(kMaxFFTSize, 0.0f);
    mSynthR.assign(kMaxFFTSize, 0.0f);
    mOutBufL.assign(kMaxFFTSize, 0.0f);
    mOutBufR.assign(kMaxFFTSize, 0.0f);
    mSineTable.resize(kSineTableSize + 1);

    // ── Per-size Hann windows + NUMERIC OLA normalisation endpoints ──────
    // w[i] = 0.5 × (1 − cos(2π i / N)); scale endpoints computed from the
    // actual window so any window shape is drop-in (see header).
    for (int w = 0; w < kNumWinSizes; ++w) {
        const int N = kWinSizes[w];
        const int H = N / 4;
        mHannWins[w].resize(N);
        const float step = kTwoPi / float(N);
        for (int i = 0; i < N; ++i)
            mHannWins[w][i] = 0.5f * (1.0f - trigpoly::pcos(step * float(i)));

        // Coherent OLA gain: mean_n Σ_{k=0..3} w_a(n+kH)·w_s(n+kH)  (= Σ w²
        // here, since analysis and synthesis share the window).
        double acc = 0.0;
        for (int n = 0; n < H; ++n) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                const double v = mHannWins[w][n + k * H];
                s += v * v;
            }
            acc += s;
        }
        const double cohGain = acc / double(H);          // Hann: 3/2 exactly
        double mw2 = 0.0;                                 // mean(w_a²)
        for (int i = 0; i < N; ++i)
            mw2 += double(mHannWins[w][i]) * mHannWins[w][i];
        mw2 /= double(N);                                 // Hann: 3/8
        mScaleCoh[w] = float(1.0 / cohGain);              // Hann: 2/3
        mScaleInc[w] = float(1.0 / sqrt(mw2 * cohGain));  // Hann: 4/3
    }

    // ── Sine lookup table (random-phase generation) ──────────────────────
    for (int i = 0; i <= kSineTableSize; ++i)
        mSineTable[i] = trigpoly::psin(kTwoPi * float(i) / float(kSineTableSize));

    // ── RFFT plans — one per size, allocated once, NEVER destroyed ───────
    for (int w = 0; w < kNumWinSizes; ++w)
        mFFTCfgs[w] = RFFT_allocate(kWinSizes[w]);

    applyWindowSize(std::max(1, std::min(mWindowOpt.value(), kNumWinSizes)) - 1);
}

/* ── Destructor ──────────────────────────────────────────────────────────────── */

PullStretch::~PullStretch()
{
    // RFFT_destroy intentionally skipped — calling it for large plan sizes
    // corrupts the ER-301 firmware's global FFT plan table (all FFT units
    // glitch after deletion).  Memory is reclaimed when the .so unloads.
    mFFTCfg = nullptr;
}

/* ── applyWindowSize (v0.4.0) ────────────────────────────────────────────────── */

void PullStretch::applyWindowSize(int sel)
{
    mWinSel  = sel;
    mFFTSize = kWinSizes[sel];
    mNumBins = mFFTSize / 2 + 1;
    mHopSize = mFFTSize / 4;
    mFFTCfg  = mFFTCfgs[sel];

    // Per-size pipeline schedule (see header).  Short has only 4 blocks per
    // hop, so the phase loop is one chunk and the synthesis window is fused
    // into the IFFT-R stage.
    switch (sel) {
        case 0:  mPhaseChunks = 1; mFuseWindow = true;  break;   // Short
        case 2:  mPhaseChunks = 5; mFuseWindow = false; break;   // Deep
        default: mPhaseChunks = 3; mFuseWindow = false; break;   // Normal
    }
    // Stages of actual work (4 / 7 / 9), plus ONE idle margin stage when the
    // hop has room for it.  The idle stage keeps Normal's commit timing
    // identical to v0.3.x (8 stages — sample-aligned for A/B), and gives
    // Normal/Deep a spare block between the last synthesis stage and the
    // commit.  Short has no room (4 stages in 4 blocks) and commits tight.
    const int base      = 1 + mPhaseChunks + (mFuseWindow ? 2 : 3);
    const int hopBlocks = mHopSize / FRAMELENGTH;                // 4 / 8 / 16
    mStagesNeeded = std::min(base + 1, hopBlocks);               // 4 / 8 / 10

    // Re-prime: clear the OLA state and hold output silent until the first
    // frame at the new size commits.  The freeze snapshot (if any) was taken
    // at the old size — invalidate it.
    std::fill(mOutBufL.begin(), mOutBufL.end(), 0.0f);
    std::fill(mOutBufR.begin(), mOutBufR.end(), 0.0f);
    std::fill(mSynthL.begin(), mSynthL.end(), 0.0f);
    std::fill(mSynthR.begin(), mSynthR.end(), 0.0f);
    mGenStage       = 0;
    mOutPos         = 0;
    mFirstFrameDone = false;
    mFrozenActive   = false;
    mGenSpec        = &mFFTOut;
}

/* ── PRNG (xorshift32) ───────────────────────────────────────────────────────── */

float PullStretch::randUnipolar()
{
    mRandState ^= mRandState << 13;
    mRandState ^= mRandState >> 17;
    mRandState ^= mRandState << 5;
    return float(mRandState & 0x7FFFFFFFu) / float(0x7FFFFFFFu);
}

/* ── frameStart (stage 0) ────────────────────────────────────────────────────── */

void PullStretch::frameStart()
{
    // Freeze edge detection.  v0.5.1: "frozen but snapshot invalid" (the gate
    // held across a window-size switch, which invalidates the snapshot) is
    // treated as newly frozen — the frame re-analyses at the held read head
    // and re-snapshots slot A at the NEW size.  Previously this state fell
    // through to a stale mFFTOut (upper bins zero/garbage on an upsize) and
    // replayed it until unfreeze.
    const bool newlyFrozen  = mFrozen && (!mWasFrozen || !mFrozenActive);
    const bool justUnfrozen = !mFrozen && mWasFrozen;
    mWasFrozen = mFrozen;
    if (justUnfrozen) mFrozenActive = false;

    // Latch this frame's parameters once so all stages stay consistent.
    const float stretch = std::max(kMinStretch,
                              std::min(mStretchIn.buffer()[0], kMaxStretch));
    mGenScatter  = std::max(0.0f, std::min(mScatterIn.buffer()[0], 1.0f));
    mGenSprd     = std::max(0.0f, std::min(mSprdIn.buffer()[0],    1.0f));
    mGenMono     = (mGenSprd < 0.001f);
    // Numeric per-size normalisation endpoints (v0.4.0).
    mGenEffScale = mScaleCoh[mWinSel]
                 + mGenScatter * (mScaleInc[mWinSel] - mScaleCoh[mWinSel]);

    const float *win = mHannWins[mWinSel].data();

    // Analysis: run a forward FFT when the read head is live, or on the freeze
    // rising edge (to take the snapshot).  While frozen, analysis is skipped
    // and the held snapshot drives synthesis.
    const bool wantLive = (!mFrozen || newlyFrozen);
    if (wantLive) {
        int readStart = int(mReadHead) & kCapBufMask;
        // Write-head straddle guard (v0.2.3): keep the analysis window clear of
        // the live capture write head so it never has a splice.
        const int gap = (mCapWrite - readStart) & kCapBufMask;
        if (gap < mFFTSize) {
            readStart = (mCapWrite - mFFTSize) & kCapBufMask;
            mReadHead = float(readStart);
        }

        for (int i = 0; i < mFFTSize; ++i)
            mFFTIn[i] = mCapBuf[(readStart + i) & kCapBufMask] * win[i];

        RFFT_forward(mFFTOut.data(), mFFTIn.data(), mFFTCfg);

        if (newlyFrozen) {
            mFrozenSpectrum  = mFFTOut;
            mFrozenRandState = mRandState;
            mFrozenActive    = true;
        }
    }

    // Frozen → the held snapshot drives synthesis; live → the fresh FFT.
    mGenSpec = mFrozenActive ? &mFrozenSpectrum : &mFFTOut;

    // FreezeLock = Static (value 2, 1-based): replay the snapshot PRNG
    // sequence each frame.  Values ≤ 1 (incl. 0 from old presets) = Evolving.
    if (mFrozenActive && mFreezeLockOpt.value() >= 2)
        mRandState = mFrozenRandState;

    // DC bin zeroed (no DC drift); Nyquist real, magnitude preserved.
    mPhasedL[0] = {0.0f, 0.0f};
    mPhasedR[0] = {0.0f, 0.0f};
    const float nyqMag = (*mGenSpec)[mNumBins - 1].r;  // imag is 0 by convention
    mPhasedL[mNumBins - 1] = {nyqMag, 0.0f};
    mPhasedR[mNumBins - 1] = {nyqMag, 0.0f};

    // Advance the read head once per frame (held when frozen).
    if (!mFrozen) {
        mReadHead += float(mHopSize) / stretch;
        if (mReadHead >= float(kCapBufSize))
            mReadHead -= float(kCapBufSize);
    }
}

/* ── phaseBins (phase-randomisation chunks) ──────────────────────────────────── */

void PullStretch::phaseBins(int b0, int b1)
{
    const std::vector<complex_float_t>& spec = *mGenSpec;
    const float  scatter     = mGenScatter;
    const float  sprd        = mGenSprd;
    const bool   fullScatter = (scatter >= 0.999f);
    const bool   coherent    = (scatter <= 0.001f);
    const bool   monoOut     = mGenMono;
    const float *sineTbl     = mSineTable.data();

    for (int b = b0; b < b1; ++b) {
        const float re  = spec[b].r;
        const float im  = spec[b].i;
        const float mag = sqrtf(re * re + im * im);

        if (mag < 1e-12f) {
            mPhasedL[b] = {0.0f, 0.0f};
            mPhasedR[b] = {0.0f, 0.0f};
            continue;
        }

        float uLr, uLi;

        if (coherent) {
            const float inv = 1.0f / mag;
            uLr = re * inv;
            uLi = im * inv;
        } else {
            const uint32_t rndL = uint32_t(randUnipolar() * float(kSineTableSize));
            const int      iLs  = int(rndL) & kSineTableMask;
            const int      iLc  = (iLs + kCosOffset) & kSineTableMask;
            const float    cL   = sineTbl[iLc];
            const float    sL   = sineTbl[iLs];

            if (fullScatter) {
                uLr = cL;
                uLi = sL;
            } else {
                const float bRe  = re + scatter * (cL * mag - re);
                const float bIm  = im + scatter * (sL * mag - im);
                const float bInv = 1.0f / sqrtf(bRe * bRe + bIm * bIm + 1e-20f);
                uLr = bRe * bInv;
                uLi = bIm * bInv;
            }
        }

        mPhasedL[b] = {mag * uLr, mag * uLi};

        if (!monoOut) {
            const uint32_t rndR = uint32_t(randUnipolar() * float(kSineTableSize));
            const int      iRs  = int(rndR) & kSineTableMask;
            const int      iRc  = (iRs + kCosOffset) & kSineTableMask;
            const float    cR   = sineTbl[iRc];
            const float    sR   = sineTbl[iRs];
            const float mrRe = uLr + sprd * (cR - uLr);
            const float mrIm = uLi + sprd * (sR - uLi);
            const float mInv = 1.0f / sqrtf(mrRe * mrRe + mrIm * mrIm + 1e-20f);
            mPhasedR[b] = {mag * mrRe * mInv, mag * mrIm * mInv};
        }
    }
}

/* ── generateStage — one pipeline stage per process() block ──────────────────── */

void PullStretch::generateStage()
{
    if (!mFFTCfg) { if (mGenStage < mStagesNeeded) ++mGenStage; return; }

    const int s = mGenStage;

    if (s == 0) {
        frameStart();
    } else if (s <= mPhaseChunks) {
        // Chunk boundaries derived from the current bin count; bins are
        // processed in ascending order so the PRNG sequence is chunk-agnostic.
        const int total = mNumBins - 2;                    // bins 1..mNumBins-2
        const int b0 = 1 + (total * (s - 1)) / mPhaseChunks;
        const int b1 = 1 + (total * s)       / mPhaseChunks;
        phaseBins(b0, b1);
    } else if (s == mPhaseChunks + 1) {
        RFFT_inverse(mSynthL.data(), mPhasedL.data(), mFFTCfg);
    } else if (s == mPhaseChunks + 2) {
        // IFFT R (stereo).  Mono R is produced from the windowed L in
        // windowStage().  Short mode fuses the window into this stage.
        if (!mGenMono)
            RFFT_inverse(mSynthR.data(), mPhasedR.data(), mFFTCfg);
        if (mFuseWindow) windowStage();
    } else if (s == mPhaseChunks + 3 && !mFuseWindow) {
        windowStage();
    }

    if (mGenStage < mStagesNeeded) ++mGenStage;
}

// ── windowStage — synthesis window × scatter-adaptive scale ─────────────────
void PullStretch::windowStage()
{
    const float *win = mHannWins[mWinSel].data();
    const float  S   = mGenEffScale;
    if (mGenMono) {
        for (int i = 0; i < mFFTSize; ++i)
            mSynthL[i] *= win[i] * S;
        std::memcpy(mSynthR.data(), mSynthL.data(),
                    size_t(mFFTSize) * sizeof(float));
    } else {
        for (int i = 0; i < mFFTSize; ++i) {
            const float w = win[i] * S;
            mSynthL[i] *= w;
            mSynthR[i] *= w;
        }
    }
}

/* ── commitFrame — OLA shift + accumulate the finished frame ────────────────── */

void PullStretch::commitFrame()
{
    const int tailLen = mFFTSize - mHopSize;
    std::memmove(mOutBufL.data(), mOutBufL.data() + mHopSize,
                 size_t(tailLen) * sizeof(float));
    std::memmove(mOutBufR.data(), mOutBufR.data() + mHopSize,
                 size_t(tailLen) * sizeof(float));
    std::memset(mOutBufL.data() + tailLen, 0, size_t(mHopSize) * sizeof(float));
    std::memset(mOutBufR.data() + tailLen, 0, size_t(mHopSize) * sizeof(float));

    for (int i = 0; i < mFFTSize; ++i) {
        mOutBufL[i] += mSynthL[i];
        mOutBufR[i] += mSynthR[i];
    }
}

/* ── process() ───────────────────────────────────────────────────────────────── */

void PullStretch::process()
{
    const float *in  = mIn.buffer();
    float *outL = mOutL.buffer();
    float *outR = mOutR.buffer();
    const int N = FRAMELENGTH;

    // ── 0. Window option (v0.4.0): apply a size change (re-primes) ────────
    // Option values are 1-based (core OptionControl convention) → sel 0..2.
    const int wsel = std::max(1, std::min(mWindowOpt.value(), kNumWinSizes)) - 1;
    if (wsel != mWinSel) applyWindowSize(wsel);

    // ── 1. Level (per-block gain ramp) + Freeze held gate ─────────────────
    const float level = std::max(0.0f, std::min(mLevelIn.buffer()[0], 2.0f));
    mFrozen = (mFreezeIn.buffer()[0] > 0.5f);

    // ── 2. Feed capture buffer (sanitized: NaN/Inf never enter the ring,
    //       so the analysis FFT — and the freeze snapshot — stay finite) ────
    for (int s = 0; s < N; ++s) {
        mCapBuf[mCapWrite] = sanitize(in[s]);
        if (++mCapWrite >= kCapBufSize) mCapWrite = 0;
    }
    if (mInputCount < kMaxFFTSize)
        mInputCount = std::min(mInputCount + N, kMaxFFTSize);

    // ── 3. Startup silence — wait for the first full analysis window ──────
    if (mInputCount < mFFTSize) {
        for (int s = 0; s < N; ++s) outL[s] = outR[s] = 0.0f;
        mLastLevel = level;
        return;
    }

    // ── 4. Advance the synthesis pipeline by ONE stage ─────────────────────
    generateStage();

    // ── 5. Priming — hold output silent until the first frame commits ──────
    if (!mFirstFrameDone) {
        if (mGenStage >= mStagesNeeded) {
            commitFrame();
            mFirstFrameDone = true;
            mGenStage       = 0;
            mOutPos         = 0;
        }
        for (int s = 0; s < N; ++s) outL[s] = outR[s] = 0.0f;
        mLastLevel = level;
        return;
    }

    // ── 6. Commit the finished frame at the hop boundary ───────────────────
    // Requires BOTH the drain to reach the hop AND the pipeline to have
    // finished (v0.3.1 guard) — in lockstep on shipped firmware.
    if (mOutPos >= mHopSize && mGenStage >= mStagesNeeded) {
        commitFrame();
        mOutPos  -= mHopSize;
        mGenStage = 0;
    }
    // Defensive: never drain past the OLA buffer even if lockstep is broken.
    if (mOutPos > mFFTSize - N) mOutPos = mFFTSize - N;

    // ── 7. Drain N samples with per-sample level ramp; sanitize + limit ────
    const float *srcL    = mOutBufL.data() + mOutPos;
    const float *srcR    = mOutBufR.data() + mOutPos;
    float        lv      = mLastLevel;
    const float  lvStep  = (level - mLastLevel) * (1.0f / float(N));
    for (int s = 0; s < N; ++s) {
        outL[s] = softLimit(sanitize(srcL[s])) * lv;
        outR[s] = softLimit(sanitize(srcR[s])) * lv;
        lv += lvStep;
    }
    mLastLevel = level;
    mOutPos   += N;
}

} // namespace pullstretch
