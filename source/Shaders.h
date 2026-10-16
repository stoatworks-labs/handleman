#pragma once

#include <string>

/**
	Three passes.

	1. **capture** -- the host's picture, copied texel for texel (still sRGB
	   code values) into one layer of the film ring, an RGBA8 texture array.
	   It runs only on a display frame where the camera's crank passed a
	   frame: that layer is the frame the camera exposed.

	2. **resample** -- one ring layer onto a new ring: a new raster (bilinear)
	   or a deeper ring (the same raster, so an exact copy). A reallocated
	   texture is a cleared texture, and the loop of film between the camera
	   and the projector is still to be shown.

	3. **output** -- to the host. For each projector frame the display frame's
	   exposure overlaps (a "segment", weighted by the light the shutter let
	   through): the strip at that frame's offset (weave, Framing, the run-out's
	   slip), so the frame above, the frame line and the frame below when the
	   offset shows them; the print (the stock's spectral weights, the frame's
	   own exposure, fog, the curve, grain, toning or a tint, scratches, dust)
	   and the gate's hair; then the weighted sum, cos^4, the sRGB encode, Mix.

	Every picture read is `texelFetch` and a bilinear written out here, not a
	hardware filter. `hmtest --dump-shaders DIR` writes exactly the strings
	the plugin compiles.

	`PrintData` is one row of RGBA32F texels the CPU writes each frame:
	  [ 0, 128 )   64 particles, two texels each: ( x, y, a, b ),
	               ( cos, sin, strength, segment + 8 x fibre )
	  [ 128, 160 ) 16 scratches, two texels each: ( left, right, strength,
	               side ), ( segment mask, 0, 0, 0 )
	  [ 160, 173 ) the hair's 13 points: ( x, y, 0, 0 )
*/
namespace handleman::shaders
{

constexpr int kPrintDataWidth    = 256;
constexpr int kPrintScratchFirst = 128;
constexpr int kPrintHairFirst    = 160;

std::string Vertex();
std::string Capture();
std::string Resample();
std::string Output();

} // namespace handleman::shaders
