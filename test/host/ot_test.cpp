// ot_test.cpp — host acceptance suite for src/ot_interp.h (Stage 0 of the
// morph suite plan).  Standalone: no PullStretch, no FFT stub needed.
//
// Build (repo root):
//   g++ -std=c++11 -O2 -fsanitize=address,undefined -Isrc \
//       test/host/ot_test.cpp -o /tmp/ot_test && /tmp/ot_test
//
// Exit 0 = all tests pass.  Each test prints PASS/FAIL with its metric.
#include "ot_interp.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>

using otx::OtParams;
using otx::ot_morph_mags;
using otx::crossfade_mags;

static int gFail = 0;

static void check(bool ok, const char *name, const char *fmt, double v)
{
    printf("%-34s %s  (", name, ok ? "PASS" : "FAIL");
    printf(fmt, v);
    printf(")\n");
    if (!ok) ++gFail;
}

// xorshift32 — deterministic random spectra
static uint32_t gRng = 0x12345678u;
static float frand()
{
    gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5;
    return float(gRng & 0x7FFFFFFFu) / float(0x7FFFFFFFu);
}

static void randomSpectrum(std::vector<float> &v)
{
    for (size_t k = 0; k < v.size(); ++k) v[k] = frand();
}

static double centroid(const std::vector<float> &mag, float p)
{
    double num = 0, den = 0;
    for (size_t k = 1; k + 1 < mag.size(); ++k) {
        const double w = (p == 2.0f) ? double(mag[k]) * mag[k] : mag[k];
        num += w * double(k);
        den += w;
    }
    return den > 0 ? num / den : 0.0;
}

static double totalMass(const std::vector<float> &mag, float p)
{
    double t = 0;
    for (size_t k = 1; k + 1 < mag.size(); ++k)
        t += (p == 2.0f) ? double(mag[k]) * mag[k] : mag[k];
    return t;
}

// Run OT on copies (the kernel clobbers its inputs).
static void runOT(const std::vector<float> &A, const std::vector<float> &B,
                  float m, bool logWarp, float p, std::vector<float> &out)
{
    std::vector<float> a(A), b(B);
    out.assign(A.size(), 0.0f);
    OtParams prm;
    prm.morph    = m;
    prm.logWarp  = logWarp;
    prm.exponent = p;
    ot_morph_mags(a.data(), b.data(), int(A.size()), out.data(), prm);
}

int main()
{
    const int N = 2049;   // Normal-window bin count

    // ── 1. Endpoint identity (m = 0 / 1 must be bit-exact copies) ─────────
    {
        std::vector<float> A(N), B(N), out;
        randomSpectrum(A); randomSpectrum(B);
        runOT(A, B, 0.0f, true, 2.0f, out);
        float err0 = 0;
        for (int k = 0; k < N; ++k) err0 = std::max(err0, fabsf(out[k] - A[k]));
        runOT(A, B, 1.0f, true, 2.0f, out);
        float err1 = 0;
        for (int k = 0; k < N; ++k) err1 = std::max(err1, fabsf(out[k] - B[k]));
        check(err0 == 0.0f && err1 == 0.0f, "endpoint identity",
              "maxAbsErr=%.2e", double(std::max(err0, err1)));
    }

    // ── 2. Glissando: two line spectra, m=0.5 → ONE peak between them ─────
    // This is the test that distinguishes transport from crossfade.
    {
        std::vector<float> A(N, 0.0f), B(N, 0.0f), out;
        A[40] = 1.0f;                 // "440 Hz"
        B[80] = 1.0f;                 // "880 Hz"

        // linear warp: expect the peak at bin 60
        runOT(A, B, 0.5f, false, 2.0f, out);
        double cLin = centroid(out, 2.0f);
        double near = 0, tot = 0;
        for (int k = 1; k < N - 1; ++k) {
            const double e = double(out[k]) * out[k];
            tot += e;
            if (std::abs(k - 60) <= 2) near += e;
        }
        check(fabs(cLin - 60.0) < 0.5 && near / tot > 0.99,
              "glissando linear (single peak)", "centroid=%.2f", cLin);

        // log warp: expect the peak at sqrt(40·80) ≈ 56.57
        runOT(A, B, 0.5f, true, 2.0f, out);
        double cLog = centroid(out, 2.0f);
        check(fabs(cLog - 56.57) < 0.6, "glissando log warp",
              "centroid=%.2f", cLog);

        // Contrast: crossfade must show TWO peaks (sanity of the test itself)
        std::vector<float> xf(N, 0.0f);
        crossfade_mags(A.data(), B.data(), N, 0.5f, xf.data());
        check(xf[40] > 0.4f && xf[80] > 0.4f, "crossfade keeps two peaks",
              "peaks=%.2f", double(xf[40]));
    }

    // ── 3. Mass conservation across the morph sweep ───────────────────────
    {
        std::vector<float> A(N), B(N), out;
        randomSpectrum(A); randomSpectrum(B);
        const double tA = totalMass(A, 2.0f), tB = totalMass(B, 2.0f);
        double worst = 0;
        for (float m : {0.25f, 0.5f, 0.75f}) {
            runOT(A, B, m, true, 2.0f, out);
            const double expect = pow(tA, 1.0 - m) * pow(tB, m);
            const double got    = totalMass(out, 2.0f);
            worst = std::max(worst, fabs(got - expect) / expect);
        }
        check(worst < 1e-3, "mass conservation (geometric)",
              "worstRelErr=%.2e", worst);
    }

    // ── 4. Silence fallback: OT vs silent side == plain crossfade ─────────
    {
        std::vector<float> A(N), B(N, 0.0f), out, ref(N);
        randomSpectrum(A);
        runOT(A, B, 0.3f, true, 2.0f, out);
        crossfade_mags(A.data(), B.data(), N, 0.3f, ref.data());
        float err = 0;
        long nf = 0;
        for (int k = 0; k < N; ++k) {
            if (!std::isfinite(out[k])) ++nf;
            err = std::max(err, fabsf(out[k] - ref[k]));
        }
        check(nf == 0 && err < 1e-5f, "silence fallback = crossfade",
              "maxAbsErr=%.2e", double(err));
    }

    // ── 5. Monotone centroid: shifted bumps, centroid rises with m ────────
    {
        std::vector<float> A(N, 0.0f), B(N, 0.0f), out;
        for (int k = -6; k <= 6; ++k) {           // Gaussian-ish bumps
            const float w = expf(-0.15f * float(k) * float(k));
            A[100 + k] = w;
            B[700 + k] = w;
        }
        double prev = -1;
        bool mono = true;
        for (float m = 0.0f; m <= 1.001f; m += 0.1f) {
            runOT(A, B, m, true, 2.0f, out);
            const double c = centroid(out, 2.0f);
            if (c < prev - 1e-6) mono = false;
            prev = c;
        }
        check(mono, "centroid monotone in morph", "final=%.1f", prev);
    }

    // ── 6. Noise ↔ tone: finite, conserved (worst case for artifacts) ─────
    {
        std::vector<float> A(N), B(N, 0.0f), out;
        randomSpectrum(A);                        // white
        B[300] = 1.0f;                            // tone
        const double tA = totalMass(A, 2.0f), tB = totalMass(B, 2.0f);
        long nf = 0;
        double worst = 0;
        for (float m : {0.1f, 0.5f, 0.9f}) {
            runOT(A, B, m, true, 2.0f, out);
            for (int k = 0; k < N; ++k)
                if (!std::isfinite(out[k])) ++nf;
            const double expect = pow(tA, 1.0 - m) * pow(tB, m);
            worst = std::max(worst,
                             fabs(totalMass(out, 2.0f) - expect) / expect);
        }
        check(nf == 0 && worst < 1e-3, "noise-to-tone", "worstRelErr=%.2e",
              worst);
    }

    // ── 7. Tiny inputs: finite output, silence fallback engages ───────────
    {
        std::vector<float> A(N, 1e-20f), B(N), out;
        randomSpectrum(B);
        long nf = 0;
        runOT(A, B, 0.5f, true, 2.0f, out);
        for (int k = 0; k < N; ++k)
            if (!std::isfinite(out[k])) ++nf;
        check(nf == 0, "denormal input finite", "nonFinite=%.0f", double(nf));
    }

    // ── 8. p = 1 + linear warp path ────────────────────────────────────────
    {
        std::vector<float> A(N), B(N), out;
        randomSpectrum(A); randomSpectrum(B);
        runOT(A, B, 0.0f, false, 1.0f, out);
        float err = 0;
        for (int k = 0; k < N; ++k) err = std::max(err, fabsf(out[k] - A[k]));
        double worst = double(err);
        const double tA = totalMass(A, 1.0f), tB = totalMass(B, 1.0f);
        runOT(A, B, 0.5f, false, 1.0f, out);
        const double expect = pow(tA, 0.5) * pow(tB, 0.5);
        worst = std::max(worst,
                         fabs(totalMass(out, 1.0f) - expect) / expect);
        check(worst < 1e-3, "p=1 / linear warp", "worst=%.2e", worst);
    }

    // ── 9. Deep-size smoke (4097 bins) — bounds + finiteness under ASan ───
    {
        const int ND = 4097;
        std::vector<float> A(ND), B(ND), out;
        randomSpectrum(A); randomSpectrum(B);
        long nf = 0;
        for (float m = 0.05f; m < 1.0f; m += 0.09f) {
            runOT(A, B, m, true, 2.0f, out);
            for (int k = 0; k < ND; ++k)
                if (!std::isfinite(out[k])) ++nf;
        }
        check(nf == 0, "deep-size sweep (4097 bins)", "nonFinite=%.0f",
              double(nf));
    }

    // ── 10. Anti-roughness: smoothing de-jags the transported envelope ────
    // A point-splat transport of two SMOOTH spectra is jagged at m=0.5; the
    // built-in smoothing (smoothHalf=2, default) must knock the bin-to-bin
    // roughness back down near the smooth-input level without moving energy.
    {
        std::vector<float> A(N, 0.0f), B(N, 0.0f);
        for (int k = 1; k < N - 1; ++k) {
            const double base = 1.0 / std::sqrt(double(k));
            A[k] = float(base + 0.6 * std::exp(-0.5 * std::pow((k - 60) / 25.0, 2)));
            B[k] = float(base + 0.6 * std::exp(-0.5 * std::pow((k - 500) / 120.0, 2)));
        }
        auto roughness = [&](const std::vector<float> &m) {
            double sum = 0, jump = 0;
            for (int k = 1; k < N - 1; ++k) sum += m[k];
            const double mean = sum / (N - 2);
            for (int k = 2; k < N - 1; ++k) jump += std::fabs(m[k] - m[k - 1]);
            return (jump / (N - 3)) / (mean + 1e-12);
        };
        auto runP = [&](bool logWarp, int smoothHalf, std::vector<float> &out) {
            std::vector<float> a(A), b(B); out.assign(N, 0.0f);
            OtParams prm; prm.morph = 0.5f; prm.exponent = 2.0f;
            prm.logWarp = logWarp; prm.smoothHalf = smoothHalf;
            ot_morph_mags(a.data(), b.data(), N, out.data(), prm);
        };
        auto mass = [&](const std::vector<float> &m) {
            double t = 0; for (int k = 1; k + 1 < N; ++k) t += double(m[k]) * m[k]; return t; };
        std::vector<float> raw, sm;
        runP(true, 0, raw);      // log, smoothing OFF
        runP(true, 2, sm);       // log, smoothing ON (default)
        const double rRaw = roughness(raw), rSm = roughness(sm);
        const double dMass = std::fabs(mass(sm) - mass(raw)) / mass(raw);
        check(rSm < 0.08 && rSm < rRaw * 0.4 && dMass < 1e-3,
              "smoothing de-roughens (log)", "roughSmoothed=%.3f", rSm);
    }

    printf("\n%s (%d failure%s)\n", gFail ? "SUITE FAILED" : "SUITE PASSED",
           gFail, gFail == 1 ? "" : "s");
    return gFail ? 1 : 0;
}
