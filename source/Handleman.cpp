#include "Handleman.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"
#include "Spectral.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using namespace ffglex;
using namespace handleman;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Handleman >,                                  // Create method
	"HM01",                                                      // Plugin unique ID of maximum length 4.
	"SW Handleman",                                              // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"A hand-cranked camera and projector.\n\nThe camera's handle decides when each frame was exposed, so motion speeds up and slows down and a frame cranked slowly prints lighter; the projector's decides how long each frame stays on the screen and how fast the shutter flickers. The print is orthochromatic or panchromatic silver, black and white, sepia toned or tinted, with grain, weave, scratches, dust and a hair in the gate. Run Out runs the roll out: the picture fogs to white from the edges, blank cells follow and the frame line crawls up the screen; press it again to thread a new roll.",// Plugin description
	"Handleman FFGL effect"                                      // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;
/// The deepest ring: what the cranks at full deviation, a quarter-second
/// stall and the frame table can need, with room.
constexpr int kMaxRing = 32;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Hash streams the plugin draws from (Model.cpp has its own).
constexpr uint32_t kStreamHandCamera    = 101;
constexpr uint32_t kStreamHandProjector = 102;
constexpr uint32_t kStreamJitter        = 103;
constexpr uint32_t kStreamGrain         = 104;
} // namespace

//---------------------------------------------------------------------------
Handleman::Handleman()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The cranks run on host time.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults (AGENTS.md, "Decisions").
	//---------------------------------------------------------------------
	params[ PT_CRANK ]          = controls::CrankParam( 16.0 );
	params[ PT_CAMERA_HAND ]    = 0.3f;
	params[ PT_PROJECTOR_HAND ] = 0.3f;
	// 3 blades at 315 degrees, the gentlest shutter there is: at 16 fps a
	// display frame is a quarter of a projector frame, so a narrower opening
	// beats against 60 Hz in steps of up to 38% of the light from one display
	// frame to the next once the projector's hand moves the beat about
	// (AGENTS.md "Decisions", the flicker survey).
	params[ PT_BLADES ]  = 2.0f;//3
	params[ PT_SHUTTER ] = controls::ShutterParam( 315.0 );

	params[ PT_STOCK ]    = 0.0f;//Orthochromatic
	params[ PT_TONE ]     = static_cast< float >( model::kSepia );
	params[ PT_CONTRAST ] = 0.5f;//gamma 1.4
	params[ PT_GRAIN ]    = 0.35f;

	params[ PT_WEAVE ]     = 0.35f;
	params[ PT_SCRATCHES ] = 0.35f;
	params[ PT_DUST ]      = 0.35f;
	params[ PT_HAIR ]      = 0.25f;
	params[ PT_FRAMING ]   = 0.5f;//centred

	params[ PT_RUN_OUT ]      = 0.0f;
	params[ PT_RUN_OUT_TIME ] = controls::RunOutParam( 1.0 );
	params[ PT_TAIL ]         = static_cast< float >( model::kRolling );

	params[ PT_VIGNETTE ] = 0.35f;
	params[ PT_MIX ]      = 1.0f;

	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	SetParamInfof( PT_CRANK, "Crank Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_CAMERA_HAND, "Camera Hand", FF_TYPE_STANDARD );
	SetParamInfof( PT_PROJECTOR_HAND, "Projector Hand", FF_TYPE_STANDARD );
	declareOptions( PT_BLADES, "Blades", controls::kBladesCount, controls::BladesName );
	SetParamInfof( PT_SHUTTER, "Shutter Angle", FF_TYPE_STANDARD );

	declareOptions( PT_STOCK, "Stock", controls::kStockCount, controls::StockName );
	declareOptions( PT_TONE, "Tone", controls::kToneCount, controls::ToneName );
	SetParamInfof( PT_CONTRAST, "Contrast", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRAIN, "Grain", FF_TYPE_STANDARD );

	SetParamInfof( PT_WEAVE, "Weave", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCRATCHES, "Scratches", FF_TYPE_STANDARD );
	SetParamInfof( PT_DUST, "Dust", FF_TYPE_STANDARD );
	SetParamInfof( PT_HAIR, "Hair", FF_TYPE_STANDARD );
	SetParamInfof( PT_FRAMING, "Framing", FF_TYPE_STANDARD );

	SetParamInfo( PT_RUN_OUT, "Run Out", FF_TYPE_EVENT, false );
	SetParamInfof( PT_RUN_OUT_TIME, "Run Out Time", FF_TYPE_STANDARD );
	declareOptions( PT_TAIL, "Tail", controls::kTailCount, controls::TailName );

	SetParamInfof( PT_VIGNETTE, "Vignette", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_CRANK; i <= PT_SHUTTER; ++i )
		SetParamGroup( i, "Crank" );
	for( FFUInt32 i = PT_STOCK; i <= PT_GRAIN; ++i )
		SetParamGroup( i, "Print" );
	for( FFUInt32 i = PT_WEAVE; i <= PT_FRAMING; ++i )
		SetParamGroup( i, "Wear" );
	for( FFUInt32 i = PT_RUN_OUT; i <= PT_TAIL; ++i )
		SetParamGroup( i, "Run Out" );
	for( FFUInt32 i = PT_VIGNETTE; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Handleman effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Handleman::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &captureShader, shaders::Capture(), "capture" },
		{ &resampleShader, shaders::Resample(), "resample" },
		{ &outputShader, shaders::Output(), "output" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Handleman: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Handleman: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//The print's particles, scratches and hair: one row, texelFetch only.
	glGenTextures( 1, &printTexture );
	glBindTexture( GL_TEXTURE_2D, printTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, shaders::kPrintDataWidth, 1, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Handleman::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Handleman::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
const Handleman::Exposed* Handleman::exposedAt( int64_t frame ) const
{
	if( exposed.empty() )
		return nullptr;
	//A frame the camera has not reached yet (the frame below the gate, when
	//the projector is right behind the camera) shows the newest picture: the
	//host only ever has the current one. Older than the ring: the oldest.
	if( frame >= exposed.back().frame )
		return &exposed.back();
	if( frame <= exposed.front().frame )
		return &exposed.front();
	return &exposed[ static_cast< size_t >( frame - exposed.front().frame ) ];
}

//---------------------------------------------------------------------------
FFResult Handleman::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	using namespace model;

	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const double outWidth  = hostViewport[ 2 ] > 0 ? hostViewport[ 2 ] : width;
	const double outHeight = hostViewport[ 3 ] > 0 ? hostViewport[ 3 ] : height;
	const double aspect    = outWidth / outHeight;

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const double fps         = controls::CrankFps( params[ PT_CRANK ] );
	const double cameraDev   = controls::CameraDeviation( params[ PT_CAMERA_HAND ] );
	const double projectorDev = controls::ProjectorDeviation( params[ PT_PROJECTOR_HAND ] );
	const int blades         = controls::Blades( controls::OptionIndex( params[ PT_BLADES ], controls::kBladesCount ) );
	const double open        = controls::ShutterOpen( params[ PT_SHUTTER ] );
	int stock                = controls::OptionIndex( params[ PT_STOCK ], controls::kStockCount );
	const int tone           = controls::OptionIndex( params[ PT_TONE ], controls::kToneCount );
	const double gamma       = controls::Gamma( params[ PT_CONTRAST ] );
	const double grain       = controls::Amount( params[ PT_GRAIN ] );
	const WeaveLaw law       = Weave( controls::Amount( params[ PT_WEAVE ] ), perturb );
	const double scratches   = controls::Amount( params[ PT_SCRATCHES ] );
	const double dust        = controls::Amount( params[ PT_DUST ] );
	const double hair        = controls::Amount( params[ PT_HAIR ] );
	const double framing     = controls::FramingHeights( params[ PT_FRAMING ] );
	const double runOutTime  = controls::RunOutSeconds( params[ PT_RUN_OUT_TIME ] );
	const int tail           = controls::OptionIndex( params[ PT_TAIL ], controls::kTailCount );
	const double vignette    = controls::VignetteDegrees( params[ PT_VIGNETTE ] );
	const float mixAmount    = controls::Amount( params[ PT_MIX ] );
	if( ( perturb & kPerturbOrthoAsPan ) && stock == 0 )
		stock = 1;

	//---------------------------------------------------------------------
	// The clock and the cranks. Frame-relative: everything is held in double
	// in frames and advanced by this frame's real seconds, so a Crank Rate
	// change carries on from where both cranks are, and nothing absolute
	// reaches the shader. The first frame is worth a nominal display frame.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	double dt        = kNominalFrame;
	if( lastNow >= 0.0 )
		dt = std::clamp( now - lastNow, 0.0, kMaxFrameDelta );
	lastNow = now;
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
	++displayFrames;

	auto projectorPhase = [ & ]() {
		const double loop = ( perturb & kPerturbNoLoop ) ? 0.0 : camera.Amplitude() + projector.Amplitude();
		return film - projector.Error() - loop;
	};

	const bool first = !running;
	if( first )
	{
		camera.Start( seed, kStreamHandCamera, kCameraHand, kCameraPartials, cameraDev );
		projector.Start( seed, kStreamHandProjector, kProjectorHand, kProjectorPartials, projectorDev );
		film   = 0.0;
		lastP1 = projectorPhase();
		reel.Reset();
	}
	camera.Advance( dt, fps, cameraDev );
	projector.Advance( dt, fps, projectorDev );
	film += fps * dt;

	const double cameraPhase = film - camera.Error();
	const double p1          = std::max( projectorPhase(), lastP1 );
	double p0                = lastP1;
	if( dt > 0.0 || first )
		lastSpan = p1 - lastP1;
	else
		p0 = p1 - lastSpan;
	lastP1   = p1;
	running  = true;
	windowP0 = p0;
	windowP1 = p1;

	Segment segments[ kMaxSegments ];
	const int count      = Segments( p0, p1, blades, open, ( perturb & kPerturbInstant ) != 0, segments );
	const int64_t oldest = segments[ 0 ].frame;
	const int64_t latest = segments[ count - 1 ].frame;

	//Every pull-down up to the newest frame in the gate, in order: the reel's
	//slip and tail are decided one pull-down at a time.
	if( first )
		lastPulled = oldest - 1;
	for( int64_t k = lastPulled + 1; k <= latest; ++k )
		reel.Pull( k, tail );
	lastPulled = std::max( lastPulled, latest );

	//Run Out, on the press: from the next pull-down.
	const bool runOutPressed = params[ PT_RUN_OUT ] >= 0.5f;
	if( runOutPressed && !runOutHeld )
		reel.Press( lastPulled + 1, std::max( 1, static_cast< int >( std::lround( runOutTime * fps ) ) ) );
	runOutHeld = runOutPressed;

	//---------------------------------------------------------------------
	// The camera. Every frame its crank passed since the last display frame
	// was exposed to the host's picture now (one capture shared by all of
	// them); each gets its own exposure, from the crank's speed and the
	// hand's jerk while the shutter was open.
	//---------------------------------------------------------------------
	const int64_t newestCamera = static_cast< int64_t >( std::floor( cameraPhase ) );
	if( first )
		lastExposed = std::min( oldest - 3, newestCamera - 1 );
	const bool expose = newestCamera > lastExposed;

	//Frames the shader may read from here on: two above the oldest segment.
	const int64_t keepFrom = oldest - 2;
	while( !exposed.empty() && exposed.front().frame < keepFrom && exposed.size() > 1 )
		exposed.pop_front();

	int captureLayer = -1;
	if( expose )
	{
		std::set< int > used;
		for( const Exposed& e : exposed )
			used.insert( e.layer );
		for( int layer = 0; layer < ring.Depth(); ++layer )
			if( !used.count( layer ) )
			{
				captureLayer = layer;
				break;
			}
		if( captureLayer < 0 )
		{
			if( ring.Depth() < kMaxRing )
				captureLayer = std::max( ring.Depth(), static_cast< int >( used.size() ) );
			else
			{
				//Only past the deepest ring: the oldest frame gives up its layer.
				captureLayer = exposed.front().layer;
				exposed.pop_front();
			}
		}
	}

	//---------------------------------------------------------------------
	// Buffers, every allocation before anything binds a texture for the
	// frame's passes. A raster change or a deeper ring COPIES the ring: the
	// loop of film is still to be projected.
	//---------------------------------------------------------------------
	//Deeper only when a capture needs a layer the ring has not got, and then
	//with two to spare, so a hand drifting out does not reallocate frame by
	//frame. Ensure() is a no-op at the same raster and a depth it already has.
	const int oldDepth = ring.Depth();
	int wantDepth      = std::max( captureLayer + 1, 1 );
	if( wantDepth > oldDepth )
		wantDepth = std::min( wantDepth + 2, kMaxRing );
	if( ring.IsValid() && ( ring.Width() != width || ring.Height() != height ) && ( perturb & kPerturbResizeClears ) )
	{
		ring.Destroy();//the negative control: the photofinish bug, on purpose
		wantDepth = std::max( wantDepth, oldDepth );
	}
	if( !ring.Ensure( width, height, wantDepth, resampleShader, quad, true ) )
	{
		diag::error( "could not allocate the film ring at " + std::to_string( width ) + "x" + std::to_string( height ) );
		return FF_FAIL;
	}

	if( expose )
	{
		const double speed = ( perturb & kPerturbExposureProjector ) ? fps - projector.ErrorRate( fps )
		                                                              : fps - camera.ErrorRate( fps );
		const double base  = fps / std::max( speed, 0.05 * fps );
		for( int64_t m = lastExposed + 1; m <= newestCamera; ++m )
		{
			const double jerk = std::exp( kExposureJitter * cameraDev * Normal( seed, kStreamJitter, m ) );
			exposed.push_back( { m, captureLayer, base * jerk } );
		}
		lastExposed = newestCamera;

		ring.BindLayer( captureLayer );
		ScopedShaderBinding shader( captureShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );
		captureShader.Set( "Source", 0 );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// The frame table: every frame the segments can show, from two above the
	// oldest to two below the newest.
	//---------------------------------------------------------------------
	const int64_t tableFirst = oldest - 2;
	const int frameCount     = static_cast< int >( latest - oldest ) + 5;
	int frameLayer[ kMaxFrames ]      = {};
	float frameFog[ kMaxFrames ]      = {};
	float frameExposure[ kMaxFrames ] = {};
	int frameSeed[ kMaxFrames ]       = {};
	for( int i = 0; i < frameCount; ++i )
	{
		const int64_t n  = tableFirst + i;
		const double fog = reel.Fog( n );
		const Exposed* e = exposedAt( n );
		frameLayer[ i ]    = reel.NoFilm( n ) ? -2 : fog >= 1.0 ? -1 : ( e ? e->layer : -1 );
		frameFog[ i ]      = static_cast< float >( FogExposure( fog ) );
		frameExposure[ i ] = static_cast< float >( e ? e->exposure : 1.0 );
		const int64_t grainKey = ( perturb & kPerturbGrainPerDisplay ) ? displayFrames * 16 + i : n;
		frameSeed[ i ]     = static_cast< int >( Key( seed, kStreamGrain, grainKey ) & 0x7fffffffu );
	}

	//---------------------------------------------------------------------
	// The segments: where each frame sits in the gate, and the print on it.
	//---------------------------------------------------------------------
	float weights[ kMaxSegments ]     = {};
	float offsets[ 2 * kMaxSegments ] = {};
	int ownFrame[ kMaxSegments ]      = {};
	std::vector< float > data( static_cast< size_t >( shaders::kPrintDataWidth ) * 4, 0.0f );
	int particleCount = 0;
	int hairFrom      = -1;
	int hairMask      = 0;
	Hair hairShape;
	weightSum = 0.0;
	for( int i = count - 1; i >= 0; --i )
	{
		const int64_t k = segments[ i ].frame;
		double wx = 0.0, wy = 0.0;
		WeaveOffset( seed, k, law, wx, wy );

		//Framing (+ moves the picture down) and the reel's slip (+ up), in
		//frames; whole frames move which frame is in the gate, the rest is the
		//offset the shader draws.
		const double o       = -framing / kPitch + reel.Offset( k );
		const double shift   = std::floor( o + 0.5 );
		const double residue = o - shift;
		const int64_t own    = k + static_cast< int64_t >( shift );

		weights[ i ]         = static_cast< float >( segments[ i ].weight );
		offsets[ 2 * i ]     = static_cast< float >( wx );
		offsets[ 2 * i + 1 ] = static_cast< float >( -residue * kPitch + wy );
		ownFrame[ i ]        = static_cast< int >( std::clamp< int64_t >( own - tableFirst, 0, frameCount - 1 ) );
		weightSum += segments[ i ].weight;

		if( !reel.NoFilm( own ) )
			for( const Particle& p : Dust( seed, own, dust, aspect ) )
			{
				if( particleCount >= kMaxParticles )
					break;
				float* t0 = data.data() + 8 * particleCount;
				t0[ 0 ]   = static_cast< float >( p.x );
				t0[ 1 ]   = static_cast< float >( p.y );
				t0[ 2 ]   = static_cast< float >( p.a );
				t0[ 3 ]   = static_cast< float >( p.b );
				t0[ 4 ]   = static_cast< float >( std::cos( p.angle ) );
				t0[ 5 ]   = static_cast< float >( std::sin( p.angle ) );
				t0[ 6 ]   = static_cast< float >( p.strength );
				t0[ 7 ]   = static_cast< float >( i + 8 * p.fibre );
				++particleCount;
			}

		const Hair h = HairAt( seed, k, hair, aspect );
		if( h.present )
		{
			hairMask |= 1 << i;
			if( hairFrom < 0 )
			{
				hairFrom  = i;
				hairShape = h;
			}
		}
	}

	lastScratches    = scratchOverride.empty() ? Scratches( seed, oldest, latest, scratches, aspect ) : scratchOverride;
	int scratchCount = 0;
	for( const Scratch& s : lastScratches )
	{
		if( scratchCount >= kMaxScratches )
			break;
		int mask = 0;
		for( int i = 0; i < count; ++i )
			if( !scratchOverride.empty() || ( segments[ i ].frame >= s.born && segments[ i ].frame < s.dies ) )
				mask |= 1 << i;
		float* t = data.data() + 4 * ( shaders::kPrintScratchFirst + 2 * scratchCount );
		t[ 0 ]   = static_cast< float >( s.left );
		t[ 1 ]   = static_cast< float >( s.right );
		t[ 2 ]   = static_cast< float >( s.strength );
		t[ 3 ]   = static_cast< float >( s.side );
		t[ 4 ]   = static_cast< float >( mask );
		++scratchCount;
	}

	if( hairShape.present )
		for( int i = 0; i < kHairPoints; ++i )
		{
			float* t = data.data() + 4 * ( shaders::kPrintHairFirst + i );
			t[ 0 ]   = static_cast< float >( hairShape.x[ i ] );
			t[ 1 ]   = static_cast< float >( hairShape.y[ i ] );
		}

	{
		Scoped2DTextureBinding texture( printTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, shaders::kPrintDataWidth, 1, GL_RGBA, GL_FLOAT, data.data() );
	}

	//---------------------------------------------------------------------
	// The projection, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( outputShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		ScopedTextureBinding ringBinding( GL_TEXTURE_2D_ARRAY, ring.Texture() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding sourceTexture( input.Handle );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding printBinding( printTexture );

		double toning[ 3 ], tint[ 3 ];
		ToneLaw( tone, perturb, toning, tint );
		const spectral::Weights& basis = spectral::kWeights[ std::clamp( stock, 0, spectral::kStockCount - 1 ) ];

		outputShader.Set( "Ring", 0 );
		outputShader.Set( "Source", 1 );
		outputShader.Set( "PrintData", 2 );
		glUniform2i( outputShader.FindUniform( "RingSize" ), ring.Width(), ring.Height() );
		outputShader.Set( "OutSize", static_cast< float >( outWidth ), static_cast< float >( outHeight ) );
		outputShader.Set( "SegCount", count );
		glUniform1fv( outputShader.FindUniform( "SegWeight" ), count, weights );
		glUniform2fv( outputShader.FindUniform( "SegOffset" ), count, offsets );
		glUniform1iv( outputShader.FindUniform( "SegFrame" ), count, ownFrame );
		outputShader.Set( "FrameCount", frameCount );
		glUniform1iv( outputShader.FindUniform( "FrameLayer" ), frameCount, frameLayer );
		glUniform1fv( outputShader.FindUniform( "FrameFog" ), frameCount, frameFog );
		glUniform1fv( outputShader.FindUniform( "FrameExposure" ), frameCount, frameExposure );
		glUniform1iv( outputShader.FindUniform( "FrameSeed" ), frameCount, frameSeed );
		outputShader.Set( "Pitch", static_cast< float >( kPitch ) );
		outputShader.Set( "Margin", static_cast< float >( kMargin ) );
		const double lineStart = 1.0 + kMargin + ( ( perturb & kPerturbFrameLineHalf ) ? 0.5 * kFrameLine : 0.0 );
		outputShader.Set( "LineStart", static_cast< float >( lineStart ) );
		outputShader.Set( "BaseDensity", static_cast< float >( kBaseDensity ) );
		outputShader.Set( "MaxDensity", static_cast< float >( kMaxDensity ) );
		outputShader.Set( "Knee", static_cast< float >( kKnee ) );
		outputShader.Set( "GreyLog", static_cast< float >( std::log10( kGreyExposure ) ) );
		outputShader.Set( "Gamma", static_cast< float >( gamma ) );
		outputShader.Set( "Colour", tone == kColour ? 1 : 0 );
		glUniform1fv( outputShader.FindUniform( "Basis" ), 7, basis.w );
		outputShader.Set( "Toning", static_cast< float >( toning[ 0 ] ), static_cast< float >( toning[ 1 ] ), static_cast< float >( toning[ 2 ] ) );
		outputShader.Set( "Tint", static_cast< float >( tint[ 0 ] ), static_cast< float >( tint[ 1 ] ), static_cast< float >( tint[ 2 ] ) );
		outputShader.Set( "FogFlat", static_cast< float >( kFogFlat ) );
		outputShader.Set( "FogReach", static_cast< float >( kFogReach ) );
		outputShader.Set( "GrainAmount", static_cast< float >( kGrainSigma * grain ) );
		outputShader.Set( "GrainFloor", static_cast< float >( kGrainFloor ) );
		outputShader.Set( "GrainCell", static_cast< float >( kGrainMm / kApertureHeightMm ) );
		outputShader.Set( "ParticleCount", particleCount );
		outputShader.Set( "ScratchCount", scratchCount );
		outputShader.Set( "HairPoints", hairShape.present ? kHairPoints : 0 );
		outputShader.Set( "HairMask", hairMask );
		outputShader.Set( "HairHalfWidth", static_cast< float >( hairShape.halfWidth ) );
		outputShader.Set( "HairOpacity", static_cast< float >( hairShape.opacity ) );
		outputShader.Set( "VignetteTan", static_cast< float >( std::tan( vignette * kPi / 180.0 ) ) );
		outputShader.Set( "Aspect", static_cast< float >( aspect ) );
		outputShader.Set( "MixAmount", mixAmount );
		outputShader.Set( "Probe", probe );
		outputShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Handleman::DeInitGL()
{
	captureShader.FreeGLResources();
	resampleShader.FreeGLResources();
	outputShader.FreeGLResources();
	quad.Release();
	ring.Destroy();
	if( printTexture != 0 )
	{
		glDeleteTextures( 1, &printTexture );
		printTexture = 0;
	}
	//The cranks and the ring go together: a fresh InitGL starts both again.
	exposed.clear();
	running = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Handleman::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Handleman::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Handleman::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Handleman::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Handleman::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Handleman::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Handleman::SetProbeForTest( int p )
{
	probe = p;
}

void Handleman::SetScratchOverrideForTest( const std::vector< model::Scratch >& list )
{
	scratchOverride = list;
}

void Handleman::LastWindowForTest( double& p0, double& p1 ) const
{
	p0 = windowP0;
	p1 = windowP1;
}

double Handleman::LastWeightSumForTest() const
{
	return weightSum;
}

void Handleman::AmplitudesForTest( double& c, double& p ) const
{
	c = camera.Amplitude();
	p = projector.Amplitude();
}

std::vector< std::pair< int64_t, double > > Handleman::ExposedForTest() const
{
	std::vector< std::pair< int64_t, double > > out;
	for( const Exposed& e : exposed )
		out.emplace_back( e.frame, e.exposure );
	return out;
}

int Handleman::RingDepthForTest() const
{
	return ring.Depth();
}

size_t Handleman::StateBytesForTest() const
{
	return ring.Bytes() + static_cast< size_t >( shaders::kPrintDataWidth ) * 16;
}
