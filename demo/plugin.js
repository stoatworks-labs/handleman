/**
 * Handleman — browser demo.
 *
 * A hand-cranked camera and projector. The one idea, from `AGENTS.md`: model
 * the two hands, not the look. A cameraman turned the camera, eight frames a
 * turn, and a projectionist turned the projector, and neither is a motor. The
 * camera's hand decides WHEN each frame was exposed (so motion rushes and drags,
 * and a frame cranked slowly is exposed for longer and prints lighter); the
 * projector's decides how long each frame stays on the screen and how fast the
 * shutter flickers. Between them runs a loop of film, held as a ring of
 * pictures. The print is orthochromatic or panchromatic silver, toned or
 * tinted, worn; Run Out fogs the roll to blank cells.
 *
 * The two halves of this page are not equally faithful, and the split is worth
 * being exact about:
 *
 *   The SHADERS are the plugin's. The five GLSL strings below -- the version
 *   line, the vertex body, and the capture, resample and output bodies -- are
 *   `source/Shaders.cpp`'s, spliced in by `demo/tools/sync_shaders.py` with
 *   their tabs and comments, and assembled the way the plugin assembles them.
 *   So the capture into the film ring (an RGBA8 texture array, one layer per
 *   exposed picture), the ring's copy when it grows or the raster changes, and
 *   the whole output pass -- the strip at each projector frame's offset, the
 *   frame line, the stock's Smits weights, the frame's exposure and fog, the
 *   print's curve, grain, toning and tints, scratches, dust, the hair, the
 *   shutter's weighted sum, cos^4, the sRGB encode and Mix -- run on the GPU
 *   here as there. The numbers they are handed -- Shaders.h's print-data
 *   layout, every constant and enum of Model.h, Spectral.h's weights, the
 *   option lists of Controls, Handleman.cpp's ring cap, hash streams and seed
 *   -- are copied by the same script. `demo/tools/check_shaders.py` holds all
 *   of it to the C++ character for character, and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT, by hand: Model.cpp (the PCG hash and its keys, the
 *   two hands and their slew-limited amplitudes, the shutter's closed form
 *   `CumulativeOpen` and the per-projector-frame weights of `Segments`, the
 *   weave's AR(1) as a moving-average sum, the scratches' birth and death scan,
 *   the dust's Poisson births, the hair, the fog's exposure, the Reel's
 *   run-out state machine -- transitions, slip, offset, the three tails, a new
 *   roll -- and the tones' two laws), Controls.cpp (every slider to its unit),
 *   FilmRing.cpp (growth by copying, never clearing) and
 *   Handleman::ProcessOpenGL (the clock, both cranks in double, the exposure
 *   window, every pull-down in order, the press, the camera's exposures and
 *   their ring layers, the frame table, the print-data upload and every
 *   uniform). In JavaScript doubles as the C++ keeps them in double, with
 *   Math.imul and >>> 0 for the hash's uint32 arithmetic, rounded to float
 *   (Math.fround) where the plugin holds or hands over a float. `hmtest
 *   --crank`, `--exposure`, `--runout` and the rest check the C++ originals and
 *   have no idea this page exists; `demo/tools/crosscheck` renders this page
 *   and `hmtest --pipe` from the same input frames and compares the pixels,
 *   which is the only thing that holds the port to the plugin, and only when
 *   somebody runs it.
 *
 * ------------------------------------------------------------- the clock
 *
 * The plugin reads the host's clock and votes on its unit (readout's scheme).
 * Here the clock is the kit's: `time` in declared seconds, accumulated from the
 * browser's requestAnimationFrame deltas while playing (the kit caps one delta
 * at 0.1 s), +1/60 on Step. No vote runs. The plugin's rules then apply
 * unchanged: dt = now - last, clamped to [0, 0.25 s], a nominal 1/60 on the
 * first frame; the cranks advance by dt; a frame on which the clock did not
 * move reuses the last exposure's length.
 *
 * So the browser's frame rate IS the display rate the shutter beats against,
 * exactly as Resolume's is in the plugin, and the camera exposes a picture only
 * on a display frame its crank passed a film frame on.
 *
 * A paused page renders only when a control moves, and each such frame is worth
 * 0 s: the cranks stand, the same projector frames, the last exposure's length
 * again. Restart sends the kit's clock back to 0; the plugin has no rule for a
 * clock that runs backwards, and its clamp makes that frame worth 0 s, so both
 * cranks carry on from where they were. That is the plugin's behaviour, not a
 * reset.
 *
 * ------------------------------------------------------------- what is missing
 *
 * Run Out is FF_TYPE_EVENT in the plugin; the kit has no event type, so it is a
 * toggle here that the renderer releases on the frame it acts, which is what a
 * host does with an event anyway (the plugin fires on the rising edge). The
 * About block is absent, as on every page in this suite. The harness-only
 * `Perturb` and `Probe` uniforms are set to what the shipped plugin sets them
 * to: 0.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, bindTexture } from './vendor/gl.js';

//=== BEGIN GENERATED by demo/tools/sync_shaders.py from source/ -- do not edit by hand.

// source/Shaders.cpp, verbatim. `assemble` (below the block) is Shaders.cpp's.
const K_VERSION = "#version 410 core\n";

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const CAPTURE_BODY = `
uniform sampler2D Source;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texelFetch( Source, ivec2( gl_FragCoord.xy ), 0 );
}
`;

const RESAMPLE_BODY = `
uniform sampler2DArray Old;
uniform int Layer;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( Old, vec3( uv, float( Layer ) ) );
}
`;

const OUTPUT_BODY = `
uniform sampler2DArray Ring;
uniform ivec2 RingSize;
uniform sampler2D Source;      //the live input, for Mix and alpha
uniform sampler2D PrintData;
uniform vec2 OutSize;

uniform int SegCount;
uniform float SegWeight[ 8 ];  //light over the display frame, / the mean
uniform vec2 SegOffset[ 8 ];   //( weave across, down ) in h-units: + down moves the picture down
uniform int SegFrame[ 8 ];     //this segment's own frame in the frame table

uniform int FrameCount;
uniform int FrameLayer[ 12 ];  //a ring layer; -1 a blank cell; -2 no film
uniform float FrameFog[ 12 ];  //fog exposure, in whites
uniform float FrameExposure[ 12 ];//the camera's exposure factor
uniform int FrameSeed[ 12 ];   //the frame's grain

uniform float Pitch;           //frame pitch, h-units
uniform float Margin;          //exposed picture beyond the aperture, each end
uniform float LineStart;       //where the frame line starts below a frame's top
uniform float BaseDensity;     //log10
uniform float MaxDensity;
uniform float Knee;
uniform float GreyLog;         //log10 of the grey that prints at its own level
uniform float Gamma;
uniform int Colour;            //1: each channel through the curve on its own
uniform float Basis[ 7 ];      //the stock's exposure per Smits basis, white = 1
uniform vec3 Toning;           //silver density per channel
uniform vec3 Tint;             //the base's transmittance per channel
uniform float FogFlat;
uniform float FogReach;
uniform float GrainAmount;     //kGrainSigma x Grain
uniform float GrainFloor;
uniform float GrainCell;       //h-units

uniform int ParticleCount;
uniform int ScratchCount;
uniform int HairPoints;
uniform int HairMask;
uniform float HairHalfWidth;
uniform float HairOpacity;
uniform float VignetteTan;
uniform float Aspect;
uniform float MixAmount;
uniform int Probe;
uniform int Perturb;

in vec2 uv;
out vec4 fragColor;

const int kScratchFirst = 128;
const int kHairFirst = 160;
const float kLn10 = 2.302585093;

vec3 decodeSrgb( vec3 c )
{
	c = max( c, vec3( 0.0 ) );
	vec3 lo = c / 12.92;
	vec3 hi = pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) );
	return mix( lo, hi, step( vec3( 0.04045 ), c ) );
}

vec3 encodeSrgb( vec3 c )
{
	c = clamp( c, vec3( 0.0 ), vec3( 1.0 ) );
	vec3 lo = c * 12.92;
	vec3 hi = 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - 0.055;
	return mix( lo, hi, step( vec3( 0.0031308 ), c ) );
}

vec3 fetchRing( int layer, ivec2 p )
{
	p = clamp( p, ivec2( 0 ), RingSize - 1 );
	return decodeSrgb( texelFetch( Ring, ivec3( p, layer ), 0 ).rgb );
}

//Bilinear by hand, in linear light: a hardware filter may weight with 8 bits.
//At a whole-texel position t is exactly 0 and the texel comes back unchanged.
vec3 tapRing( int layer, vec2 s )
{
	vec2 f0 = floor( s );
	vec2 t = s - f0;
	ivec2 i0 = ivec2( f0 );
	vec3 a = fetchRing( layer, i0 );
	vec3 b = fetchRing( layer, i0 + ivec2( 1, 0 ) );
	vec3 c = fetchRing( layer, i0 + ivec2( 0, 1 ) );
	vec3 d = fetchRing( layer, i0 + ivec2( 1, 1 ) );
	return mix( mix( a, b, t.x ), mix( c, d, t.x ), t.y );
}

//Smits (1999): the reflectance of an RGB is a sum of seven basis spectra
//chosen by the order of its components, so the stock's exposure is the same
//sum of the seven exposures in Basis[] (white, cyan, magenta, yellow, red,
//green, blue). Pure red, green and blue come out as exactly Basis[4], [5]
//and [6]; white as exactly Basis[0], which is 1.
float stockExposure( vec3 rgb )
{
	float r = rgb.r, g = rgb.g, b = rgb.b;
	float e;
	if( r <= g && r <= b )
	{
		e = r * Basis[ 0 ];
		if( g <= b )
			e += ( g - r ) * Basis[ 1 ] + ( b - g ) * Basis[ 6 ];
		else
			e += ( b - r ) * Basis[ 1 ] + ( g - b ) * Basis[ 5 ];
	}
	else if( g <= r && g <= b )
	{
		e = g * Basis[ 0 ];
		if( r <= b )
			e += ( r - g ) * Basis[ 2 ] + ( b - r ) * Basis[ 6 ];
		else
			e += ( b - g ) * Basis[ 2 ] + ( r - b ) * Basis[ 4 ];
	}
	else
	{
		e = b * Basis[ 0 ];
		if( r <= g )
			e += ( r - b ) * Basis[ 3 ] + ( g - r ) * Basis[ 5 ];
		else
			e += ( g - b ) * Basis[ 3 ] + ( r - g ) * Basis[ 4 ];
	}
	return max( e, 0.0 );
}

float softKnee( float u )
{
	float z = u / Knee;
	return z > 30.0 ? u : Knee * log( 1.0 + exp( z ) );
}

//The print's density for a log10 exposure: a straight line of slope -Gamma
//through the grey that prints at its own level, clamped softly at the base
//and at the print's black.
float density( float logExposure )
{
	float line = BaseDensity - GreyLog - Gamma * ( logExposure - GreyLog );
	float floor1 = BaseDensity + softKnee( line - BaseDensity );
	return MaxDensity - softKnee( MaxDensity - floor1 );
}

//PCG's output permutation, as the CPU's Hash.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//A unit normal from four uniforms (Irwin-Hall), keyed by a lattice cell and
//a frame.
float cellNormal( ivec2 cell, uint seed )
{
	uint h = pcg( seed ^ pcg( uint( cell.x ) ^ pcg( uint( cell.y ) + 0x9e3779b9u ) ) );
	float sum = 0.0;
	for( int k = 0; k < 4; ++k )
	{
		h = pcg( h );
		sum += ( float( h ) + 0.5 ) * ( 1.0 / 4294967296.0 );
	}
	return ( sum - 2.0 ) * 1.7320508;
}

//Grain on the film: clumps GrainCell across. Larger than a pixel, a lattice
//of normals interpolated and renormalised to unit variance; smaller, one
//draw per pixel scaled by the clump's size in pixels, which is what the
//mean of that many clumps would leave.
float grain( vec2 film, uint seed, float H )
{
	float cellPx = GrainCell * H;
	if( cellPx < 1.0 )
		return cellNormal( ivec2( floor( film * H ) ), seed ) * cellPx;
	vec2 g = film / GrainCell;
	vec2 g0 = floor( g );
	vec2 t = g - g0;
	ivec2 c = ivec2( g0 );
	vec4 w = vec4( ( 1.0 - t.x ) * ( 1.0 - t.y ), t.x * ( 1.0 - t.y ), ( 1.0 - t.x ) * t.y, t.x * t.y );
	float n = w.x * cellNormal( c, seed ) + w.y * cellNormal( c + ivec2( 1, 0 ), seed )
	        + w.z * cellNormal( c + ivec2( 0, 1 ), seed ) + w.w * cellNormal( c + ivec2( 1, 1 ), seed );
	return n / sqrt( dot( w, w ) );
}

float segmentLine( vec2 p, vec2 a, vec2 b )
{
	vec2 ab = b - a;
	float t = clamp( dot( p - a, ab ) / max( dot( ab, ab ), 1e-12 ), 0.0, 1.0 );
	return length( p - a - ab * t );
}

void main()
{
	vec2 pix = gl_FragCoord.xy;
	float H = OutSize.y;
	float pxh = 1.0 / H;                        //one pixel, in h-units
	vec2 hpos = vec2( pix.x, H - pix.y ) / H;   //h-units, y down
	float x0 = pix.x - 0.5;

	//The hair is in the gate: one shape for every frame it is in.
	float hairCover = 0.0;
	if( HairPoints > 1 )
	{
		float dist = 1e9;
		vec2 prev = texelFetch( PrintData, ivec2( kHairFirst, 0 ), 0 ).xy;
		for( int k = 1; k < HairPoints; ++k )
		{
			vec2 next = texelFetch( PrintData, ivec2( kHairFirst + k, 0 ), 0 ).xy;
			dist = min( dist, segmentLine( hpos, prev, next ) );
			prev = next;
		}
		float w = max( HairHalfWidth, 0.5 * pxh );
		hairCover = clamp( ( w - dist ) / pxh + 0.5, 0.0, 1.0 ) * ( HairHalfWidth / w );
	}

	vec3 light = vec3( 0.0 );
	for( int i = 0; i < SegCount; ++i )
	{
		vec2 off = SegOffset[ i ];
		float u = hpos.y - off.y;                 //h-units below this frame's top
		float fx = hpos.x - off.x;                //across the film, h-units

		//Which frame of the strip: j = 0 is the one in the gate, -1 above it,
		//+1 below. Each frame's exposed picture runs from -Margin to
		//1 + Margin below its top, then the frame line to the next.
		int j = int( floor( ( u + Margin ) / Pitch ) );
		float local = u - float( j ) * Pitch;
		int f = clamp( SegFrame[ i ] + j, 0, FrameCount - 1 );
		int layer = FrameLayer[ f ];
		vec3 T;

		if( layer == -2 )
		{
			//No film: the lamp on the screen, nothing on it but the gate's hair.
			T = vec3( exp( BaseDensity * kLn10 ) );
		}
		else
		{
			//The silver, in density above the base.
			vec3 silver;
			if( local >= LineStart )
				silver = vec3( MaxDensity - BaseDensity );//the frame line: never exposed
			else if( layer == -1 )
				silver = vec3( 0.0 );                    //a blank cell: exposed to white
			else
			{
				vec2 s = vec2( fx / Aspect * float( RingSize.x ), ( 1.0 - local ) * float( RingSize.y ) ) - 0.5;
				vec3 lin = tapRing( layer, s );
				float fog = FrameFog[ f ] * ( FogFlat + ( 1.0 - FogFlat ) * ( exp( -fx / FogReach ) + exp( -( Aspect - fx ) / FogReach ) ) );
				if( Colour != 0 )
				{
					vec3 e = lin * FrameExposure[ f ] + fog;
					e = max( e, vec3( 1e-7 ) );
					silver = vec3( density( log( e.r ) / kLn10 ), density( log( e.g ) / kLn10 ), density( log( e.b ) / kLn10 ) ) - BaseDensity;
				}
				else
				{
					float e = max( stockExposure( lin ) * FrameExposure[ f ] + fog, 1e-7 );
					silver = vec3( density( log( e ) / kLn10 ) - BaseDensity );
				}
			}

			//Grain: the frame's own, riding with it.
			if( GrainAmount > 0.0 )
			{
				float n = grain( vec2( fx, local ), uint( FrameSeed[ f ] ), H );
				silver += GrainAmount * sqrt( silver / ( MaxDensity - BaseDensity ) + GrainFloor ) * n;
				silver = max( silver, vec3( 0.0 ) );
			}

			//The gate's grit: straight lines at a fixed x. The base side
			//scatters light away; the emulsion side takes the silver off, and
			//a tint's dye with it.
			vec3 tint = Tint;
			float baseLoss = 1.0;
			for( int k = 0; k < ScratchCount; ++k )
			{
				vec4 s0 = texelFetch( PrintData, ivec2( kScratchFirst + 2 * k, 0 ), 0 );
				vec4 s1 = texelFetch( PrintData, ivec2( kScratchFirst + 2 * k + 1, 0 ), 0 );
				if( ( ( int( s1.x ) >> i ) & 1 ) == 0 )
					continue;
				float l = s0.x * OutSize.x;
				float r = s0.y * OutSize.x;
				if( ( Perturb & 32 ) != 0 )
				{
					l += off.x * H;
					r += off.x * H;
				}
				float cover = clamp( min( r, x0 + 1.0 ) - max( l, x0 ), 0.0, 1.0 );
				if( cover <= 0.0 )
					continue;
				if( s0.w < 0.5 )
					baseLoss *= 1.0 - cover * s0.z;
				else
				{
					silver *= 1.0 - cover * s0.z;
					tint = mix( tint, vec3( 1.0 ), cover * s0.z );
				}
			}

			//Dust on this frame of the print: dark specks are dirt on the print;
			//white ones were dirt on the negative, so the print has no silver
			//there (but keeps its tint).
			float dark = 1.0;
			if( j == 0 )
			{
				vec2 film = vec2( fx, local );
				for( int k = 0; k < ParticleCount; ++k )
				{
					vec4 p0 = texelFetch( PrintData, ivec2( 2 * k, 0 ), 0 );
					vec4 p1 = texelFetch( PrintData, ivec2( 2 * k + 1, 0 ), 0 );
					int code = int( p1.w );
					if( ( code & 7 ) != i )
						continue;
					vec2 d = film - p0.xy;
					if( abs( d.x ) > p0.z + 3.0 * pxh || abs( d.y ) > p0.z + 3.0 * pxh )
						continue;
					vec2 q = vec2( p1.x * d.x + p1.y * d.y, -p1.y * d.x + p1.x * d.y );
					float cover;
					if( code >= 8 )
					{
						float t = max( p0.w, 0.5 * pxh );
						float along = clamp( q.x, -p0.z, p0.z );
						float dist = length( vec2( q.x - along, q.y ) );
						cover = clamp( ( t - dist ) / pxh + 0.5, 0.0, 1.0 ) * ( p0.w / t );
					}
					else
					{
						vec2 rr = max( p0.zw, vec2( 0.5 * pxh ) );
						float dist = ( length( q / rr ) - 1.0 ) * min( rr.x, rr.y );
						cover = clamp( 0.5 - dist / pxh, 0.0, 1.0 ) * ( p0.z * p0.w ) / ( rr.x * rr.y );
					}
					if( p1.z > 0.0 )
						dark *= 1.0 - cover * p1.z;
					else
						silver *= 1.0 - cover * -p1.z;
				}
			}

			//Toning scales the silver's density per channel; a tint multiplies
			//the light.
			T = exp( -silver * Toning * kLn10 ) * tint * ( baseLoss * dark );
		}

		if( ( ( HairMask >> i ) & 1 ) != 0 )
			T *= 1.0 - HairOpacity * hairCover;

		light += SegWeight[ i ] * T;
	}

	//The lens: cos^4 of the field angle.
	vec2 fromCentre = hpos - vec2( 0.5 * Aspect, 0.5 );
	float t = length( fromCentre ) / length( vec2( 0.5 * Aspect, 0.5 ) ) * VignetteTan;
	float falloff = 1.0 / ( ( 1.0 + t * t ) * ( 1.0 + t * t ) );
	vec3 lin = light * falloff;

	if( Probe == 1 )
	{
		fragColor = vec4( lin, 1.0 );
		return;
	}

	//The host's own texel, nearest, at the picture's raster: at Mix 0 the host's
	//picture comes back bit for bit, on any rasteriser. A filtered read at an
	//interpolated coordinate is exact on this Mac's GPU and not on Apple's
	//software renderer, which blends the neighbour in at an edge.
	ivec2 own = clamp( ivec2( floor( pix * vec2( RingSize ) / OutSize ) ), ivec2( 0 ), RingSize - 1 );
	vec4 src = texelFetch( Source, own, 0 );
	fragColor = vec4( mix( src.rgb, encodeSrgb( lin ), MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
`;

// source/Shaders.h: the print data's layout, one row of RGBA32F texels.
const SHADERS_H = {};
SHADERS_H.kPrintDataWidth = 256;
SHADERS_H.kPrintScratchFirst = 128;
SHADERS_H.kPrintHairFirst = 160;

// source/Model.h: the film, the hands, the shutter, the weave, the print, the reel, the tones.
const MODEL_H = {};
MODEL_H.kPi = 3.14159265358979323846;
MODEL_H.kNominalFrame = 1.0 / 60.0;
MODEL_H.kMaxFrameDelta = 0.25;
MODEL_H.kMaxSegments = 8;
MODEL_H.kMaxFrames = MODEL_H.kMaxSegments + 4;
MODEL_H.kProjectorApertureIn = 0.6795;
MODEL_H.kCameraApertureIn = 0.709;
MODEL_H.kFramePitchIn = 0.748;
MODEL_H.kApertureHeightMm = MODEL_H.kProjectorApertureIn * 25.4;
MODEL_H.kMargin = 0.5 * ( MODEL_H.kCameraApertureIn - MODEL_H.kProjectorApertureIn ) / MODEL_H.kProjectorApertureIn;
MODEL_H.kPitch = MODEL_H.kFramePitchIn / MODEL_H.kProjectorApertureIn;
MODEL_H.kFrameLine = MODEL_H.kPitch - 1.0 - 2.0 * MODEL_H.kMargin;
MODEL_H.kFramesPerTurn = 8.0;
MODEL_H.kCameraPartials = 4;
MODEL_H.kProjectorPartials = 3;
MODEL_H.kMaxPartials = 4;
MODEL_H.kCameraMaxDeviation = 0.5;
MODEL_H.kProjectorMaxDeviation = 0.4;
MODEL_H.kAmplitudeSlew = 0.25;
MODEL_H.kExposureJitter = 0.15;
MODEL_H.kWeaveMm = 0.15;
MODEL_H.kWeaveVertical = 0.6;
MODEL_H.kWeaveRho = 0.75;
MODEL_H.kScratchBirths = 6.0 / 96.0;
MODEL_H.kScratchLifeMin = 24;
MODEL_H.kScratchLifeMean = 96.0;
MODEL_H.kScratchLifeMax = 480;
MODEL_H.kMaxScratches = 16;
MODEL_H.kScratchHalfMmMin = 0.004;
MODEL_H.kScratchHalfMmMax = 0.020;
MODEL_H.kDustBirths = 8.0;
MODEL_H.kDustBirthsMax = 16;
MODEL_H.kMaxParticles = 64;
MODEL_H.kHairSlot = 96;
MODEL_H.kHairPoints = 13;
MODEL_H.kBaseDensity = 0.06;
MODEL_H.kMaxDensity = 2.4;
MODEL_H.kKnee = 0.12;
MODEL_H.kGreyExposure = 0.18;
MODEL_H.kGrainSigma = 0.22;
MODEL_H.kGrainFloor = 0.03;
MODEL_H.kGrainMm = 0.02;
MODEL_H.kFogCurve = 7.0;
MODEL_H.kFogMax = 6.0;
MODEL_H.kFogFlat = 0.3;
MODEL_H.kFogReach = 0.2;
MODEL_H.kSlip = 0.06;
MODEL_H.kSlipBuild = 8;
MODEL_H.kSlipFog = 0.5;
MODEL_H.kFramedAfter = 24;
MODEL_H.kTailCells = 24;
MODEL_H.kFlick = 0.35;
MODEL_H.kHeadCells = 6;
MODEL_H.kCameraHand = [{ cyclesPerFrame: 1.0 / MODEL_H.kFramesPerTurn, share: 0.40 }, { cyclesPerFrame: 2.0 / MODEL_H.kFramesPerTurn, share: 0.10 }, { cyclesPerFrame: 1.0 / 70.0, share: 0.30 }, { cyclesPerFrame: 1.0 / 43.0, share: 0.20 }];
MODEL_H.kProjectorHand = [{ cyclesPerFrame: 1.0 / MODEL_H.kFramesPerTurn, share: 0.55 }, { cyclesPerFrame: 2.0 / MODEL_H.kFramesPerTurn, share: 0.15 }, { cyclesPerFrame: 1.0 / 50.0, share: 0.30 }];
MODEL_H.kSepiaDensity = [0.70, 0.93, 1.35];
MODEL_H.kAmberTint = [1.00, 0.70, 0.28];
MODEL_H.kBlueTint = [0.35, 0.55, 1.00];
MODEL_H.kGreenTint = [0.45, 0.85, 0.40];
MODEL_H.kBase = 0;
MODEL_H.kEmulsion = 1;
MODEL_H.kRolling = 0;
MODEL_H.kFramed = 1;
MODEL_H.kOffTheEnd = 2;
MODEL_H.kColour = 0;
MODEL_H.kBlackAndWhite = 1;
MODEL_H.kSepia = 2;
MODEL_H.kAmber = 3;
MODEL_H.kBlue = 4;
MODEL_H.kGreen = 5;
MODEL_H.kToneCount = 6;
MODEL_H.kPerturbInstant = 1;
MODEL_H.kPerturbExposureProjector = 2;
MODEL_H.kPerturbNoLoop = 4;
MODEL_H.kPerturbToneAsTint = 8;
MODEL_H.kPerturbGrainPerDisplay = 16;
MODEL_H.kPerturbPictureSpace = 32;
MODEL_H.kPerturbWhiteWeave = 64;
MODEL_H.kPerturbResizeClears = 128;
MODEL_H.kPerturbOrthoAsPan = 256;
MODEL_H.kPerturbFrameLineHalf = 512;
MODEL_H.kProbeNone = 0;
MODEL_H.kProbeLinear = 1;

// source/Model.cpp: one hash stream per thing the film draws.
const MODEL_CPP = {};
MODEL_CPP.kStreamWeaveX = 1;
MODEL_CPP.kStreamWeaveY = 2;
MODEL_CPP.kStreamScratch = 3;
MODEL_CPP.kStreamDust = 4;
MODEL_CPP.kStreamHair = 5;
MODEL_CPP.kStreamHairTremble = 6;
MODEL_CPP.kStreamCamera = 7;
MODEL_CPP.kStreamProjector = 8;

// source/Spectral.h (generated by tools/spectral_weights.py): each stock's Smits weights,
// white, cyan, magenta, yellow, red, green, blue.
const SPECTRAL_H = {};
SPECTRAL_H.kStockCount = 2;
SPECTRAL_H.kOrthoStartNm = 380;
SPECTRAL_H.kOrthoCount = 24;
const SPECTRAL_WEIGHTS = [
  ['Orthochromatic', [Math.fround(1.0), Math.fround(0.968058144), Math.fround(0.666209766), Math.fround(0.354154602), Math.fround(0.0482027843), Math.fround(0.333064043), Math.fround(0.645948535)]],
  ['Panchromatic', [Math.fround(1.0), Math.fround(0.7874), Math.fround(0.2848), Math.fround(0.9278), Math.fround(0.2126), Math.fround(0.7152), Math.fround(0.0722)]],
];

// source/Controls.h and Controls.cpp: the option counts and lists.
const CONTROLS_H = {};
CONTROLS_H.kBladesCount = 3;
CONTROLS_H.kStockCount = 2;
CONTROLS_H.kToneCount = 6;
CONTROLS_H.kTailCount = 3;
const BLADES_NAMES = ['1', '2', '3'];
const STOCK_NAMES = ['Orthochromatic', 'Panchromatic'];
const TONE_NAMES = ['Colour', 'Black & White', 'Sepia', 'Amber Tint', 'Blue Tint', 'Green Tint'];
const TAIL_NAMES = ['Rolling', 'Framed', 'Off the End'];

// source/Handleman.cpp's constants and source/Handleman.h's print seed.
const HANDLEMAN_CPP = {};
HANDLEMAN_CPP.kClockVotes = 4;
HANDLEMAN_CPP.kMaxRing = 32;
HANDLEMAN_CPP.kStreamHandCamera = 101;
HANDLEMAN_CPP.kStreamHandProjector = 102;
HANDLEMAN_CPP.kStreamJitter = 103;
HANDLEMAN_CPP.kStreamGrain = 104;
HANDLEMAN_CPP.seed = 0x4a4d1e5e;

//=== END GENERATED

//---------------------------------------------------------------------------
// Shaders.cpp's `assemble`: the version line, then the body.
//---------------------------------------------------------------------------
const assemble = (body) => K_VERSION + body;
const VERTEX = assemble(VERTEX_BODY);
const CAPTURE = assemble(CAPTURE_BODY);
const RESAMPLE = assemble(RESAMPLE_BODY);
const OUTPUT = assemble(OUTPUT_BODY);

/// What the shipped plugin sets its test hooks to.
const PERTURB = 0;
const PROBE = 0;

//===========================================================================
// The port. Controls.cpp, then Model.cpp, then FilmRing.cpp, then
// Handleman::ProcessOpenGL, by hand. JavaScript numbers are doubles, as the
// C++ model is; Math.fround stands in for every float the plugin stores or
// computes in.
//===========================================================================

const f = Math.fround;
const M = MODEL_H;
const H = HANDLEMAN_CPP;
const clampTo = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
const clamp01f = (value) => clampTo(f(value), 0.0, 1.0);
/// std::lround: halves away from zero (Math.round sends -0.5 up).
const lround = (value) => (value < 0 ? -Math.round(-value) : Math.round(value));

//--- Controls.cpp ----------------------------------------------------------
const controls = {
  crankFps: (value) => 8.0 + 16.0 * clamp01f(value),
  crankParam: (fps) => clampTo(f((fps - 8.0) / 16.0), 0.0, 1.0),
  cameraDeviation: (value) => M.kCameraMaxDeviation * clamp01f(value),
  projectorDeviation: (value) => M.kProjectorMaxDeviation * clamp01f(value),
  blades: (index) => clampTo(index, 0, CONTROLS_H.kBladesCount - 1) + 1,
  shutterDegrees: (value) => 45.0 + 270.0 * clamp01f(value),
  shutterParam: (degrees) => clampTo(f((degrees - 45.0) / 270.0), 0.0, 1.0),
  shutterOpen: (value) => controls.shutterDegrees(value) / 360.0,
  gamma: (value) => 0.6 + 1.6 * clamp01f(value),
  gammaParam: (gamma) => clampTo(f((gamma - 0.6) / 1.6), 0.0, 1.0),
  framingHeights: (value) => clamp01f(value) - 0.5,
  framingParam: (heights) => clampTo(f(heights + 0.5), 0.0, 1.0),
  runOutSeconds: (value) => 0.25 * Math.pow(16.0, clamp01f(value)),
  runOutParam: (seconds) => clampTo(f(Math.log(seconds / 0.25) / Math.log(16.0)), 0.0, 1.0),
  vignetteDegrees: (value) => 35.0 * clamp01f(value),
  amount: (value) => clamp01f(value),
  optionIndex: (value, count) => clampTo(lround(f(value)), 0, count - 1),
};

//--- Model.cpp: the hash ---------------------------------------------------

/// PCG's output permutation, in uint32: Math.imul for the wrapping multiply,
/// >>> 0 to read the bits back as unsigned.
function hash(v) {
  const state = (Math.imul(v, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

/// Key( seed, stream, index, sub ): an int64 index is hashed as its low then
/// its high 32 bits, two's complement, as the C++'s uint64_t cast gives them.
function key(seed, stream, index, sub = 0) {
  const low = index >>> 0;
  const high = Math.floor(index / 4294967296) >>> 0;
  let h = hash((seed ^ hash((stream + 0x9e3779b9) >>> 0)) >>> 0);
  h = hash((h ^ low) >>> 0);
  h = hash((h ^ high) >>> 0);
  return hash((h ^ Math.imul(sub, 0x85ebca6b)) >>> 0);
}

const uniform = (k) => (k + 0.5) / 4294967296.0;
const lerp = (a, b, t) => a + (b - a) * t;
const wrapHalf = (x) => x - Math.floor(x + 0.5);

function normal(seed, stream, index) {
  const u1 = uniform(key(seed, stream, index, 0));
  const u2 = uniform(key(seed, stream, index, 1));
  return Math.sqrt(-2.0 * Math.log(u1)) * Math.cos(2.0 * M.kPi * u2);
}

//--- Model.cpp: the hands --------------------------------------------------
function amplitudeOf(partials, count, deviation) {
  let sum = 0.0;
  for (let i = 0; i < count; i += 1) sum += partials[i].share / partials[i].cyclesPerFrame;
  return Math.max(deviation, 0.0) * sum / (2.0 * M.kPi);
}

/// One hand on one crank. Its timing error is A w( t ), in frames.
class Hand {
  constructor() {
    this.partials = [];
    this.count = 0;
    this.phase = [0, 0, 0, 0];
    this.coeff = [0, 0, 0, 0];
    this.amplitude = 0.0;
    this.amplitudeRate = 0.0;
  }

  start(seed, stream, list, n, deviation) {
    this.partials = list;
    this.count = Math.min(n, M.kMaxPartials);
    let sum = 0.0;
    for (let i = 0; i < this.count; i += 1) sum += list[i].share / list[i].cyclesPerFrame;
    for (let i = 0; i < this.count; i += 1) {
      this.coeff[i] = list[i].share / list[i].cyclesPerFrame / sum;
      this.phase[i] = 2.0 * M.kPi * uniform(key(seed, stream, i));
    }
    this.amplitude = amplitudeOf(list, this.count, deviation);
    this.amplitudeRate = 0.0;
  }

  advance(dt, fps, deviation) {
    for (let i = 0; i < this.count; i += 1) {
      this.phase[i] += 2.0 * M.kPi * this.partials[i].cyclesPerFrame * fps * dt;
      this.phase[i] -= 2.0 * M.kPi * Math.floor(this.phase[i] / (2.0 * M.kPi));
    }
    const target = amplitudeOf(this.partials, this.count, deviation);
    const step = M.kAmplitudeSlew * fps * dt;
    const delta = clampTo(target - this.amplitude, -step, step);
    this.amplitudeRate = dt > 0.0 ? delta / dt : 0.0;
    this.amplitude += delta;
  }

  /// A w, frames.
  error() {
    let w = 0.0;
    for (let i = 0; i < this.count; i += 1) w += this.coeff[i] * Math.sin(this.phase[i]);
    return this.amplitude * w;
  }

  /// d( A w ) / dt, frames per second: the crank turns at fps minus this.
  errorRate(fps) {
    let w = 0.0;
    let dw = 0.0;
    for (let i = 0; i < this.count; i += 1) {
      w += this.coeff[i] * Math.sin(this.phase[i]);
      dw += this.coeff[i] * Math.cos(this.phase[i]) * 2.0 * M.kPi * this.partials[i].cyclesPerFrame * fps;
    }
    return this.amplitudeRate * w + this.amplitude * dw;
  }
}

//--- Model.cpp: the shutter (gate's) ---------------------------------------
function shutterOpenAt(p, blades, open) {
  const c = p * blades;
  const r = c - Math.floor(c);
  return (r >= 0.5 * (1.0 - open) && r < 0.5 * (1.0 + open)) ? 1.0 : 0.0;
}

function cumulativeOpen(p, blades, open) {
  const c = p * blades;
  const n = Math.floor(c);
  const r = c - n;
  return (n * open + clampTo(r - 0.5 * (1.0 - open), 0.0, open)) / blades;
}

/// The projector frames the exposure [p0, p1] overlaps, oldest first, each
/// with the light the shutter passed during the overlap over open x (p1 - p0).
function segments(p0, p1, blades, open, instant) {
  const out = [];
  if (instant || !(p1 > p0)) {
    out.push({ frame: Math.floor(p1), weight: shutterOpenAt(p1, blades, open) / open });
    return out;
  }
  const base = Math.floor(p0);
  const q0 = p0 - base;
  const q1 = p1 - base;
  const last = Math.trunc(Math.ceil(q1)) - 1;
  const scale = 1.0 / (open * (q1 - q0));
  for (let j = 0; j <= last; j += 1) {
    const a = Math.max(q0, j);
    const b = Math.min(q1, j + 1);
    const w = (cumulativeOpen(b, blades, open) - cumulativeOpen(a, blades, open)) * scale;
    const k = base + j;
    if (out.length < M.kMaxSegments) out.push({ frame: k, weight: w });
    else {
      // Only after a long host stall at a fast crank (the kit caps a frame at
      // 0.1 s, so never here): the plugin's shift, as it is written.
      out.shift();
      out[0].weight += w;
      out.push({ frame: k, weight: w });
    }
  }
  return out;
}

//--- Model.cpp: the weave --------------------------------------------------
function weaveLaw(weave, perturb) {
  const sigmaX = M.kWeaveMm * clampTo(weave, 0.0, 1.0) / M.kApertureHeightMm;
  return {
    sigmaX,
    sigmaY: M.kWeaveVertical * sigmaX,
    rho: (perturb & M.kPerturbWhiteWeave) ? 0.0 : M.kWeaveRho,
  };
}

function weaveOffset(seed, frame, law) {
  let x = 0.0;
  let y = 0.0;
  if (law.sigmaX <= 0.0) return [0.0, 0.0];
  const terms = law.rho > 0.0 ? Math.trunc(Math.ceil(Math.log(1e-9) / Math.log(law.rho))) : 0;
  let weight = 1.0;
  for (let j = 0; j <= terms; j += 1) {
    x += weight * normal(seed, MODEL_CPP.kStreamWeaveX, frame - j);
    y += weight * normal(seed, MODEL_CPP.kStreamWeaveY, frame - j);
    weight *= law.rho;
  }
  const innovation = Math.sqrt(1.0 - law.rho * law.rho);
  return [x * (innovation * law.sigmaX), y * (innovation * law.sigmaY)];
}

//--- Model.cpp: scratches --------------------------------------------------
function scratchesAlive(seed, first, last, amount, aspect) {
  const list = [];
  const p = clampTo(amount, 0.0, 1.0) * M.kScratchBirths;
  if (p <= 0.0) return list;
  // Newest first, so the cap drops the oldest grit.
  for (let b = last; b > first - M.kScratchLifeMax && list.length < M.kMaxScratches; b -= 1) {
    if (uniform(key(seed, MODEL_CPP.kStreamScratch, b)) >= p) continue;
    const life = Math.min(M.kScratchLifeMax,
      M.kScratchLifeMin - M.kScratchLifeMean * Math.log(uniform(key(seed, MODEL_CPP.kStreamScratch, b, 1))));
    const s = { born: b, dies: b + Math.trunc(life) };
    if (s.dies <= first) continue;
    const centre = lerp(0.03, 0.97, uniform(key(seed, MODEL_CPP.kStreamScratch, b, 2)));
    const halfMm = lerp(M.kScratchHalfMmMin, M.kScratchHalfMmMax, uniform(key(seed, MODEL_CPP.kStreamScratch, b, 3)));
    const half = halfMm / M.kApertureHeightMm / aspect;
    s.left = centre - half;
    s.right = centre + half;
    const u = uniform(key(seed, MODEL_CPP.kStreamScratch, b, 5));
    s.side = uniform(key(seed, MODEL_CPP.kStreamScratch, b, 4)) < 0.55 ? M.kBase : M.kEmulsion;
    s.strength = s.side === M.kBase ? lerp(0.25, 0.65, u) : lerp(0.15, 1.0, u);
    list.push(s);
  }
  return list;
}

//--- Model.cpp: dust -------------------------------------------------------
function dustOn(seed, frame, amount, aspect) {
  const list = [];
  const mean = M.kDustBirths * clampTo(amount, 0.0, 1.0);
  if (mean <= 0.0) return list;
  for (let b = frame - 3; b <= frame; b += 1) {
    // Poisson by its inverse CDF, one uniform per frame.
    const u = uniform(key(seed, MODEL_CPP.kStreamDust, b));
    let n = 0;
    let term = Math.exp(-mean);
    let cdf = term;
    while (u > cdf && n < M.kDustBirthsMax) {
      n += 1;
      term *= mean / n;
      cdf += term;
    }
    for (let j = 0; j < n; j += 1) {
      const draw = (field) => uniform(key(seed, MODEL_CPP.kStreamDust, b, (1 + j * 16 + field) >>> 0));
      const l = draw(0);
      const life = l < 0.70 ? 1 : l < 0.85 ? 2 : l < 0.95 ? 3 : 4;
      const age = frame - b;
      if (age >= life) continue;
      const p = {};
      p.x = draw(1) * aspect + (draw(2) - 0.5) * 0.008 * age;
      p.y = draw(3) + (draw(4) - 0.5) * 0.008 * age;
      p.angle = draw(5) * M.kPi;
      p.fibre = draw(6) < 0.25 ? 1 : 0;
      if (p.fibre) {
        p.a = lerp(0.08, 0.45, draw(7)) / M.kApertureHeightMm;
        p.b = lerp(0.008, 0.016, draw(8)) / M.kApertureHeightMm;
      } else {
        const r = Math.min(0.15, 0.015 * Math.pow(draw(7), -1.0 / 2.0)) / M.kApertureHeightMm;
        p.a = r;
        p.b = r * lerp(0.5, 1.0, draw(8));
      }
      const s = draw(10);
      p.strength = draw(9) < 0.85 ? lerp(0.7, 1.0, s) : -lerp(0.6, 1.0, s);
      if (list.length < M.kMaxParticles) list.push(p);
    }
  }
  return list;
}

//--- Model.cpp: the hair ---------------------------------------------------
function hairAt(seed, frame, amount, aspect) {
  const hair = { present: false, x: [], y: [], halfWidth: 0.0, opacity: 0.0 };
  if (amount <= 0.0) return hair;
  // C++ integer division truncates; the negative branch makes it a floor.
  const slot = frame >= 0 ? Math.trunc(frame / M.kHairSlot) : Math.trunc((frame - M.kHairSlot + 1) / M.kHairSlot);
  const draw = (field) => uniform(key(seed, MODEL_CPP.kStreamHair, slot, field));
  if (draw(0) >= amount) return hair;
  const start = Math.trunc(draw(1) * 48.0);
  const end = Math.min(M.kHairSlot - 1, start + 36 + Math.trunc(draw(2) * 57.0));
  const t = frame - slot * M.kHairSlot;
  if (t < start || t >= end) return hair;
  hair.present = true;

  // Where it hangs: an edge of the aperture, a place along it, a direction
  // in, a length, a curl.
  const e = draw(3);
  const along = lerp(0.1, 0.9, draw(4));
  let ax; let ay; let dx; let dy;
  if (e < 0.40) { ax = along * aspect; ay = 1.0; dx = 0.0; dy = -1.0; } // bottom
  else if (e < 0.65) { ax = along * aspect; ay = 0.0; dx = 0.0; dy = 1.0; } // top
  else if (e < 0.85) { ax = 0.0; ay = along; dx = 1.0; dy = 0.0; } // left
  else { ax = aspect; ay = along; dx = -1.0; dy = 0.0; } // right
  const tilt = (draw(5) - 0.5) * 1.8;
  const c = Math.cos(tilt);
  const s = Math.sin(tilt);
  const ux = c * dx - s * dy;
  const uy = s * dx + c * dy; // in
  const vx = -uy;
  const vy = ux; // across
  const length = lerp(0.06, 0.18, draw(6));
  const curl = (draw(7) - 0.5) * 0.8;
  const waves = lerp(0.6, 2.2, draw(8));

  // It shakes loose: in its last four frames it slides back out of the gate.
  const left = end - t;
  const out = left <= 4 ? length * (1.0 - left / 5.0) : 0.0;
  const tremble = 0.0012 * (left <= 4 ? 3.0 : 1.0);
  for (let i = 0; i < M.kHairPoints; i += 1) {
    const u = i / (M.kHairPoints - 1);
    const bend = curl * length * Math.sin(M.kPi * u * waves) * u;
    // The pull-down shakes it every frame, the tip most.
    const jitter = tremble * u * normal(seed, MODEL_CPP.kStreamHairTremble, frame * M.kHairPoints + Math.trunc(i / 4));
    const along2 = u * length - 0.01 - out;
    hair.x.push(ax + ux * along2 + vx * (bend + jitter));
    hair.y.push(ay + uy * along2 + vy * (bend + jitter));
  }
  hair.halfWidth = lerp(0.020, 0.035, draw(9)) / M.kApertureHeightMm;
  hair.opacity = 0.92;
  return hair;
}

//--- Model.cpp: the fog ----------------------------------------------------
function fogExposure(fog) {
  const v = clampTo(fog, 0.0, 1.0);
  return (Math.pow(2.0, M.kFogCurve * v) - 1.0) / (Math.pow(2.0, M.kFogCurve) - 1.0) * M.kFogMax;
}

//--- Model.cpp: the reel ---------------------------------------------------
/// INT64_MAX in the plugin: "never". Infinity compares the same way.
const NEVER = Infinity;
const HOLD = 0;
const ROLL = 1;
const WHIP = 2;

class Reel {
  constructor() { this.reset(); }

  reset() {
    this.transitions = [];
    this.pulled = [];
    this.slip = 0.0;
    this.offset = 0.0;
    this.locked = true;
    this.blankRun = 0;
    this.filmEnd = NEVER;
    this.filmBack = NEVER;
    this.flicking = false;
  }

  /// Running out or out (the next press threads a new roll).
  out() {
    return this.transitions.length > 0 && this.transitions[this.transitions.length - 1].direction > 0;
  }

  /// Run Out pressed; the next pull-down will be `next`.
  press(next, ramp) {
    ramp = Math.max(ramp, 1);
    const from = this.fog(next - 1);
    const t = { start: next, direction: this.out() ? -1 : +1, from, ramp, head: 0 };
    // `ramp / 3` is C++ integer division.
    t.head = (t.direction < 0 && from >= 1.0) ? Math.max(M.kHeadCells, Math.trunc(ramp / 3)) : 0;
    if (t.direction > 0) {
      // A new run-out: whatever the last roll's tail did is history.
      this.filmEnd = NEVER;
      this.filmBack = NEVER;
      this.flicking = false;
    } else if (this.filmEnd !== NEVER && next >= this.filmEnd) {
      this.filmBack = next; // a new roll, threaded into an empty gate
    }
    this.transitions.push(t);
    if (this.transitions.length > 8) this.transitions.shift();
  }

  /// The fog on film frame n.
  fog(n) {
    for (let i = this.transitions.length - 1; i >= 0; i -= 1) {
      const it = this.transitions[i];
      if (it.start > n) continue;
      const d = n - it.start;
      if (it.direction > 0) return Math.min(1.0, it.from + (d + 1.0) / it.ramp);
      if (d < it.head) return it.from;
      return Math.max(0.0, it.from - (d - it.head + 1.0) / it.ramp);
    }
    return 0.0;
  }

  /// No film in the gate at frame n (Off the End).
  noFilm(n) {
    return n >= this.filmEnd && n < this.filmBack;
  }

  /// Pull-down k happened (called once for each k, in order): the slip, the
  /// offset, the tail.
  pull(k, tail) {
    const fog = this.fog(k);
    const blank = fog >= 1.0;
    this.blankRun = blank ? this.blankRun + 1 : 0;

    let want = HOLD;
    let t = null;
    for (let i = this.transitions.length - 1; i >= 0; i -= 1) {
      if (this.transitions[i].start <= k) {
        t = this.transitions[i];
        break;
      }
    }

    if (t !== null && t.direction > 0) {
      if (fog >= M.kSlipFog) want = ROLL;
      if (blank && tail === M.kFramed && this.blankRun >= M.kFramedAfter) want = HOLD;
      if (blank && tail === M.kOffTheEnd && this.blankRun >= M.kTailCells && this.filmEnd === NEVER) {
        this.filmEnd = k + 2;
        this.flicking = true;
      }
      if (this.flicking) want = WHIP;
    } else if (t !== null && k === this.filmBack) {
      // The new roll's leading edge comes up into the gate from below.
      this.offset = -0.45;
      this.slip = M.kSlip;
      this.locked = false;
      this.flicking = false;
    }

    if (want === HOLD) {
      if (!this.locked) {
        // The projectionist frames up: the strip keeps creeping the way it
        // was going until the frame line has gone, then the claw seats.
        const s = Math.max(this.slip, 0.5 * M.kSlip);
        const next = this.offset + s;
        if (this.offset < 0.0 && next >= 0.0) {
          this.offset = 0.0;
          this.slip = 0.0;
          this.locked = true;
        } else {
          this.offset = wrapHalf(next);
          this.slip = s;
        }
      }
    } else {
      const target = want === ROLL ? M.kSlip : M.kFlick;
      const step = (want === ROLL ? M.kSlip : M.kFlick) / M.kSlipBuild;
      this.slip += clampTo(target - this.slip, -step, step);
      this.locked = false;
      this.offset = wrapHalf(this.offset + this.slip);
    }

    this.pulled.push({ k, offset: this.offset });
    while (this.pulled.length > 64) this.pulled.shift();
  }

  /// The strip's offset at pull-down k, frames, in [ -0.5, 0.5 ).
  offsetAt(k) {
    // The latest pull-down at or before k: a frame never pulled (the one
    // below the gate) sits where the strip is now.
    for (let i = this.pulled.length - 1; i >= 0; i -= 1) {
      if (this.pulled[i].k <= k) return this.pulled[i].offset;
    }
    return this.pulled.length === 0 ? 0.0 : this.pulled[0].offset;
  }
}

//--- Model.cpp: the tones --------------------------------------------------
function toneLaw(tone, perturb) {
  const density = [1.0, 1.0, 1.0];
  const tint = [1.0, 1.0, 1.0];
  switch (tone) {
    case M.kSepia:
      if (perturb & M.kPerturbToneAsTint) {
        // The negative control: sepia as a coloured base.
        const lo = Math.min(...M.kSepiaDensity);
        for (let i = 0; i < 3; i += 1) tint[i] = Math.pow(10.0, -0.7447 * (M.kSepiaDensity[i] - lo));
      } else {
        for (let i = 0; i < 3; i += 1) density[i] = M.kSepiaDensity[i];
      }
      break;
    case M.kAmber:
      for (let i = 0; i < 3; i += 1) tint[i] = M.kAmberTint[i];
      break;
    case M.kBlue:
      for (let i = 0; i < 3; i += 1) tint[i] = M.kBlueTint[i];
      break;
    case M.kGreen:
      for (let i = 0; i < 3; i += 1) tint[i] = M.kGreenTint[i];
      break;
    default:
      break;
  }
  return { density, tint };
}

//===========================================================================
// FilmRing.cpp: the loop of film. An RGBA8 texture array, one layer per
// exposed picture. It grows; it never clears what it holds: a deeper ring or a
// new raster is a new texture with every old layer copied in (same raster:
// exactly; new raster: bilinear) through the resample shader.
//===========================================================================
class FilmRing {
  constructor(gl, quad, resample) {
    this.gl = gl;
    this.quad = quad;
    this.resample = resample;
    this.texture = null;
    this.fbo = null;
    this.width = 0;
    this.height = 0;
    this.depth = 0;
  }

  // glTexImage3D( ..., nullptr ) in the plugin; texStorage3D here, the same
  // uninitialised RGBA8 layers, which Ensure clears before it copies.
  allocate(w, h, d) {
    const gl = this.gl;
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, t);
    gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.RGBA8, w, h, d);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_BASE_LEVEL, 0);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAX_LEVEL, 0);
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
    return t;
  }

  isValid() { return this.texture !== null; }

  /// At least `d` layers at w x h, every layer it held carried across.
  ensure(w, h, d, keep = true) {
    const gl = this.gl;
    if (this.texture !== null && w === this.width && h === this.height && d <= this.depth) return true;
    if (w <= 0 || h <= 0 || d <= 0) return false;

    const newDepth = Math.max(d, this.depth);
    const fresh = this.allocate(w, h, newDepth);
    if (this.fbo === null) this.fbo = gl.createFramebuffer();

    const previousFbo = gl.getParameter(gl.FRAMEBUFFER_BINDING);
    const previousViewport = gl.getParameter(gl.VIEWPORT);

    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.viewport(0, 0, w, h);
    gl.disable(gl.BLEND);
    let complete = true;
    for (let layer = 0; layer < newDepth; layer += 1) {
      gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, fresh, 0, layer);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
        complete = false;
        break;
      }
      // A new texture's contents are whatever the driver handed back.
      gl.clearColor(0.0, 0.0, 0.0, 0.0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      if (keep && this.texture !== null && layer < this.depth) {
        this.resample.use();
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.texture);
        this.resample.setSampler('Old', 0);
        this.resample.setInt('Layer', layer);
        this.quad.draw();
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
      }
    }
    gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, null, 0, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, previousFbo);
    gl.viewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);

    if (!complete) {
      gl.deleteTexture(fresh);
      return false;
    }
    if (this.texture !== null) gl.deleteTexture(this.texture);
    this.texture = fresh;
    this.width = w;
    this.height = h;
    this.depth = newDepth;
    return true;
  }

  /// This ring's framebuffer with `layer` attached, the viewport its raster.
  bindLayer(layer) {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, this.texture, 0, layer);
    gl.viewport(0, 0, this.width, this.height);
  }

  destroy() {
    const gl = this.gl;
    if (this.fbo !== null) gl.deleteFramebuffer(this.fbo);
    if (this.texture !== null) gl.deleteTexture(this.texture);
    this.fbo = null;
    this.texture = null;
    this.width = 0;
    this.height = 0;
    this.depth = 0;
  }
}

//===========================================================================
// The renderer: Handleman's members and Handleman::ProcessOpenGL.
//===========================================================================

/// What the line under the picture reports, and what a driven check reads.
const telemetry = {
  ticked: false,
  dt: 0,
  p0: 0,
  p1: 0,
  segments: [],
  fps: 16,
  blades: 3,
  degrees: 315,
  displayRate: 0,
  cameraSpeed: 0,
  projectorSpeed: 0,
  loop: 0,
  ringDepth: 0,
  exposed: 0,
  captured: false,
  captures: 0,
  fog: 0,
  out: false,
  noFilm: false,
  scratches: 0,
  particles: 0,
  hair: false,
  missing: [],
};

/// For a driven check (AGENTS.md, "The browser demo"). `fresh()` puts the
/// renderer back to a newly instantiated plugin; `afterRender(gl, input)` runs
/// inside the frame, before the canvas is composited, so it can read it back.
const hooks = { afterRender: null, fresh: null };

function createRenderer(gl, quad) {
  const program = (fragment, label) => new Program(gl, VERTEX, fragment, label);
  const captureShader = program(CAPTURE, 'capture');
  const resampleShader = program(RESAMPLE, 'resample');
  const outputShader = program(OUTPUT, 'output');

  // The print's particles, scratches and hair: one row, texelFetch only.
  const printTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, printTexture);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, SHADERS_H.kPrintDataWidth, 1);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const ring = new FilmRing(gl, quad, resampleShader);

  //--- Handleman's members ---------------------------------------------------
  let running = false;
  let film = 0.0;          // the nominal rate's integral, frames
  let camera = new Hand();
  let projector = new Hand();
  let lastP1 = 0.0;        // the projector's phase at the end of the last exposure
  let lastSpan = 0.0;      // that exposure's length in frames, for a frame the clock did not move
  let lastPulled = 0;
  let lastExposed = 0;
  let exposed = [];        // { frame, layer, exposure }, oldest first
  const seed = HANDLEMAN_CPP.seed;
  let displayFrames = 0;
  let reel = new Reel();
  let runOutHeld = false;
  let lastNow = -1.0;

  const location = (shader, name) => shader.location(`${name}[0]`) ?? shader.location(name);

  /// A frame the camera has not reached yet (the frame below the gate, when
  /// the projector is right behind the camera) shows the newest picture; older
  /// than the ring, the oldest.
  const exposedAt = (frame) => {
    if (exposed.length === 0) return null;
    const back = exposed[exposed.length - 1];
    if (frame >= back.frame) return back;
    if (frame <= exposed[0].frame) return exposed[0];
    return exposed[frame - exposed[0].frame];
  };

  // The page's own measurement of the display rate: the median of the last
  // 60 frame deltas. Reported, never fed to the plugin's maths.
  const deltas = [];

  hooks.fresh = () => {
    ring.destroy();
    running = false;
    film = 0.0;
    camera = new Hand();
    projector = new Hand();
    lastP1 = 0.0;
    lastSpan = 0.0;
    lastPulled = 0;
    lastExposed = 0;
    exposed = [];
    displayFrames = 0;
    reel = new Reel();
    runOutHeld = false;
    lastNow = -1.0;
    telemetry.captures = 0;
  };

  return {
    render({ input, params, width: outWidth, height: outHeight, time }) {
      const width = input.width;
      const height = input.height;
      const aspect = outWidth / outHeight;
      const p = (id) => params.get(id);

      //------------------------------------------------------------------
      // What the controls say.
      //------------------------------------------------------------------
      const fps = controls.crankFps(p('crank'));
      const cameraDev = controls.cameraDeviation(p('cameraHand'));
      const projectorDev = controls.projectorDeviation(p('projectorHand'));
      const blades = controls.blades(controls.optionIndex(p('blades'), CONTROLS_H.kBladesCount));
      const open = controls.shutterOpen(p('shutter'));
      let stock = controls.optionIndex(p('stock'), CONTROLS_H.kStockCount);
      const tone = controls.optionIndex(p('tone'), CONTROLS_H.kToneCount);
      const gamma = controls.gamma(p('contrast'));
      const grain = controls.amount(p('grain'));
      const law = weaveLaw(controls.amount(p('weave')), PERTURB);
      const scratches = controls.amount(p('scratches'));
      const dust = controls.amount(p('dust'));
      const hair = controls.amount(p('hair'));
      const framing = controls.framingHeights(p('framing'));
      const runOutTime = controls.runOutSeconds(p('runOutTime'));
      const tail = controls.optionIndex(p('tail'), CONTROLS_H.kTailCount);
      const vignette = controls.vignetteDegrees(p('vignette'));
      const mixAmount = controls.amount(p('mix'));
      if ((PERTURB & M.kPerturbOrthoAsPan) && stock === 0) stock = 1;

      //------------------------------------------------------------------
      // The clock and the cranks, frame-relative, in double. The first frame
      // is worth a nominal display frame.
      //------------------------------------------------------------------
      const now = time;
      let dt = M.kNominalFrame;
      if (lastNow >= 0.0) dt = clampTo(now - lastNow, 0.0, M.kMaxFrameDelta);
      lastNow = now;
      displayFrames += 1;

      const projectorPhase = () => {
        const loop = (PERTURB & M.kPerturbNoLoop) ? 0.0 : camera.amplitude + projector.amplitude;
        return film - projector.error() - loop;
      };

      const first = !running;
      if (first) {
        camera.start(seed, H.kStreamHandCamera, M.kCameraHand, M.kCameraPartials, cameraDev);
        projector.start(seed, H.kStreamHandProjector, M.kProjectorHand, M.kProjectorPartials, projectorDev);
        film = 0.0;
        lastP1 = projectorPhase();
        reel.reset();
      }
      camera.advance(dt, fps, cameraDev);
      projector.advance(dt, fps, projectorDev);
      film += fps * dt;

      const cameraPhase = film - camera.error();
      const p1 = Math.max(projectorPhase(), lastP1);
      let p0 = lastP1;
      if (dt > 0.0 || first) lastSpan = p1 - lastP1;
      else p0 = p1 - lastSpan;
      lastP1 = p1;
      running = true;

      const segs = segments(p0, p1, blades, open, (PERTURB & M.kPerturbInstant) !== 0);
      const count = segs.length;
      const oldest = segs[0].frame;
      const latest = segs[count - 1].frame;

      // Every pull-down up to the newest frame in the gate, in order: the
      // reel's slip and tail are decided one pull-down at a time.
      if (first) lastPulled = oldest - 1;
      for (let k = lastPulled + 1; k <= latest; k += 1) reel.pull(k, tail);
      lastPulled = Math.max(lastPulled, latest);

      // Run Out, on the press: from the next pull-down. An event in the
      // plugin; a toggle here, released below once it acts.
      const runOutPressed = f(p('runOut')) >= 0.5;
      if (runOutPressed && !runOutHeld) {
        reel.press(lastPulled + 1, Math.max(1, lround(runOutTime * fps)));
      }
      runOutHeld = runOutPressed;

      //------------------------------------------------------------------
      // The camera. Every frame its crank passed since the last display
      // frame is exposed to the host's picture now (one capture shared by
      // all of them); each gets its own exposure.
      //------------------------------------------------------------------
      const newestCamera = Math.floor(cameraPhase);
      if (first) lastExposed = Math.min(oldest - 3, newestCamera - 1);
      const expose = newestCamera > lastExposed;

      // Frames the shader may read from here on: two above the oldest segment.
      const keepFrom = oldest - 2;
      while (exposed.length > 1 && exposed[0].frame < keepFrom) exposed.shift();

      let captureLayer = -1;
      if (expose) {
        const used = new Set(exposed.map((e) => e.layer));
        for (let layer = 0; layer < ring.depth; layer += 1) {
          if (!used.has(layer)) {
            captureLayer = layer;
            break;
          }
        }
        if (captureLayer < 0) {
          if (ring.depth < H.kMaxRing) captureLayer = Math.max(ring.depth, used.size);
          else {
            // Only past the deepest ring: the oldest frame gives up its layer.
            captureLayer = exposed[0].layer;
            exposed.shift();
          }
        }
      }

      //------------------------------------------------------------------
      // Buffers. A raster change or a deeper ring COPIES the ring: the loop
      // of film is still to be projected. Deeper only when a capture needs a
      // layer the ring has not got, and then with two to spare.
      //------------------------------------------------------------------
      const oldDepth = ring.depth;
      let wantDepth = Math.max(captureLayer + 1, 1);
      if (wantDepth > oldDepth) wantDepth = Math.min(wantDepth + 2, H.kMaxRing);
      ring.ensure(width, height, wantDepth, true);

      if (expose) {
        const speed = (PERTURB & M.kPerturbExposureProjector) ? fps - projector.errorRate(fps) : fps - camera.errorRate(fps);
        const base = fps / Math.max(speed, 0.05 * fps);
        for (let m = lastExposed + 1; m <= newestCamera; m += 1) {
          const jerk = Math.exp(M.kExposureJitter * cameraDev * normal(seed, H.kStreamJitter, m));
          exposed.push({ frame: m, layer: captureLayer, exposure: base * jerk });
        }
        lastExposed = newestCamera;

        ring.bindLayer(captureLayer);
        captureShader.use();
        bindTexture(gl, 0, input.texture);
        captureShader.setSampler('Source', 0);
        gl.disable(gl.BLEND);
        quad.draw();
        bindTexture(gl, 0, null);
        telemetry.captures += 1;
      }

      //------------------------------------------------------------------
      // The frame table: every frame the segments can show, from two above
      // the oldest to two below the newest.
      //------------------------------------------------------------------
      const tableFirst = oldest - 2;
      const frameCount = (latest - oldest) + 5;
      const frameLayer = new Int32Array(frameCount);
      const frameFog = new Float32Array(frameCount);
      const frameExposure = new Float32Array(frameCount);
      const frameSeed = new Int32Array(frameCount);
      for (let i = 0; i < frameCount; i += 1) {
        const n = tableFirst + i;
        const fog = reel.fog(n);
        const e = exposedAt(n);
        frameLayer[i] = reel.noFilm(n) ? -2 : fog >= 1.0 ? -1 : (e ? e.layer : -1);
        frameFog[i] = fogExposure(fog);
        frameExposure[i] = e ? e.exposure : 1.0;
        const grainKey = (PERTURB & M.kPerturbGrainPerDisplay) ? displayFrames * 16 + i : n;
        frameSeed[i] = key(seed, H.kStreamGrain, grainKey) & 0x7fffffff;
      }

      //------------------------------------------------------------------
      // The segments: where each frame sits in the gate, and the print on it.
      //------------------------------------------------------------------
      const weights = new Float32Array(count);
      const offsets = new Float32Array(2 * count);
      const ownFrame = new Int32Array(count);
      const data = new Float32Array(SHADERS_H.kPrintDataWidth * 4);
      let particleCount = 0;
      let hairFrom = -1;
      let hairMask = 0;
      let hairShape = { present: false, halfWidth: 0.0, opacity: 0.0 };
      for (let i = count - 1; i >= 0; i -= 1) {
        const k = segs[i].frame;
        const [wx, wy] = weaveOffset(seed, k, law);

        // Framing (+ moves the picture down) and the reel's slip (+ up), in
        // frames; whole frames move which frame is in the gate, the rest is
        // the offset the shader draws.
        const o = -framing / M.kPitch + reel.offsetAt(k);
        const shift = Math.floor(o + 0.5);
        const residue = o - shift;
        const own = k + shift;

        weights[i] = segs[i].weight;
        offsets[2 * i] = wx;
        offsets[2 * i + 1] = -residue * M.kPitch + wy;
        ownFrame[i] = clampTo(own - tableFirst, 0, frameCount - 1);

        if (!reel.noFilm(own)) {
          for (const q of dustOn(seed, own, dust, aspect)) {
            if (particleCount >= M.kMaxParticles) break;
            const t0 = 8 * particleCount;
            data[t0 + 0] = q.x;
            data[t0 + 1] = q.y;
            data[t0 + 2] = q.a;
            data[t0 + 3] = q.b;
            data[t0 + 4] = Math.cos(q.angle);
            data[t0 + 5] = Math.sin(q.angle);
            data[t0 + 6] = q.strength;
            data[t0 + 7] = i + 8 * q.fibre;
            particleCount += 1;
          }
        }

        const h = hairAt(seed, k, hair, aspect);
        if (h.present) {
          hairMask |= 1 << i;
          if (hairFrom < 0) {
            hairFrom = i;
            hairShape = h;
          }
        }
      }

      const alive = scratchesAlive(seed, oldest, latest, scratches, aspect);
      let scratchCount = 0;
      for (const s of alive) {
        if (scratchCount >= M.kMaxScratches) break;
        let mask = 0;
        for (let i = 0; i < count; i += 1) {
          if (segs[i].frame >= s.born && segs[i].frame < s.dies) mask |= 1 << i;
        }
        const t = 4 * (SHADERS_H.kPrintScratchFirst + 2 * scratchCount);
        data[t + 0] = s.left;
        data[t + 1] = s.right;
        data[t + 2] = s.strength;
        data[t + 3] = s.side;
        data[t + 4] = mask;
        scratchCount += 1;
      }

      if (hairShape.present) {
        for (let i = 0; i < M.kHairPoints; i += 1) {
          const t = 4 * (SHADERS_H.kPrintHairFirst + i);
          data[t + 0] = hairShape.x[i];
          data[t + 1] = hairShape.y[i];
        }
      }

      gl.bindTexture(gl.TEXTURE_2D, printTexture);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, SHADERS_H.kPrintDataWidth, 1, gl.RGBA, gl.FLOAT, data);
      gl.bindTexture(gl.TEXTURE_2D, null);

      //------------------------------------------------------------------
      // The projection, straight to the host (here, the canvas).
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, outWidth, outHeight);

      outputShader.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D_ARRAY, ring.texture);
      bindTexture(gl, 1, input.texture);
      bindTexture(gl, 2, printTexture);

      const { density: toning, tint } = toneLaw(tone, PERTURB);
      const basis = SPECTRAL_WEIGHTS[clampTo(stock, 0, SPECTRAL_H.kStockCount - 1)][1];

      outputShader.setSampler('Ring', 0);
      outputShader.setSampler('Source', 1);
      outputShader.setSampler('PrintData', 2);
      gl.uniform2i(outputShader.location('RingSize'), ring.width, ring.height);
      outputShader.set('OutSize', outWidth, outHeight);
      outputShader.setInt('SegCount', count);
      gl.uniform1fv(location(outputShader, 'SegWeight'), weights);
      gl.uniform2fv(location(outputShader, 'SegOffset'), offsets);
      gl.uniform1iv(location(outputShader, 'SegFrame'), ownFrame);
      outputShader.setInt('FrameCount', frameCount);
      gl.uniform1iv(location(outputShader, 'FrameLayer'), frameLayer);
      gl.uniform1fv(location(outputShader, 'FrameFog'), frameFog);
      gl.uniform1fv(location(outputShader, 'FrameExposure'), frameExposure);
      gl.uniform1iv(location(outputShader, 'FrameSeed'), frameSeed);
      outputShader.set('Pitch', M.kPitch);
      outputShader.set('Margin', M.kMargin);
      const lineStart = 1.0 + M.kMargin + ((PERTURB & M.kPerturbFrameLineHalf) ? 0.5 * M.kFrameLine : 0.0);
      outputShader.set('LineStart', lineStart);
      outputShader.set('BaseDensity', M.kBaseDensity);
      outputShader.set('MaxDensity', M.kMaxDensity);
      outputShader.set('Knee', M.kKnee);
      outputShader.set('GreyLog', Math.log10(M.kGreyExposure));
      outputShader.set('Gamma', gamma);
      outputShader.setInt('Colour', tone === M.kColour ? 1 : 0);
      gl.uniform1fv(location(outputShader, 'Basis'), new Float32Array(basis));
      outputShader.set('Toning', toning[0], toning[1], toning[2]);
      outputShader.set('Tint', tint[0], tint[1], tint[2]);
      outputShader.set('FogFlat', M.kFogFlat);
      outputShader.set('FogReach', M.kFogReach);
      outputShader.set('GrainAmount', M.kGrainSigma * grain);
      outputShader.set('GrainFloor', M.kGrainFloor);
      outputShader.set('GrainCell', M.kGrainMm / M.kApertureHeightMm);
      outputShader.setInt('ParticleCount', particleCount);
      outputShader.setInt('ScratchCount', scratchCount);
      outputShader.setInt('HairPoints', hairShape.present ? M.kHairPoints : 0);
      outputShader.setInt('HairMask', hairMask);
      outputShader.set('HairHalfWidth', hairShape.halfWidth);
      outputShader.set('HairOpacity', hairShape.opacity);
      outputShader.set('VignetteTan', Math.tan(vignette * M.kPi / 180.0));
      outputShader.set('Aspect', aspect);
      outputShader.set('MixAmount', mixAmount);
      outputShader.setInt('Probe', PROBE);
      outputShader.setInt('Perturb', PERTURB);
      gl.disable(gl.BLEND);
      quad.draw();

      // The event, released on the frame it acted (a host's press is short).
      if (runOutPressed) params.set('runOut', 0);

      if (dt > 0.0) {
        deltas.push(dt);
        if (deltas.length > 60) deltas.shift();
      }
      const sorted = [...deltas].sort((a, b) => a - b);
      telemetry.displayRate = sorted.length ? 1.0 / sorted[Math.floor(sorted.length / 2)] : 0;
      telemetry.ticked = true;
      telemetry.dt = dt;
      telemetry.p0 = p0;
      telemetry.p1 = p1;
      telemetry.segments = segs;
      telemetry.fps = fps;
      telemetry.blades = blades;
      telemetry.degrees = open * 360.0;
      telemetry.cameraSpeed = fps - camera.errorRate(fps);
      telemetry.projectorSpeed = fps - projector.errorRate(fps);
      telemetry.loop = cameraPhase - p1;
      telemetry.ringDepth = ring.depth;
      telemetry.exposed = exposed.length;
      telemetry.captured = expose;
      telemetry.fog = reel.fog(latest);
      telemetry.out = reel.out();
      telemetry.noFilm = reel.noFilm(latest);
      telemetry.scratches = scratchCount;
      telemetry.particles = particleCount;
      telemetry.hair = hairShape.present;
      // A uniform name the shader does not have is a dead control (glUniform
      // on -1 is a no-op); the kit counts them. Empty is right.
      telemetry.missing = [...captureShader.missing, ...resampleShader.missing, ...outputShader.missing];

      if (hooks.afterRender) hooks.afterRender(gl, input);
      bindTexture(gl, 2, null);
      bindTexture(gl, 1, null);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
    },
  };
}

//===========================================================================
// The controls, read out of Handleman::Handleman(). Same names, same groups,
// same order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: f(def), group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const button = (id, name, group, hint) => ({ id, name, type: 'boolean', default: 0, group, hint });
const signed = (x, digits) => `${x >= 0 ? '+' : '−'}${Math.abs(x).toFixed(digits)}`;
const percent = (x) => `±${(100 * x).toFixed(0)}%`;

const demo = mountDemo({
  name: 'Handleman',
  pluginId: 'HM01',
  kind: 'effect',
  tagline:
    'A hand-cranked camera and projector, for your clip. The camera’s hand decides when each frame was exposed, so motion rushes and drags and a frame cranked slowly prints lighter; the projector’s decides how long each frame stays on the screen and how fast the shutter flickers. The print is orthochromatic or panchromatic silver, black and white, sepia toned or tinted, with grain, weave, scratches, dust and a hair in the gate. Run Out fogs the roll to blank cells and the frame line crawls up the screen; press it again to thread a new roll.',
  repo: 'https://github.com/stoatworks-labs/handleman',
  video: 'https://www.youtube.com/watch?v=yHAiZe-qLzk',

  // The stock sentence says "same maths", which is most of the truth: the
  // print runs in the plugin's shaders, but the two hands, the shutter's
  // weights, the reel and the print's marks are computed on the CPU, and that
  // half is a hand port.
  blurb:
    'It is Handleman’s own GLSL — the capture, resample and output passes — ported from the repository to WebGL2, so the loop of film (a stack of 8-bit pictures, one per frame the camera exposed), the strip and its frame line, the stock, the print’s curve, the grain, toning and tints, the scratches, dust and hair and the shutter’s weighted sum run on your GPU as they do in the plugin. The CPU half — the two hands and the loop between them, the shutter’s light per projector frame, the weave, every scratch, speck and hair, the run-out’s fog, slip and tails, and every control’s law — is ported to JavaScript by hand. The flicker beats against this browser’s own frame rate, so it depends on your display. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  params: [
    std('crank', 'Crank Rate', controls.crankParam(16.0), 'Crank', {
      display: (v) => `${controls.crankFps(v).toFixed(1)} fps`,
      hint: 'How fast both handles turn, 8 to 24 frames a second in a straight line (0.5 is 16). A turn is eight frames. It is the camera’s rate and the projector’s; a change carries on from where both cranks are.',
    }),
    std('cameraHand', 'Camera Hand', 0.3, 'Crank', {
      display: (v) => percent(controls.cameraDeviation(v)),
      hint: 'How far the cameraman’s speed wanders, up to 50% of Crank Rate either way: once a turn, a harmonic, and a count that drifts over five and nine turns. A slow stretch spaces the frames out in the scene’s time and exposes each for longer, so the action rushes and the picture lightens; a hurried one drags and darkens. At 0 the camera turns like a motor.',
    }),
    std('projectorHand', 'Projector Hand', 0.3, 'Crank', {
      display: (v) => percent(controls.projectorDeviation(v)),
      hint: 'How far the projectionist’s speed wanders, up to 40% of Crank Rate either way. Some frames stay up longer than others and the shutter’s flicker speeds up and slows down with the handle. It never changes which picture is shown or what is in it.',
    }),
    opt('blades', 'Blades', BLADES_NAMES, 2, 'Crank',
      'How many times the shutter cuts the light per frame. The pull-down always happens behind a blade. The lamp flickers Blades × Crank Rate times a second, 48 at the defaults, and that beats against your display.'),
    std('shutter', 'Shutter Angle', controls.shutterParam(315.0), 'Crank', {
      display: (v) => `${controls.shutterDegrees(v).toFixed(1)}° open`,
      hint: 'Each blade’s opening, 45° to 315° in a straight line (0.5 is 180°). Wider flickers less. The light is normalised to the opening, so a narrow shutter’s lit moments are brighter than the picture and whites clip. Three blades at 315°, the default, is the gentlest there is.',
    }),

    opt('stock', 'Stock', STOCK_NAMES, 0, 'Print',
      'What the negative saw. Orthochromatic is blind to red: red exposes it 4.8% as much as white, green 33%, blue 65%, so red lips go dark and skies go pale (an assumed sensitivity through Smits’ spectra). Panchromatic is Rec. 709 luminance. Nothing to do with Tone on Colour.'),
    opt('tone', 'Tone', TONE_NAMES, M.kSepia, 'Print',
      'What became of the silver. Colour keeps the clip’s colours, each channel through the curve on its own. Sepia toning scales the silver’s density by (0.70, 0.93, 1.35), so shadows go brown and white stays white. A tint is a dye in the base: it multiplies the light, so white goes the colour and black stays black.'),
    std('contrast', 'Contrast', 0.5, 'Print', {
      display: (v) => `γ ${controls.gamma(v).toFixed(2)}`,
      hint: 'The print’s gamma, 0.6 to 2.2 (0.5 is 1.4): the slope of its curve, which pivots on an 18% grey, with soft knees at the clear base and the print’s deepest black.',
    }),
    std('grain', 'Grain', 0.35, 'Print', {
      hint: 'Silver grain: noise in the print’s density, in clumps 0.02 mm across, more where there is more silver. Each film frame has its own, so it holds still while a frame is up and changes at every pull-down.',
    }),

    std('weave', 'Weave', 0.35, 'Wear', {
      display: (v) => `σ ${(M.kWeaveMm * controls.amount(v)).toFixed(3)} mm`,
      hint: 'How far each frame lands off true in the gate: 0.15 mm of film at 1, 0.6 of that up and down, each frame like the last by a correlation of 0.75.',
    }),
    std('scratches', 'Scratches', 0.35, 'Wear', {
      hint: 'How much grit gets into the gate. Each bit cuts a scratch at one place across the frame on every frame that goes past, so it stays put while the picture moves and weaves under it. Base side dark, emulsion side white.',
    }),
    std('dust', 'Dust', 0.35, 'Wear', {
      hint: 'Specks and fibres riding the print for one to four frames: on average 8 × Dust new ones a frame. Most are dark; about one in seven is white, dirt that was on the negative.',
    }),
    std('hair', 'Hair', 0.25, 'Wear', {
      hint: 'The chance, in each 96-frame stretch, that a hair gets caught at an edge of the aperture. It stays 36 to 92 frames, trembles at every pull-down and slides out over its last four.',
    }),
    std('framing', 'Framing', 0.5, 'Wear', {
      display: (v) => `${signed(controls.framingHeights(v), 3)} h`,
      hint: 'The framing knob, ±half a picture height (0.5 is centred). Past 2.2% of the height either way comes the frame line, 5.7% of the height, and past that the next frame. Below the middle moves the picture up.',
    }),

    button('runOut', 'Run Out', 'Run Out',
      'The first press runs the roll out: from the next pull-down the picture fogs to white from the edges in, then blank cells, and the frame line crawls up the screen. The next press threads a new roll. An event button in the plugin; here a toggle the renderer releases on the frame it acts, which is why it blinks.'),
    std('runOutTime', 'Run Out Time', controls.runOutParam(1.0), 'Run Out', {
      display: (v) => `${controls.runOutSeconds(v).toFixed(2)} s`,
      hint: 'How long the fog takes, 0.25 to 4 seconds, each quarter of the slider doubling it (0.5 is 1 s). Turned into film frames at the press, so a change counts from the next press. It also sets how long a new roll takes to clear.',
    }),
    opt('tail', 'Tail', TAIL_NAMES, M.kRolling, 'Run Out',
      'What happens once the cells are blank. Rolling: the frame line keeps crawling. Framed: after 24 blank cells the strip creeps on until the line has gone, and you are left on a still blank cell. Off the End: after 24 the tail whips out of the gate and the lamp shines on nothing. Read as each cell goes through.'),

    std('vignette', 'Vignette', 0.35, 'Output', {
      display: (v) => `${controls.vignetteDegrees(v).toFixed(1)}°`,
      hint: 'The lens’s half-angle to the corner, 0° to 35°; the light there is cos⁴ of it.',
    }),
    std('mix', 'Mix', 1.0, 'Output', {
      hint: 'The projection over the clip. Alpha is mix(clip alpha, 1, Mix): at 1 the projection paints the whole frame, and a transparent part of the clip prints as black film.',
    }),
  ],

  // A moving picture first: the hands need motion to be seen. Bars show the
  // stocks (orthochromatic red goes dark); the ramp shows the curve and the
  // tones; the geometry card makes the weave and the frame line readable.
  sources: ['scene', 'bars', 'ramp', 'grid', 'spot', 'alpha'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls; most
  // are the user guide's recipes.
  presets: {
    'Steady hands (two motors)': { cameraHand: 0, projectorHand: 0 },
    'Both hands at full': { cameraHand: 1, projectorHand: 1 },
    'Black & white, panchromatic': { tone: M.kBlackAndWhite, stock: 1 },
    'Red-blind (black & white, orthochromatic)': { tone: M.kBlackAndWhite, stock: 0 },
    'A night scene tinted blue': { tone: M.kBlue, contrast: 0.6, vignette: 0.6 },
    'Amber tint': { tone: M.kAmber },
    'Colour (the hands and the wear only)': { tone: M.kColour },
    'A steady modern restoration': {
      projectorHand: 0, weave: 0, scratches: 0, hair: 0, dust: 0.05, grain: 0.2,
      tone: M.kBlackAndWhite, vignette: 0.15, crank: controls.crankParam(20.0),
    },
    'A rough nickelodeon print': {
      crank: controls.crankParam(14.0), cameraHand: 0.8, projectorHand: 0.8, contrast: 0.75, grain: 0.7,
      weave: 0.8, scratches: 0.9, dust: 0.8, hair: 1, vignette: 0.8, framing: 0.45, tone: M.kBlackAndWhite,
    },
    'Framed off (the frame line)': { framing: 0.4 },
    'Tail: Off the End': { tail: M.kOffTheEnd },
  },

  differences: [
    'The print runs in the plugin’s own shaders, and those are not a port. The capture, resample and output bodies are the plugin’s GLSL, assembled as Shaders.cpp assembles them; the numbers they are handed (the print-data layout, the film’s geometry, the curve, the grain, the fog, the tones, both stocks’ Smits weights, the option lists, the ring’s cap and the print’s seed) are copied by a script. demo/tools/check_shaders.py fails the repository’s verify script if a character of any of it drifts.',
    'The CPU half is a PORT, not the plugin’s own code: Model.cpp’s PCG hash and its keys, the two hands (their partials, phases and slew-limited amplitudes) and the loop between them, the shutter’s closed form (CumulativeOpen) and the per-projector-frame weights (Segments), the weave’s AR(1) moving-average sum, the scratches’ birth-and-death scan, the dust’s Poisson births, the hair, the fog’s exposure, the Reel’s run-out state machine (the press, the slip, the offset, the three tails, a new roll) and the tones’ two laws; Controls.cpp’s laws; FilmRing.cpp’s growth by copying; and Handleman::ProcessOpenGL’s clock, cranks, exposure window, pull-downs, the camera’s exposures and their ring layers, the frame table and the print-data upload. All of it is ported by hand, in JavaScript doubles as the plugin keeps them, with the hash in uint32 through Math.imul, rounded to float where the plugin holds a float. The repository’s hmtest --crank, --exposure, --runout and the rest check the C++, not this page; a crosscheck in the repository (demo/tools/crosscheck) renders the page and hmtest --pipe from the same input frames and compares them, when somebody runs it.',
    'The flicker depends on your display. The plugin adds the shutter’s light up over each of the host’s frames, and here the host’s frame is the browser’s requestAnimationFrame: 48 Hz of blade (3 blades at 16 fps) beats at 12 Hz against a 60 Hz display, differently against 120 or 144 Hz, and the projector’s hand moves it about either way. A browser that drops frames changes the beat, as a host that drops frames would. The line under the picture gives this browser’s rate and the beat it makes.',
    'The camera only sees what the page hands it: every film frame its crank passes during a browser frame is exposed to that frame’s picture of the clip, as it is to the host’s in Resolume. The generated clips are a function of time, so a slow display shares a picture between frames sooner than a 60 Hz one would.',
    'The hands, the jerk, the slip, the fog’s curve, the grain, the tones’ colours and the marks are the plugin’s chosen numbers, not measurements of a cameraman, a projector or a print, and the orthochromatic sensitivity is an assumed shape. The published figures are the eight frames a turn and the film’s apertures and pitch.',
    'The clock is the kit’s, in declared seconds; the plugin’s unit vote and wall-clock fallback never run. Everything downstream is the plugin’s rule: dt is the frame delta clamped to 0–0.25 s (the kit itself caps a delta at 0.1 s), a nominal 1/60 on the first frame. A paused page renders only when a control moves, and each such frame is worth 0 s: the cranks stand still. Restart sends the clock back to 0, which the plugin’s clamp reads as a frame of 0 s, so both cranks carry on where they were; Step adds exactly 1/60 s.',
    'Run Out is an FF_TYPE_EVENT button in the plugin. The kit has no event type, so it is a toggle here that the renderer releases on the frame it acts, which is what a host does with an event anyway, and why the button blinks.',
    'The plugin stores each host value as a float; the page’s sliders are doubles, so every value is rounded through Math.fround before its law is applied, and the defaults are the plugin’s float defaults (Shutter Angle 1, which is 315°; Crank Rate and Run Out Time 0.5, which are 16 fps and 1 s).',
    'The print’s seed is the plugin’s fixed one, so this page shows the same print and the same two hands — the same scratches, specks and hairs at the same film frames — as every instance of the plugin. The cranks start when the page loads, as they do when the plugin is instantiated, and the loop is threaded with the first picture, as the plugin’s is.',
    'The harness-only Perturb and Probe uniforms are set to what the shipped plugin sets them to, 0. The ten negative controls and the linear-light probe hmtest reads through them are not on this page. The About block is absent, as on every page in this suite.',
  ],

  createRenderer,
});

// For a driven check (AGENTS.md, "The browser demo"): the kit's state and
// redraw, the telemetry, and the hooks, so a script can pause, set the clock
// to n / 60 and render one frame at a time, as `hmtest --pipe --fps 60` does.
window.__handlemanDemo = {
  demo,
  telemetry,
  hooks,
  port: { hash, key, normal, segments, weaveLaw, weaveOffset, scratchesAlive, dustOn, hairAt, fogExposure, toneLaw, Hand, Reel },
};

//---------------------------------------------------------------------------
// On the page: a photosensitivity note above the picture, and under it a line
// reporting the display rate, the beat, the two hands and the reel. Skipped in
// embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const note = document.createElement('p');
    note.className = 'stage__status';
    note.setAttribute('role', 'note');
    note.textContent =
      'Photosensitivity: the shutter flickers the whole picture, mostly at 12–15 Hz on a 60 Hz display, and the projector’s hand moves the beat about. '
      + 'The plugin’s defaults, 3 blades at 315°, are the gentlest shutter it has; 2 blades at 180° is far stronger, and every Run Out ends on a white screen with the same flicker. '
      + 'Pause stops it.';
    stage.prepend(note);

    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      if (!telemetry.ticked) return;
      const t = telemetry;
      const blade = t.blades * t.projectorSpeed;
      const rate = t.displayRate;
      const beat = rate > 0 ? Math.abs(blade - Math.round(blade / rate) * rate) : 0;
      const shown = t.segments.map((s) => `${s.frame} ×${s.weight.toFixed(2)}`).join(', ');
      let reel = 'the roll running';
      if (t.noFilm) reel = 'off the end: the lamp on an empty gate';
      else if (t.out && t.fog >= 1.0) reel = 'run out: blank cells';
      else if (t.out) reel = `running out: fog ${(100 * t.fog).toFixed(0)}%`;
      else if (t.fog > 0.0) reel = `a new roll threading: fog ${(100 * t.fog).toFixed(0)}%`;
      line.textContent =
        `Display: ${rate > 0 ? `${rate.toFixed(1)} Hz` : '—'} (this browser’s frame clock, which the plugin would take from the host). `
        + `Crank ${t.fps.toFixed(1)} fps; the camera turning at ${t.cameraSpeed.toFixed(1)}, the projector at ${t.projectorSpeed.toFixed(1)}, `
        + `${t.loop.toFixed(2)} frames of film between them, ${t.ringDepth} pictures’ room in the loop. `
        + `Shutter: ${t.blades} blade${t.blades === 1 ? '' : 's'} at ${t.degrees.toFixed(0)}°, ${blade.toFixed(1)} Hz`
        + `${rate > 0 ? `, beating against this display at ${beat.toFixed(1)} Hz` : ''}. `
        + `This frame: projector frame${t.segments.length === 1 ? '' : 's'} ${shown}`
        + `${t.captured ? '; the camera exposed a picture' : ''}; `
        + `${t.scratches} scratch${t.scratches === 1 ? '' : 'es'}, ${t.particles} speck${t.particles === 1 ? '' : 's'}${t.hair ? ', a hair' : ''}; ${reel}.`;
    }, 250);
  }
}
