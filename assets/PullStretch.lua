-- PullStretch.lua — ER-301 unit wrapper for Pull Stretch v0.7.0
--
-- SIGNAL FLOW
-- ───────────
--   In (chain audio) ──→ [capture buffer, ~2.73 s, NaN-scrubbed]
--                              ↓ (read head crawls at 1/stretch speed)
--                    [Hann window → RFFT, 4096-pt]
--                              ↓
--                    [keep magnitudes, randomise phases per bin]
--                              ↓
--                    [IRFFT → overlap-add] (L and R independent)
--                              ↓
--              OutL → Out1,  OutR → Out2
--
--   PullStretch requires audio input.  There is ~85 ms of silence on load
--   while the first analysis window fills (4096 samples at 48 kHz), plus one
--   hop (~21 ms) of pipeline priming.
--
-- CONCEPT
-- ───────
--   Classic Paulstretch algorithm (Nasca Octavian Paul, 2006) adapted for
--   real-time live input on the ER-301.  Every 21 ms (kHopSize = 1024 samples,
--   75% overlap) a 4096-sample window of the input is RFFT-analysed; the bin
--   magnitudes are preserved but phases are randomised and two independent
--   IFFTs (L, R) are overlap-added to the output.  A "read head" into the
--   capture buffer advances at 1/stretch speed, causing the input to be
--   re-analysed many times before the spectral content changes — producing
--   the signature smeared, frozen, atmospheric stretch effect.  Synthesis is
--   pipelined across the hop (one stage per block) for flat CPU.
--
-- CONTROLS
-- ────────
--   stretch   1–10000      Time stretch factor (10 = slow smear, 1000 = near-static drone)
--   scatter   0–1          Phase randomisation (0 = frozen coherent, 1 = full Paulstretch)
--   sprd      0–1          Stereo width (0 = mono, 1 = fully decorrelated L/R)
--   freeze    toggle       Latching freeze — one trigger freezes the spectral
--                          snapshot, the next releases (FreezeLock option:
--                          Evolving/Static phases while frozen)
--   level     0–2          Output gain (unity = 1.0; output soft-limited pre-level)
--
-- NOTES
-- ─────
--   • At scatter=0, phases are preserved → output sounds like a frozen chorus.
--   • At scatter=1, phases are fully random → classic Paulstretch smear.
--   • Freeze + scatter=1 produces an evolving drone from a held spectral snapshot.
--   • Stretch CV: ±5 V maps to ±5× on top of the bias; use the bias for base rate.
--   • ~106 ms startup silence is expected — analysis window fill + one hop priming.

local app      = app
local Class    = Class or require "Base.Class"
local Unit     = require "Unit"
local GainBias = require "Unit.ViewControl.GainBias"
local Gate     = require "Unit.ViewControl.Gate"
local Encoder  = require "Encoder"
local MenuHeader    = require "Unit.MenuControl.Header"
local OptionControl = require "Unit.MenuControl.OptionControl"

local libpullstretch = require "pullstretch.libpullstretch"

local PullStretch = Class {}
PullStretch:include(Unit)

function PullStretch:init(args)
  args.title    = "Pull Stretch"
  args.mnemonic = "PL"
  Unit.init(self, args)
end

-- ── Signal graph ──────────────────────────────────────────────────────────────

function PullStretch:onLoadGraph(channelCount)
  local ps = self:addObject("ps", libpullstretch.PullStretch())

  -- Route chain input to the analysis inlet
  connect(self, "In1", ps, "In")

  -- ── Stretch — time stretch factor [1, 10000] ──────────────────────────
  local stretchParam = self:addObject("stretchParam", app.GainBias())
  local stretchRange = self:addObject("stretchRange", app.MinMax())
  stretchParam:hardSet("Bias", 10.0)
  connect(stretchParam, "Out", stretchRange, "In")
  connect(stretchParam, "Out", ps, "Stretch")
  self:addMonoBranch("stretchMod", stretchParam, "In", stretchParam, "Out")

  -- ── Scatter — phase randomisation [0, 1] ─────────────────────────────
  local scatterParam = self:addObject("scatterParam", app.GainBias())
  local scatterRange = self:addObject("scatterRange", app.MinMax())
  scatterParam:hardSet("Bias", 1.0)
  connect(scatterParam, "Out", scatterRange, "In")
  connect(scatterParam, "Out", ps, "Scatter")
  self:addMonoBranch("scatterMod", scatterParam, "In", scatterParam, "Out")

  -- ── Sprd — stereo decorrelation [0, 1] ───────────────────────────────
  local sprdParam = self:addObject("sprdParam", app.GainBias())
  local sprdRange = self:addObject("sprdRange", app.MinMax())
  sprdParam:hardSet("Bias", 0.75)
  connect(sprdParam, "Out", sprdRange, "In")
  connect(sprdParam, "Out", ps, "Sprd")
  self:addMonoBranch("sprdMod", sprdParam, "In", sprdParam, "Out")

  -- ── Freeze — latching toggle (push-on / push-off) ────────────────────
  -- Toggle mode: each rising edge flips the comparator's HELD output, so the
  -- on-screen Freeze control reflects the latched on/off state (a plain gate
  -- comparator only showed the momentary input).  The DSP simply follows this
  -- held gate.  A brief trigger pulse latches; the next pulse releases.
  local freezeGate = self:addObject("freezeGate", app.Comparator())
  freezeGate:setToggleMode()
  connect(freezeGate, "Out", ps, "Freeze")
  self:addMonoBranch("freezeMod", freezeGate, "In", freezeGate, "Out")

  -- ── Level — output gain [0, 2] ────────────────────────────────────────
  local levelParam = self:addObject("levelParam", app.GainBias())
  local levelRange = self:addObject("levelRange", app.MinMax())
  levelParam:hardSet("Bias", 1.0)
  connect(levelParam, "Out", levelRange, "In")
  connect(levelParam, "Out", ps, "Level")
  self:addMonoBranch("levelMod", levelParam, "In", levelParam, "Out")

  -- ── Output routing (stereo) ───────────────────────────────────────────
  connect(ps, "OutL", self, "Out1")
  if channelCount > 1 then
    connect(ps, "OutR", self, "Out2")
  end
end

-- ── Unit menu: config options (v0.4.0) ───────────────────────────────────────
-- Window (2048/4096/8192) and FreezeLock (Evolving/Static) are od::Options —
-- static config, preset-serialised, not CV-able.  (v0.4.0 also fixes that
-- FreezeLock previously had no menu wiring and was unreachable in the UI.)

function PullStretch:onShowMenu(objects, branches)
  local controls = {}
  local menu = { "optionsHeader", "window", "freezeLock" }

  controls.optionsHeader = MenuHeader {
    description = "Pull Stretch Options"
  }

  controls.window = OptionControl {
    description = "Window",
    option      = objects.ps:getOption("Window"),
    choices     = { "short 43ms", "normal 85ms", "deep 171ms" },
    descriptionWidth = 2,
    muteOnChange = true,   -- structural change: mute the chain during re-prime
  }

  controls.freezeLock = OptionControl {
    description = "Freeze Phases",
    option      = objects.ps:getOption("FreezeLock"),
    choices     = { "evolving", "static" },
    descriptionWidth = 2,
  }

  return controls, menu
end

-- ── Encoder views ─────────────────────────────────────────────────────────────

function PullStretch:onLoadViews(objects, branches)
  local controls = {}
  local views = {
    expanded  = {"stretch", "scatter", "sprd", "freeze", "level"},
    collapsed = {},
  }

  -- Stretch: 1–10000, coarse steps of ~100
  local stretchMap     = app.LinearDialMap(1.0, 10000.0)
  local stretchGainMap = app.LinearDialMap(0.0, 10000.0)
  stretchMap:setCoarseRadix(100)
  stretchGainMap:setCoarseRadix(1000)

  controls.stretch = GainBias {
    button      = "stretch",
    description = "Time Stretch Factor  1=real-time  10=slow  1000=drone",
    branch      = branches.stretchMod,
    gainbias    = objects.stretchParam,
    range       = objects.stretchRange,
    biasMap     = stretchMap,
    initialBias = 10.0,
    gainMap     = stretchGainMap,
  }

  controls.scatter = GainBias {
    button      = "scatter",
    description = "Phase Scatter  0=coherent freeze  1=full Paulstretch smear",
    branch      = branches.scatterMod,
    gainbias    = objects.scatterParam,
    range       = objects.scatterRange,
    biasMap     = Encoder.getMap("[0,1]"),
    initialBias = 1.0,
    gainMap     = Encoder.getMap("[-1,1]"),
  }

  controls.sprd = GainBias {
    button      = "sprd",
    description = "Stereo Width  0=mono  1=fully decorrelated L/R",
    branch      = branches.sprdMod,
    gainbias    = objects.sprdParam,
    range       = objects.sprdRange,
    biasMap     = Encoder.getMap("[0,1]"),
    initialBias = 0.75,
    gainMap     = Encoder.getMap("[-1,1]"),
  }

  controls.freeze = Gate {
    button      = "freeze",
    description = "Freeze Read Head — hold spectral slice, vary phases",
    branch      = branches.freezeMod,
    comparator  = objects.freezeGate,
  }

  controls.level = GainBias {
    button      = "level",
    description = "Output Level (unity = 1)",
    branch      = branches.levelMod,
    -- v0.3.1: [0,2] map — the DSP accepts up to 2× but the old [0,1] map
    -- stopped the dial at unity, making the documented range unreachable.
    gainbias    = objects.levelParam,
    range       = objects.levelRange,
    biasMap     = Encoder.getMap("[0,2]"),
    initialBias = 1.0,
    gainMap     = Encoder.getMap("[-1,1]"),
  }

  return controls, views
end

return PullStretch
