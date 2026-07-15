/* trig_poly.h — libm-free polynomial sine/cosine for building tables at ctor.
 *
 * WHY (am335x): runtime sinf/cosf called from a package .so miscompute on the
 * ER-301's am335x — a package→firmware libm call-boundary bug documented in the
 * stolmine/habitat projects (feedback_package_trig_lut): sinf/cosf are confirmed
 * bad, logf/expf are NOT flagged, and the emulator is unaffected (shows only on
 * hardware).  These pure-arithmetic approximations reference NO libm trig
 * symbol, so table construction is correct on hardware and host alike.
 *
 * Used ONLY at construction (sine lookup table + Hann windows), never per
 * sample.  Accuracy ≈ 6e-6 over the full circle — ample for a Hann window and
 * irrelevant for the random-phase sine table.  Reduction assumes x ≥ 0 (all our
 * call sites pass non-negative phase) and uses an int cast, not floorf (floorf
 * is also libm).
 */
#pragma once

namespace trigpoly {

static constexpr float kPi     = 3.14159265358979f;
static constexpr float kTwoPi  = 6.28318530717959f;
static constexpr float kHalfPi = 1.57079632679490f;

static inline float psin(float x)
{
    // Reduce to [0, 2π):  x ≥ 0, so int() truncation equals floor().
    x -= kTwoPi * float(int(x * (1.0f / kTwoPi)));
    if (x < 0.0f) x += kTwoPi;            // guard tiny negative from rounding
    if (x > kPi) x -= kTwoPi;             // → [-π, π]
    if (x > kHalfPi) x = kPi - x;         // → [-π/2, π/2]   (sin(π−x) = sin x)
    else if (x < -kHalfPi) x = -kPi - x;
    const float x2 = x * x;               // odd Taylor series, degree 9
    return x * (1.0f + x2 * (-0.166666667f + x2 * (0.00833333333f
             + x2 * (-0.000198412698f + x2 * 0.0000027557319f))));
}

static inline float pcos(float x) { return psin(x + kHalfPi); }

} // namespace trigpoly
