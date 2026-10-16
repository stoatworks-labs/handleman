# Attributions

Handleman is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is a PROVISIONAL HAND COPY, written 2026-10-16 before Handleman was registered:
once it is, the master lists in the `stoatworks-backend` repo generate this file
through `scripts/sync-attributions.py` (with `--adopt`, since this one was written by
hand). Edit it there after that, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The projector — Stoatworks gate

<https://github.com/stoatworks-labs/gate>  
Licence: MIT  
Copyright: Stoatworks Labs

The shutter integrated in closed form over each display frame, the segments' weighted sum, the weave as a stationary AR(1), the scratches in the gate, the dust on the print, the hair, the frame line and the neighbour frames, cos⁴, the plugin's shape (the OBJECT core, the clock-unit voting, the About block, the Diag logger), the harness, `--pipe`, the sweep, the verify script and the negative-control pattern are gate's, which had most of them from filament, wetplate, toner, readout and tinsel. Gate is the projector at a motor's steady rate; Handleman puts a hand on it and a camera in front of it.

### Spectral weights from Smits' basis — Stoatworks wetplate

<https://github.com/stoatworks-labs/wetplate>  
Licence: MIT  
Copyright: Stoatworks Labs

The orthochromatic stock's exposure through Smits' seven basis spectra, computed offline by a script and summed in the shader, is wetplate's method and wetplate's transcription of the Smits table.

### The resize-mid-run guard — Stoatworks photofinish

<https://github.com/stoatworks-labs/photofinish>  
Licence: MIT  
Copyright: Stoatworks Labs

The trap that a reallocated buffer is a cleared buffer, and the check that guards it, are photofinish's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Eight frames a turn — period handbooks, through the Media History Digital Library

That a standard motion-picture camera passes eight frames (six inches of 35 mm) for each turn of the crank, so two turns a second is sixteen frames a second: a handbook of motion-picture photography in the Media History Digital Library (lantern.mediahist.org, `handbookofmotion00herb`, p. 132), and the Science Museum Group's catalogue entry for the Cine-Kodak ("hand turned at 2 turns per second for 16 fps"). Read through search results, not the scanned page.

### 35 mm silent, full-aperture geometry — SMPTE's 1930 projection aperture; the NFSA's film-format glossary

The standard projection aperture of 0.906 × 0.6795 in, the full camera aperture of 0.945 × 0.709 in, and four perforations of 0.187 in to a frame (0.748 in). They set the frame line's height and how much exposed picture lies beyond the aperture. Taken from search-result summaries of the SMPTE Transactions and the NFSA glossary (the NFSA page refused a direct fetch); not read in the original standard.

### An RGB-to-Spectrum Conversion for Reflectances — Brian Smits, Journal of Graphics Tools 4(4), 1999

The seven basis reflectance spectra the stocks' weights are computed through.

### The variance of an autocorrelation estimate — M. S. Bartlett, 1946

The large-sample variance that sets hmtest --weave's tolerances.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The hand-cranked cinema

Bell & Howell, Debrie and Pathé cameras turned at two turns a second, hand-turned projectors, orthochromatic negative, sepia toning and the tinting baths of the 1910s and 1920s, and the end of a roll running out through the gate. No code, assets or binaries were used or examined.

## Standards and published specifications

What the implementation is measured against.

- **IEC 61966-2-1:1999** — the sRGB primaries, white and transfer function.
- **ITU-R BT.709** — the luminance weights the panchromatic stock uses.
- **Planck's law** — the daylight (6504 K) the orthochromatic weights are computed under.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
