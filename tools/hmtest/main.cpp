/**
	hmtest -- render Handleman offline, and measure the two hands out of it.

	Every check here drives the REAL plugin class through a headless GL
	context on a synthetic clock and measures the property it claims from the
	picture the plugin renders, against a prediction derived here by a
	different route from the one the plugin takes. Most of them feed a coded
	picture whose top eighth says, in black and white blocks, which display
	frame it was: so the picture on the screen says when the camera exposed it.

		hmtest --out /tmp/frame.png     a picture, on the moving test card
		hmtest --list                   every parameter, its kind and default
		hmtest --crank                  each hand moves what it should and
		                                nothing else; the loop is bounded
		hmtest --exposure               a frame cranked slowly prints lighter
		hmtest --flicker                the shutter's spectrum, the beat on top
		hmtest --tone                   neutral, gamma, toning, tints, stocks
		hmtest --hold                   grain and dust belong to the film frame
		hmtest --scratch                scratches stay put in the gate
		hmtest --weave                  the weave's sigma and correlation
		hmtest --runout                 blank cells, the frame line, the tails,
		                                threading a new roll
		hmtest --resize                 the loop survives a resize
		hmtest --alpha                  what happens to the host's alpha
		hmtest --negative               every check above can FAIL
		hmtest --names                  nothing the host will truncate
		hmtest --bench                  the render cost and the state held
		hmtest --dump-shaders DIR       the exact GLSL the plugin compiles
		hmtest --pipe                   raw frames in, raw frames out

	AGENTS.md has one line per check on where each tolerance comes from.
*/

#include "Controls.h"
#include "Handleman.h"
#include "Model.h"
#include "Shaders.h"
#include "Spectral.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model    = handleman::model;
namespace controls = handleman::controls;

int g_checks   = 0;
int g_failures = 0;

/// 2^-24: a float's relative rounding error, half an ULP at 1.
constexpr double kU = 5.9604644775390625e-8;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first, in sRGB code values (what a host hands an
// effect).
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double level )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ]                       = 1.0f;
	}
	return p;
}

void put( Picture& p, int W, int x, int y, double level )
{
	float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
	px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( level );
	px[ 3 ]                     = 1.0f;
}

/// The sRGB decode, in double: what the capture pass computes in float.
double decode( double c )
{
	return c <= 0.04045 ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.4 );
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//HMTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not bit-repeatable frame to frame (repousse's resize check failed CI
	//by one ulp), so a check that would fail only in CI can be run here first.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "HMTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "hmtest: HMTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Handleman::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Handleman& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Handleman::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Handleman& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Handleman& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Handleman& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// The machine a check runs: the controls it moves, everything else OFF
/// (no hands, no marks, no grain, no weave, no lens falloff, centred, black
/// and white panchromatic), so each check sees only what it measures.
struct Knobs
{
	double fps       = 16.0;
	double camera    = 0.0;///< Camera Hand, 0..1
	double projector = 0.0;///< Projector Hand, 0..1
	int blades       = 1;  ///< 2
	double degrees   = 180.0;
	int stock        = 1;  ///< Panchromatic
	int tone         = model::kBlackAndWhite;
	double gamma     = 1.4;
	double grain     = 0.0;
	double weave     = 0.0;
	double scratches = 0.0;
	double dust      = 0.0;
	double hair      = 0.0;
	double framing   = 0.0;
	double runOut    = 1.0;///< seconds
	int tail         = model::kRolling;
	double vignette  = 0.0;
};

void apply( Handleman& p, const Knobs& k )
{
	set( p, "Crank Rate", controls::CrankParam( k.fps ) );
	set( p, "Camera Hand", static_cast< float >( k.camera ) );
	set( p, "Projector Hand", static_cast< float >( k.projector ) );
	set( p, "Blades", static_cast< float >( k.blades ) );
	set( p, "Shutter Angle", controls::ShutterParam( k.degrees ) );
	set( p, "Stock", static_cast< float >( k.stock ) );
	set( p, "Tone", static_cast< float >( k.tone ) );
	set( p, "Contrast", controls::GammaParam( k.gamma ) );
	set( p, "Grain", static_cast< float >( k.grain ) );
	set( p, "Weave", static_cast< float >( k.weave ) );
	set( p, "Scratches", static_cast< float >( k.scratches ) );
	set( p, "Dust", static_cast< float >( k.dust ) );
	set( p, "Hair", static_cast< float >( k.hair ) );
	set( p, "Framing", controls::FramingParam( k.framing ) );
	set( p, "Run Out Time", controls::RunOutParam( k.runOut ) );
	set( p, "Tail", static_cast< float >( k.tail ) );
	set( p, "Vignette", static_cast< float >( k.vignette ) );
	set( p, "Mix", 1.0f );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Handleman plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// Seconds added to every frame's clock from frame 1 on: moves the
	/// projector's phase against the display's, so no display frame's
	/// exposure ends exactly on a pull-down (where the plugin's accumulated
	/// double could fall either side).
	double lead = 0.0;
	/// Render into an RGBA32F framebuffer rather than RGBA8.
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	void upload( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void upload( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// Rows [ y0, y1 ) from the TOP of a whole picture: a host's frame that
	/// changed only there costs only those rows.
	void uploadRows( const Picture& pixels, int y0, int y1 )
	{
		std::vector< float > rows( static_cast< size_t >( y1 - y0 ) * width * 4 );
		for( int y = y0; y < y1; ++y )
			std::copy( pixels.begin() + static_cast< long >( static_cast< size_t >( y ) * width * 4 ),
			           pixels.begin() + static_cast< long >( static_cast< size_t >( y + 1 ) * width * 4 ),
			           rows.begin() + static_cast< long >( static_cast< size_t >( y1 - 1 - y ) * width * 4 ) );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, height - y1, width, y1 - y0, GL_RGBA, GL_FLOAT, rows.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	double timeOf( long frame ) const
	{
		return static_cast< double >( frame ) / fps + ( frame > 0 ? lead : 0.0 );
	}

	/// A synthetic clock, and it has to be synthetic: frame n is clocked at
	/// n / fps (+ lead), the unit declared, not inferred.
	bool renderAt( long frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( timeOf( frame ) );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %ld\n", frame );
		return ok;
	}

	bool render( long frame, const std::vector< unsigned char >& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	bool render( long frame, const Picture& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	/// One pixel as floats; x from the left, y from the TOP.
	void readPixel( int x, int y, float out[ 4 ] )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( x, height - 1 - y, 1, 1, GL_RGBA, GL_FLOAT, out );
	}

	/// A row from the TOP, RGBA floats.
	std::vector< float > readRow( int y )
	{
		std::vector< float > row( static_cast< size_t >( width ) * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, height - 1 - y, width, 1, GL_RGBA, GL_FLOAT, row.data() );
		return row;
	}

	/// A column, top first, RGBA floats.
	std::vector< float > readColumn( int x )
	{
		std::vector< float > column( static_cast< size_t >( height ) * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( x, 0, 1, height, GL_RGBA, GL_FLOAT, column.data() );
		return flipRows( column, 1, height );
	}

	std::vector< float > readAll()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// The harness's own model of the plugin's clock with steady hands (the
/// stated contract, in Model.h): the first frame is worth kNominalFrame,
/// every later one its real seconds, and the film runs at the crank's rate
/// from 0.
struct Clock
{
	/// `offset` is how far the projector runs behind the nominal crank: the
	/// loop, when only the camera's hand moves (it is then constant).
	double fps = 60.0, lead = 0.0, projector = 16.0, offset = 0.0;
	double t( long m ) const
	{
		return static_cast< double >( m ) / fps + ( m > 0 ? lead : 0.0 );
	}
	double p1( long m ) const
	{
		return projector * ( model::kNominalFrame + t( m ) - t( 0 ) ) - offset;
	}
	double p0( long m ) const
	{
		return p1( m ) - projector * ( m == 0 ? model::kNominalFrame : t( m ) - t( m - 1 ) );
	}
	/// The newest projector frame the exposure overlaps.
	int64_t latest( long m ) const
	{
		return static_cast< int64_t >( std::ceil( p1( m ) ) ) - 1;
	}
	int64_t first( long m ) const
	{
		return static_cast< int64_t >( std::floor( p0( m ) ) );
	}
	/// How far the exposure's ends are from any pull-down, in frames.
	double margin( long m ) const
	{
		auto d = []( double p ) { return std::fabs( p - std::round( p ) ); };
		return std::min( d( p0( m ) ), d( p1( m ) ) );
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

void note( bool quiet, const char* format, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
void note( bool quiet, const char* format, ... )
{
	if( quiet )
		return;
	va_list args;
	va_start( args, format );
	std::printf( "        " );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
}


/// The light the shutter passes over [a, b], in frames: the stated geometry
/// (blade openings of `open` of each 1 / B, centred in it) intersected with
/// the interval, opening by opening. Not the plugin's cumulative function.
double openTime( double a, double b, int B, double open )
{
	double sum = 0.0;
	for( long j = static_cast< long >( std::floor( a * B ) ) - 1; j <= static_cast< long >( std::ceil( b * B ) ); ++j )
	{
		const double lo = ( j + 0.5 * ( 1.0 - open ) ) / B, hi = ( j + 0.5 * ( 1.0 + open ) ) / B;
		sum += std::max( 0.0, std::min( b, hi ) - std::max( a, lo ) );
	}
	return sum;
}

double sinc( double x )
{
	return std::fabs( x ) < 1e-300 ? 1.0 : std::sin( model::kPi * x ) / ( model::kPi * x );
}

/// The projector frames a display frame shows, and each one's share of its
/// light, from the steady clock and the stated shutter.
struct Lit
{
	int64_t frame;
	double share;
};

std::vector< Lit > litFrames( const Clock& c, long m, int B, double open )
{
	const double p0 = c.p0( m ), p1 = c.p1( m );
	std::vector< Lit > out;
	double total = 0.0;
	for( int64_t j = static_cast< int64_t >( std::floor( p0 ) ); static_cast< double >( j ) < p1; ++j )
	{
		const double a = std::max( p0, static_cast< double >( j ) ), b = std::min( p1, static_cast< double >( j + 1 ) );
		const double light = openTime( a, b, B, open );
		if( light > 0.0 )
			out.push_back( { j, light } );
		total += light;
	}
	for( Lit& l : out )
		l.share /= total > 0.0 ? total : 1.0;
	return out;
}

//---------------------------------------------------------------------------
// The coded picture: the top eighth says which display frame it is, in
// fourteen blocks of the frame number's bits (white 1, black 0) and a white
// and a black reference block; below, a flat grey. Through Black & White
// Panchromatic a white block prints at the shoulder and a black one at the
// print's black, so a block reads as a fraction between its references:
// 0 or 1 when one picture is on the screen, between when the shutter let
// two through.
//---------------------------------------------------------------------------
constexpr int kBits   = 14;
constexpr int kBlocks = kBits + 2;

int bandRows( int H )
{
	return std::max( 4, H / 8 );
}

void writeBand( Picture& p, int W, int H, long m )
{
	const int band = bandRows( H );
	for( int y = 0; y < band; ++y )
		for( int x = 0; x < W; ++x )
		{
			const int b    = x * kBlocks / W;
			const double v = b < kBits ? static_cast< double >( ( m >> ( kBits - 1 - b ) ) & 1 ) : ( b == kBits ? 1.0 : 0.0 );
			put( p, W, x, y, v );
		}
}

struct Decoded
{
	bool crisp = false;
	long value = -1;
	double worst = 0.0;///< the bit fraction furthest from 0 or 1
};

/// Read the band back, from the probe's linear output.
Decoded decodeBand( Session& s )
{
	const int W = s.width;
	const std::vector< float > row = s.readRow( bandRows( s.height ) / 2 );
	auto block = [ & ]( int b ) {
		const int x0 = b * W / kBlocks, x1 = ( b + 1 ) * W / kBlocks;
		const int a = x0 + ( x1 - x0 ) / 4, e = x1 - ( x1 - x0 ) / 4;
		double sum = 0.0;
		for( int x = a; x < e; ++x )
			sum += row[ 4 * x + 1 ];
		return sum / std::max( 1, e - a );
	};
	Decoded d;
	const double white = block( kBits ), black = block( kBits + 1 );
	if( !( white - black > 1e-6 ) )
		return d;
	d.crisp = true;
	d.value = 0;
	for( int b = 0; b < kBits; ++b )
	{
		const double f = ( block( b ) - black ) / ( white - black );
		d.value        = ( d.value << 1 ) | ( f > 0.5 ? 1 : 0 );
		const double off = std::min( std::fabs( f ), std::fabs( 1.0 - f ) );
		d.worst          = std::max( d.worst, off );
		if( off > 0.03 )
			d.crisp = false;
	}
	return d;
}

/// N display frames of the coded picture through a session set up by `k`;
/// each frame's band, decoded. `level` is the grey below the band.
struct Frame
{
	Decoded band;
	double grey   = 0.0;///< the probe at the picture's centre, below the band
	double weight = 0.0;///< the segments' summed weight (the plugin's)
};

bool runCoded( const Knobs& k, int W, int H, int perturb, long N, double displayFps, double lead, double level, std::vector< Frame >& out )
{
	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeLinear );
	s.fps  = displayFps;
	s.lead = lead;
	if( !s.begin( W, H ) )
		return false;
	Picture p = flat( W, H, level );
	s.upload( p );
	out.clear();
	for( long m = 0; m < N; ++m )
	{
		writeBand( p, W, H, m );
		s.uploadRows( p, 0, bandRows( H ) );
		if( !s.renderAt( m ) )
			return false;
		Frame f;
		f.band = decodeBand( s );
		float px[ 4 ];
		s.readPixel( W / 2, ( H + bandRows( H ) ) / 2, px );
		f.grey   = px[ 1 ];
		f.weight = s.plugin.LastWeightSumForTest();
		if( std::getenv( "HMTEST_DEBUG" ) )
		{
			const auto ex = s.plugin.ExposedForTest();
			std::fprintf( stderr, "m %ld band %ld crisp %d worst %.3f light %.4f  newest %lld E %.3f\n", m, f.band.value, f.band.crisp, f.band.worst,
			              f.weight > 0 ? f.grey / f.weight : -1.0, ex.empty() ? -1LL : static_cast< long long >( ex.back().first ), ex.empty() ? 0.0 : ex.back().second );
		}
		out.push_back( f );
	}
	s.end();
	return true;
}

//---------------------------------------------------------------------------
// --crank
//
// The coded picture says, on every display frame, which display frame the
// camera exposed the picture on the screen at. Four runs:
//  A. both hands still: every display frame lit by one projector frame
//     shows exactly the picture of the display frame where the steady clock
//     put that frame's pull-down, and one lit by two shows a blend;
//  B. the projector's hand alone: the pictures are exactly the steady
//     camera's (the hand never touches WHAT is on a frame), but they stay
//     on the screen for varying times, within the hand's bounds, after a
//     delay that varies and never exceeds the loop's bound;
//  C. the camera's hand alone: the switches are exactly the steady
//     projector's, a loop behind (the hand never touches WHEN a frame is
//     shown), every pull-down brings a new picture, and the pictures come
//     from varying instants: the scene's time runs fast and slow within the
//     hand's bounds;
//  D. both: the mean rate over the run is the crank's.
//---------------------------------------------------------------------------
int runCrank( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "crank: which instant each projected frame was exposed at, read out of the picture, %dx%d\n", W, H );
	int failures       = 0;
	const double fps   = 16.0;
	const double lead  = 0.0071;
	const int B        = 2;
	const double open  = 0.5;
	const double hold  = 60.0 / fps;//display frames per projector frame, steady

	//The camera's steady capture: projector frame j's picture is the display
	//frame on which the crank first reaches j.
	auto captureOf = []( const Clock& c, int64_t j ) {
		long n = 0;
		while( c.p1( n ) < static_cast< double >( j ) )
			++n;
		return n;
	};

	//--- A: still hands --------------------------------------------------
	{
		Knobs k;
		k.fps = fps;
		std::vector< Frame > frames;
		const long N = 600;
		if( !runCoded( k, W, H, perturb, N, 60.0, lead, 0.5, frames ) )
			return 1;
		Clock c;
		c.lead = lead;
		int judged = 0, wrong = 0;
		for( long m = 2; m < N; ++m )
		{
			const std::vector< Lit > lit = litFrames( c, m, B, open );
			if( lit.empty() )
				continue;
			double minor = 1.0;
			for( const Lit& l : lit )
				minor = std::min( minor, l.share );
			if( lit.size() == 1 )
			{
				++judged;
				if( !frames[ m ].band.crisp || frames[ m ].band.value != captureOf( c, lit[ 0 ].frame ) )
					++wrong;
			}
			else if( minor > 0.05 )
			{
				++judged;
				if( frames[ m ].band.crisp )
					++wrong;
			}
		}
		failures += report( wrong == 0 && judged > N / 2, quiet, "still hands: %d of %d display frames show the steady clock's picture (or blend where two frames are lit)",
		                    judged - wrong, judged );
	}

	//--- B: the projector's hand alone ------------------------------------
	{
		Knobs k;
		k.fps       = fps;
		k.projector = 1.0;
		const double s  = controls::ProjectorDeviation( 1.0f );
		const double Ap = model::Amplitude( model::kProjectorHand, model::kProjectorPartials, s );
		std::vector< Frame > frames;
		const long N = 1200;
		if( !runCoded( k, W, H, perturb, N, 60.0, lead, 0.5, frames ) )
			return 1;
		Clock c;
		c.lead = lead;
		std::set< long > schedule;
		for( int64_t j = 0; c.p1( N ) > static_cast< double >( j ); ++j )
			schedule.insert( captureOf( c, j ) );
		std::vector< std::pair< long, long > > firsts;//( picture, first display frame showing it )
		long last = -1;
		int foreign = 0, backwards = 0;
		for( long m = 2; m < N; ++m )
		{
			const Decoded& d = frames[ m ].band;
			if( !d.crisp )
				continue;
			if( !schedule.count( d.value ) )
				++foreign;
			if( d.value < last )
				++backwards;
			if( d.value != last )
				firsts.push_back( { d.value, m } );
			last = std::max( last, d.value );
		}
		failures += report( foreign == 0 && backwards == 0 && firsts.size() > N / hold * 0.8, quiet,
		                    "projector hand: all %zu pictures are the steady camera's, in order (%d not, %d backwards)", firsts.size(), foreign, backwards );

		std::vector< double > holds;
		double lo = 1e9, hi = -1e9, dlo = 1e9, dhi = -1e9;
		for( size_t i = 0; i + 1 < firsts.size(); ++i )
		{
			const double d = static_cast< double >( firsts[ i + 1 ].second - firsts[ i ].second );
			holds.push_back( d );
			lo = std::min( lo, d );
			hi = std::max( hi, d );
		}
		for( const auto& f : firsts )
		{
			const double delay = static_cast< double >( f.second - f.first );
			dlo                = std::min( dlo, delay );
			dhi                = std::max( dhi, delay );
		}
		//A frame is held between 60 / ( f ( 1 + s ) ) and 60 / ( f ( 1 - s ) )
		//display frames; its first clean frame can be one late at either end.
		//At a slow crank a display frame can fall wholly in a dark stretch, so
		//two may pass before a new frame shows alone.
		const double holdLo = std::floor( 60.0 / ( fps * ( 1.0 + s ) ) ) - 2.0;
		const double holdHi = std::ceil( 60.0 / ( fps * ( 1.0 - s ) ) ) + 2.0;
		failures += report( lo >= holdLo && hi <= holdHi, quiet, "projector hand: holds %.0f to %.0f display frames, inside the hand's %.0f to %.0f", lo, hi, holdLo, holdHi );
		failures += report( hi - lo >= 3.0, quiet, "projector hand: the holds vary (%.0f to %.0f; a steady crank gives 3 and 4, plus a frame where a pull-down is lit)", lo, hi );
		//The loop: the camera is up to 2 Ap frames ahead, so a picture waits
		//up to 2 Ap / f seconds; plus a display frame each end and a lit
		//pull-down.
		const double delayHi = 2.0 * Ap * hold / ( 1.0 - s ) + 4.0;
		failures += report( dlo >= 0.0 && dhi <= delayHi, quiet, "projector hand: each picture waits %.0f to %.0f display frames, inside the loop's 0 to %.1f", dlo, dhi, delayHi );
		failures += report( dhi - dlo >= Ap * hold, quiet, "projector hand: the wait varies by %.0f display frames (at least half the loop, %.1f)", dhi - dlo, Ap * hold );
	}

	//--- C: the camera's hand alone ---------------------------------------
	{
		Knobs k;
		k.fps    = fps;
		k.camera = 1.0;
		const double s  = controls::CameraDeviation( 1.0f );
		const double Ac = model::Amplitude( model::kCameraHand, model::kCameraPartials, s );
		std::vector< Frame > frames;
		const long N = 1200;
		if( !runCoded( k, W, H, perturb, N, 60.0, lead, 0.5, frames ) )
			return 1;
		Clock c;
		c.lead   = lead;
		c.offset = Ac;
		int judged = 0, wrong = 0;
		std::map< int64_t, long > picture;//projector frame -> its picture
		std::map< int64_t, long > firstShown;
		//From a second in: the frames the projector shows before the camera's
		//first real exposure all carry the first picture (the loop is threaded
		//with it), so they would read as blends that are not, and repeats.
		for( long m = 60; m < N; ++m )
		{
			const std::vector< Lit > lit = litFrames( c, m, B, open );
			if( lit.empty() )
				continue;
			double minor = 1.0;
			for( const Lit& l : lit )
				minor = std::min( minor, l.share );
			const Decoded& d = frames[ m ].band;
			if( lit.size() == 1 )
			{
				++judged;
				if( !d.crisp )
					++wrong;
				else if( !picture.count( lit[ 0 ].frame ) )
				{
					picture[ lit[ 0 ].frame ]    = d.value;
					firstShown[ lit[ 0 ].frame ] = m;
				}
				else if( picture[ lit[ 0 ].frame ] != d.value )
					++wrong;
			}
			else if( minor > 0.05 )
			{
				++judged;
				if( d.crisp )
					++wrong;
			}
		}
		failures += report( wrong == 0 && judged > N / 2, quiet, "camera hand: %d of %d display frames switch where the steady projector does, %.2f frames behind",
		                    judged - wrong, judged, Ac );

		int repeats = 0, steps = 0;
		double rlo = 1e9, rhi = -1e9, dlo = 1e9, dhi = -1e9;
		const int window = 8;
		for( auto it = picture.begin(); it != picture.end(); ++it )
		{
			auto next = std::next( it );
			if( next != picture.end() && next->first == it->first + 1 )
			{
				++steps;
				if( next->second <= it->second )
					++repeats;
			}
			auto far = picture.find( it->first + window );
			if( far != picture.end() )
			{
				const double rate = static_cast< double >( far->second - it->second ) / ( window * hold );
				rlo               = std::min( rlo, rate );
				rhi               = std::max( rhi, rate );
			}
			const double delay = static_cast< double >( firstShown[ it->first ] - it->second );
			dlo                = std::min( dlo, delay );
			dhi                = std::max( dhi, delay );
		}
		failures += report( repeats == 0 && steps > 100, quiet, "camera hand: every one of %d pull-downs brings a newer picture (%d did not)", steps, repeats );
		//The scene's time against the projector's, over a turn: the camera's
		//speed is within 1 +- s, and each end of the window is one display
		//frame uncertain.
		const double q = 2.0 / ( window * hold );
		failures += report( rlo >= 1.0 - s - q && rhi <= 1.0 + s + q, quiet, "camera hand: over a turn the scene runs at %.2f to %.2f of real time, inside 1 -+ %.2f (+- %.3f)", rlo, rhi, s, q );
		failures += report( rhi - rlo >= 0.5 * s, quiet, "camera hand: the scene's speed varies by %.2f (at least half the hand's %.2f)", rhi - rlo, s );
		const double delayHi = 2.0 * Ac * hold / ( 1.0 - s ) + 3.0;
		failures += report( dlo >= 0.0 && dhi <= delayHi, quiet, "camera hand: each picture is %.0f to %.0f display frames old when shown, inside the loop's 0 to %.1f", dlo, dhi, delayHi );
	}

	//--- D: both hands, the mean rate ------------------------------------
	{
		Knobs k;
		k.fps       = fps;
		k.camera    = 1.0;
		k.projector = 1.0;
		const double Ac = model::Amplitude( model::kCameraHand, model::kCameraPartials, controls::CameraDeviation( 1.0f ) );
		const double Ap = model::Amplitude( model::kProjectorHand, model::kProjectorPartials, controls::ProjectorDeviation( 1.0f ) );
		std::vector< Frame > frames;
		const long N = 1800;
		if( !runCoded( k, W, H, perturb, N, 60.0, lead, 0.5, frames ) )
			return 1;
		std::set< long > seen;
		long firstM = -1, lastM = -1;
		for( long m = 60; m < N; ++m )
			if( frames[ m ].band.crisp )
			{
				seen.insert( frames[ m ].band.value );
				if( firstM < 0 )
					firstM = m;
				lastM = m;
			}
		const double expected = fps * static_cast< double >( lastM - firstM ) / 60.0;
		const double bound    = 2.0 * ( Ac + Ap ) + 3.0;
		failures += report( std::fabs( static_cast< double >( seen.size() ) - expected ) <= bound, quiet,
		                    "both hands: %zu pictures in %.1f s, the crank's %.1f (+- %.1f, the loop's swing)", seen.size(), ( lastM - firstM ) / 60.0, expected, bound );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --exposure
//
// The camera's shutter is a fixed angle, so a frame cranked slowly is
// exposed for longer. On a flat grey, each projected frame's light against
// the camera's interval around it (read from the coded picture at 240 Hz,
// so an interval is good to 1/240 s): log light rises with log interval at
// the print's gamma, the straight line's slope.
//---------------------------------------------------------------------------
int runExposure( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "exposure: each frame's light against the camera's speed when it was exposed, %dx%d\n", W, H );
	int failures   = 0;
	const double f = 16.0, display = 240.0;
	Knobs k;
	k.fps    = f;
	k.camera = 1.0;
	k.gamma  = 1.4;
	std::vector< Frame > frames;
	const long N = static_cast< long >( 15.0 * display );
	//sRGB 118 / 255 is 18.1% grey: the middle of the straight line.
	if( !runCoded( k, W, H, perturb, N, display, 0.0011, 118.0 / 255.0, frames ) )
		return 1;

	//Each picture's light: the probe over the segments' weight, on display
	//frames that show it alone.
	std::map< long, std::pair< double, int > > light;
	for( long m = 8; m < N; ++m )
	{
		const Frame& fr = frames[ m ];
		if( !fr.band.crisp || fr.weight <= 0.0 || fr.band.worst > 1e-3 )
			continue;
		auto& e = light[ fr.band.value ];
		e.first += fr.grey / fr.weight;
		e.second += 1;
	}
	std::vector< long > keys;
	for( const auto& e : light )
		keys.push_back( e.first );
	std::vector< double > xs, ys;
	for( size_t i = 1; i + 1 < keys.size(); ++i )
	{
		const double interval = 0.5 * static_cast< double >( keys[ i + 1 ] - keys[ i - 1 ] );
		const double y        = light[ keys[ i ] ].first / light[ keys[ i ] ].second;
		//A picture never seen alone would double an interval: keep the ones a
		//hand can make (0.5 to 2 times the steady interval).
		const double steady = display / f;
		if( interval < 0.5 * steady || interval > 2.0 * steady || y <= 0.0 )
			continue;
		xs.push_back( std::log10( interval * f / display ) );
		ys.push_back( std::log10( y ) );
	}
	const size_t n = xs.size();
	double mx = 0.0, my = 0.0;
	for( size_t i = 0; i < n; ++i )
	{
		mx += xs[ i ];
		my += ys[ i ];
	}
	mx /= std::max< size_t >( n, 1 );
	my /= std::max< size_t >( n, 1 );
	double sxx = 0.0, sxy = 0.0;
	for( size_t i = 0; i < n; ++i )
	{
		sxx += ( xs[ i ] - mx ) * ( xs[ i ] - mx );
		sxy += ( xs[ i ] - mx ) * ( ys[ i ] - my );
	}
	const double slope = sxx > 0.0 ? sxy / sxx : 0.0;
	double sse         = 0.0;
	for( size_t i = 0; i < n; ++i )
	{
		const double r = ys[ i ] - my - slope * ( xs[ i ] - mx );
		sse += r * r;
	}
	const double se = n > 2 && sxx > 0.0 ? std::sqrt( sse / ( n - 2 ) / sxx ) : 1e9;
	//Five standard errors of the slope (the hand's jerk is noise on y), plus
	//5% for the knees' curvature over the swing and the interval's quantum.
	const double tolerance = 5.0 * se + 0.05 * k.gamma;
	failures += report( n > 150, quiet, "%zu projected frames, each with its camera interval", n );
	failures += report( slope > 0.0, quiet, "a frame cranked slowly prints lighter: slope %+.3f", slope );
	failures += report( std::fabs( slope - k.gamma ) <= tolerance, quiet, "log light against log interval: slope %.3f, the print's gamma %.2f (+- %.3f)", slope, k.gamma, tolerance );
	return failures;
}

//---------------------------------------------------------------------------
// --flicker (gate's)
//
// A flat grey, hands still, read at the centre through the linear probe for
// N display frames at 60 fps, each divided by the series' mean, against the
// shutter's Fourier series averaged over each exposure (a sinc per
// harmonic) and folded to its alias.
//---------------------------------------------------------------------------
int runFlicker( int W, int H, int perturb, bool quiet = false )
{
	struct Case
	{
		double fps;
		int blades;
		double degrees;
	};
	const Case cases[] = { { 24.0, 1, 180.0 }, { 24.0, 2, 180.0 }, { 16.0, 2, 180.0 }, { 18.0, 2, 240.0 } };
	const int N = 300, skip = 4;
	if( !quiet )
		std::printf( "flicker: a flat grey at 60 Hz, %d frames, its spectrum against the shutter's Fourier series, %dx%d\n", N, W, H );
	int failures = 0;
	for( const Case& c : cases )
	{
		Session s;
		Knobs k;
		k.fps     = c.fps;
		k.blades  = c.blades;
		k.degrees = c.degrees;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		if( !s.begin( W, H ) )
			return 1;
		s.upload( flat( W, H, 0.5 ) );
		std::vector< double > y;
		for( long m = 0; m < skip + N; ++m )
		{
			if( !s.renderAt( m ) )
				return 1;
			if( m < skip )
				continue;
			float px[ 4 ];
			s.readPixel( W / 2, H / 2, px );
			y.push_back( px[ 1 ] );
		}
		s.end();

		double mean = 0.0;
		for( double v : y )
			mean += v;
		mean /= N;
		for( double& v : y )
			v /= mean;

		std::vector< std::complex< double > > X( N / 2 + 1 );
		for( int j = 0; j <= N / 2; ++j )
		{
			std::complex< double > sum = 0.0;
			for( int m = 0; m < N; ++m )
				sum += y[ m ] * std::polar( 1.0, -2.0 * model::kPi * j * m / N );
			X[ j ] = sum / static_cast< double >( N );
		}

		Clock clock;
		clock.projector   = c.fps;
		const double B    = controls::Blades( c.blades );
		const double f    = controls::ShutterOpen( controls::ShutterParam( c.degrees ) );
		const double D    = clock.projector / clock.fps;
		const double pMid = clock.p1( skip ) - 0.5 * D;
		std::vector< std::complex< double > > P( N / 2 + 1, 0.0 );
		P[ 0 ]            = 1.0;
		const long terms  = 2000000;
		bool commensurate = true;
		for( long n = 1; n <= terms; ++n )
		{
			const double sign = ( n & 1 ) ? -1.0 : 1.0;
			const double cn   = sign * 2.0 * std::sin( model::kPi * n * f ) / ( model::kPi * n * f ) * sinc( n * B * D );
			const double nu   = n * B * D - std::floor( n * B * D );
			const double bin  = nu * N;
			const long j      = std::lround( bin );
			if( std::fabs( bin - j ) > 1e-6 )
				commensurate = false;
			const double arg = n * B * pMid;
			const double th  = 2.0 * model::kPi * ( arg - std::floor( arg ) );
			const long jj    = j % N;
			if( jj <= N / 2 )
				P[ jj ] += 0.5 * cn * std::polar( 1.0, th );
			if( N - jj <= N / 2 && jj != 0 )
				P[ N - jj ] += 0.5 * cn * std::polar( 1.0, -th );
		}
		const double tail = 2.0 / ( model::kPi * model::kPi * f * B * D * terms );
		double peak       = 0.0;
		for( double v : y )
			peak = std::max( peak, std::fabs( v ) );
		//Each sample is Sum w_k T through float: the float weights, a product
		//and a sum per segment (at most two here), the cos^4 factor: 8
		//roundings, doubled for the division by the mean.
		const double tolerance = 2.0 * 8.0 * kU * peak + tail;

		double worst  = 0.0;
		int strongest = 1;
		for( int j = 1; j <= N / 2; ++j )
		{
			worst = std::max( worst, std::abs( X[ j ] - P[ j ] ) );
			if( std::abs( X[ j ] ) > std::abs( X[ strongest ] ) )
				strongest = j;
		}
		const double shutterHz  = B * clock.projector;
		const double alias      = std::fabs( shutterHz - std::round( shutterHz / clock.fps ) * clock.fps );
		const double measuredHz = strongest * clock.fps / N;
		const double amplitude  = 2.0 * std::abs( X[ strongest ] );
		const double predicted  = 2.0 * std::abs( P[ std::lround( alias * N / clock.fps ) ] );

		char label[ 96 ];
		std::snprintf( label, sizeof( label ), "%g fps x %g blades at %g deg (%g Hz)", clock.projector, B, c.degrees, shutterHz );
		failures += report( commensurate, quiet, "%s: %d frames hold whole periods of every harmonic (no leakage)", label, N );
		failures += report( std::fabs( measuredHz - alias ) < 0.5 * clock.fps / N, quiet,
		                    "%s: strongest line at %.2f Hz, the beat |%g - 60| folds to %.2f Hz", label, measuredHz, shutterHz, alias );
		failures += report( worst <= tolerance, quiet, "%s: every DFT bin on the Fourier series, worst %.2e (tolerance %.2e); the beat's amplitude %.5f, predicted %.5f",
		                    label, worst, tolerance, amplitude, predicted );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --tone
//
// A still picture of patches through each tone and stock, read through the
// linear probe and divided by the segments' weight, so each patch reads as
// the print's transmittance over the clear base. The claims are laws, not
// numbers typed twice:
//  - Black & White is neutral;
//  - the straight line's slope is Contrast's gamma (density against log
//    exposure, between patches on the straight part);
//  - toning scales the silver's density: ln T_c / ln T_neutral is the same
//    per-channel constant at every density, so white stays white;
//  - a tint multiplies the light: T_c / T_neutral is the same per-channel
//    constant at every density, so black stays black;
//  - Colour through the curve on a grey is Black & White Panchromatic;
//  - each stock's weights, measured as the grey that prints like a pure
//    red, green or blue, are the ones tools/spectral_weights.py computed.
//---------------------------------------------------------------------------
constexpr int kGridColumns = 16;
constexpr int kGridRows    = 17;//16 rows of the 256 grey codes, then the primaries

Picture patches( int W, int H )
{
	Picture p = flat( W, H, 0.0 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const int col = x * kGridColumns / W, row = y * kGridRows / H;
			float* px     = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			if( row < 16 )
				px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( ( row * 16 + col ) / 255.0 );
			else
			{
				px[ 0 ] = col == 0 || col == 3 ? 1.0f : 0.0f;
				px[ 1 ] = col == 1 || col == 3 ? 1.0f : 0.0f;
				px[ 2 ] = col == 2 || col == 3 ? 1.0f : 0.0f;
			}
			px[ 3 ] = 1.0f;
		}
	return p;
}

/// Every patch's transmittance (RGB) through one setting: [ 0, 256 ) the grey
/// codes, then red, green, blue, white.
bool readPatches( const Knobs& k, int W, int H, int perturb, std::vector< std::array< double, 3 > >& out )
{
	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeLinear );
	if( !s.begin( W, H ) )
		return false;
	s.upload( patches( W, H ) );
	for( long m = 0; m < 3; ++m )
		if( !s.renderAt( m ) )
			return false;
	const double w                = s.plugin.LastWeightSumForTest();
	const std::vector< float > px = s.readAll();//bottom row first
	s.end();
	out.assign( 260, {} );
	auto at = [ & ]( int col, int row, int c ) {
		const int x = ( 2 * col + 1 ) * W / ( 2 * kGridColumns );
		const int y = ( 2 * row + 1 ) * H / ( 2 * kGridRows );//from the top
		return px[ ( static_cast< size_t >( H - 1 - y ) * W + x ) * 4 + c ] / w;
	};
	for( int code = 0; code < 256; ++code )
		for( int c = 0; c < 3; ++c )
			out[ code ][ c ] = at( code % 16, code / 16, c );
	for( int i = 0; i < 4; ++i )
		for( int c = 0; c < 3; ++c )
			out[ 256 + i ][ c ] = at( i, 16, c );
	return true;
}

int runTone( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "tone: every patch's transmittance through each tone and stock, %dx%d\n", W, H );
	if( W < 4 * kGridColumns || H < 4 * kGridRows )
	{
		report( false, quiet, "the raster is too small for the patch grid" );
		return 1;
	}
	int failures = 0;
	using Patches = std::vector< std::array< double, 3 > >;
	Knobs bw;
	Patches neutral;
	if( !readPatches( bw, W, H, perturb, neutral ) )
		return 1;

	//Neutral.
	{
		double worst = 0.0;
		for( int code = 0; code < 256; ++code )
			worst = std::max( { worst, std::fabs( neutral[ code ][ 0 ] - neutral[ code ][ 1 ] ), std::fabs( neutral[ code ][ 2 ] - neutral[ code ][ 1 ] ) } );
		failures += report( worst == 0.0, quiet, "Black & White: every grey patch has R = G = B exactly (worst %.1e)", worst );
	}

	//Contrast: the straight line's slope. Density D = -log10 T; exposure is
	//the patch's linear light. Pairs of patches on the straight part, well
	//clear of the knees (more than 0.7 above the base, 0.8 below the black):
	//a softplus of width 0.12 is within 0.12 e^-5.8 = 3.6e-4 of its line
	//there, at each end of the pair.
	for( double gamma : { 0.6, 1.4, 2.2 } )
	{
		Knobs k;
		k.gamma = gamma;
		Patches t;
		if( !readPatches( k, W, H, perturb, t ) )
			return 1;
		double worst = 0.0, deviation = 0.0;
		int pairs    = 0;
		double tol   = 0.0;
		for( int a = 1; a + 8 < 256; ++a )
		{
			const int b    = a + 8;
			const double Da = -std::log10( t[ a ][ 1 ] ), Db = -std::log10( t[ b ][ 1 ] );
			const double base = model::kBaseDensity, top = model::kMaxDensity;
			if( Da - base < 0.75 || Db - base < 0.75 || top - Da < 0.85 || top - Db < 0.85 )
				continue;
			const double xa = std::log10( decode( a / 255.0 ) ), xb = std::log10( decode( b / 255.0 ) );
			const double slope = -( Db - Da ) / ( xb - xa );
			const double t1    = 2.0 * ( 2.0 * 0.12 * std::exp( -0.7 / 0.12 ) + 64.0 * kU ) / ( xb - xa );
			tol                = std::max( tol, t1 );
			worst              = std::max( worst, std::fabs( slope - gamma ) - t1 );
			deviation          = std::max( deviation, std::fabs( slope - gamma ) );
			++pairs;
		}
		failures += report( pairs >= 8 && worst <= 0.0, quiet, "Contrast %.1f: %d pairs on the straight line, density against log exposure at slope gamma to %.1e (the knees' bound, %.1e)",
		                    gamma, pairs, deviation, tol );
	}

	//Toning: ln T_c / ln T_neutral = the channel's density factor, at every
	//patch with silver enough to measure (T < 0.9).
	{
		Knobs k;
		k.tone = model::kSepia;
		Patches t;
		if( !readPatches( k, W, H, perturb, t ) )
			return 1;
		double worst = 0.0;
		int n        = 0;
		for( int code = 0; code < 256; ++code )
		{
			if( neutral[ code ][ 1 ] > 0.9 )
				continue;
			const double ln = std::log( neutral[ code ][ 1 ] );
			for( int c = 0; c < 3; ++c )
				worst = std::max( worst, std::fabs( std::log( t[ code ][ c ] ) / ln - model::kSepiaDensity[ c ] ) );
			++n;
		}
		//Each log is good to a few float roundings of T relative, over |ln T|
		//>= 0.105: 1e-5 covers it with room.
		failures += report( n > 100 && worst <= 1e-5, quiet, "Sepia: on %d patches each channel's ln T is the neutral's times %.2f / %.2f / %.2f (worst %.1e)",
		                    n, model::kSepiaDensity[ 0 ], model::kSepiaDensity[ 1 ], model::kSepiaDensity[ 2 ], worst );
		failures += report( t[ 255 ][ 0 ] > t[ 255 ][ 2 ] && t[ 255 ][ 2 ] > 0.9 * neutral[ 255 ][ 2 ], quiet, "Sepia: white stays near white (%.3f %.3f %.3f), the shadows go brown",
		                    t[ 255 ][ 0 ], t[ 255 ][ 1 ], t[ 255 ][ 2 ] );
	}

	//Tints: T_c / T_neutral = the tint, at every patch.
	{
		const int tones[]          = { model::kAmber, model::kBlue, model::kGreen };
		const double* tints[]      = { model::kAmberTint, model::kBlueTint, model::kGreenTint };
		const char* names[]        = { "Amber", "Blue", "Green" };
		for( int i = 0; i < 3; ++i )
		{
			Knobs k;
			k.tone = tones[ i ];
			Patches t;
			if( !readPatches( k, W, H, perturb, t ) )
				return 1;
			double worst = 0.0;
			for( int code = 0; code < 256; ++code )
				for( int c = 0; c < 3; ++c )
					worst = std::max( worst, std::fabs( t[ code ][ c ] / neutral[ code ][ c ] - tints[ i ][ c ] ) );
			failures += report( worst <= 1e-5, quiet, "%s tint: every patch's light is the neutral's times ( %.2f %.2f %.2f ) (worst %.1e); black stays black",
			                    names[ i ], tints[ i ][ 0 ], tints[ i ][ 1 ], tints[ i ][ 2 ], worst );
		}
	}

	//Colour on a grey is Panchromatic Black & White, bit for bit.
	{
		Knobs k;
		k.tone = model::kColour;
		Patches t;
		if( !readPatches( k, W, H, perturb, t ) )
			return 1;
		double worst = 0.0;
		for( int code = 0; code < 256; ++code )
			for( int c = 0; c < 3; ++c )
				worst = std::max( worst, std::fabs( t[ code ][ c ] - neutral[ code ][ c ] ) );
		failures += report( worst <= 4.0 * kU, quiet, "Colour: a grey prints as Panchromatic Black & White does (worst %.1e)", worst );
	}

	//The stocks' weights, read from the picture: the grey code whose print
	//matches a primary's, interpolated in linear light. 1% covers the
	//interpolation across one code value of a smooth curve.
	for( int stock = 0; stock < 2; ++stock )
	{
		Knobs k;
		k.stock = stock;
		Patches t;
		if( !readPatches( k, W, H, perturb, t ) )
			return 1;
		const handleman::spectral::Weights& w = handleman::spectral::kWeights[ stock ];
		const char* primary[]                  = { "red", "green", "blue" };
		for( int p = 0; p < 3; ++p )
		{
			const double target = t[ 256 + p ][ 1 ];
			double measured     = -1.0;
			for( int code = 0; code < 255; ++code )
			{
				const double a = t[ code ][ 1 ], b = t[ code + 1 ][ 1 ];
				if( ( a - target ) * ( b - target ) > 0.0 || a == b )
					continue;
				const double la = decode( code / 255.0 ), lb = decode( ( code + 1 ) / 255.0 );
				measured        = la + ( lb - la ) * ( target - a ) / ( b - a );
				break;
			}
			const double stated = w.w[ 4 + p ];
			failures += report( measured > 0.0 && std::fabs( measured / stated - 1.0 ) <= 0.01, quiet,
			                    "%s: pure %s exposes like a grey of %.4f of white; the weights say %.4f", w.stock, primary[ p ], measured, stated );
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --hold
//
// Grain and dust belong to the film frame. Hands still, a flat grey through
// the probe divided by the segments' weight: two display frames lit by the
// same single projector frame are the same picture, pixel for pixel; two lit
// by different frames are not.
//---------------------------------------------------------------------------
int runHold( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "hold: the grain and the dust change with the film frame, not the display frame, %dx%d\n", W, H );
	int failures = 0;
	Session s;
	Knobs k;
	k.grain = 1.0;
	k.dust  = 1.0;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeLinear );
	const double lead = 0.0071;
	s.lead            = lead;
	if( !s.begin( W, H ) )
		return 1;
	s.upload( flat( W, H, 118.0 / 255.0 ) );
	Clock c;
	c.lead = lead;
	std::vector< float > previous;
	int64_t previousFrame = -1;
	int same = 0, sameWrong = 0, differ = 0, differWrong = 0;
	double worstSame = 0.0, leastDiffer = 1e9;
	for( long m = 0; m < 240; ++m )
	{
		if( !s.renderAt( m ) )
			return 1;
		const std::vector< Lit > lit = litFrames( c, m, 2, 0.5 );
		if( m < 4 || lit.size() != 1 )
		{
			previous.clear();
			continue;
		}
		std::vector< float > px = s.readAll();
		const double w          = s.plugin.LastWeightSumForTest();
		for( float& v : px )
			v = static_cast< float >( v / w );
		if( !previous.empty() )
		{
			//Relative: each is the frame's T times a float weight, divided by
			//that weight in double: two roundings of 2^-24 a frame, two frames.
			double maxDiff = 0.0, meanDiff = 0.0;
			for( size_t i = 0; i < px.size(); i += 4 )
			{
				const double d = std::fabs( px[ i + 1 ] - previous[ i + 1 ] );
				maxDiff        = std::max( maxDiff, d / std::max( { std::fabs( px[ i + 1 ] ), std::fabs( previous[ i + 1 ] ), 1e-30f } ) );
				meanDiff += d;
			}
			meanDiff /= px.size() / 4;
			if( lit[ 0 ].frame == previousFrame )
			{
				++same;
				worstSame = std::max( worstSame, maxDiff );
				//The same picture through a different float weight: a division's
				//rounding and the weight's, relative.
				if( maxDiff > 2.0 * 2.0 * 2.0 * kU )
					++sameWrong;
			}
			else
			{
				++differ;
				leastDiffer = std::min( leastDiffer, meanDiff );
				if( meanDiff < 0.002 )
					++differWrong;
			}
		}
		previous      = px;
		previousFrame = lit[ 0 ].frame;
	}
	s.end();
	failures += report( same > 20 && sameWrong == 0, quiet, "within a hold: %d pairs of display frames identical (worst %.1e relative, bound %.1e)", same, worstSame, 8.0 * kU );
	failures += report( differ > 20 && differWrong == 0, quiet, "across a pull-down: %d pairs differ (least mean difference %.4f)", differ, leastDiffer );
	return failures;
}

//---------------------------------------------------------------------------
// --scratch
//
// Grit in the gate cuts a scratch at a fixed x. Two scratches of known
// width on whole pixels (a base-side one taking half the light, an
// emulsion-side one taking all the silver), over a moving, weaving picture:
// against the same run without them, exactly those columns change, by
// exactly that, on every frame.
//---------------------------------------------------------------------------
Picture movingRamp( int W, int H, long m )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
			put( p, W, x, y, 0.15 + 0.6 * std::fmod( ( x + 3.0 * m ) / W + 0.3 * y / H, 1.0 ) );
	return p;
}

int runScratch( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "scratch: scratches stay at their x while the picture moves and weaves, %dx%d\n", W, H );
	if( W % 64 != 0 )
	{
		note( quiet, "the raster's width is not a multiple of 64: whole-pixel scratches are not possible; skipped" );
		return 0;
	}
	int failures = 0;
	const int frames = 40;
	std::vector< model::Scratch > scratches( 2 );
	scratches[ 0 ].left     = 5.0 / 16.0;
	scratches[ 0 ].right    = 5.0 / 16.0 + 1.0 / 64.0;
	scratches[ 0 ].strength = 0.5;
	scratches[ 0 ].side     = model::kBase;
	scratches[ 1 ].left     = 9.0 / 16.0;
	scratches[ 1 ].right    = 9.0 / 16.0 + 1.0 / 64.0;
	scratches[ 1 ].strength = 1.0;
	scratches[ 1 ].side     = model::kEmulsion;

	auto run = [ & ]( bool scratched, std::vector< std::vector< float > >& out, std::vector< double >& weights ) {
		Session s;
		Knobs k;
		k.weave = 1.0;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		if( scratched )
			s.plugin.SetScratchOverrideForTest( scratches );
		if( !s.begin( W, H ) )
			return false;
		for( long m = 0; m < frames; ++m )
		{
			if( !s.render( m, movingRamp( W, H, m ) ) )
				return false;
			out.push_back( s.readRow( H / 2 ) );
			weights.push_back( s.plugin.LastWeightSumForTest() );
		}
		s.end();
		return true;
	};
	std::vector< std::vector< float > > clean, cut;
	std::vector< double > w0, w1;
	if( !run( false, clean, w0 ) || !run( true, cut, w1 ) )
		return 1;

	const int b0 = 5 * W / 16, b1 = b0 + W / 64, e0 = 9 * W / 16, e1 = e0 + W / 64;
	int baseWrong = 0, emulsionWrong = 0, untouchedWrong = 0;
	double worst  = 0.0;
	for( int m = 2; m < frames; ++m )
		for( int x = 0; x < W; ++x )
		{
			const double a = clean[ m ][ 4 * x + 1 ], b = cut[ m ][ 4 * x + 1 ];
			if( x >= b0 && x < b1 )
			{
				const double r = b / a;
				worst          = std::max( worst, std::fabs( r - 0.5 ) );
				if( std::fabs( r - 0.5 ) > 32.0 * 2.0 * kU )
					++baseWrong;
			}
			else if( x >= e0 && x < e1 )
			{
				//No silver at all: the clear base, so the light is the weight.
				const double r = b / w1[ m ];
				worst          = std::max( worst, std::fabs( r - 1.0 ) );
				if( std::fabs( r - 1.0 ) > 32.0 * 2.0 * kU )
					++emulsionWrong;
			}
			else if( a != b )
				++untouchedWrong;
		}
	failures += report( baseWrong == 0, quiet, "base side: columns %d-%d lose exactly half the light on %d frames of a moving, weaving picture (%d samples off)", b0, b1 - 1, frames - 2, baseWrong );
	failures += report( emulsionWrong == 0, quiet, "emulsion side: columns %d-%d are clear base on every frame (%d samples off; worst of both %.1e)", e0, e1 - 1, emulsionWrong, worst );
	failures += report( untouchedWrong == 0, quiet, "every other column is untouched (%d samples changed)", untouchedWrong );
	return failures;
}

//---------------------------------------------------------------------------
// --weave (gate's)
//
// A step's offset read out of each projector frame's picture: a row sum
// over a vertical edge and a column sum over a horizontal one conserve the
// step's area under the shader's tent filter, so the offset is ( R W - S ) /
// ( R - L ) minus the edge's place. At gamma 1 the print's straight line is
// T = exposure, so the curve does not bend the edge pixel; the two levels
// sit on the straight part. Then the sample sd and lag-1 correlation of
// 1000 frames against the stated sigma and rho, to five standard errors of
// an AR(1) series' statistics (Bartlett).
//---------------------------------------------------------------------------
Picture steps( int W, int H )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	//sRGB codes for linear 0.05 and 0.25: density 1.30 and 0.60 at gamma 1.
	const double lo = 63.0 / 255.0, hi = 137.0 / 255.0;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			double v = x < W / 2 ? lo : hi;
			if( y >= H / 2 )
				v = y < 3 * H / 4 ? lo : hi;
			put( p, W, x, y, v );
		}
	return p;
}

int runWeave( int W, int H, int perturb, bool quiet = false )
{
	const int wanted = 1000;
	if( !quiet )
		std::printf( "weave: %d projector frames' offsets read out of the picture, against the stated AR(1), %dx%d\n", wanted, W, H );
	int failures = 0;
	Session s;
	Knobs k;
	k.weave = 0.5;
	k.gamma = 1.0;
	k.fps   = 24.0;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeLinear );
	//48 display frames a second against 24: every exposure is half a
	//projector frame, and with the lead every other one lies wholly inside
	//a frame, clear of each pull-down.
	s.fps  = 48.0;
	s.lead = 0.0071;
	if( !s.begin( W, H ) )
		return 1;
	s.upload( steps( W, H ) );
	Clock clock;
	clock.fps       = s.fps;
	clock.lead      = s.lead;
	clock.projector = 24.0;
	std::map< int64_t, std::pair< double, double > > offsets;
	for( long m = 0; static_cast< int >( offsets.size() ) < wanted && m < 4 * wanted; ++m )
	{
		if( !s.renderAt( m ) )
			return 1;
		if( clock.first( m ) != clock.latest( m ) || clock.margin( m ) < 0.01 || offsets.count( clock.latest( m ) ) )
			continue;
		const std::vector< float > rowPx = s.readRow( H / 4 );
		const std::vector< float > colPx = s.readColumn( W / 4 );
		double S = 0.0, C = 0.0;
		for( int x = 0; x < W; ++x )
			S += rowPx[ 4 * x + 1 ];
		for( int y = 0; y < H; ++y )
			C += colPx[ 4 * y + 1 ];
		const double L = rowPx[ 4 * ( W / 16 ) + 1 ], R = rowPx[ 4 * ( W - 1 - W / 16 ) + 1 ];
		const double Tt = colPx[ 4 * ( H / 16 ) + 1 ], Bt = colPx[ 4 * ( H - 1 - H / 16 ) + 1 ];
		const double dx = ( R * W - S ) / ( R - L ) - W / 2;
		const double dy = ( Bt * H - C ) / ( Bt - Tt ) - 3 * H / 4;
		offsets[ clock.latest( m ) ] = { dx, dy };
	}
	s.end();

	const model::WeaveLaw law = model::Weave( 0.5, 0 );
	auto stats                = [ & ]( int axis, double& sd, double& r1 ) {
		std::vector< double > v;
		int64_t expect = offsets.begin()->first;
		for( const auto& e : offsets )
		{
			if( e.first != expect )
				break;
			v.push_back( axis == 0 ? e.second.first : e.second.second );
			++expect;
		}
		double mean = 0.0;
		for( double x : v )
			mean += x;
		mean /= v.size();
		double c0 = 0.0, c1 = 0.0;
		for( size_t i = 0; i < v.size(); ++i )
		{
			c0 += ( v[ i ] - mean ) * ( v[ i ] - mean );
			if( i + 1 < v.size() )
				c1 += ( v[ i ] - mean ) * ( v[ i + 1 ] - mean );
		}
		sd = std::sqrt( c0 / ( v.size() - 1 ) );
		r1 = c1 / c0;
		return static_cast< int >( v.size() );
	};
	for( int axis = 0; axis < 2; ++axis )
	{
		double sd = 0.0, r1 = 0.0;
		const int n        = stats( axis, sd, r1 );
		const double sigma = ( axis == 0 ? law.sigmaX : law.sigmaY ) * H;
		const double rho   = law.rho;
		const double sdTol = 5.0 * sigma * std::sqrt( ( 1.0 + rho * rho ) / ( 2.0 * n * ( 1.0 - rho * rho ) ) ) + 1e-3;
		const double rTol  = 5.0 * std::sqrt( ( 1.0 - rho * rho ) / n );
		const char* label  = axis == 0 ? "horizontal" : "vertical";
		failures += report( n >= wanted * 9 / 10, quiet, "%s: %d consecutive projector frames measured", label, n );
		failures += report( std::fabs( sd - sigma ) <= sdTol, quiet, "%s: sd %.3f px against sigma %.3f px (+- %.3f)", label, sd, sigma, sdTol );
		failures += report( std::fabs( r1 - rho ) <= rTol, quiet, "%s: lag-1 correlation %.3f against rho %.3f (+- %.3f)", label, r1, rho, rTol );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --runout
//
// Run Out pressed on a running picture, hands still, through the probe over
// the segments' weight (so a blank cell, clear base, reads exactly 1):
//  - the picture is still there half way through the ramp, and every row
//    off the frame line is blank by the ramp's end;
//  - the frame line is kFrameLine of the picture's height (the published
//    pitch less the full camera aperture), to a row;
//  - Rolling: it climbs kSlip of a pitch a pull-down;
//  - Framed: it leaves the screen and stays gone;
//  - Off the End: the bare lamp, 10^Dbase over the base, everywhere;
//  - pressed again: the picture comes back, framed.
//---------------------------------------------------------------------------
struct Column
{
	std::vector< double > T;///< top first, over the weight
	Decoded band;
	bool single = false;///< one projector frame lit this display frame
};

bool runReel( const Knobs& k, int W, int H, int perturb, long N, const std::vector< long >& presses, std::vector< Column >& out )
{
	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeLinear );
	s.lead = 0.0071;
	if( !s.begin( W, H ) )
		return false;
	const int runOut = indexOfParameter( s.plugin, "Run Out" );
	Picture p        = flat( W, H, 0.5 );
	Clock clock;
	clock.lead      = s.lead;
	clock.projector = k.fps;
	s.upload( p );
	out.clear();
	for( long m = 0; m < N; ++m )
	{
		const bool press = std::find( presses.begin(), presses.end(), m ) != presses.end();
		s.plugin.SetFloatParameter( static_cast< unsigned int >( runOut ), press ? 1.0f : 0.0f );
		writeBand( p, W, H, m );
		s.uploadRows( p, 0, bandRows( H ) );
		if( !s.renderAt( m ) )
			return false;
		Column c;
		const double w                 = s.plugin.LastWeightSumForTest();
		const std::vector< float > col = s.readColumn( W / 2 );
		for( int y = 0; y < H; ++y )
			c.T.push_back( w > 0.0 ? col[ 4 * y + 1 ] / w : 0.0 );
		c.band   = decodeBand( s );
		c.single = litFrames( clock, m, controls::Blades( k.blades ), controls::ShutterOpen( controls::ShutterParam( k.degrees ) ) ).size() == 1;
		out.push_back( c );
	}
	s.end();
	return true;
}

/// A frame line prints at the print's black, 10^-2.34 = 0.005 of the base;
/// the darkest thing the coded picture holds below its band is 0.23.
constexpr double kLineLevel = 0.05;

/// The dark rows of a column from row `from` down (the frame line): how
/// many, and the middle of the first run of them.
int darkRows( const Column& c, double& middle, int from = 0 )
{
	int count = 0, first = -1, last = -1;
	for( int y = from; y < static_cast< int >( c.T.size() ); ++y )
		if( c.T[ y ] < kLineLevel )
		{
			++count;
			if( first < 0 )
				first = y;
			if( last < 0 || y == last + 1 )
				last = y;
		}
	middle = first < 0 ? -1.0 : 0.5 * ( first + last );
	return count;
}

int runRunOut( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "runout: the roll runs out to blank cells, the frame line crawls, the tail ends, a new roll threads, %dx%d\n", W, H );
	int failures       = 0;
	const double fps   = 16.0;
	const double hold  = 60.0 / fps;
	const long press   = 40;
	const int ramp     = 16;//Run Out Time 1 s at 16 fps
	const double lineH = model::kFrameLine * H;

	//--- Rolling, then threaded again ---------------------------------------
	{
		Knobs k;
		k.fps    = fps;
		k.runOut = 1.0;
		k.tail   = model::kRolling;
		const long again = press + 300;
		std::vector< Column > cols;
		if( !runReel( k, W, H, perturb, again + 300, { press, again }, cols ) )
			return 1;

		//Half way: the picture still shows somewhere off the frame line (on the
		//first display frame from there that one pull-down lit).
		long half = press + static_cast< long >( std::floor( ( ramp / 2 ) * hold ) );
		while( !cols[ half ].single )
			++half;
		int picture = 0;
		for( double v : cols[ half ].T )
			if( v > kLineLevel && v < 0.98 )
				++picture;
		failures += report( picture > H / 4, quiet, "half way through the ramp the picture is still there (%d of %d rows neither blank nor frame line)", picture, H );

		//By the ramp's end (and a pull-down to land and one for a lit
		//exposure), every row off the frame line is clear base.
		const long blankBy = press + static_cast< long >( std::ceil( ( ramp + 2 ) * hold ) ) + 1;
		int notBlank       = 0;
		int judged = 0;
		for( long m = blankBy; m < again; ++m )
		{
			if( !cols[ m ].single )
				continue;
			++judged;
			for( double v : cols[ m ].T )
				if( v >= kLineLevel && std::fabs( v - 1.0 ) > 8.0 * kU )
					++notBlank;
		}
		failures += report( judged > 50 && notBlank == 0, quiet, "from %ld display frames after the press every row off the frame line is a blank cell, exactly, on %d frames lit by one pull-down (%d samples not)",
		                    blankBy - press, judged, notBlank );

		//The frame line's height, on every display frame that shows one whole
		//(not cut by the screen's edge), lit by one projector frame.
		int lines = 0, wrongHeight = 0;
		for( long m = blankBy; m < again; ++m )
		{
			double mid    = 0.0;
			const int n   = darkRows( cols[ m ], mid );
			if( !cols[ m ].single || n == 0 || mid < lineH || mid > H - lineH )
				continue;
			++lines;
			if( n < std::floor( lineH ) || n > std::ceil( lineH ) )
				++wrongHeight;
		}
		failures += report( lines > 50 && wrongHeight == 0, quiet, "the frame line is %d to %d rows (%.2f: the pitch less the camera aperture) on all %d display frames that show it (%d not)",
		                    static_cast< int >( std::floor( lineH ) ), static_cast< int >( std::ceil( lineH ) ), lineH, lines, wrongHeight );

		//The crawl: once built up, from one pull-down to the next the line
		//rises kSlip of a pitch (or the next line comes up from below).
		const double step = model::kSlip * model::kPitch * H;
		const double pitchPx = model::kPitch * H;
		int moves = 0, wrongMove = 0;
		double previous = -1.0;
		long previousM  = -1;
		for( long m = blankBy + static_cast< long >( model::kSlipBuild * hold ); m < again; ++m )
		{
			double mid  = 0.0;
			const int n = darkRows( cols[ m ], mid );
			if( !cols[ m ].single || n < std::floor( lineH ) || mid < lineH || mid > H - lineH )
			{
				previous = -1.0;
				continue;
			}
			if( previous >= 0.0 && m - previousM <= 5 && std::fabs( mid - previous ) > 0.25 )
			{
				double d = mid - previous;
				d -= pitchPx * std::round( d / pitchPx );
				++moves;
				//Each end of a run of rows is a whole row: the middle is good to a
				//row.
				if( std::fabs( d + step ) > 1.0 )
					++wrongMove;
			}
			previous  = mid;
			previousM = m;
		}
		failures += report( moves > 20 && wrongMove == 0, quiet, "Rolling: the frame line climbs %.2f rows a pull-down (%d moves, %d not)", step, moves, wrongMove );

		//Threaded again: the head's blank cells, the fog clearing, the strip
		//settling. Within the head, the ramp and a crawl to the frame line's
		//end, the coded picture reads and no row is the frame line.
		const long back = again + static_cast< long >( std::ceil( ( model::kHeadCells + ramp + 1.0 / model::kSlip + 4 ) * hold ) );
		int unread = 0, line = 0;
		for( long m = back; m < static_cast< long >( cols.size() ); ++m )
		{
			double mid = 0.0;
			if( darkRows( cols[ m ], mid, bandRows( H ) ) > 0 )
				++line;
			if( cols[ m ].band.value < 0 )
				++unread;
		}
		failures += report( unread == 0 && line == 0, quiet, "pressed again: the picture is back and framed %ld display frames later (%d frames unread, %d with a frame line)", back - again, unread, line );
	}

	//--- Framed --------------------------------------------------------------
	{
		Knobs k;
		k.fps    = fps;
		k.runOut = 1.0;
		k.tail   = model::kFramed;
		std::vector< Column > cols;
		const long N = press + 600;
		if( !runReel( k, W, H, perturb, N, { press }, cols ) )
			return 1;
		const long settled = press + static_cast< long >( std::ceil( ( ramp + model::kFramedAfter + 1.0 / ( 0.5 * model::kSlip ) + 4 ) * hold ) );
		int lineRows = 0;
		for( long m = settled; m < N; ++m )
		{
			double mid = 0.0;
			lineRows += darkRows( cols[ m ], mid, bandRows( H ) );
		}
		failures += report( lineRows == 0, quiet, "Framed: from %ld display frames after the press the frame line is gone and stays gone (%d dark rows)", settled - press, lineRows );
	}

	//--- Off the End -------------------------------------------------------
	{
		Knobs k;
		k.fps    = fps;
		k.runOut = 1.0;
		k.tail   = model::kOffTheEnd;
		std::vector< Column > cols;
		const long N = press + 500;
		if( !runReel( k, W, H, perturb, N, { press }, cols ) )
			return 1;
		const long gone = press + static_cast< long >( std::ceil( ( ramp + model::kTailCells + 4 + 1.0 / model::kFlick ) * hold ) );
		const double lamp = std::pow( 10.0, model::kBaseDensity );
		int off = 0;
		for( long m = gone; m < N; ++m )
			for( double v : cols[ m ].T )
				if( std::fabs( v / lamp - 1.0 ) > 16.0 * kU )
					++off;
		failures += report( off == 0, quiet, "Off the End: from %ld display frames after the press the screen is the bare lamp, %.4f of the clear base, everywhere (%d samples not)", gone - press, lamp, off );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//
// The ring is copied, not cleared, when the raster changes: with both hands
// turning, a run resized to 1.5x at frame 100 and back at 140 shows the same
// pictures, display frame for display frame, as a run that never resized,
// the resize frames included.
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "resize: the loop of film survives the raster changing under it, %dx%d\n", W, H );
	int failures = 0;
	auto run     = [ & ]( bool resize, std::vector< Decoded >& out ) {
		Session s;
		Knobs k;
		k.camera    = 0.8;
		k.projector = 0.8;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		s.lead = 0.0071;
		if( !s.begin( W, H ) )
			return false;
		for( long m = 0; m < 200; ++m )
		{
			if( resize && m == 100 )
				s.resize( W * 3 / 2, H * 3 / 2 );
			if( resize && m == 140 )
				s.resize( W, H );
			Picture p = flat( s.width, s.height, 0.5 );
			writeBand( p, s.width, s.height, m );
			if( !s.render( m, p ) )
				return false;
			out.push_back( decodeBand( s ) );
		}
		s.end();
		return true;
	};
	std::vector< Decoded > a, b;
	if( !run( false, a ) || !run( true, b ) )
		return 1;
	int compared = 0, differ = 0;
	for( long m = 100; m < 200; ++m )
	{
		if( !a[ m ].crisp )
			continue;
		++compared;
		if( !b[ m ].crisp || b[ m ].value != a[ m ].value )
			++differ;
	}
	failures += report( compared > 40 && differ == 0, quiet, "after a resize to 1.5x and back, %d of %d display frames show the unresized run's picture (%d do not)", compared - differ, compared, differ );
	failures += report( b[ 100 ].value == a[ 100 ].value && b[ 100 ].value >= 0, quiet, "on the resize frame the picture is the loop's, %ld (the unresized run's %ld)", b[ 100 ].value, a[ 100 ].value );
	return failures;
}

//---------------------------------------------------------------------------
// --alpha
//
// The projection paints the whole frame: alpha is 1 at Mix 1, and at Mix 0
// the host's picture comes back exactly, alpha and all.
//---------------------------------------------------------------------------
int runAlpha( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "alpha: what the effect does to the host's alpha, %dx%d\n", W, H );
	int failures = 0;
	Picture p    = flat( W, H, 0.5 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W / 2; ++x )
			p[ ( static_cast< size_t >( y ) * W + x ) * 4 + 3 ] = 0.25f;
	for( float mix : { 1.0f, 0.0f } )
	{
		Session s;
		Knobs k;
		apply( s.plugin, k );
		set( s.plugin, "Mix", mix );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( W, H ) )
			return 1;
		for( long m = 0; m < 4; ++m )
			if( !s.render( m, p ) )
				return 1;
		const std::vector< float > out = s.readAll();
		s.end();
		const std::vector< float > in = flipRows( p, W, H );
		int wrong = 0;
		for( size_t i = 0; i < out.size(); i += 4 )
		{
			if( mix == 1.0f && out[ i + 3 ] != 1.0f )
				++wrong;
			//Mix 0 is the host's own texel, read with texelFetch: bit for bit.
			if( mix == 0.0f && ( out[ i ] != in[ i ] || out[ i + 3 ] != in[ i + 3 ] ) )
				++wrong;
		}
		failures += report( wrong == 0, quiet, mix == 1.0f ? "Mix 1: alpha 1 everywhere (%d pixels not)" : "Mix 0: the host's picture and alpha back exactly (%d pixels not)", wrong );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "negative controls: each perturbation of the plugin's model must FAIL its check, %dx%d\n", W, H );
	struct Control
	{
		int bits;
		const char* what;
		int ( *check )( int, int, int, bool );
		const char* name;
	};
	const Control list[] = {
		{ model::kPerturbNoLoop, "the projector without the loop's delay", runCrank, "--crank" },
		{ model::kPerturbExposureProjector, "exposure from the projector's speed", runExposure, "--exposure" },
		{ model::kPerturbInstant, "the shutter sampled at one instant", runFlicker, "--flicker" },
		{ model::kPerturbToneAsTint, "sepia as a tint", runTone, "--tone" },
		{ model::kPerturbOrthoAsPan, "orthochromatic seeing luminance", runTone, "--tone" },
		{ model::kPerturbGrainPerDisplay, "grain reseeded every display frame", runHold, "--hold" },
		{ model::kPerturbPictureSpace, "scratches in picture space", runScratch, "--scratch" },
		{ model::kPerturbWhiteWeave, "weave with no correlation", runWeave, "--weave" },
		{ model::kPerturbFrameLineHalf, "the frame line drawn half as tall", runRunOut, "--runout" },
		{ model::kPerturbResizeClears, "a resize clears the ring", runResize, "--resize" },
	};
	int failures = 0;
	for( const Control& c : list )
	{
		const int before = g_failures;
		const int checks = g_checks;
		const int failed = c.check( W, H, c.bits, true );
		g_failures       = before;
		g_checks         = checks;
		failures += report( failed > 0, false, "%-40s -> %s fails (%d of its checks)", c.what, c.name, failed );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "names: nothing the host will silently truncate; every name unique\n" );
	Handleman plugin;
	int failures = 0;
	std::set< std::string > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.index >= Handleman::PT_ABOUT_FIRST )
			continue;
		failures += report( p.name.size() <= 16, false, "%-16s %2zu characters", p.name.c_str(), p.name.size() );
		failures += report( seen.insert( p.name ).second, false, "%-16s unique", p.name.c_str() );
	}
	failures += report( std::string( "SW Handleman" ).size() <= 16, false, "display name 'SW Handleman' is %zu characters", std::string( "SW Handleman" ).size() );
	return failures;
}


//---------------------------------------------------------------------------
// The moving card, for --out, the sweep and the bench: a row of colour
// patches, a grey ramp, a white disc on an orbit, a static black square, a
// drifting blue bar, and a flashing patch (on for 6 frames in 30).
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t  = static_cast< double >( frame ) / 60.0;
	const double cx = 0.72 + 0.14 * std::cos( 1.2 * t ), cy = 0.68 + 0.16 * std::sin( 1.2 * t );
	const double barX = std::fmod( 0.05 * t, 1.0 );
	const bool flash  = frame % 30 < 6;
	const double patches[ 8 ][ 3 ] = {
		{ 0.80, 0.10, 0.10 }, { 0.88, 0.67, 0.55 }, { 0.90, 0.85, 0.15 }, { 0.15, 0.60, 0.20 },
		{ 0.20, 0.80, 0.85 }, { 0.15, 0.25, 0.85 }, { 0.53, 0.81, 0.92 }, { 0.95, 0.95, 0.95 },
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 0.40, g = 0.40, b = 0.40;
			if( fy > 0.06 && fy < 0.30 )
			{
				const int i = std::clamp( static_cast< int >( ( fx - 0.04 ) / 0.115 ), 0, 7 );
				if( fx > 0.04 && fx < 0.96 && std::fmod( fx - 0.04, 0.115 ) < 0.105 )
				{
					r = patches[ i ][ 0 ];
					g = patches[ i ][ 1 ];
					b = patches[ i ][ 2 ];
				}
			}
			if( fy > 0.36 && fy < 0.46 && fx > 0.04 && fx < 0.96 )
				r = g = b = ( fx - 0.04 ) / 0.92;
			if( fx > 0.08 && fx < 0.28 && fy > 0.56 && fy < 0.92 )
				r = g = b = flash ? 0.98 : 0.02;
			const double dx = ( fx - cx ) * width, dy = ( fy - cy ) * height;
			if( dx * dx + dy * dy < ( height * 0.08 ) * ( height * 0.08 ) )
				r = g = b = 0.98;
			if( std::fabs( fx - barX ) < 0.02 && fy > 0.5 )
			{
				r = 0.2;
				g = 0.3;
				b = 0.9;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]           = static_cast< unsigned char >( std::lround( 255.0 * r ) );
			px[ 1 ]           = static_cast< unsigned char >( std::lround( 255.0 * g ) );
			px[ 2 ]           = static_cast< unsigned char >( std::lround( 255.0 * b ) );
			px[ 3 ]           = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Handleman& plugin, int width, int height, int frames, double fps, size_t& stateBytes )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Handleman::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT && p.type != FF_TYPE_TEXT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is uploaded once: a host's frame is already on the GPU, and
	//the plugin's cost does not depend on what the frame holds.
	const std::vector< unsigned char > card = buildCard( width, height, 0 );
	const int warmup                        = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}
	stateBytes = session.plugin.StateBytesForTest();
	session.end();
	return best;
}

int runBench( Handleman& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides, the card uploaded once.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   state held\n" );
	for( const Size& size : sizes )
	{
		size_t bytes    = 0;
		const double ms = benchAt( plugin, size.width, size.height, frames, fps, bytes );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %6.2f MB\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, static_cast< double >( bytes ) / 1048576.0 );
	}
	std::printf( "\nState is the film ring (RGBA8 layers at the input raster, as deep as the\n"
	             "hands have needed) and the print's one-row data texture. Whatever the\n"
	             "settings above were, they are what was measured; --set measures another.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace shaders = handleman::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },
		{ "capture.frag", shaders::Capture() },
		{ "resample.frag", shaders::Resample() },
		{ "output.frag", shaders::Output() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//
// A STANDARD parameter ramps linearly between cues. An option, a boolean
// and an integer STEP: they hold the last cue at or before the frame,
// because there is nothing between Xenon and Tungsten to ramp through. An
// event fires on its cue frame only.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"hmtest -- render and measure the Handleman cranks\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/handleman.png)\n"
		"  --average           write the mean of every frame rendered, not the last\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 90)\n"
		"  --fps N             synthetic display rate driving the clock (default 60)\n"
		"  --source card|flat|white|black   what to feed (card moves); --level V for flat\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options).\n"
		"                      Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --crank             each hand moves what it should and nothing else; the loop is bounded\n"
		"  --exposure          a frame cranked slowly prints lighter, at the print's gamma\n"
		"  --flicker           a flat grey's spectrum is the shutter's Fourier series, the beat on top\n"
		"  --tone              neutral, gamma, toning scales density, a tint multiplies, the stocks' weights\n"
		"  --hold              grain and dust change with the film frame, not the display frame\n"
		"  --scratch           scratches stay at their x while the picture moves and weaves\n"
		"  --weave             the offsets have the stated sigma and lag-1 correlation\n"
		"  --runout            the roll runs out to blank cells; the frame line; the tails; threading\n"
		"  --resize            the loop of film survives a resize\n"
		"  --alpha             alpha 1 at Mix 1, the host's own at Mix 0\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --names             nothing the host will silently truncate\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, and the state held\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/handleman.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string source = "card";
	double level   = 0.5;
	int width      = 1280;
	int height     = 720;
	int frames     = 90;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool average   = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--crank", "--exposure", "--flicker", "--tone", "--hold", "--scratch", "--weave", "--runout", "--resize", "--alpha", "--negative" };
	const std::set< std::string > offline  = { "--names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			source = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--average" )
			average = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Handleman plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL = false;
		for( const std::string& check : checks )
		{
			if( check == "--names" )
			{
				runNames();
				std::printf( "\n" );
			}
			else
				needGL = true;
		}

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--crank" )
						runCrank( width, height, perturb );
					else if( check == "--exposure" )
						runExposure( width, height, perturb );
					else if( check == "--flicker" )
						runFlicker( width, height, perturb );
					else if( check == "--tone" )
						runTone( width, height, perturb );
					else if( check == "--hold" )
						runHold( width, height, perturb );
					else if( check == "--scratch" )
						runScratch( width, height, perturb );
					else if( check == "--weave" )
						runWeave( width, height, perturb );
					else if( check == "--runout" )
						runRunOut( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--alpha" )
						runAlpha( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantBench )
		return finish( runBench( session.plugin, frames < 40 ? 60 : frames, fps ) );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider
			//would, and an event is a press.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			const bool ok = index != failRender && session.render( index, frame );
			if( !ok )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	std::vector< double > sum;
	for( int frame = 0; frame < frames; ++frame )
	{
		bool ok = false;
		if( source == "card" )
			ok = session.render( frame, buildCard( width, height, frame ) );
		else if( source == "flat" )
			ok = session.render( frame, flat( width, height, level ) );
		else if( source == "white" )
			ok = session.render( frame, flat( width, height, 1.0 ) );
		else if( source == "black" )
			ok = session.render( frame, flat( width, height, 0.0 ) );
		else
		{
			std::fprintf( stderr, "unknown --source %s\n", source.c_str() );
			return finish( 2 );
		}
		if( !ok )
			return finish( 1 );
		if( average )
		{
			const std::vector< unsigned char > image = session.readBack();
			sum.resize( image.size(), 0.0 );
			for( size_t i = 0; i < image.size(); ++i )
				sum[ i ] += image[ i ];
		}
	}

	std::vector< unsigned char > image = session.readBack();
	if( average )
		for( size_t i = 0; i < image.size(); ++i )
			image[ i ] = static_cast< unsigned char >( std::lround( sum[ i ] / frames ) );
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames%s)\n", outPath.c_str(), width, height, frames, average ? ", averaged" : "" );
	return finish( 0 );
}

