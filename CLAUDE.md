# handleman

A hand-cranked camera and projector — the camera's hand decides when each frame was
exposed and how long its shutter was open, the projector's how long each frame stays on
the screen, with a loop of film between them; the print is orthochromatic or
panchromatic silver, toned or tinted, with grain, weave, scratches, dust and a hair; Run
Out fogs the roll to blank cells — as an FFGL **effect** for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing a hand, the loop, the curve, the reel or a check's
tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/hmtest --out /tmp/f.png --size 1920x1080`
  (90 frames of the moving card at a synthetic 60 fps, then the last one;
  `--average` writes the mean of every frame instead)
- Set anything by name: `--set "Crank Rate=0.25" --set "Tone=2" --set "Camera Hand=1"`
  (0..1 for sliders, the element index for options, 1 to press Run Out)
- List parameters, kinds, defaults and ranges: `./build/hmtest --list`
- Other sources: `--source flat --level 0.5`, `--source white`, `--source black`
- The exact GLSL the plugin compiles: `./build/hmtest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues. A slider ramps linearly between cues;
  an option and an integer STEP; an event fires on its cue frame only. A cue naming no
  parameter is refused with exit 2, a partial frame at the end of stdin ends the stream
  cleanly, a failed render or a closed stdout exits 1 (SIGPIPE is ignored, so never 141):
  `ffmpeg -i clip.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/hmtest --pipe --size 1280x720 [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mov`
  (Resolume's demo clips are 30 fps: `fps=60` shows each twice, as a 60 Hz host does).
  A Run Out at frame 120 and a new roll at frame 480: `printf '120 Run Out 1\n480 Run Out 1\n'`.
- The stocks' weights: `python3 tools/spectral_weights.py` writes `source/Spectral.h`;
  `--check` compares, `--print` shows them. Never edit the header by hand.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the reserved-word grep +
  the spectral check + every check at 320x180 AND 1280x720 AND on the software renderer
  + --pipe + the sweep + the bundle; ~15 min on this Mac, most of it the software
  renderer's --crank and --exposure)
- Each hand moves what it should and nothing else; the loop is bounded: `./build/hmtest --crank`
- A frame cranked slowly prints lighter, at the print's gamma: `./build/hmtest --exposure`
- The flicker's spectrum is the shutter's Fourier series: `./build/hmtest --flicker`
- Neutral; the curve's slope; toning scales density; tints multiply; the stocks' weights
  read back from the picture: `./build/hmtest --tone`
- Grain and dust change with the film frame, not the display frame: `./build/hmtest --hold`
- Scratches stay put while the picture moves and weaves: `./build/hmtest --scratch`
- Weave sigma and lag-1 correlation: `./build/hmtest --weave`
- Blank cells, the frame line's height and crawl, the three tails, a new roll: `./build/hmtest --runout`
- The loop of film survives a resize: `./build/hmtest --resize`
- Alpha 1 at Mix 1, the host's own at Mix 0: `./build/hmtest --alpha`
- The checks can fail: `./build/hmtest --negative`; one perturbation verbosely:
  `./build/hmtest --perturb BITS --crank` (bits in `Model.h`)
- Every check takes `--size WxH`; CI runs the quick ones at 320x180
- CI's renderer, on this Mac: `HMTEST_RENDERER=software ./build/hmtest --tone --size 320x180`
  (Apple's software renderer, not repeatable at the last bit; verify.sh runs every check on it)
- Each coded frame as the checks read it: `HMTEST_DEBUG=1 ./build/hmtest --exposure 2>&1 | head`
- No name over 16 characters, none duplicated: `./build/hmtest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost and the state held: `./build/hmtest --bench`
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Handleman.bundle`

## Notes
- **The hands, not the look.** `Model.{h,cpp}` is the two cranks, the shutter, the weave,
  the print's marks, the curve and the reel, in double on the CPU. Each crank's phase is
  `F − A w(t)` (the projector also `− (A_c + A_p)` behind), every frequency in cycles per
  frame, so the loop's length in frames does not depend on the crank rate. A wrong hand is
  a fix in `Model.cpp`; a wrong print on screen, in GLSL.
- **Time is the film position, in double.** Both cranks advance by each display frame's
  real seconds; the exposure is the projector's phase over the whole display period; a
  frame on which the host clock did not move reuses the last exposure's length. The shader
  sees weights, offsets and ring layers, never a time.
- **The ring is the loop of film.** An RGBA8 texture array, one layer per exposed picture
  (frames exposed on the same display frame share one). It grows to what the hands need
  and never shrinks while it lives; a resize or a deeper ring COPIES every layer. The
  host's 8-bit picture is stored exactly; a 16-bit composition is quantised here.
- **A frame's exposure belongs to the frame.** The camera's speed when it was exposed (and
  the hand's jerk while the shutter was open) is recorded with it; the projector never
  touches it.
- **Grain, dust and fog belong to the film frame; scratches and the hair to the gate.**
- **The reel decides per pull-down.** Fog is a pure function of the frame and the presses;
  the slip, the offset and the tail are a little state machine run once per pull-down in
  order (`Reel::Pull`). The offset wraps at half a frame; a wrap repeats a picture.
- **Output alpha is 1 at Mix 1** (`mix( src.a, 1, Mix )`): the projection paints the
  whole frame. A transparent pixel of the clip prints as black film.
- **`Perturb` bits, `Probe`, the scratch override and the `...ForTest` getters are test
  hooks**, inert in the plugin.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen it,
  so every slider is 0..1 and `Controls.cpp` holds the units, with inverses. Options map by
  index; their range reads back 0..1.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `handleman_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- GLSL 4.10 reserved words are not identifiers: `patch sample input output filter common
  active half layout flat packed` and the rest of s3.6; `verify.sh` greps the dumped
  shaders for them. MSVC has no `M_PI` (use `model::kPi`), `far`/`near` are macros there,
  and `<cmath>` is included wherever `std::lround` is used.
- FFGL id is `HM01`, display name `SW Handleman`.
- Use `git -C /Users/allansargeant/dev/handleman`: the worktree guard judges a bare `git`
  by the session's directory.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline against the real
  plugin class in a headless CGL context, plus an `oxbow` load. Never built for Windows.
- No OpenFX port, no browser demo, no presets, no user guide.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are PROVISIONAL HAND COPIES (`guide = ""`);
  registration and the backend's sync scripts replace them. No `.github/FUNDING.yml` or
  issue templates yet (the sync scripts write those too).

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/handleman/handleman.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\handleman\logs\handleman.YYYY-MM-DD.log   (Windows)

Frame 60 logs the host clock's raw value and the unit the voting settled on.
