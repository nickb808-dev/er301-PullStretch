/* ot_interp.h — 1D optimal-transport (displacement) interpolation between
 * two magnitude spectra.  Shared morph-suite kernel (see
 * crossfilter/MORPH-SUITE-PLAN.md, Stage 0).
 *
 * WHAT / WHY
 * ──────────
 * A plain crossfade interpolates spectra VERTICALLY: bin k of A fades into
 * bin k of B, so a 500 Hz peak morphing to 3 kHz passes through "both peaks
 * at half level" — a mix, not a morph.  Displacement interpolation moves the
 * spectral mass HORIZONTALLY along the frequency axis (Henderson & Solomon,
 * "Audio Transport", DAFx 2019): the peak glissandos through the bins in
 * between.  In 1D the optimal plan is the monotone quantile matching, so the
 * whole thing is a two-pointer merge — O(nBins), no matrices, no iteration.
 *
 * ALGORITHM (ot_morph_mags)
 * ─────────────────────────
 *  1. mass[k] = mag[k]^p          (p = OtParams::exponent; fast paths 1, 2)
 *  2. normalise each side to Σ = 1, remembering totals totA, totB
 *  3. two-pointer quantile match: each matched mass quantum q with source
 *     bin a and destination bin b is deposited at the fractional bin
 *        linear warp:  f = (1−m)·a + m·b
 *        log warp:     f = a^(1−m) · b^m      (glissandi in cents, not Hz)
 *     with a 2-tap linear splat into the output mass array
 *  4. output scale: totOut = totA^(1−m) · totB^m (geometric — loudness
 *     interpolates in dB, no mid-morph energy bulge), then mag = mass^(1/p)
 *  5. bin 0 (DC) and bin nBins−1 (Nyquist) are PINNED — excluded from
 *     transport, plain-lerped in the magnitude domain
 *
 * DEGENERATE CASES
 * ────────────────
 * If either side's total mass is below OtParams::massFloor the transport
 * plan is undefined (OT to silence collapses everything to one bin), so the
 * function falls back to a plain magnitude crossfade.  m ≤ 0 / m ≥ 1 return
 * exact copies of A / B (bit-exact endpoint identity by construction).
 *
 * CONTRACT
 * ────────
 *  - magA and magB are CLOBBERED (reused as mass/probability scratch) —
 *    callers pass copies, never live analysis buffers.
 *  - magOut must not alias magA/magB.  All arrays length nBins.
 *  - No allocation, no exceptions, C++11, header-only: safe for the ER-301
 *    audio thread and for host test builds alike.
 *  - NaN/Inf are NOT scrubbed here — per house style the capture path
 *    sanitizes before anything reaches analysis (see PullStretch v0.3.1).
 *
 * CPU NOTE (Cortex-A8)
 * ────────────────────
 * ≤ 2·nBins quanta per call; log warp costs one log2 per pointer advance and
 * one exp2 per quantum.  These use the pure-arithmetic detail::plog2/pexp2
 * (see below) — NOT libm.  That is both a speed win at Deep (4097 bins) and,
 * more importantly, the am335x CORRECTNESS fix: libm log2f/exp2f/powf from a
 * package .so miscompute on the ER-301 (same boundary bug as sinf/cosf), which
 * scattered the transported energy to wrong bins — the audible "aliased" morph.
 */

#pragma once

#include <cmath>
#include <cstdint>   // uint32_t — bit-twiddling in the libm-free plog2/pexp2

namespace otx {

struct OtParams {
    float morph;      // 0 = pure A … 1 = pure B (clamped)
    float exponent;   // mass = mag^p.  2 = energy transport (default, fast),
                      // 1 = magnitude transport (fast); other p uses powf.
    bool  logWarp;    // true: geometric bin interpolation (musical intervals)
    float massFloor;  // total-mass floor for the silence fallback
    int   smoothHalf; // ANTI-ROUGHNESS (see note): energy-preserving triangular
                      // smoothing half-width applied to the transported
                      // magnitudes.  0 = off; 2 (default) = 5-tap.
    OtParams() : morph(0.0f), exponent(2.0f), logWarp(true),
                 massFloor(1e-12f), smoothHalf(2) {}
};

/* WHY SMOOTHING (smoothHalf)
 * ──────────────────────────
 * A point-splat transport map deposits each mass quantum at a fractional
 * destination bin.  Where the map STRETCHES the axis (strongly so for the log
 * warp at low bins) destination bins are skipped → gaps; where it COMPRESSES,
 * mass piles → spikes.  The resulting magnitude envelope is jagged/comb-like at
 * intermediate morph (endpoints are the original smooth spectra), and once it
 * is resynthesised with random phase that jaggedness is audible as roughness /
 * distortion.  A short energy-preserving triangular smoothing of the OUTPUT
 * magnitudes removes it while leaving the transported shape (centroid, single-
 * peak glissando) intact.  half = 2 keeps a single-bin tonal glissando inside
 * ±2 bins; larger values smooth more but blur sharp peaks. */

namespace detail {

/* ── libm-free log2 / exp2 / pow (am335x package-trig fix) ──────────────────
 * On the ER-301's am335x, libm transcendentals called from a package .so
 * miscompute across the package→firmware boundary (confirmed for sinf/cosf;
 * the log-warp transport's log2f/exp2f/powf are the same class and were the
 * audible "aliased" morph on hardware — energy placed at wrong bins).  These
 * pure-arithmetic versions reference NO libm symbol.  Accuracy over the ranges
 * used here (log2 of bins 1..4096, exp2 of 0..12): |log2 err| ≲ 1.2e-4,
 * rel exp2 err ≲ 8e-6, composite bin-placement error < 0.22 bin (well inside
 * the kernel's ±smoothHalf splat).  sqrtf is left as-is — it is a VFP hardware
 * instruction, not a flagged libm call. */
inline float pexp2(float x)                       // 2^x, any x
{
    int   i = int(x);
    float f = x - float(i);
    if (f < 0.0f) { f += 1.0f; i -= 1; }          // floor split, no floorf
    // 2^f — degree-6 Taylor of exp(f·ln2).
    const float p = 1.0f + f * (0.6931471806f + f * (0.2402265070f
                  + f * (0.0555041087f + f * (0.0096181291f
                  + f * (0.0013333559f + f * 0.0001540353f)))));
    union { float f; uint32_t u; } v;
    int e = i + 127;
    if (e <= 0)   return 0.0f;                     // underflow → 0
    if (e >= 255) e = 254;                         // clamp overflow
    v.u = uint32_t(e) << 23;                       // 2^i
    return p * v.f;
}

inline float plog2(float x)                       // log2(x), x > 0
{
    if (x <= 0.0f) return -127.0f;
    union { float f; uint32_t u; } v;  v.f = x;
    int   e = int((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;       // mantissa → [1,2)
    float m = v.f;
    if (m > 1.41421356f) { m *= 0.5f; e += 1; }    // centre → [√½,√2): tiny t
    const float t = m - 1.0f;
    // log2(1+t) = 1.442695·ln(1+t), degree-7.
    const float p = t * (1.4426950409f + t * (-0.7213475204f + t * (0.4808983470f
                  + t * (-0.3606737602f + t * (0.2885390082f
                  + t * (-0.2404491733f + t * 0.2060992910f))))));
    return float(e) + p;
}

inline float ppow(float base, float e)            // base^e, base > 0
{
    if (base <= 0.0f) return 0.0f;
    return pexp2(e * plog2(base));
}

inline float massOf(float mag, float p)
{
    if (p == 2.0f) return mag * mag;
    if (p == 1.0f) return mag;
    return ppow(mag, p);
}

/* Energy-preserving triangular smoothing of mag[i0..i1], in place, NO heap
 * allocation (a small circular history of the last 2H+1 ORIGINAL values feeds
 * a delayed write, so each output is produced only after its input neighbours
 * are safely saved).  Edge bins use a symmetrically shrunk window.  The total
 * energy Σ mag² over the interval is restored after smoothing, so loudness (and
 * the kernel's mass-conservation guarantee) is preserved.  The conserved
 * quantity is Σ mag^p (the transport "mass"), so the rescale matches the active
 * exponent p — Σmag² for energy transport, Σmag for magnitude transport. */
inline float massOf(float mag, float p);      // fwd (defined below)
inline void smoothMags(float *mag, int i0, int i1, int half, float p)
{
    if (half < 1) return;
    if (half > 31) half = 31;                 // hist[] bound
    const int W = 2 * half + 1;
    if (i1 - i0 + 1 < W) return;              // too few bins to smooth

    double e0 = 0.0;
    for (int k = i0; k <= i1; ++k) e0 += double(massOf(mag[k], p));

    float hist[64];                           // W ≤ 63
    for (int r = i0; r <= i1 + half; ++r) {
        if (r <= i1) hist[r % W] = mag[r];    // save original before any overwrite
        const int c = r - half;               // center emitted with `half` delay
        if (c < i0) continue;
        if (c > i1) break;
        int uh = half;                        // shrink window at the edges
        if (c - i0 < uh) uh = c - i0;
        if (i1 - c < uh) uh = i1 - c;
        float acc = 0.0f, wsum = 0.0f;
        for (int d = -uh; d <= uh; ++d) {
            const float w = float(half + 1 - (d < 0 ? -d : d));
            acc  += w * hist[(c + d) % W];
            wsum += w;
        }
        mag[c] = acc / wsum;                  // mag[c] no longer needed as input
    }

    double e1 = 0.0;
    for (int k = i0; k <= i1; ++k) e1 += double(massOf(mag[k], p));
    if (e1 > 1e-30) {
        // preserve Σ mag^p — libm-free (p==2 is the only caller; sqrtf is HW).
        const float ratio = float(e0 / e1);
        const float g = (p == 2.0f) ? sqrtf(ratio)
                      : (p == 1.0f) ? ratio
                      : ppow(ratio, 1.0f / p);
        for (int k = i0; k <= i1; ++k) mag[k] *= g;
    }
}

inline float magOf(float mass, float p)
{
    if (mass <= 0.0f) return 0.0f;
    if (p == 2.0f) return sqrtf(mass);       // sqrtf = VFP hardware instr
    if (p == 1.0f) return mass;
    return ppow(mass, 1.0f / p);
}

} // namespace detail

/* Plain magnitude crossfade — the "Fade" morph law and the silence
 * fallback.  Same signature family as ot_morph_mags for easy A/B. */
inline void crossfade_mags(const float *magA, const float *magB, int nBins,
                           float morph, float *magOut)
{
    const float m  = morph < 0.0f ? 0.0f : (morph > 1.0f ? 1.0f : morph);
    const float wa = 1.0f - m;
    for (int k = 0; k < nBins; ++k)
        magOut[k] = wa * magA[k] + m * magB[k];
}

/* Displacement-interpolate |A| → |B|.
 * magA / magB: length nBins, CLOBBERED (become normalised mass arrays).
 * magOut:      length nBins, must not alias magA/magB. */
inline void ot_morph_mags(float *magA, float *magB, int nBins,
                          float *magOut, const OtParams &prm)
{
    const float m = prm.morph < 0.0f ? 0.0f
                  : (prm.morph > 1.0f ? 1.0f : prm.morph);
    const float p = prm.exponent;

    // Exact endpoints — bit-identical to the inputs, no transport noise.
    if (m <= 0.0f) {
        for (int k = 0; k < nBins; ++k) magOut[k] = magA[k];
        return;
    }
    if (m >= 1.0f) {
        for (int k = 0; k < nBins; ++k) magOut[k] = magB[k];
        return;
    }

    const int i0 = 1;              // DC pinned
    const int i1 = nBins - 2;      // Nyquist pinned
    if (i1 < i0) { crossfade_mags(magA, magB, nBins, m, magOut); return; }

    // Save pinned-bin magnitudes BEFORE clobbering the inputs.
    const float dcA = magA[0],         dcB = magB[0];
    const float nyA = magA[nBins - 1], nyB = magB[nBins - 1];

    // 1–2. magnitudes → masses, in place; totals.
    float totA = 0.0f, totB = 0.0f;
    for (int k = i0; k <= i1; ++k) {
        const float a = detail::massOf(magA[k], p);
        const float b = detail::massOf(magB[k], p);
        magA[k] = a;  totA += a;
        magB[k] = b;  totB += b;
    }

    // Silence fallback: transport to/from (near-)silence is undefined and
    // sounds like a pitch collapse — crossfade instead.  Recover the
    // magnitudes from the masses (~1-ulp round trip for p=2 (sqrtf(x·x)),
    // exact for p=1).
    if (totA <= prm.massFloor || totB <= prm.massFloor) {
        const float wa = 1.0f - m;
        for (int k = i0; k <= i1; ++k)
            magOut[k] = wa * detail::magOf(magA[k], p)
                      +  m * detail::magOf(magB[k], p);
        magOut[0]         = (1.0f - m) * dcA + m * dcB;
        magOut[nBins - 1] = (1.0f - m) * nyA + m * nyB;
        return;
    }

    const float invA = 1.0f / totA, invB = 1.0f / totB;
    for (int k = i0; k <= i1; ++k) { magA[k] *= invA; magB[k] *= invB; }

    for (int k = 0; k < nBins; ++k) magOut[k] = 0.0f;

    // 3. Two-pointer quantile match with 2-tap splat.
    const float wa = 1.0f - m, wb = m;
    int   ia = i0, ib = i0;
    float ra = magA[ia], rb = magB[ib];
    float lgA = prm.logWarp ? detail::plog2(float(ia)) : 0.0f;
    float lgB = prm.logWarp ? detail::plog2(float(ib)) : 0.0f;

    while (true) {
        while (ia <= i1 && ra <= 0.0f) {
            ++ia;
            if (ia <= i1) {
                ra = magA[ia];
                if (prm.logWarp && ra > 0.0f) lgA = detail::plog2(float(ia));
            }
        }
        while (ib <= i1 && rb <= 0.0f) {
            ++ib;
            if (ib <= i1) {
                rb = magB[ib];
                if (prm.logWarp && rb > 0.0f) lgB = detail::plog2(float(ib));
            }
        }
        if (ia > i1 || ib > i1) break;

        const float q = ra < rb ? ra : rb;

        // Destination as a fractional bin (libm-free exp2 — am335x fix).
        float f = prm.logWarp ? detail::pexp2(wa * lgA + wb * lgB)
                              : wa * float(ia) + wb * float(ib);
        if (f < float(i0)) f = float(i0);
        if (f > float(i1)) f = float(i1);
        const int   fl   = int(f);
        const float frac = f - float(fl);
        magOut[fl] += q * (1.0f - frac);
        if (fl < i1) magOut[fl + 1] += q * frac;
        else         magOut[fl]     += q * frac;   // clamp spill at the top

        ra -= q;
        rb -= q;
    }
    // Float drift can leave a ~1e-6 residual on one side at loop exit; it is
    // dropped (measured: total-mass error < 1e-6 rel — see ot_test.cpp #3).

    // 4. Geometric total interpolation, then mass → magnitude (libm-free).
    const float totOut = detail::ppow(totA, wa) * detail::ppow(totB, wb);
    for (int k = i0; k <= i1; ++k)
        magOut[k] = detail::magOf(magOut[k] * totOut, p);

    // 4b. De-jag the transported envelope (see WHY SMOOTHING above).  Skipped
    //     at the exact endpoints, which return earlier as bit-exact copies.
    detail::smoothMags(magOut, i0, i1, prm.smoothHalf, p);

    // 5. Pinned bins.
    magOut[0]         = wa * dcA + wb * dcB;
    magOut[nBins - 1] = wa * nyA + wb * nyB;
}

} // namespace otx
