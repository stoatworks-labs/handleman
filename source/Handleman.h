#pragma once

#include "FilmRing.h"
#include "Model.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <deque>
#include <string>
#include <vector>

/**
	Handleman -- a hand-cranked camera and projector, as an FFGL effect.

	**The one idea.** Model the two hands, not the look. A cameraman turned
	the camera's handle, eight frames a turn, and a projectionist turned the
	projector's; neither is a motor. The camera's hand decides WHEN each
	frame was exposed (so motion speeds up and slows down, and a frame cranked
	slowly is exposed longer and prints lighter); the projector's decides how
	long each frame stays on the screen and how fast the shutter flickers.
	Between them runs a loop of film, held here as a ring of pictures.

	Time is frame-relative: both cranks are kept in double, in frames, and
	advanced by each frame's real elapsed seconds; the shader sees weights,
	offsets and ring layers, never a clock. The print's marks are pure
	functions of the frame and a seed (Model.h); the ring is copied, not
	cleared, when the raster changes. See AGENTS.md for the traps.
*/
class Handleman : public CFFGLPlugin
{
public:
	Handleman();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook: the harness DECLARES its unit rather than leaving the
	/// voting to infer one.
	void SetClockScaleForTest( double scale );

	/// Negative-control hooks, a bitmask of `model::Perturb`. Always 0.
	void SetPerturbForTest( int bits );

	/// Probe hook: `model::Probe`.
	void SetProbeForTest( int probe );

	/// Replace the print's scratches with these (edges in width fractions),
	/// alive on every frame. Empty: the model's own.
	void SetScratchOverrideForTest( const std::vector< handleman::model::Scratch >& list );

	/// What the last frame did: its exposure in projector frames, the sum of
	/// its segments' weights, and the cranks' amplitudes (frames).
	void LastWindowForTest( double& p0, double& p1 ) const;
	double LastWeightSumForTest() const;
	void AmplitudesForTest( double& camera, double& projector ) const;

	/// The camera's frames still in the ring: ( frame, exposure ).
	std::vector< std::pair< int64_t, double > > ExposedForTest() const;

	/// Bytes of GPU state held across frames.
	size_t StateBytesForTest() const;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Crank
		PT_CRANK,
		PT_CAMERA_HAND,
		PT_PROJECTOR_HAND,
		PT_BLADES,
		PT_SHUTTER,

		//Print
		PT_STOCK,
		PT_TONE,
		PT_CONTRAST,
		PT_GRAIN,

		//Wear
		PT_WEAVE,
		PT_SCRATCHES,
		PT_DUST,
		PT_HAIR,
		PT_FRAMING,

		//Run Out
		PT_RUN_OUT,
		PT_RUN_OUT_TIME,
		PT_TAIL,

		//Output
		PT_VIGNETTE,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	/// A frame the camera exposed: which ring layer holds it, and how much
	/// light it got relative to a frame cranked at the nominal rate.
	struct Exposed
	{
		int64_t frame;
		int layer;
		double exposure;
	};
	const Exposed* exposedAt( int64_t frame ) const;

	ffglex::FFGLShader captureShader;
	ffglex::FFGLShader resampleShader;
	ffglex::FFGLShader outputShader;
	ffglex::FFGLScreenQuad quad;

	handleman::FilmRing ring;
	GLuint printTexture = 0;

	//--- the cranks ---------------------------------------------------------
	bool running     = false;
	double film      = 0.0;///< the nominal rate's integral, frames
	handleman::model::Hand camera;
	handleman::model::Hand projector;
	double lastP1    = 0.0;///< the projector's phase at the end of the last exposure
	double lastSpan  = 0.0;///< that exposure's length in frames, for a frame the clock did not move
	double windowP0 = 0.0, windowP1 = 0.0, weightSum = 0.0;
	int64_t lastPulled  = 0;
	int64_t lastExposed = 0;
	std::deque< Exposed > exposed;
	uint32_t seed       = 0x4a4d1e5eu;
	int64_t displayFrames = 0;

	handleman::model::Reel reel;
	bool runOutHeld = false;

	std::vector< handleman::model::Scratch > lastScratches;
	std::vector< handleman::model::Scratch > scratchOverride;

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	int perturb = 0;
	int probe   = 0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
