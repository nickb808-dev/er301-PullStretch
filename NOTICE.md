# NOTICE — PullStretch attribution & provenance

**PullStretch** is an original implementation of the *Paulstretch* extreme
time‑stretching algorithm for the Orthogonal Devices **ER‑301** Sound Computer.

## Algorithm attribution

The Paulstretch technique — analyse a long windowed FFT frame, keep the bin
magnitudes, replace each bin's phase with a random value, inverse‑FFT and
overlap‑add — was created by **Nasca Octavian Paul** (2006). PullStretch
implements that published algorithm. Algorithms and mathematical methods are
not protected by copyright; this attribution is given as a courtesy and to
credit the technique's inventor.

The spectral **morph** added in v0.5+ (and in the sibling *Entangler* unit) uses
1‑D optimal‑transport interpolation after **Henderson & Solomon, "Audio
Transport" (DAFx 2019)**, independently implemented in `src/ot_interp.h`.

## Independent implementation — not derived from PaulXStretch or Nasca's source

PullStretch was written natively against the ER‑301 SDK. It **contains no source
code from, and is not a derivative work of**, PaulXStretch
(github.com/essej/paulxstretch, GPLv3) or Nasca Octavian Paul's original
`Stretch.cpp` (GPLv2). A structural comparison of the core DSP confirms fully
independent expression:

| Aspect | Nasca / PaulXStretch | PullStretch |
|---|---|---|
| Framework | JUCE (desktop/plugin) | ER‑301 `od::Object` + Lua/SWIG |
| FFT | FFTW / pffft / vDSP | ER‑301 NE10 RFFT (`hal/fft.h`) |
| PRNG | `std::mt19937` (Mersenne Twister) | xorshift32 |
| Phase generation | `mag·cos(phase)`, `mag·sin(phase)` via `std::cos/sin` | power‑of‑two **sine lookup table** with bit‑mask wrap; scatter‑blend; independent L/R decorrelation |
| Windowing | selectable window enum | Hann with numeric, scatter‑adaptive OLA normalisation |
| Synthesis | block overlap‑add + onset detection | per‑block **pipelined** synthesis, custom OLA, freeze snapshot |

The only necessary commonality is the algorithm itself (`re = mag·cos(phase)`,
`im = mag·sin(phase)`, then IFFT + overlap‑add) and the standard textbook Hann
window `0.5·(1 − cos(2πi/N))` — neither of which is copyrightable expression.

## Naming

The unit is named **"PullStretch,"** deliberately distinct from "Paulstretch"
and "PaulXStretch," and does not imply endorsement by their authors.

---
*This NOTICE records provenance; it is not legal advice. See LICENSE for the
terms under which PullStretch itself is distributed.*
