#include "Shaders.h"

namespace handleman::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// capture: the host's picture into a ring layer, texel for texel. The ring
// keeps the code values; the output pass decodes them.
//---------------------------------------------------------------------------
const char* const kCaptureBody = R"(
uniform sampler2D Source;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texelFetch( Source, ivec2( gl_FragCoord.xy ), 0 );
}
)";

//---------------------------------------------------------------------------
// resample: one layer of the old ring onto the new one.
//---------------------------------------------------------------------------
const char* const kResampleBody = R"(
uniform sampler2DArray Old;
uniform int Layer;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( Old, vec3( uv, float( Layer ) ) );
}
)";

//---------------------------------------------------------------------------
// output: the strip, the print, the shutter's sum and the lens.
//---------------------------------------------------------------------------
const char* const kOutputBody = R"(
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
)";

std::string assemble( const char* body )
{
	return std::string( kVersion ) + body;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}

std::string Capture()
{
	return assemble( kCaptureBody );
}

std::string Resample()
{
	return assemble( kResampleBody );
}

std::string Output()
{
	return assemble( kOutputBody );
}

} // namespace handleman::shaders
