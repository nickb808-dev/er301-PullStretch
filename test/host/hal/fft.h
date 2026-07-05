// Host-test stub of hal/fft.h — iterative radix-2 complex FFT wrapped as RFFT.
// Forward: unscaled.  Inverse: 1/N (matches NE10 is_backward_scaled=1, the
// ER-301 semantics).
#pragma once
#include <cmath>
#include <cstdlib>

typedef struct { float r, i; } complex_float_t;

struct rfft_stub { int n; };
typedef rfft_stub *handle_rfft_t;

static inline handle_rfft_t RFFT_allocate(int n)
{
    handle_rfft_t h = (handle_rfft_t)malloc(sizeof(rfft_stub));
    h->n = n;
    return h;
}
static inline void RFFT_destroy(handle_rfft_t h) { free(h); }

static inline void fft_core(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t;
                     t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / len;
        const double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const double tr = re[b] * cr - im[b] * ci;
                const double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

static inline void ensure(double *&re, double *&im, int &cap, int n)
{
    if (cap < n) {
        free(re); free(im);
        re = (double*)malloc(n * sizeof(double));
        im = (double*)malloc(n * sizeof(double));
        cap = n;
    }
}

static inline void RFFT_forward(complex_float_t *out, const float *in, handle_rfft_t h)
{
    const int n = h->n;
    static double *re = nullptr, *im = nullptr; static int cap = 0;
    ensure(re, im, cap, n);
    for (int i = 0; i < n; ++i) { re[i] = in[i]; im[i] = 0.0; }
    fft_core(re, im, n);
    for (int k = 0; k <= n / 2; ++k) { out[k].r = (float)re[k]; out[k].i = (float)im[k]; }
}

static inline void RFFT_inverse(float *out, const complex_float_t *in, handle_rfft_t h)
{
    const int n = h->n;
    static double *re = nullptr, *im = nullptr; static int cap = 0;
    ensure(re, im, cap, n);
    // x = conj(FFT(conj(X))) / N  — reconstruct the Hermitian spectrum first.
    for (int k = 0; k <= n / 2; ++k) { re[k] = in[k].r; im[k] = -double(in[k].i); }
    for (int k = n / 2 + 1; k < n; ++k) { re[k] = in[n - k].r; im[k] = double(in[n - k].i); }
    fft_core(re, im, n);
    const double inv = 1.0 / double(n);
    for (int i = 0; i < n; ++i) out[i] = (float)(re[i] * inv);
}
