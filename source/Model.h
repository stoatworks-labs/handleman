#pragma once

#include <cstdint>
#include <deque>
#include <vector>

/**
	The two hands, the loop of film between them, the shutter, the reel and
	the print, as numbers, in double, on the CPU.

	Time is in FRAMES. Each crank's phase is a film position: the camera's
	says which frame it is exposing, the projector's which frame is in the
	gate. Both are `F - A w( t )` (the projector also runs a fixed distance
	behind): F is the integral of the nominal rate, w a bounded sum of
	sinusoids, A the amplitude that makes the hand's peak speed deviation what
	the control says. Because |w| <= 1 the delay between the two is bounded,
	and because every frequency is in cycles per FRAME (a crank turn is eight
	frames) that bound does not depend on the crank rate.

	Geometry is in picture HEIGHTS ("h-units") of the projector's aperture, y
	down from the top of the frame, x across from the left to `aspect`.
	Scratches are the exception: their edges are fractions of the output
	WIDTH, so a scratch can sit on whole pixels exactly (`hmtest --scratch`).

	See AGENTS.md for which constants are published and which are chosen.
*/
namespace handleman::model
{

constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// Time.
//---------------------------------------------------------------------------

/// What a display frame is worth when the host's clock has not moved yet.
constexpr double kNominalFrame = 1.0 / 60.0;
/// A host stall is integrated for this long and no longer.
constexpr double kMaxFrameDelta = 0.25;
/// At most this many projector frames overlap one display frame: 0.25 s at
/// 24 fps and 40% fast is 8.4 frames, which the oldest-keeps-the-light rule
/// in Segments() absorbs.
constexpr int kMaxSegments = 8;
/// The shader's table of film frames: every segment's own frame, plus two
/// either side (the frame above and below, and Framing's whole-frame shift).
constexpr int kMaxFrames = kMaxSegments + 4;

//---------------------------------------------------------------------------
// The film: 35 mm silent, full aperture, the picture between the
// perforations. Published figures, in inches. The clip is what the
// projector's aperture shows; the camera exposed a little more, so a small
// weave shows more picture, not the frame line.
//---------------------------------------------------------------------------

/// The projection aperture, 0.906 x 0.6795 in (the SMPTE standard of 1930,
/// before the Academy aperture): the h-unit is its height.
constexpr double kProjectorApertureIn = 0.6795;
/// The full camera aperture's height, 0.709 in (of 0.945 x 0.709).
constexpr double kCameraApertureIn = 0.709;
/// One frame's pitch along the strip: four perforations of 0.187 in.
constexpr double kFramePitchIn = 0.748;
/// The h-unit in millimetres, for the print's sizes (dust, hair, scratches).
constexpr double kApertureHeightMm = kProjectorApertureIn * 25.4;
/// Exposed picture beyond the aperture, top and bottom, in h-units: 0.0217.
constexpr double kMargin = 0.5 * ( kCameraApertureIn - kProjectorApertureIn ) / kProjectorApertureIn;
/// Frame pitch in h-units: 1.1008.
constexpr double kPitch = kFramePitchIn / kProjectorApertureIn;
/// The frame line, in h-units: 0.0574 (0.039 in of unexposed negative).
constexpr double kFrameLine = kPitch - 1.0 - 2.0 * kMargin;

//---------------------------------------------------------------------------
// The two hands.
//
// A partial is one sinusoid of the hand's timing error: its frequency in
// cycles per frame and its share of the peak SPEED deviation. The shares sum
// to 1. Its coefficient in w is share / frequency, normalised so the
// coefficients' magnitudes sum to 1 (|w| <= 1); the amplitude
//   A = deviation x sum( share / frequency ) / 2 pi
// then makes the peak speed deviation exactly `deviation` of the nominal
// rate when every partial peaks together (they rarely do).
//---------------------------------------------------------------------------

constexpr double kFramesPerTurn = 8.0;///< one turn of the handle

struct Partial
{
	double cyclesPerFrame;
	double share;
};

/// The cameraman: once a turn (the arm pushes harder on the down stroke),
/// its second harmonic, and a count that drifts over about six and nine turns.
constexpr int kCameraPartials                    = 4;
constexpr Partial kCameraHand[ kCameraPartials ] = {
	{ 1.0 / kFramesPerTurn, 0.40 },
	{ 2.0 / kFramesPerTurn, 0.10 },
	{ 1.0 / 70.0, 0.30 },
	{ 1.0 / 43.0, 0.20 },
};

/// The projectionist: mostly the turn, and one slower drift.
constexpr int kProjectorPartials                       = 3;
constexpr Partial kProjectorHand[ kProjectorPartials ] = {
	{ 1.0 / kFramesPerTurn, 0.55 },
	{ 2.0 / kFramesPerTurn, 0.15 },
	{ 1.0 / 50.0, 0.30 },
};
constexpr int kMaxPartials = 4;

/// Peak speed deviation at Camera Hand 1 and Projector Hand 1.
constexpr double kCameraMaxDeviation    = 0.5;
constexpr double kProjectorMaxDeviation = 0.4;
/// How fast a hand's amplitude may follow its control, in frames per second
/// per frame per second of crank rate: it keeps the crank turning forwards
/// (|dA/dt w| <= this x the rate) when the control jumps.
constexpr double kAmplitudeSlew = 0.25;
/// The camera's shutter is open for part of each frame; the hand's speed
/// while it is open varies frame to frame about the turn's average. Each
/// frame's exposure carries a lognormal factor of sigma this x the deviation.
constexpr double kExposureJitter = 0.15;

/// The timing error's amplitude, in frames, for a peak speed deviation.
double Amplitude( const Partial* partials, int count, double deviation );

/// One hand on one crank. Its timing error is A w( t ), in frames.
class Hand
{
public:
	/// Random starting phases from the seed; the amplitude starts at its
	/// target, so the hand is already turning on the first frame.
	void Start( uint32_t seed, uint32_t stream, const Partial* partials, int count, double deviation );

	/// Advance by dt seconds at `fps`: every partial's phase by its
	/// frequency, the amplitude toward the control's, slew-limited.
	void Advance( double dt, double fps, double deviation );

	double Error() const;    ///< A w, frames
	double Amplitude() const;///< A, frames
	/// d( A w ) / dt, frames per second: the crank turns at fps minus this.
	double ErrorRate( double fps ) const;

private:
	const Partial* partials = nullptr;
	int count               = 0;
	double phase[ kMaxPartials ] = {};///< radians, reduced
	double coeff[ kMaxPartials ] = {};///< signed share of w, sum of |coeff| = 1
	double amplitude        = 0.0;
	double amplitudeRate    = 0.0;
};

//---------------------------------------------------------------------------
// The shutter. gate's, unchanged: one revolution per projector frame,
// `blades` openings, each open for `open` of its blade cycle and centred on
// it, so the pull-down at the start of the frame is in the dark. The
// projector's phase is taken as linear across one display frame (its speed
// changes by well under 1% in 1/60 s).
//---------------------------------------------------------------------------

double ShutterOpen( double p, int blades, double open );
double CumulativeOpen( double p, int blades, double open );

struct Segment
{
	int64_t frame; ///< the projector frame (pull-down count)
	double weight; ///< its light over the display frame, as a fraction of the mean
};

/// The projector frames the display frame's exposure [p0, p1] overlaps, oldest
/// first, each weighted by the light the shutter passed during the overlap,
/// divided by ( open x ( p1 - p0 ) ): so a steady picture averages to itself.
/// `instant` samples the shutter at p1 only (a negative control).
int Segments( double p0, double p1, int blades, double open, bool instant, Segment out[ kMaxSegments ] );

//---------------------------------------------------------------------------
// The gate: weave. A stationary AR(1) per projector frame, as gate states it,
//   x_k = rho x_{k-1} + sqrt( 1 - rho^2 ) sigma e_k,
// sigma kWeaveMm x Weave across, kWeaveVertical of that down. A hand-turned
// projector's claw and teeth are looser than a motor's, so the scale is
// larger than gate's.
//---------------------------------------------------------------------------

constexpr double kWeaveMm       = 0.15;///< sigma at Weave 1
constexpr double kWeaveVertical = 0.6;
constexpr double kWeaveRho      = 0.75;

struct WeaveLaw
{
	double sigmaX = 0.0;///< h-units
	double sigmaY = 0.0;
	double rho    = 0.0;
};

WeaveLaw Weave( double weave, int perturb );
void WeaveOffset( uint32_t seed, int64_t frame, const WeaveLaw& law, double& x, double& y );

//---------------------------------------------------------------------------
// The print's marks: gate's grit, dust and hair, in this film's millimetres.
//---------------------------------------------------------------------------

enum Side : int
{
	kBase     = 0,///< scatters light out of the lens: prints dark
	kEmulsion = 1,///< takes silver (and a tint's dye) off: prints light
};

struct Scratch
{
	double left     = 0.0;///< fraction of the output width
	double right    = 0.0;
	double strength = 0.0;///< base: fraction of light lost; emulsion: fraction of silver taken
	int side        = kBase;
	int64_t born    = 0;
	int64_t dies    = 0;
};

constexpr double kScratchBirths    = 6.0 / 96.0;
constexpr int kScratchLifeMin      = 24;
constexpr double kScratchLifeMean  = 96.0;
constexpr int kScratchLifeMax      = 480;
constexpr int kMaxScratches        = 16;
constexpr double kScratchHalfMmMin = 0.004;
constexpr double kScratchHalfMmMax = 0.020;
std::vector< Scratch > Scratches( uint32_t seed, int64_t first, int64_t last, double amount, double aspect );

struct Particle
{
	double x = 0.0, y = 0.0;///< h-units, on the frame
	double a = 0.0, b = 0.0;
	double angle = 0.0;
	int fibre    = 0;
	double strength = 0.0;///< > 0 dark (opacity); < 0 printed-in white
};

constexpr double kDustBirths = 8.0;
constexpr int kDustBirthsMax = 16;
constexpr int kMaxParticles  = 64;
std::vector< Particle > Dust( uint32_t seed, int64_t frame, double amount, double aspect );

constexpr int kHairSlot   = 96;
constexpr int kHairPoints = 13;
struct Hair
{
	bool present = false;
	double x[ kHairPoints ] = {}, y[ kHairPoints ] = {};
	double halfWidth = 0.0;
	double opacity   = 0.0;
};
Hair HairAt( uint32_t seed, int64_t frame, double amount, double aspect );

//---------------------------------------------------------------------------
// The print's silver: a characteristic curve from the negative's log
// exposure to the positive's density, log10 throughout.
//
// The straight line has slope -gamma (Contrast) and passes through an 18%
// grey printing at 18% of the clear base's light, so Contrast never moves
// the middle grey; it is clamped softly at the base's density below and the
// print's maximum above (a softplus of width kKnee each side).
//---------------------------------------------------------------------------

constexpr double kBaseDensity = 0.06;///< clear nitrate base and fog
constexpr double kMaxDensity  = 2.4; ///< the print's deepest black
constexpr double kKnee        = 0.12;
constexpr double kGreyExposure = 0.18;

/// Print density for a log10 exposure (white = 0).
double Density( double logExposure, double gamma );

/// Grain: density noise, sigma kGrainSigma x Grain x sqrt( silver / range +
/// kGrainFloor ), in clumps kGrainMm across, a new pattern on every frame.
constexpr double kGrainSigma = 0.22;
constexpr double kGrainFloor = 0.03;
constexpr double kGrainMm    = 0.02;

//---------------------------------------------------------------------------
// The reel: Run Out and threading.
//
// Fog in [ 0, 1 ] belongs to a film frame (by pull-down count). Pressed while
// the picture runs, the roll runs out: from the next pull-down the fog climbs
// to 1 over `ramp` frames, and a frame at 1 is a blank cell. Pressed again,
// a new roll is threaded: `head` blank cells, then the fog falls to 0. A
// press mid-ramp turns back from where the fog had got to.
//
// Fog is an exposure added to the negative, (2^( kFogCurve f ) - 1) /
// (2^kFogCurve - 1) x kFogMax whites, kFogEdge of it pouring in from each
// edge of the film over kFogReach and the rest flat: the picture burns to
// white from the perforations in.
//---------------------------------------------------------------------------

constexpr double kFogCurve = 7.0;
constexpr double kFogMax   = 6.0;
constexpr double kFogFlat  = 0.3;
constexpr double kFogReach = 0.2;///< h-units
double FogExposure( double fog );

/// The loop is lost while the tail runs through: the claw pulls 1 + slip
/// frames, so the frame line climbs the screen kSlip of a pitch per
/// pull-down once it has built up (kSlipBuild pull-downs). It starts when
/// the fog passes kSlipFog.
constexpr double kSlip      = 0.06;
constexpr int kSlipBuild    = 8;
constexpr double kSlipFog   = 0.5;
/// Framed: the projectionist reframes after this many blank pull-downs.
constexpr int kFramedAfter  = 24;
/// Off the End: this many blank cells, then the tail whips through the
/// gate at kFlick of a pitch per pull-down and the lamp shines on nothing.
constexpr int kTailCells    = 24;
constexpr double kFlick     = 0.35;
/// A new roll's head: blank cells before the fog starts to clear, at least
/// this many and at least a third of the ramp.
constexpr int kHeadCells    = 6;

enum Tail : int
{
	kRolling   = 0,
	kFramed    = 1,
	kOffTheEnd = 2,
};

class Reel
{
public:
	/// Run Out pressed; the next pull-down will be `next`.
	void Press( int64_t next, int ramp );

	/// The fog on film frame n.
	double Fog( int64_t n ) const;
	/// No film in the gate at frame n (Off the End).
	bool NoFilm( int64_t n ) const;

	/// Pull-down k happened (called once for each k, in order): the slip,
	/// the offset, the tail.
	void Pull( int64_t k, int tail );

	/// The strip's offset at pull-down k, frames, in [ -0.5, 0.5 ): positive
	/// moves the picture up the screen. 0 for a k never pulled.
	double Offset( int64_t k ) const;

	/// Running out or out (the next press threads a new roll).
	bool Out() const;

	void Reset();

private:
	struct Transition
	{
		int64_t start;
		int direction;///< +1 running out, -1 threading in
		double from;  ///< the fog when pressed
		int ramp;
		int head;
	};
	std::vector< Transition > transitions;

	struct Pulled
	{
		int64_t k;
		double offset;
	};
	std::deque< Pulled > pulled;

	double slip       = 0.0;///< frames per pull-down, now
	double offset     = 0.0;
	bool locked       = true;///< registered: no slip, offset 0
	int blankRun      = 0;   ///< consecutive blank pull-downs
	int64_t filmEnd   = INT64_MAX;
	int64_t filmBack  = INT64_MAX;///< a new roll threaded from here on
	bool flicking     = false;
};

//---------------------------------------------------------------------------
// The tones. Toning converts the silver, so it scales the silver's density
// per channel (white stays white); a tint is a dye in the base, so it
// multiplies the light per channel (black stays black).
//---------------------------------------------------------------------------

enum Tone : int
{
	kColour  = 0,
	kBlackAndWhite,
	kSepia,
	kAmber,
	kBlue,
	kGreen,
	kToneCount,
};

/// Sepia's silver sulphide: density per channel, relative to neutral silver.
constexpr double kSepiaDensity[ 3 ] = { 0.70, 0.93, 1.35 };
/// The tints' transmittance per channel, linear light.
constexpr double kAmberTint[ 3 ] = { 1.00, 0.70, 0.28 };
constexpr double kBlueTint[ 3 ]  = { 0.35, 0.55, 1.00 };
constexpr double kGreenTint[ 3 ] = { 0.45, 0.85, 0.40 };

/// ( silver density per channel, base transmittance per channel ) for a tone.
void ToneLaw( int tone, int perturb, double density[ 3 ], double tint[ 3 ] );

//---------------------------------------------------------------------------
// Test hooks. Always 0 in the plugin.
//---------------------------------------------------------------------------

enum Perturb : int
{
	kPerturbInstant          = 1 << 0,///< the shutter sampled at one instant per display frame
	kPerturbExposureProjector = 1 << 1,///< the camera's exposure from the projector's speed
	kPerturbNoLoop           = 1 << 2,///< the projector runs without the loop's delay
	kPerturbToneAsTint       = 1 << 3,///< sepia multiplies the light, as a tint does
	kPerturbGrainPerDisplay  = 1 << 4,///< grain reseeded every display frame
	kPerturbPictureSpace     = 1 << 5,///< scratches ride with the picture, not the gate
	kPerturbWhiteWeave       = 1 << 6,///< rho = 0
	kPerturbResizeClears     = 1 << 7,///< a resize clears the ring
	kPerturbOrthoAsPan       = 1 << 8,///< orthochromatic stock sees luminance
	kPerturbFrameLineHalf    = 1 << 9,///< the frame line drawn half as tall
};

enum Probe : int
{
	kProbeNone   = 0,
	kProbeLinear = 1,///< linear light, before the encode and the Mix
};

/// PCG's output permutation: integer, exact on every platform.
uint32_t Hash( uint32_t v );
uint32_t Key( uint32_t seed, uint32_t stream, int64_t index, uint32_t sub = 0 );
double Uniform( uint32_t key );
double Normal( uint32_t seed, uint32_t stream, int64_t index );

} // namespace handleman::model
