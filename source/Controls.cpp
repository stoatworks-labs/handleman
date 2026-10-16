#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace handleman::controls
{
namespace
{
float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}
} // namespace

double CrankFps( float value )
{
	return 8.0 + 16.0 * static_cast< double >( clamp01( value ) );
}

float CrankParam( double fps )
{
	return clamp01( static_cast< float >( ( fps - 8.0 ) / 16.0 ) );
}

double CameraDeviation( float value )
{
	return model::kCameraMaxDeviation * static_cast< double >( clamp01( value ) );
}

double ProjectorDeviation( float value )
{
	return model::kProjectorMaxDeviation * static_cast< double >( clamp01( value ) );
}

const char* BladesName( int index )
{
	static const char* const names[ kBladesCount ] = { "1", "2", "3" };
	return names[ std::clamp( index, 0, kBladesCount - 1 ) ];
}

int Blades( int index )
{
	return std::clamp( index, 0, kBladesCount - 1 ) + 1;
}

double ShutterDegrees( float value )
{
	return 45.0 + 270.0 * static_cast< double >( clamp01( value ) );
}

float ShutterParam( double degrees )
{
	return clamp01( static_cast< float >( ( degrees - 45.0 ) / 270.0 ) );
}

double ShutterOpen( float value )
{
	return ShutterDegrees( value ) / 360.0;
}

const char* StockName( int index )
{
	static const char* const names[ kStockCount ] = { "Orthochromatic", "Panchromatic" };
	return names[ std::clamp( index, 0, kStockCount - 1 ) ];
}

const char* ToneName( int index )
{
	static const char* const names[ kToneCount ] = { "Colour", "Black & White", "Sepia", "Amber Tint", "Blue Tint", "Green Tint" };
	return names[ std::clamp( index, 0, kToneCount - 1 ) ];
}

double Gamma( float value )
{
	return 0.6 + 1.6 * static_cast< double >( clamp01( value ) );
}

float GammaParam( double gamma )
{
	return clamp01( static_cast< float >( ( gamma - 0.6 ) / 1.6 ) );
}

double FramingHeights( float value )
{
	return static_cast< double >( clamp01( value ) ) - 0.5;
}

float FramingParam( double heights )
{
	return clamp01( static_cast< float >( heights + 0.5 ) );
}

double RunOutSeconds( float value )
{
	return 0.25 * std::pow( 16.0, static_cast< double >( clamp01( value ) ) );
}

float RunOutParam( double seconds )
{
	return clamp01( static_cast< float >( std::log( seconds / 0.25 ) / std::log( 16.0 ) ) );
}

const char* TailName( int index )
{
	static const char* const names[ kTailCount ] = { "Rolling", "Framed", "Off the End" };
	return names[ std::clamp( index, 0, kTailCount - 1 ) ];
}

double VignetteDegrees( float value )
{
	return 35.0 * static_cast< double >( clamp01( value ) );
}

float Amount( float value )
{
	return clamp01( value );
}

int OptionIndex( float value, int count )
{
	//Hosts send an option either as its element value (0, 1, 2 ...) or, some
	//of them, normalised; the element values here ARE the indices.
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace handleman::controls
