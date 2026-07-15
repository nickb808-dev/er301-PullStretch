/* PullStretch.h — Paul's Extreme Time Stretch for ER-301 v0.7.0
 *
 * v0.7.0 — MORPH REMOVED.  The freeze-morph / slot-bank experiment (v0.5.0–
 * v0.6.0: Grab B, the 7-slot spectral scanner, the Morph knob, MorphWarp, and
 * the optimal-transport interpolation) is removed.  PullStretch is back to the
 * pure extreme time-stretch with the Freeze hold — the part that always sounded
 * good.  (The transport morph never resolved cleanly on hardware; see the
 * Entangler package for the shared OT kernel and that investigation.)
 *
 * CONCEPT
 * ───────
 * PullStretch (originally by Nasca Octavian Paul) performs extreme time
 * stretching by treating audio spectrally: for each output hop, a large FFT
 * window of the input is captured, the magnitudes of each frequency bin are
 * preserved, but the phases are replaced with random values.  The IFFT of
 * this phase-randomised spectrum is then overlap-added to the output.
 *
 * The result at high stretch ratios is a smeared, frozen, spectral "cloud"
 * that sounds like the input has been slowed down to an extreme degree while
 * retaining its harmonic character.
 *
 * WINDOW SIZE OPTION (v0.4.0)
 * ───────────────────────────
 * The analysis/synthesis window is now a runtime CONFIG option (unit menu,
 * preset-serialised, not CV-able):
 *
 *   Window 0 = Short   2048 pt  (~43 ms)  — tighter, more granular, cheapest
 *   Window 1 = Normal  4096 pt  (~85 ms)  — default, = v0.3.x sound
 *   Window 2 = Deep    8192 pt  (~171 ms) — the classic deep Paulstretch smear
 *
 * The smear character scales with window size; Deep gets closest to the
 * big-CPU original (Nasca used up to 65536).  The v0.2.1 8192→4096 downsize
 * predates the pipelined synthesis — with the pipeline, Deep's peak per-block
 * cost is still ≈ one 8192-pt FFT (~1 ms), inside the 2.67 ms budget.
 *
 * All buffers are allocated at the 8192 maximum and all three FFT plans are
 * created once in the constructor (and never destroyed — see STABILITY).
 * Switching sizes resets and re-primes the pipeline: a brief output mute
 * (window fill + one hop) — normal for a menu config change.  Hop is always
 * window/4 (75 % overlap), so the pipeline gets 4/8/16 blocks per hop and the
 * per-size stage schedule is derived at switch time (see PIPELINE below).
 *
 * NORMALISATION (v0.4.0 — numeric, window-agnostic)
 * ─────────────────────────────────────────────────
 * The scatter-adaptive synthesis scale is now computed NUMERICALLY from the
 * actual window at construction, per size:
 *
 *   coherent   (scatter 0):  gain = S · mean_n Σ_k w_a·w_s(n−kH)   → S_coh = 1/gain
 *   incoherent (scatter 1):  RMS  = S · √(mean(w_a²) · mean_n Σ_k w_s²(n−kH))
 *                                                                   → S_inc = 1/RMS
 *   effScale(s) = S_coh + s · (S_inc − S_coh)      (linear, as before)
 *
 * For Hann at 75 % overlap this reproduces the analytic 2/3 and 4/3 exactly;
 * numerically deriving it makes ANY window (e.g. the authentic Nasca
 * (1−x²)^1.25 curve, planned) drop-in without re-deriving constants.
 *
 * FREEZE (v0.3.0 — toggle latch in Lua; Evolving/Static via FreezeLock)
 * ─────────────────────────────────────────────────────────────────────
 * On the rising edge of the (held) Freeze gate the current spectrum is
 * snapshotted; while frozen, analysis is skipped and the snapshot drives
 * synthesis.  FreezeLock 0 = phases re-randomise per hop (evolving drone);
 * 1 = PRNG snapshot replayed per hop (fully static).  A window-size change
 * while frozen invalidates the snapshot (it was taken at the old size).
 *
 * PIPELINE (v0.3.0, generalised in v0.4.0)
 * ────────────────────────────────────────
 * Synthesis is spread across the hop, one stage per block:
 *   stage 0                     frameStart (read+window+forward FFT)
 *   stages 1..P                 phase-randomise chunks (P = 1/3/5 by size)
 *   stage P+1                   IFFT L
 *   stage P+2                   IFFT R (mono: deferred to the window stage)
 *   stage P+3 (Normal/Deep)     synthesis window (Short fuses it into P+2)
 * The frame commits (OLA shift+add) at the hop boundary once complete.
 * Peak per block ≤ one FFT of the current size.
 *
 * CPU ESTIMATE (AM3358 @ 600 MHz, mono)
 * ─────────────────────────────────────
 *   Short  2048:  ~6 %      Normal 4096:  ~10 %      Deep 8192:  ~20 %
 * (hardware to confirm Deep; during development measure before shipping)
 *
 * GLOBAL STABILITY NOTE
 * ─────────────────────
 * RFFT_destroy is intentionally NOT called in the destructor.  Testing shows
 * that calling RFFT_destroy with large plan sizes (> 1024) can corrupt the
 * ER-301 firmware's internal FFT state table, causing all FFT-using units to
 * produce glitched audio until firmware restart.  All three plans are made
 * once at construction and live until the .so unloads — nothing is ever
 * re-planned at runtime.
 *
 * MEMORY (per instance, v0.7.0 max-size allocation)
 * ─────────────────────────────────────────────────
 *   Capture buffer:              131072 × 4 B = 512 KB
 *   Analysis scratch:              8192 × 4 B =  32 KB
 *   Spectra (out/frozen/L/R):   4× 4097 × 8 B = 128 KB
 *   Synthesis + OLA (×4):       4× 8192 × 4 B = 128 KB
 *   Windows (3 sizes):          14336  × 4 B =  56 KB
 *   Total:                                     ≈ 856 KB */

#pragma once

// od/objects/Object.h and od/config.h included unconditionally so that
// SWIG sees od::Object when parsing this header — identical to GaborScatter.h.
#include <od/objects/Object.h>
#include <od/config.h>

#ifndef SWIGLUA
#include <hal/fft.h>
#include <cmath>
#include <algorithm>
#include <vector>
#endif

#include <cstdint>

namespace pullstretch {

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr int   kNumWinSizes = 3;
static constexpr int   kWinSizes[kNumWinSizes] = {2048, 4096, 8192};
static constexpr int   kMaxFFTSize = 8192;               // Deep window
static constexpr int   kMaxNumBins = kMaxFFTSize / 2 + 1;
static constexpr int   kCapBufSize = 131072;             // ~2.73 s at 48 kHz (power of 2)
static constexpr int   kCapBufMask = kCapBufSize - 1;    // wrap via mask (signed % by a
                                                         // power of 2 does NOT reduce to a
                                                         // single AND; the mask does)
static constexpr int   kSampleRate = 48000;
static constexpr float kPi         = 3.14159265358979f;
static constexpr float kTwoPi      = 2.0f * kPi;
static constexpr float kMinStretch = 1.0f;
static constexpr float kMaxStretch = 10000.0f;

// Sine lookup table for the per-bin random-phase generation.  Random angles
// don't need interpolation accuracy — a power-of-2 table with bit-mask
// wrap-around is ~13× faster than sinf/cosf on Cortex-A8 (≈5 cycles vs 65).
static constexpr int   kSineTableSize = 2048;     // power of 2
static constexpr int   kSineTableMask = kSineTableSize - 1;
static constexpr int   kCosOffset     = kSineTableSize / 4;  // cos(θ) = sin(θ + π/2)

// ── PullStretch class ─────────────────────────────────────────────────────────

class PullStretch : public od::Object
{
public:
    PullStretch();
    virtual ~PullStretch();

#ifndef SWIGLUA
    void process() override;

private:
    // ── Inlets ────────────────────────────────────────────────────────────
    od::Inlet  mIn        {"In"};      // live audio input
    od::Inlet  mStretchIn {"Stretch"}; // time stretch factor [1, 10000]
    od::Inlet  mScatterIn {"Scatter"}; // phase randomisation [0=coherent, 1=full]
    od::Inlet  mSprdIn    {"Sprd"};    // stereo decorrelation [0=mono, 1=wide]
    od::Inlet  mFreezeIn  {"Freeze"};  // freeze read head (gate > 0.5)
    od::Inlet  mLevelIn   {"Level"};   // output gain [0, 2]

    // ── Outlets ───────────────────────────────────────────────────────────
    od::Outlet mOutL {"OutL"};
    od::Outlet mOutR {"OutR"};

    // ── Capture buffer ────────────────────────────────────────────────────
    std::vector<float> mCapBuf;        // kCapBufSize — circular input ring
    int   mCapWrite   = 0;             // next write position in mCapBuf
    int   mInputCount = 0;             // samples written (clamps at kMaxFFTSize)
    float mReadHead   = 0.0f;          // analysis read position (samples, fractional)

    // ── Window size state (v0.4.0 — runtime, from the Window option) ──────
    int   mWinSel   = 1;               // 0=Short 1=Normal 2=Deep (applied)
    int   mFFTSize  = 4096;            // current window size
    int   mNumBins  = 4096 / 2 + 1;    // current RFFT bins
    int   mHopSize  = 4096 / 4;        // 75 % overlap hop
    // Per-size pipeline schedule (derived in applyWindowSize):
    int   mPhaseChunks  = 3;           // phase-loop chunks (1 / 3 / 5)
    int   mStagesNeeded = 7;           // stage counter value when frame complete
    bool  mFuseWindow   = false;       // Short: synthesis window fused into IFFT-R stage
    // Numeric per-size normalisation endpoints (computed at ctor):
    float mScaleCoh[kNumWinSizes] = {0};   // scatter = 0 endpoint
    float mScaleInc[kNumWinSizes] = {0};   // scatter = 1 endpoint

    // ── Analysis buffers — heap-allocated for NEON 16-byte alignment ──────
    // All sized for the MAX window (8192); the active size uses a prefix.
    std::vector<float>           mHannWins[kNumWinSizes]; // per-size Hann windows
    std::vector<float>           mFFTIn;      // kMaxFFTSize — windowed analysis scratch
    std::vector<complex_float_t> mFFTOut;     // kMaxNumBins — forward RFFT output

    // Sine lookup table — kSineTableSize + 1 with a guard entry equal to
    // sineTable[0] so a (mask + 1) read in interp paths never overruns.
    std::vector<float>           mSineTable;  // kSineTableSize + 1

    // ── Phase-randomised spectra ──────────────────────────────────────────
    std::vector<complex_float_t> mPhasedL;       // kMaxNumBins — L channel
    std::vector<complex_float_t> mPhasedR;       // kMaxNumBins — R channel

    // ── Freeze snapshot ───────────────────────────────────────────────────
    // Captured on the rising edge of the Freeze gate; invalidated by a
    // window-size change (it was taken at the old size).
    std::vector<complex_float_t> mFrozenSpectrum; // kMaxNumBins — slot A (complex)
    bool mFrozenActive = false;  // true while frozen and snapshot is valid
    bool mWasFrozen    = false;  // previous-block freeze state for edge detection

    // ── Synthesis scratch ─────────────────────────────────────────────────
    std::vector<float>           mSynthL;     // kMaxFFTSize — IRFFT output L
    std::vector<float>           mSynthR;     // kMaxFFTSize — IRFFT output R

    // ── Overlap-add output accumulators ──────────────────────────────────
    std::vector<float>           mOutBufL;    // kMaxFFTSize — OLA accumulator L
    std::vector<float>           mOutBufR;    // kMaxFFTSize — OLA accumulator R

    int  mOutPos  = 0;   // samples consumed from the current OLA frame
    bool mFrozen  = false;

    bool mPrevFreezeIn = false;

    // PRNG snapshot for FreezeLock = Static (replayed each hop while frozen).
    uint32_t mFrozenRandState = 0;

    // Previous block's clamped level — per-block gain ramp (no zipper).
    float mLastLevel = 1.0f;

    // ── Context-menu options ──────────────────────────────────────────────
    // 1-BASED values (the core OptionControl convention: set(choice) with
    // choice = 1..N — verified against the SDK's OptionControl.lua):
    //   Window     1 = Short (2048) · 2 = Normal (4096, default) · 3 = Deep (8192)
    //   FreezeLock 1 = Evolving (default) · 2 = Static
    // Old presets serialized FreezeLock = 0 (pre-menu 0-based era); the DSP
    // clamps low values to Evolving, so they load with unchanged behaviour.
    // (v0.4.0 also fixes that FreezeLock previously had NO menu wiring and
    // was unreachable in the UI.)
    od::Option mWindowOpt     {"Window",     2};
    od::Option mFreezeLockOpt {"FreezeLock", 1};

    // ── RFFT plans — one per window size, allocated once, never destroyed ──
    handle_rfft_t mFFTCfgs[kNumWinSizes]{};
    handle_rfft_t mFFTCfg{nullptr};    // active plan (alias into mFFTCfgs)

    // ── PRNG (xorshift32) ─────────────────────────────────────────────────
    uint32_t mRandState = 0xdeadbeef;
    float randUnipolar();

    // ── Pipelined synthesis (v0.3.0; per-size schedule v0.4.0) ────────────
    void generateStage();                 // advance the pipeline by one stage
    void frameStart();                    // stage 0: analyse / freeze / params
    void phaseBins(int b0, int b1);       // one chunk of the phase-randomise loop
    void windowStage();                   // synthesis window × adaptive scale
    void commitFrame();                   // OLA shift + accumulate finished frame
    void applyWindowSize(int sel);        // switch window: derive schedule, re-prime

    int   mGenStage       = 0;     // pipeline stage counter (mStagesNeeded = done)
    bool  mFirstFrameDone = false; // suppress output until the first frame commits
    // Per-frame parameters latched at frameStart().
    float mGenScatter    = 1.0f;
    float mGenSprd       = 0.0f;
    bool  mGenMono       = false;
    float mGenEffScale   = 1.0f;
    const std::vector<complex_float_t>* mGenSpec = nullptr;

#endif // SWIGLUA
};

} // namespace pullstretch
