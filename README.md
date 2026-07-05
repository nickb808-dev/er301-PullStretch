# Pull Stretch ER-301 Package

**Package:** `pullstretch` · **Version:** 0.4.0 · **Category:** Spectral

An extreme time-stretch / spectral smear for the ER-301, based on the Paulstretch algorithm
(Nasca Octavian Paul, 2006), adapted for **live, real-time input**. Patch any audio into the
chain and it becomes a slowly-evolving spectral cloud — from gentle smear at low stretch to a
frozen, glassy drone at extreme ratios. Works in mono or stereo.

## How it works

Incoming audio is continuously written into a ~2.73-second capture buffer. A "read head"
crawls through that buffer at `1/stretch` speed. Every hop (75% overlap) the unit takes an
FFT window at the read head, runs an FFT, **keeps each bin's magnitude but replaces its
phase** (randomised, blended with the original by `scatter`), and overlap-adds the inverse
FFT back to the output. Because the read head moves slowly, the same spectral slice is
re-synthesised many times with fresh phases. That is what produces the smooth, time-frozen
smear that retains the harmonic character of the source.

## Window size (v0.4.0)

The analysis/synthesis window is a unit-menu option (long-press the unit → Options),
saved with presets:

| Window | Size | Character | CPU (mono, est.) |
|---|---|---|---|
| **short** | 2048 (~43 ms) | tighter, more granular, fastest response | ~6% |
| **normal** | 4096 (~85 ms) | the v0.3.x sound (default) | ~10% |
| **deep** | 8192 (~171 ms) | the classic deep Paulstretch smear — closest to the original | ~20% |

Switching mutes briefly while the engine re-primes at the new size (and releases any
active freeze). Startup silence is one analysis window plus one hop
(~53 / 106 / 213 ms by size).

## Controls

| Control | Range | Default | Description |
|---------|-------|---------|-------------|
| **stretch** | 1 – 10000 | 10 | Time-stretch factor. 1 = real-time, 10 = slow smear, 1000+ = near-static drone. CV-patchable. |
| **scatter** | 0 – 1 | 1.0 | Phase randomisation. 0 = original phases preserved (coherent, chorus-like), 1 = fully random (classic Paulstretch smear). CV-patchable. |
| **sprd** | 0 – 1 | 0.75 | Stereo width. 0 = mono, 1 = fully decorrelated L/R. CV-patchable. |
| **freeze** | toggle | off | Latches the current spectral slice — push on / push off. The on-screen button shows the held state. CV-patchable (a trigger pulse toggles). |
| **level** | 0 – 2 | 1.0 | Output gain (unity ≈ 1.0). |

All continuous controls are CV-patchable via MonoBranch. Output level stays roughly constant
as you sweep `scatter`, so it changes spectral character rather than loudness.

## Freeze

**Freeze** latches the spectral snapshot taken at the moment you engage it. The read head
stops, and every hop re-synthesises that frozen slice — so the character holds indefinitely
regardless of what the live input does. It's a toggle: one trigger freezes, the next releases
(gate length doesn't matter), and the button reflects the on/off state.

A context-menu option, **FreezeLock**, controls how the frozen sound behaves:

- **Evolving** (default) — phases re-randomise each hop, giving a living, shifting drone.
- **Static** — the random-phase sequence is locked to the freeze moment, giving a fully
  static, unchanging tone.

(Long-press the unit → Options to set FreezeLock.)

## Stereo

In a stereo chain the unit produces two independent inverse FFTs (L and R) from the same
captured magnitudes, decorrelated by `sprd`. At `sprd = 0` both channels are identical
(mono) and the second IFFT is skipped to save CPU; at `sprd = 1` the channels are fully
independent for a wide, immersive field.

## Tips

- For the signature Paulstretch drone, set `stretch` high (500–5000) and `scatter` to 1.
- Lower `scatter` toward 0 for a more pitched, chorus-like freeze that tracks the source.
- Patch a slow LFO or envelope into `stretch` for breathing, accelerating/decelerating smears.
- Freeze a transient-rich moment (a vocal syllable, a cymbal) and sweep `scatter` to morph
  between a recognisable tone and pure spectral haze.
- CV slewing slower than the block rate (LFOs, envelopes) is smooth on all controls.

## Notes

- Startup silence is expected — the first analysis window must fill (see table above).
- Synthesis is **pipelined** across the blocks of each hop (v0.3.0, generalised per window
  size in v0.4.0), so per-block CPU is flat — no periodic spike. This is what removed the
  earlier hop-rate buzz and the engine-wide distortion it caused.
- Input is NaN/Inf-scrubbed and the output is soft-limited (v0.3.1) — glitchy sources can't
  poison the spectrum or freeze snapshot, and dense phase-stacking can't hard-clip the DAC.
- Deep mode's CPU on hardware is estimated, not yet measured — prefer mono until confirmed.

## Building

macOS (Docker):

```bash
make docker-image
make swig-docker  ER301_SDK=~/er-301
make docker-build ER301_SDK=~/er-301
make pkg
```

Linux (native):

```bash
make swig ER301_SDK=~/er-301
make build TOOLCHAIN=native ER301_SDK=~/er-301
make pkg
```

Output: `build/am335x/pullstretch-0.4.0.pkg`

## Credits

**Pull Stretch** is a tribute to [**Paulstretch**](https://hypermammut.sourceforge.net/paulstretch/)
("Paul's Extreme Time Stretch"), the extreme audio time-stretching algorithm and
software created by Nasca Octavian Paul in 2006. This package is based on
Paulstretch's code and algorithm, reimplemented and adapted for real-time,
live-input use on the ER-301.
