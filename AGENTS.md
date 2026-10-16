# AGENTS.md — Handleman

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you tell
anybody this works.

---

## What the plugin is

A hand-cranked camera and projector, as an FFGL 2.1 effect (`HM01`, shown as
`SW Handleman`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS
`.bundle` and (in CI, never run) a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/handleman`. Local only: `~/dev/handleman`, `main`, no remote.

Built 2026-10-16 in one session at Allan's request — "a hand cranked film
camera/projector … simple options for sepia/black and white images … a slightly unsteady
frame rate, and some common 'old film' distortions, ideally with a one hit 'leader' which
collapses the images to blank 'film cells' like the end of a reel" — to
`specs/BRIEF.md` + `BRIEF-ADDENDUM.md` and `specs/SPEC-handleman.md` (written by the same
session). The template is **gate**, the fleet's projector, almost whole: its shutter, hold
segments, weave, scratches, dust, hair, frame line, cos⁴, harness and verify. wetplate
gave the spectral-weights method; photofinish the resize trap.

**The neighbour to stay distinct from is gate.** Gate is a motor-driven projector at a
fixed rate showing a colour print that fades. Handleman is two hands, a camera as well as a
projector, a silver print toned or tinted, and the end of a reel. It deliberately has no
splices, cue dots, lamps or colour fading (gate's), no strip geometry or countdown
(gauge's), no negative or scan (rebate's).

---

## The one idea

**Model the two hands, not the look.** Before about 1927 the camera and the projector were
both turned by hand, eight frames a turn, about two turns a second. A hand is not a motor:

| the machine | what comes out |
| --- | --- |
| the cameraman's speed ripples once a turn (the arm pushes harder on the down stroke), with a harmonic, and his count drifts over six and nine turns | **the scene's time runs fast and slow**: each frame holds the instant the camera's crank passed it, so motion speeds up and slows down. Camera Hand 1: over a turn, 0.77 to 1.37 of real time |
| a fixed shutter angle | **breathing**: a frame cranked slowly was exposed for longer and prints lighter, a fast one darker, in step with the motion's speed (log light rises with log exposure at the print's gamma) |
| the hand's jerk while the shutter was open | a small **frame-to-frame density flicker** at the crank rate (a lognormal factor, sigma 0.15 × the hand's deviation) |
| the projectionist's speed ripples and drifts too | **holds of varying length**; the shutter, geared to the handle, **beats against 60 Hz at a wandering rate** |
| between the two, a loop of film | **a delay**, bounded: the projector runs `A_c + A_p` frames behind the camera, so it never asks for a frame not yet exposed, and the camera is never more than `2 (A_c + A_p)` ahead |
| gate's shutter, weave, grit, dust and hair | flicker at the beat, each frame landing off, scratches that stay put, specks that ride a frame, a hair in the gate |
| orthochromatic negative: blind to red | **red lips and skin go dark, skies white**: red exposes 4.8% of white, green 33%, blue 65% |
| a characteristic curve | contrast about a fixed middle grey; soft knees at the base and the print's black |
| silver grain | **grain that belongs to the frame**: still through a hold, new at every pull-down |
| sepia toning converts the silver | **density × (0.70, 0.93, 1.35)** per channel: the shadows go brown, white stays white |
| a tint is a dye in the base | **light × the tint** per channel: white goes amber (blue, green), black stays black |
| the roll runs out | **fog from the edges, then blank cells**: white in the aperture, black at the frame line; the loop is lost and the frame line crawls up the screen |

### The pipeline

1. **Clock and cranks** (CPU, double). `dt` is the host's real seconds since the last frame
   (readout's unit voting; clamped to [0, 0.25 s]; the first frame a nominal 1/60). `F +=
   fps × dt`; each hand's partials advance in phase by `2π r fps dt` (r in cycles per
   frame); its amplitude slews toward `deviation × Σ(share / r) / 2π` at no more than 0.25
   × fps frames a second. Camera phase `F − A_c w_c`; projector `F − A_p w_p − (A_c + A_p)`.
2. **Shutter** (CPU, closed form, gate's). The display frame's exposure is the projector's
   phase from the last frame's end to this one's; the projector frames it overlaps, each
   weighted by `CumulativeOpen` over its share, divided by `open × (p1 − p0)`. The phase is
   taken as linear across one display frame.
3. **Pull-downs** (CPU). Every projector frame up to the newest, in order, goes through
   `Reel::Pull`: the slip, the offset, the tail. Run Out's press is taken after them, from
   the next pull-down.
4. **Camera** (CPU + one GPU capture). Every frame the camera's phase passed since the last
   display frame is exposed to the host's picture now (one capture into one ring layer,
   shared), each with its exposure `fps / ω_c × exp(0.15 s_c N)`.
5. **Ring** (GPU). An RGBA8 texture array; a layer is free when no retained frame uses it;
   it grows (copying) when none is free, to 32 at most. Frames older than two above the
   oldest segment are dropped.
6. **Frame table** (CPU → uniforms). Every film frame the segments can show, from two above
   the oldest to two below the newest: its layer (−1 a blank cell, −2 no film), its fog, its
   exposure, its grain seed.
7. **Segments** (CPU → uniforms). Per projector frame: the weave, Framing and the reel's
   offset (whole frames move which frame is in the gate, the rest is drawn), the dust on its
   own frame, the hair; the scratches alive; the print data row.
8. **Output** (GPU, to the host). Per segment, per pixel: which frame of the strip (above,
   own, below) and where on it; the picture through the stock's Smits weights, the frame's
   exposure and fog, the curve; or the frame line at the print's black; or a blank cell at
   the base; grain on the silver; emulsion scratches take silver and tint off, white dust
   takes silver off; toning and the tint; base scratches, dark dust and the hair take light
   off. The weighted sum, cos⁴, the sRGB encode, Mix.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.{h,cpp}` | The hands, the shutter, the weave, the print's marks, the curve, the fog, the reel and the tones, in double. Every constant with its reason. The `Perturb` bits. |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, with inverses; the option lists. |
| `source/Spectral.h` | GENERATED by `tools/spectral_weights.py`: each stock's seven Smits weights. |
| `source/FilmRing.{h,cpp}` | The texture array, its framebuffer, growth and copy-on-resize. |
| `source/Shaders.{h,cpp}` | Capture, resample, output. |
| `source/Handleman.{h,cpp}` | The plugin: parameters, the clock, the cranks, the ring's bookkeeping, the frame table, the passes, the test hooks. |
| `tools/hmtest/` | The harness: the coded picture, every check, the negative controls, bench, pipe, shader dump. |
| `tools/sweep.py`, `tools/verify.sh`, `tools/spectral_weights.py` | No dead control; all of it; the stocks. |

---

## Traps

In the order they bit, or would have.

### ☠️ A frame code has to be wide enough for the longest run

The first `--exposure` read a slope of 0.32 against a gamma of 1.40. The plugin was right
(frame 27 exposed ×1.585 printed exactly 1.39 gamma lighter than frame 26 at ×1.256); the
coded picture's frame number was 12 bits, and the 240 Hz run is 4,800 frames, so pictures
4,096 frames apart decoded alike and the intervals between them were nonsense. It is 14
bits now (16,384 frames, three times the longest run).

### ☠️ The loop is threaded with the first picture

On the first frame the projector is up to `A_c + A_p` frames behind the camera, and every
frame between them is filled with the one picture there is. So for the first second the
projector shows several frames carrying the same picture: "a pull-down that did not bring
a newer picture", and two lit frames that blend to a crisp picture. `--crank` judges from
a second in. A real projector threaded with leader would show the same thing as blank
cells; showing the first picture was the better start in a show.

### ☠️ A display frame can be entirely dark

With two blades at 180° a closed stretch is a quarter of a frame; at a slow crank (or 240
Hz) a whole display frame fits in it, its weight is 0 and the probe over the weight is
0/0. The run-out's half-way probe landed on one. Every run-out judgement now uses only
display frames the steady clock says one pull-down lit.

### ☠️ The frame line is darker than anything else, but the picture is not light

The first "frame line" detector counted rows below half the clear base, and found the
coded picture's grey (0.23 of the base) as frame line on every frame after a new roll. A
frame line prints at the print's black, 0.005 of the base; the threshold is 0.05.

### ☠️ When only the camera's hand moves, the projector is steady but late

`--crank`'s camera-hand case predicts every switch from a steady clock — offset by `A_c`,
the loop, which is constant when the projector's hand is still. Without the offset the
harness's lit sets are wrong by a fraction of a frame and every judgement near a pull-down
fails.

### ☠️ gate's default shutter is too deep at 16 fps

Gate ships 3 blades at 270° because 2 at 180° beat ±25–29% at 12 Hz. At 16 fps a display
frame is a quarter of a projector frame, not 0.4, so the shutter is averaged over less of
its cycle, and once the projector's hand moves the beat about, a display frame can sit
wholly inside an opening. 270° stepped by up to 38% of the light from one display frame to
the next (95th percentile) on a flat grey; 315° by 21%. The default is 315°; see the
survey below. The camera's per-frame jitter was 0.35 × the deviation and is 0.15 for the
same reason: at 16 frames a second it is a flicker in the 3–30 Hz band.

### ☠️ `Scoped2DTextureBinding` binds GL_TEXTURE_2D only

The ring is a texture array; it is bound with `ScopedTextureBinding( GL_TEXTURE_2D_ARRAY,
… )` after a `ScopedSamplerActivation`. The fleet's pattern leaves units other than 0 bound
at scope exit (the activation's destructor returns to unit 0 before the inner bindings
unbind); gate does the same and Arena has not minded.

### ☠️ An interval is not the exposure

`--exposure` estimates each frame's camera speed from the capture times either side of it.
The exposure is the speed at the instant of capture times the hand's jerk; the interval
averages two frames, which attenuates the 4-frame partial to 0.64 of itself, and knows
nothing of the jerk. The jerk is noise on y and does not bias the slope; the attenuation
does a little, so the tolerance is five standard errors of the regression plus 5% of
gamma. Measured slope: 1.440 at both rasters against 1.40 (1.394 to 1.421 with the first,
larger jitter).

### ☠️ "As deep as it needs, plus two" asked every frame is "two more every frame"

The ring was asked for `max( depth, needed ) + 2` layers on every frame, and `Ensure()`
reallocates (copying every layer) whenever it is asked for more than it has. So it grew by
two layers a frame until it hit the cap of 32: 112 MB at 720p and a gigabyte at 4K, where
the defaults need six layers. Nothing in the picture showed it; `verify.sh`'s bench line
did. It now asks for more only when a capture has no free layer, and `--crank` checks the
deepest the ring got against the loop's bound (12 against 15 with both hands at full).

### ☠️ A filtered read is exact on one renderer and not another

The output pass first read the host's picture for Mix and alpha with `texture( Source,
uv × MaxUV )`, as gate does. On this Mac's GPU that returns the texel exactly; on Apple's
software renderer the interpolated `uv` lands a hair off the texel centre, and at a hard
alpha edge (0.25 to 1) 151 pixels of Mix 0 came back blended with their neighbour. The
host's picture is now read with `texelFetch` at the pixel's own texel (nearest, scaled if
the host's viewport differs from the picture), so Mix 0 is bit for bit everywhere, and
`--alpha` asks for exactly that.

### The worktree guard judges by the session's directory

Since v0.1.0 the shared checkout is `~/Projects/resolume/handleman`, so work in a worktree
and use `git -C <literal absolute path>` always: a bare `git` after a `cd`, or a `$VAR`
path, is judged by wherever the session started.

### ☠️ Hurrying the camera makes the action drag, not rush

The README shipped to v0.1.0's release worktree saying the opposite, and the site entry and
the release notes copied it before the guide's author caught it on a render. A hurried
crank exposes frames closer together in the scene's time, so played back at a steady rate
the action is slowed (overcranking is slow motion); a slow crank spaces them out and the
action rushes, and those frames were exposed for longer, so **rushing and brightening go
together**. Check any new prose about the camera hand against that sentence.

### Release traps (v0.1.0, 2026-10-16)

- **The auto-mode classifier refused `gh workflow run release.yml --ref <branch>`** (a
  pre-tag Windows build) as a CI bypass, and Allan chose the by-hand route. The DLL was built
  on win-lab: tar the worktree (`--exclude ./build* ./.git`, COPYFILE_DISABLE=1, the FFGL
  submodule included), scp to `C:\build\hm-rel.tgz`, a `.cmd` that extracts with Windows'
  `tar`, calls `vcvars64.bat` and runs the CI's configure line, started by WMI
  (`Invoke-CimMethod Win32_Process Create`) and polled for its `JOB-DONE` marker. About 9
  minutes, most of it vcpkg's first GLEW install.
- **gate's `Diag.cpp` reads `FILAMENT_LOG_DIR`**; Handleman's read it too until v0.1.0
  (now `HANDLEMAN_LOG_DIR`). Grep a forked plugin for its parent's names before release.
- **The README's attributions block and `ATTRIBUTIONS.md` are generated** since
  registration; the derived entry is `attributions/derived.json` in stoatworks-backend.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put back
before the output pass); every `ffglex::Scoped*` clears to 0 on exit, so every allocation
and the print-data upload happen before the passes bind anything; a reallocated texture is
a cleared texture, so the ring is copied across a resize; `SetParamInfo` clamps a STANDARD
default into 0..1; the core is an **OBJECT** library; `SetTextParameter` must return
`FF_SUCCESS` for the About block; the harness drives a synthetic clock; an option's range
reads back 0..1; Resolume's clock overflows a float, so both cranks are double and the
shader sees only weights, offsets and layers; GLSL 4.10's reserved words (`packed`
included) are not identifiers (verify greps); no `M_PI`, no `far`, no `near`; a hardware
linear filter may weight with 8 bits, so the ring's bilinear is written out; randomness is
PCG integer hashing, on the CPU and in the shader; the demo clips carry alpha.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at 320x180 and
1280x720 and on Apple's software renderer at 320x180 in `verify.sh`. `kU` = 2^-24.

| check | what it measures | tolerance and where it comes from | raster / renderer dependence |
| --- | --- | --- | --- |
| `--crank` still | which display frame's picture each display frame shows, against the steady clock and the stated shutter | exact (a picture is a 14-bit number read from black and white blocks; a block within 3% of its references) | none: the band is at least 4 rows and each block W/16 wide, read at its centre half; no fractional geometry |
| `--crank` projector hand | the pictures are the steady camera's; holds and waits within the hand's bounds | holds `[⌊60/f(1+s)⌋ − 2, ⌈60/f(1−s)⌉ + 2]` display frames (a speed within 1 ± s, two display frames for a dark one and a lit pull-down); waits `[0, 2 A_p 60/f/(1−s) + 4]` | none |
| `--crank` camera hand | every switch where the steady projector, `A_c` behind, puts it; a newer picture at every pull-down; the scene's rate over a turn | rate within `1 ± s` ± two display frames over the turn | none |
| `--crank` both | pictures counted over 29 s against the crank; the ring's deepest | `2 (A_c + A_p) + 3` frames: the loop's swing and a frame each end; depth `⌈2 (A_c + A_p)⌉ + 7` (two above the gate, two straddled, one exposing, two of growth) | none |
| `--exposure` | the regression slope of log light on log camera interval | five standard errors (from the residuals) + 5% of gamma | none: one pixel of a flat grey; 240 Hz for the intervals |
| `--flicker` | gate's: every DFT bin against the shutter's Fourier series | `2 × 8 kU × max y` + the series' tail | none |
| `--tone` neutral | R = G = B on every grey patch | exactly 0 (the same float operations per channel) | none |
| `--tone` gamma | density against log exposure between patches 8 codes apart on the straight part | the knees' softplus at least 0.7 from either knee, `2 × 0.12 e^(−0.7/0.12)`, + 64 kU, over the pair's log exposure | none: patches read at their centres, whole texels |
| `--tone` sepia, tints | ratios of ln T or of T to the neutral render | 1e-5 (a few float roundings over \|ln T\| ≥ 0.1) | none |
| `--tone` stocks | the grey code whose print matches a primary's, interpolated in linear light | 1% (linear interpolation across one 8-bit code of a smooth curve) | none |
| `--hold` | consecutive display frames lit by one pull-down, relative | 8 kU: a float weight times T, divided by the double weight, two frames | none |
| `--scratch` | whole-pixel scratches' columns: exactly half the light (base), exactly the clear base (emulsion), nothing else touched | 32 × 2 kU on each ratio | whole pixels only where W is a multiple of 64 (320 and 1280 are); otherwise skipped, and the check says so |
| `--weave` | gate's: sd and lag-1 of 1000 frames' offsets from a step's row sum, at gamma 1 so the curve is a straight line through the edge pixel | Bartlett, 5 standard errors | the sd in px scales with H; the statistic does not |
| `--runout` blank | rows off the frame line on frames one pull-down lit | 8 kU of 1 | none |
| `--runout` line | the frame line's height in rows | `⌊kFrameLine H⌋` to `⌈kFrameLine H⌉`: each edge is a pixel centre either side | the count scales with H |
| `--runout` crawl | the line's middle from one pull-down to the next | ±1 row (each end is a whole row) | none |
| `--runout` tails | Framed: no dark rows after the stated pull-downs; Off the End: `10^0.06` everywhere | no dark row; 16 kU | none |
| `--resize` | the picture decoded on every display frame of a resized run against an unresized one | exact | the resize is 1.5x whatever the raster |
| `--alpha` | output alpha 1 at Mix 1; the host's RGBA at Mix 0 | exact both (the host's texel by `texelFetch`) | none |

Deliberately NOT relied on: a hardware-filtered read of the ring (the bilinear is by
hand), exact cancellation between two formulas, and any GLSL `sin`/`cos` in anything
checked (the hands are CPU double; the shader's `exp`/`log` appear only inside ratios).
Grain, dust and the hair are drawn but only swept and held, not measured.

### The negative controls

`hmtest --negative` runs ten, and `--perturb BITS` runs any check verbosely against one.
Each perturbs the *plugin's* model — a `Perturb` bit the shipped plugin carries at zero —
never the harness's expectation. At 320x180:

| perturbation | what fails |
| --- | --- |
| the projector without the loop's delay | `--crank`: 2 of 12 — 553 of 1140 display frames off the steady projector's switches, 11 pull-downs bring no newer picture |
| the camera's exposure from the projector's speed | `--exposure`: 2 of 3 — slope −0.03 against 1.40 |
| the shutter sampled at one instant | `--flicker`: 4 of 12 — every spectrum (amplitude 1.618 against 0.293); the frequency lines pass, which is gate's trap |
| sepia as a tint | `--tone`: 2 of 16 — the ln-ratio law off by 10, and white goes brown (0.98 0.66 0.32) |
| orthochromatic seeing luminance | `--tone`: 3 of 16 — red reads 0.2126 against 0.0482, green 0.7152 against 0.3331, blue 0.0722 against 0.6459 |
| grain reseeded every display frame | `--hold`: 1 of 2 — frames within a hold differ by 32% |
| scratches in picture space | `--scratch`: 3 of 3 — 95 + 95 samples off, 198 elsewhere changed |
| weave with no correlation | `--weave`: 2 of 6 — lag-1 0.02 and 0.03 against 0.75 |
| the frame line drawn half as tall | `--runout`: 2 of 7 — every frame line short; the crawl never finds a full one |
| a resize clears the ring | `--resize`: 2 of 2 — 11 of 92 frames wrong, the resize frame unreadable |

### The mutation

One character of the shipped GLSL, on a committed `source/Shaders.cpp`: in `density()`,
`float line = BaseDensity - GreyLog - Gamma * …` → `… + Gamma * …` (the curve's slope
inverted). Caught by `--crank` (7 of 12: white prints darker than black, so no picture
decodes), `--tone` (4 of 16: slopes off by 1.2, 2.8 and 4.4; sepia's white goes to 0.06),
`--exposure` (2 of 3: nothing decodes), `--resize` (2 of 2) and `--runout` (1 of 7: the
new roll never reads). `--flicker`, `--hold`, `--scratch`, `--weave` and `--alpha` passed,
which is right: each reads ratios or weights that do not depend on the curve. Reverted with
`git -C ~/dev/handleman checkout -- source/Shaders.cpp`, rebuilt, all five green again.

---

## Decisions taken without asking

- **Two hands, bounded in phase, not in speed.** Each crank's phase is `F − A w(t)` with
  `|w| ≤ 1`, so the loop between camera and projector is bounded by construction; a hand
  modelled as a speed noise would integrate to a random walk and the loop would grow
  without limit. The speed is what the control sets: `A` is chosen so the partials, peaking
  together, deviate by exactly the hand's percentage (50% at Camera Hand 1, 40% at
  Projector Hand 1). Frequencies are in cycles per frame, so the loop in frames is the same
  at any Crank Rate.
- **A hand's control jumping** moves its amplitude at no more than 0.25 × the crank rate
  frames a second, so the camera always turns forwards. The projector runs `A_c + A_p`
  behind, so pushing both hands up at once can lengthen the loop faster than the projector
  turns: it then stands still for a moment (its phase is clamped, never run backwards)
  while the camera gets ahead.
- **The partials** (chosen, not measured from a cameraman): the camera's once a turn (40%
  of the deviation), its second harmonic (10%), and drifts of 70 and 43 frames (30%, 20%);
  the projector's once a turn (55%), harmonic (15%), a 50-frame drift (30%). Starting
  phases from a fixed seed, so two instances turn alike.
- **Exposure ∝ 1 / the camera's speed** at the display frame that captures the frame, times
  `exp(0.15 s_c N)` for the hand's jerk while the shutter was open. The camera's shutter
  angle itself is not a control: the operator set the aperture for it.
- **A capture is the host's current picture.** Frames the camera passes in one display
  frame share it (at 60 Hz the camera never passes two: 24 fps at its fastest, 50% over
  and the slew's 25% more, is 42).
- **The frame below the gate**, not yet exposed when the projector is right behind the
  camera, shows the newest picture (gate's choice for the same case).
- **The film**: 35 mm silent full aperture. The clip fills the projector's aperture
  (0.906 × 0.6795 in); the exposed picture runs 2.2% beyond it each end (camera aperture
  0.709 in), edge-repeated; the frame line is the rest of the 0.748 in pitch, 5.74% of the
  picture's height. The clip is stretched to the output's aspect, not pillarboxed to 4:3.
- **The curve**: a straight line of slope −gamma through 18% grey printing at 18% of the
  clear base (so Contrast never moves the middle), softplus knees of width 0.12 at the base
  (density 0.06) and the print's black (2.4). Chosen, not fitted to a stock. White prints at
  0.96 of the base at gamma 1.4; screen white is the clear base.
- **Orthochromatic** is an assumed sensitivity shape (the bromide's blue band, a gap near
  500 nm, erythrosine's green band to 600 nm) through Smits' basis in 6504 K daylight.
  **Panchromatic** is Rec. 709 luminance. Both stated in `tools/spectral_weights.py`.
- **Tones** are two laws. Sepia scales the silver's density by (0.70, 0.93, 1.35); tints
  multiply the light by amber (1.00, 0.70, 0.28), blue (0.35, 0.55, 1.00) and green (0.45,
  0.85, 0.40). The numbers are chosen by eye; the laws are what the checks hold.
  **Colour** puts each channel through the curve on its own (Stock does nothing there).
- **Grain** is density noise, sigma `0.22 × Grain × √(silver / 2.34 + 0.03)`, in clumps
  0.02 mm across; finer than a pixel it is one draw a pixel scaled by the clump's size in
  pixels.
- **Run Out**: fog from the next pull-down, rising linearly in a fog level over `Run Out
  Time × Crank Rate` frames, mapped to an exposure `(2^(7f) − 1) / 127 × 6` whites, 70% of
  it pouring in from each edge of the film over 0.2 of the picture's height. A frame at fog
  1 is a blank cell: no silver in the aperture, the frame line at the print's black. The
  loop starts slipping when the fog passes 0.5, building over 8 pull-downs to 0.06 of a
  pitch a pull-down. **Rolling** keeps crawling; **Framed** reframes after 24 blank
  pull-downs (the strip keeps creeping until the frame line has gone, then the claw seats);
  **Off the End** runs 24 blank cells then whips the tail out at 0.35 of a pitch a
  pull-down and shows the bare lamp, 10^0.06 of the base. A second press threads a new
  roll: at least 6 blank cells (a third of the ramp), the fog clearing over the ramp, the
  strip settling.
- **The offset wraps at half a frame.** The strip's offset is kept in [−0.5, 0.5) and the
  whole frames move which frame is in the gate, so a long roll never runs off the ring; at a
  wrap the picture repeats once (the slip is real, the frames it skips are not available).
- **Defaults** (on Resolume's demo clips and the flicker survey): 16 fps, both hands 0.3
  (15% and 12%), 3 blades at 315°, Orthochromatic, Sepia, gamma 1.4, Grain 0.35, Weave
  0.35, Scratches 0.35, Dust 0.35, Hair 0.25, Run Out Time 1 s, Rolling, Vignette 0.35.
- **About and attributions are provisional hand copies** (`guide = ""`): Handleman is not
  registered in the website's `projects.json`, so the sync scripts have nothing to generate
  from. No `FUNDING.yml` or issue templates yet.
- **Test hooks live in the shipped plugin** (`Perturb`, `Probe`, the scratch override and
  the `…ForTest` getters), inert in a host.

### The flicker survey (2026-10-16)

A flat 18% grey through `hmtest --pipe` at 60 Hz, 160x90, 9 s, the marks off, the light
at the centre:

| setting | strongest 3–30 Hz line | frame-to-frame step, 95th pct | peak to peak |
| --- | --- | --- | --- |
| 3 blades 270°, hands 0.3/0.3 (gate's shutter), jitter 0.35 | 3.9% at 14 Hz | 39% | 96% |
| 3 blades 270°, hands still | 13.6% at 12 Hz | 21% | 21% |
| 2 blades 180° | 30.5% at 27 Hz | 193% | 240% |
| **3 blades 315°, hands 0.3/0.3, jitter 0.15 (the default)** | **2.0% at 14 Hz** | **21%** | **55%** |
| 3 blades 315°, camera hand only | 5.8% at 12 Hz | 13% | 51% |
| 3 blades 315°, projector hand only | 1.5% at 15 Hz | 20% | 21% |

Most of the default's peak to peak is the camera hand's breathing, under 3 Hz (the turn at
2 Hz, the drifts slower). The steps are the shutter beating against 60 Hz as the
projector's hand moves it. Camera Hand 0 stops the breathing; Projector Hand 0 makes the
beat regular.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-10-16)

Every number is `tools/verify.sh` on this machine against a fresh universal Release build,
at 320x180 and 1280x720 and on the software renderer, or `hmtest` at 1280x720.

- **The hands.** Still: 598 of 598 display frames show the steady clock's picture. The
  projector's hand alone (40%): all 319 pictures are the steady camera's, in order; holds 2
  to 8 display frames; waits 1 to 10, within the loop's 19.9. The camera's hand alone
  (50%): 1140 of 1140 display frames switch where the steady projector 2.64 frames behind
  puts them; every one of 304 pull-downs brings a newer picture; over a turn the scene runs
  at 0.77 to 1.37 of real time. Both: 466 pictures in 29.0 s against the crank's 463.7,
  and the ring never deeper than 12 layers (bound 15).
- **Exposure.** Log light on log camera interval: slope 1.440 at 320x180 and 1280x720
  against gamma 1.40 (±0.289).
- **Flicker.** gate's numbers, at Handleman's rates: 24 fps × 2 blades 0.29271 at 12 Hz;
  × 3 blades 0.19514; 16 fps × 3 blades 0.29271 at 12 Hz; 18 fps × 3 blades at 240° 0.08953
  at 6 Hz. Every DFT bin within 2.3e-8.
- **Tone.** Black & White exactly neutral; the straight line's slope gamma to 5.7e-4,
  1.1e-3 and 1.4e-3 at 0.6, 1.4 and 2.2; sepia's ln-ratio law to 1.3e-6 on 220 patches;
  the three tints' ratio to 1.1e-7; Colour on a grey is Panchromatic B&W bit for bit; the
  stocks' weights read back as 0.0482 / 0.3331 / 0.6460 (ortho) and 0.2126 / 0.7152 /
  0.0722 (pan) against the script's 0.0482 / 0.3331 / 0.6459 and Rec. 709.
- **Hold.** 173 pairs of display frames within a hold identical to 2.2e-7 relative; 62
  across a pull-down differ.
- **Scratch.** Columns 400–419 lose exactly half the light and 720–739 are exactly the clear
  base on 38 frames of a moving, weaving picture; no other column changes.
- **Weave.** 1000 frames: sd 2.886 px against 3.129 (±0.662), lag-1 0.718 against 0.750;
  vertical 1.920 against 1.877, 0.764.
- **Run Out.** Blank cells, exactly the base, from 69 display frames after the press; the
  frame line 41 to 42 rows (41.32) at 720 and 10 to 11 (10.33) at 180; climbing 47.55 rows a
  pull-down at 720; Framed gone and staying gone; Off the End the bare lamp (1.1482);
  pressed again, the picture back and framed within 161 display frames.
- **Resize.** 92 of 92 display frames after a resize to 1.5x and back show the unresized
  run's picture, the resize frame included.
- **Alpha.** 1 everywhere at Mix 1; the host's picture and alpha at Mix 0.
- **Negative controls.** All ten fail their check. **Mutation** caught (above).
- **No dead controls**, all 19; with the context table emptied, Hair, Run Out Time and Tail
  go dead, which is what the table is for.
- **Every shader compiles** through `glslc`; no reserved word.
- **`--pipe`**: two frames for two and a half, exit 2 on an unknown cue, exit 1 on a closed
  stdout and on a failed render, an option steps, an event fires on its frame, a slider
  ramps.
- **Render cost** (`hmtest --bench`, the defaults, best of three, `glFinish` both sides, a
  shared GPU): 0.050 / 0.066 / 0.175 ms at 720p / 1080p / 4K (0.3 / 0.4 / 1.0% of a 60 fps
  frame), holding 21 / 47 / 190 MB (a six-layer ring; twelve layers with both hands at full).
- **The bundle** is universal, exports `_plugMain`, carries `com.stoatworks.ffgl.handleman`,
  ad-hoc signs, and `oxbow` reports `SW Handleman` / `HM01` / `effect` and renders 120
  frames through `plugMain`.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** On Windows: built by MSVC 19.44 on win-lab
  with no warnings, and gated in Arena 7.27.1 (Mesa llvmpipe) twice, 9 of 9 checks both times.
  At the defaults 8 controls read live and 9 inconclusive (the projection moves by itself on
  the gate's still carrier, floor 11 levels); with Camera Hand, Projector Hand, Grain, Weave,
  Scratches, Dust and Hair held at 0 in every row but their own (plugin-bench's committed
  `arena/expect/handleman.json`), 12 live and 5 inconclusive: Crank Rate, Blades, Shutter
  Angle and Projector Hand (they change the light over time, which Arena's slow software
  frames average away) and Hair (a rare event). None dead. Run Out Time and Tail are `inert`
  there: the gate never presses Run Out. Run 3 gated the SHIPPED v0.1.0 zip (the release
  workflow's MSVC build) with the same expectation: 9 of 9, 15 live, only Crank Rate and Hair
  inconclusive.
- **The hands are chosen.** The partials, their shares and periods, the jerk, the slip, the
  fog's curve and reach, the grain and the tones are numbers picked to look and behave
  right, not measured from a cameraman, a projector or a print. The published figures are
  the eight frames a turn and the film's apertures and pitch.
- **The look is judged by eye**: the grain, the fog's creep, the tones' colours, the dust,
  the hair. The sweep proves each control moves the picture; nothing measures how well.
- **Seen on Resolume's bundled demo clips** through `--pipe`; never on camera footage.
- **A 16-bit composition** is quantised to 8 bits in the ring.
- **Not verified at 4K**, only benchmarked there.
- **The picture beyond the aperture is invented.** The camera exposed 2.2% more at each
  end than the projector shows; the clip has no such rows, so it is the clip's edge row
  repeated, which shows as streaks either side of the frame line when Framing is off the
  middle (the guide's `wear.png` shows it).
- **No OpenFX port, browser demo or presets.** The user guide exists since v0.1.0.

---

## Open questions

- **The first picture.** The loop is threaded with the first picture: it holds 8 display
  frames at the defaults and 27 with both hands at full (against a normal hold of about 4),
  measured for the guide; "the first second" overstated it. Should a fresh
  instance thread with blank leader instead, so it opens like a reel?
- **Travel ghost.** The pull-down is instantaneous and always in the dark (gate's shutter
  range, 45–315°). A narrow blade over a slow pull-down smears the picture vertically; a
  Pull-down control and a Shutter Angle past the pull-down would show it.
- **Undercranking.** Silent film shot at 16 and shown at 20 or 24 runs fast. Here both
  cranks share a nominal rate, because a live clip has no future to run ahead into; a
  "Projector Rate" above the camera's would need the loop to empty and refill (a jump back
  every few seconds).
- **Tinted and toned.** Real prints were often both (a blue-toned night scene tinted
  amber). One option each would cover it.
- **The camera's shutter angle.** Fixed (the exposure is relative). A control would trade
  motion blur, which is not modelled, for exposure, which is.
- **The flicker default.** 315° at 3 blades is the gentlest the shutter allows; a "no
  shutter" setting would need the travel ghost to be honest.

---

## Siblings

- **gate** — the projector: shutter, hold, weave, scratches, dust, hair, harness, verify.
- **wetplate** — spectral weights through Smits' basis.
- **gauge** — the strip's geometry and a head leader with a countdown (the other end of
  the reel).
- **rebate** — the negative and its scan.
- **photofinish** — the resize-mid-run trap.
- **oxbow** — `oxbow probe` and `oxbow selftest` load this bundle as a host.
