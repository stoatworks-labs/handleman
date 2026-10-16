#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`SetParamInfo` clamps a STANDARD default into 0..1 *before* returning, and
	`SetParamRange` can only be called afterwards, so every slider here is a
	plain 0..1 float and the conversions live in this one file, which the
	plugin and the harness both use. Options are mapped by INDEX: an option
	parameter's range reads back 0..1 from the SDK whatever its element count.

	Positions a check needs to land on exactly do: Crank Rate 0.5 is exactly
	16 fps and 1 is 24, Shutter Angle 0.5 is exactly 180 degrees, Framing 0.5
	is exactly centred, Contrast 0.5 is gamma 1.4.
*/
namespace handleman::controls
{

/// Crank Rate: 8 to 24 frames a second (one to three turns of the handle).
double CrankFps( float value );
float CrankParam( double fps );

/// Camera Hand and Projector Hand: the hand's peak speed deviation, as a
/// fraction of the crank rate.
double CameraDeviation( float value );
double ProjectorDeviation( float value );

constexpr int kBladesCount = 3;
const char* BladesName( int index );
int Blades( int index );

/// Shutter Angle: each blade cycle's opening, 45 to 315 degrees. 0.5 is 180.
double ShutterDegrees( float value );
float ShutterParam( double degrees );
double ShutterOpen( float value );

constexpr int kStockCount = 2;
const char* StockName( int index );

constexpr int kToneCount = 6;
const char* ToneName( int index );

/// Contrast: the print's gamma, 0.6 to 2.2. 0.5 is 1.4.
double Gamma( float value );
float GammaParam( double gamma );

/// Framing: -0.5 to +0.5 picture heights. 0.5 is 0.
double FramingHeights( float value );
float FramingParam( double heights );

/// Run Out Time: 0.25 to 4 seconds, geometric. 0.5 is 1 second.
double RunOutSeconds( float value );
float RunOutParam( double seconds );

constexpr int kTailCount = 3;
const char* TailName( int index );

/// Vignette: the lens's half-angle to the corner, 0 to 35 degrees; the
/// light there is cos^4 of it.
double VignetteDegrees( float value );

/// Grain, Weave, Scratches, Dust, Hair and Mix are 0..1 amounts.
float Amount( float value );

int OptionIndex( float value, int count );

} // namespace handleman::controls
