#include "Model.h"

#include <algorithm>
#include <cmath>

namespace handleman::model
{
namespace
{
/// One independent hash stream per thing the film draws.
enum Stream : uint32_t
{
	kStreamWeaveX = 1,
	kStreamWeaveY,
	kStreamScratch,
	kStreamDust,
	kStreamHair,
	kStreamHairTremble,
	kStreamCamera,
	kStreamProjector,
};

double lerp( double a, double b, double t )
{
	return a + ( b - a ) * t;
}

double wrapHalf( double x )
{
	return x - std::floor( x + 0.5 );
}

/// kKnee x ln( 1 + e^( u / kKnee ) ), without overflow either way.
double softplus( double u )
{
	const double z = u / kKnee;
	return z > 30.0 ? u : kKnee * std::log1p( std::exp( z ) );
}
} // namespace

//---------------------------------------------------------------------------
uint32_t Hash( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

uint32_t Key( uint32_t seed, uint32_t stream, int64_t index, uint32_t sub )
{
	const uint64_t bits = static_cast< uint64_t >( index );
	uint32_t h          = Hash( seed ^ Hash( stream + 0x9e3779b9u ) );
	h                   = Hash( h ^ static_cast< uint32_t >( bits ) );
	h                   = Hash( h ^ static_cast< uint32_t >( bits >> 32 ) );
	return Hash( h ^ ( sub * 0x85ebca6bu ) );
}

double Uniform( uint32_t key )
{
	return ( static_cast< double >( key ) + 0.5 ) / 4294967296.0;
}

double Normal( uint32_t seed, uint32_t stream, int64_t index )
{
	const double u1 = Uniform( Key( seed, stream, index, 0 ) );
	const double u2 = Uniform( Key( seed, stream, index, 1 ) );
	return std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( 2.0 * kPi * u2 );
}

//---------------------------------------------------------------------------
// The hands.
//---------------------------------------------------------------------------
double Amplitude( const Partial* partials, int count, double deviation )
{
	double sum = 0.0;
	for( int i = 0; i < count; ++i )
		sum += partials[ i ].share / partials[ i ].cyclesPerFrame;
	return std::max( deviation, 0.0 ) * sum / ( 2.0 * kPi );
}

void Hand::Start( uint32_t seed, uint32_t stream, const Partial* list, int n, double deviation )
{
	partials = list;
	count    = std::min( n, kMaxPartials );
	double sum = 0.0;
	for( int i = 0; i < count; ++i )
		sum += partials[ i ].share / partials[ i ].cyclesPerFrame;
	for( int i = 0; i < count; ++i )
	{
		coeff[ i ] = partials[ i ].share / partials[ i ].cyclesPerFrame / sum;
		phase[ i ] = 2.0 * kPi * Uniform( Key( seed, stream, i ) );
	}
	amplitude     = model::Amplitude( partials, count, deviation );
	amplitudeRate = 0.0;
}

void Hand::Advance( double dt, double fps, double deviation )
{
	for( int i = 0; i < count; ++i )
	{
		phase[ i ] += 2.0 * kPi * partials[ i ].cyclesPerFrame * fps * dt;
		phase[ i ] -= 2.0 * kPi * std::floor( phase[ i ] / ( 2.0 * kPi ) );
	}
	const double target = model::Amplitude( partials, count, deviation );
	const double step   = kAmplitudeSlew * fps * dt;
	const double delta  = std::clamp( target - amplitude, -step, step );
	amplitudeRate       = dt > 0.0 ? delta / dt : 0.0;
	amplitude += delta;
}

double Hand::Error() const
{
	double w = 0.0;
	for( int i = 0; i < count; ++i )
		w += coeff[ i ] * std::sin( phase[ i ] );
	return amplitude * w;
}

double Hand::Amplitude() const
{
	return amplitude;
}

double Hand::ErrorRate( double fps ) const
{
	double w = 0.0, dw = 0.0;
	for( int i = 0; i < count; ++i )
	{
		w += coeff[ i ] * std::sin( phase[ i ] );
		dw += coeff[ i ] * std::cos( phase[ i ] ) * 2.0 * kPi * partials[ i ].cyclesPerFrame * fps;
	}
	return amplitudeRate * w + amplitude * dw;
}

//---------------------------------------------------------------------------
// The shutter (gate's).
//---------------------------------------------------------------------------
double ShutterOpen( double p, int blades, double open )
{
	const double c = p * blades;
	const double r = c - std::floor( c );
	return ( r >= 0.5 * ( 1.0 - open ) && r < 0.5 * ( 1.0 + open ) ) ? 1.0 : 0.0;
}

double CumulativeOpen( double p, int blades, double open )
{
	const double c = p * blades;
	const double n = std::floor( c );
	const double r = c - n;
	return ( n * open + std::clamp( r - 0.5 * ( 1.0 - open ), 0.0, open ) ) / blades;
}

int Segments( double p0, double p1, int blades, double open, bool instant, Segment out[ kMaxSegments ] )
{
	if( instant || !( p1 > p0 ) )
	{
		out[ 0 ].frame  = static_cast< int64_t >( std::floor( p1 ) );
		out[ 0 ].weight = ShutterOpen( p1, blades, open ) / open;
		return 1;
	}

	//Relative to the first frame: the shutter repeats every frame, so a
	//whole-frame shift changes nothing but keeps the numbers small.
	const double base  = std::floor( p0 );
	const double q0    = p0 - base, q1 = p1 - base;
	const int last     = static_cast< int >( std::ceil( q1 ) ) - 1;
	const double scale = 1.0 / ( open * ( q1 - q0 ) );
	int count          = 0;
	for( int j = 0; j <= last; ++j )
	{
		const double a  = std::max( q0, static_cast< double >( j ) );
		const double b  = std::min( q1, static_cast< double >( j + 1 ) );
		const double w  = ( CumulativeOpen( b, blades, open ) - CumulativeOpen( a, blades, open ) ) * scale;
		const int64_t k = static_cast< int64_t >( base ) + j;
		if( count < kMaxSegments )
			out[ count++ ] = { k, w };
		else
		{
			//Only after a long host stall at a fast crank: the oldest keeps the
			//light rather than the light being lost.
			for( int i = 0; i + 1 < kMaxSegments; ++i )
				out[ i ] = out[ i + 1 ];
			out[ 0 ].weight += w;
			out[ kMaxSegments - 1 ] = { k, w };
		}
	}
	return count;
}

//---------------------------------------------------------------------------
// Weave.
//---------------------------------------------------------------------------
WeaveLaw Weave( double weave, int perturb )
{
	WeaveLaw law;
	law.sigmaX = kWeaveMm * std::clamp( weave, 0.0, 1.0 ) / kApertureHeightMm;
	law.sigmaY = kWeaveVertical * law.sigmaX;
	law.rho    = ( perturb & kPerturbWhiteWeave ) ? 0.0 : kWeaveRho;
	return law;
}

void WeaveOffset( uint32_t seed, int64_t frame, const WeaveLaw& law, double& x, double& y )
{
	x = y = 0.0;
	if( law.sigmaX <= 0.0 )
		return;
	const int terms = law.rho > 0.0 ? static_cast< int >( std::ceil( std::log( 1e-9 ) / std::log( law.rho ) ) ) : 0;
	double weight   = 1.0;
	for( int j = 0; j <= terms; ++j )
	{
		x += weight * Normal( seed, kStreamWeaveX, frame - j );
		y += weight * Normal( seed, kStreamWeaveY, frame - j );
		weight *= law.rho;
	}
	const double innovation = std::sqrt( 1.0 - law.rho * law.rho );
	x *= innovation * law.sigmaX;
	y *= innovation * law.sigmaY;
}

//---------------------------------------------------------------------------
// Scratches.
//---------------------------------------------------------------------------
std::vector< Scratch > Scratches( uint32_t seed, int64_t first, int64_t last, double amount, double aspect )
{
	std::vector< Scratch > list;
	const double p = std::clamp( amount, 0.0, 1.0 ) * kScratchBirths;
	if( p <= 0.0 )
		return list;
	//Newest first, so the cap drops the oldest grit.
	for( int64_t b = last; b > first - kScratchLifeMax && static_cast< int >( list.size() ) < kMaxScratches; --b )
	{
		if( Uniform( Key( seed, kStreamScratch, b ) ) >= p )
			continue;
		const double life = std::min( static_cast< double >( kScratchLifeMax ),
		                              kScratchLifeMin - kScratchLifeMean * std::log( Uniform( Key( seed, kStreamScratch, b, 1 ) ) ) );
		Scratch s;
		s.born = b;
		s.dies = b + static_cast< int64_t >( life );
		if( s.dies <= first )
			continue;
		const double centre = lerp( 0.03, 0.97, Uniform( Key( seed, kStreamScratch, b, 2 ) ) );
		const double halfMm = lerp( kScratchHalfMmMin, kScratchHalfMmMax, Uniform( Key( seed, kStreamScratch, b, 3 ) ) );
		const double half   = halfMm / kApertureHeightMm / aspect;
		s.left              = centre - half;
		s.right             = centre + half;
		const double u      = Uniform( Key( seed, kStreamScratch, b, 5 ) );
		s.side              = Uniform( Key( seed, kStreamScratch, b, 4 ) ) < 0.55 ? kBase : kEmulsion;
		s.strength          = s.side == kBase ? lerp( 0.25, 0.65, u ) : lerp( 0.15, 1.0, u );
		list.push_back( s );
	}
	return list;
}

//---------------------------------------------------------------------------
// Dust.
//---------------------------------------------------------------------------
std::vector< Particle > Dust( uint32_t seed, int64_t frame, double amount, double aspect )
{
	std::vector< Particle > list;
	const double mean = kDustBirths * std::clamp( amount, 0.0, 1.0 );
	if( mean <= 0.0 )
		return list;
	for( int64_t b = frame - 3; b <= frame; ++b )
	{
		//Poisson by its inverse CDF, one uniform per frame.
		const double u = Uniform( Key( seed, kStreamDust, b ) );
		int n          = 0;
		double term    = std::exp( -mean ), cdf = term;
		while( u > cdf && n < kDustBirthsMax )
		{
			++n;
			term *= mean / n;
			cdf += term;
		}
		for( int j = 0; j < n; ++j )
		{
			auto draw = [ & ]( uint32_t field ) { return Uniform( Key( seed, kStreamDust, b, 1 + static_cast< uint32_t >( j ) * 16u + field ) ); };
			const double l = draw( 0 );
			const int life = l < 0.70 ? 1 : l < 0.85 ? 2 : l < 0.95 ? 3 : 4;
			const int age  = static_cast< int >( frame - b );
			if( age >= life )
				continue;
			Particle p;
			p.x     = draw( 1 ) * aspect + ( draw( 2 ) - 0.5 ) * 0.008 * age;
			p.y     = draw( 3 ) + ( draw( 4 ) - 0.5 ) * 0.008 * age;
			p.angle = draw( 5 ) * kPi;
			p.fibre = draw( 6 ) < 0.25 ? 1 : 0;
			if( p.fibre )
			{
				p.a = lerp( 0.08, 0.45, draw( 7 ) ) / kApertureHeightMm;
				p.b = lerp( 0.008, 0.016, draw( 8 ) ) / kApertureHeightMm;
			}
			else
			{
				const double r = std::min( 0.15, 0.015 * std::pow( draw( 7 ), -1.0 / 2.0 ) ) / kApertureHeightMm;
				p.a            = r;
				p.b            = r * lerp( 0.5, 1.0, draw( 8 ) );
			}
			const double s = draw( 10 );
			p.strength     = draw( 9 ) < 0.85 ? lerp( 0.7, 1.0, s ) : -lerp( 0.6, 1.0, s );
			if( static_cast< int >( list.size() ) < kMaxParticles )
				list.push_back( p );
		}
	}
	return list;
}

//---------------------------------------------------------------------------
// The hair.
//---------------------------------------------------------------------------
Hair HairAt( uint32_t seed, int64_t frame, double amount, double aspect )
{
	Hair hair;
	if( amount <= 0.0 )
		return hair;
	const int64_t slot = frame >= 0 ? frame / kHairSlot : ( frame - kHairSlot + 1 ) / kHairSlot;
	auto draw          = [ & ]( uint32_t field ) { return Uniform( Key( seed, kStreamHair, slot, field ) ); };
	if( draw( 0 ) >= amount )
		return hair;
	const int start = static_cast< int >( draw( 1 ) * 48.0 );
	const int end   = std::min( kHairSlot - 1, start + 36 + static_cast< int >( draw( 2 ) * 57.0 ) );
	const int t     = static_cast< int >( frame - slot * kHairSlot );
	if( t < start || t >= end )
		return hair;
	hair.present = true;

	//Where it hangs: an edge of the aperture, a place along it, a direction
	//in, a length, a curl.
	const double e = draw( 3 ), along = lerp( 0.1, 0.9, draw( 4 ) );
	double ax, ay, dx, dy;
	if( e < 0.40 )
		ax = along * aspect, ay = 1.0, dx = 0.0, dy = -1.0;//bottom
	else if( e < 0.65 )
		ax = along * aspect, ay = 0.0, dx = 0.0, dy = 1.0;//top
	else if( e < 0.85 )
		ax = 0.0, ay = along, dx = 1.0, dy = 0.0;//left
	else
		ax = aspect, ay = along, dx = -1.0, dy = 0.0;//right
	const double tilt = ( draw( 5 ) - 0.5 ) * 1.8;
	const double c = std::cos( tilt ), s = std::sin( tilt );
	const double ux = c * dx - s * dy, uy = s * dx + c * dy;//in
	const double vx = -uy, vy = ux;                          //across
	const double length = lerp( 0.06, 0.18, draw( 6 ) );
	const double curl   = ( draw( 7 ) - 0.5 ) * 0.8;
	const double waves  = lerp( 0.6, 2.2, draw( 8 ) );

	//It shakes loose: in its last four frames it slides back out of the gate.
	const int left      = end - t;
	const double out    = left <= 4 ? length * ( 1.0 - left / 5.0 ) : 0.0;
	const double tremble = 0.0012 * ( left <= 4 ? 3.0 : 1.0 );
	for( int i = 0; i < kHairPoints; ++i )
	{
		const double u    = static_cast< double >( i ) / ( kHairPoints - 1 );
		const double bend = curl * length * std::sin( kPi * u * waves ) * u;
		//The pull-down shakes it every frame, the tip most.
		const double jitter = tremble * u * Normal( seed, kStreamHairTremble, frame * kHairPoints + i / 4 );
		const double along2 = u * length - 0.01 - out;
		hair.x[ i ]         = ax + ux * along2 + vx * ( bend + jitter );
		hair.y[ i ]         = ay + uy * along2 + vy * ( bend + jitter );
	}
	hair.halfWidth = lerp( 0.020, 0.035, draw( 9 ) ) / kApertureHeightMm;
	hair.opacity   = 0.92;
	return hair;
}

//---------------------------------------------------------------------------
// The curve.
//---------------------------------------------------------------------------
double Density( double logExposure, double gamma )
{
	const double grey   = std::log10( kGreyExposure );
	const double line   = kBaseDensity - grey - gamma * ( logExposure - grey );
	const double floor1 = kBaseDensity + softplus( line - kBaseDensity );
	return kMaxDensity - softplus( kMaxDensity - floor1 );
}

double FogExposure( double fog )
{
	const double f = std::clamp( fog, 0.0, 1.0 );
	return ( std::exp2( kFogCurve * f ) - 1.0 ) / ( std::exp2( kFogCurve ) - 1.0 ) * kFogMax;
}

//---------------------------------------------------------------------------
// The reel.
//---------------------------------------------------------------------------
void Reel::Reset()
{
	transitions.clear();
	pulled.clear();
	slip     = 0.0;
	offset   = 0.0;
	locked   = true;
	blankRun = 0;
	filmEnd = filmBack = INT64_MAX;
	flicking = false;
}

bool Reel::Out() const
{
	return !transitions.empty() && transitions.back().direction > 0;
}

void Reel::Press( int64_t next, int ramp )
{
	ramp = std::max( ramp, 1 );
	const double from = Fog( next - 1 );
	Transition t;
	t.start     = next;
	t.direction = Out() ? -1 : +1;
	t.from      = from;
	t.ramp      = ramp;
	t.head      = ( t.direction < 0 && from >= 1.0 ) ? std::max( kHeadCells, ramp / 3 ) : 0;
	if( t.direction > 0 )
	{
		//A new run-out: whatever the last roll's tail did is history.
		filmEnd = filmBack = INT64_MAX;
		flicking = false;
	}
	else if( filmEnd != INT64_MAX && next >= filmEnd )
		filmBack = next;//a new roll, threaded into an empty gate
	transitions.push_back( t );
	if( transitions.size() > 8 )
		transitions.erase( transitions.begin() );
}

double Reel::Fog( int64_t n ) const
{
	for( auto it = transitions.rbegin(); it != transitions.rend(); ++it )
	{
		if( it->start > n )
			continue;
		const double d = static_cast< double >( n - it->start );
		if( it->direction > 0 )
			return std::min( 1.0, it->from + ( d + 1.0 ) / it->ramp );
		if( d < it->head )
			return it->from;
		return std::max( 0.0, it->from - ( d - it->head + 1.0 ) / it->ramp );
	}
	return 0.0;
}

bool Reel::NoFilm( int64_t n ) const
{
	return n >= filmEnd && n < filmBack;
}

void Reel::Pull( int64_t k, int tail )
{
	const double fog = Fog( k );
	const bool blank = fog >= 1.0;
	blankRun         = blank ? blankRun + 1 : 0;

	enum
	{
		kHold,
		kRoll,
		kWhip,
	} want = kHold;

	const Transition* t = nullptr;
	for( auto it = transitions.rbegin(); it != transitions.rend(); ++it )
		if( it->start <= k )
		{
			t = &*it;
			break;
		}

	if( t != nullptr && t->direction > 0 )
	{
		if( fog >= kSlipFog )
			want = kRoll;
		if( blank && tail == kFramed && blankRun >= kFramedAfter )
			want = kHold;
		if( blank && tail == kOffTheEnd && blankRun >= kTailCells && filmEnd == INT64_MAX )
		{
			filmEnd  = k + 2;
			flicking = true;
		}
		if( flicking )
			want = kWhip;
	}
	else if( t != nullptr && k == filmBack )
	{
		//The new roll's leading edge comes up into the gate from below.
		offset   = -0.45;
		slip     = kSlip;
		locked   = false;
		flicking = false;
	}

	if( want == kHold )
	{
		if( !locked )
		{
			//The projectionist frames up: the strip keeps creeping the way it
			//was going until the frame line has gone, then the claw seats.
			const double s    = std::max( slip, 0.5 * kSlip );
			const double next = offset + s;
			if( offset < 0.0 && next >= 0.0 )
			{
				offset = 0.0;
				slip   = 0.0;
				locked = true;
			}
			else
			{
				offset = wrapHalf( next );
				slip   = s;
			}
		}
	}
	else
	{
		const double target = want == kRoll ? kSlip : kFlick;
		const double step   = ( want == kRoll ? kSlip : kFlick ) / kSlipBuild;
		slip += std::clamp( target - slip, -step, step );
		locked = false;
		offset = wrapHalf( offset + slip );
	}

	pulled.push_back( { k, offset } );
	while( pulled.size() > 64 )
		pulled.pop_front();
}

double Reel::Offset( int64_t k ) const
{
	//The latest pull-down at or before k: a frame never pulled (the one below
	//the gate) sits where the strip is now.
	for( auto it = pulled.rbegin(); it != pulled.rend(); ++it )
		if( it->k <= k )
			return it->offset;
	return pulled.empty() ? 0.0 : pulled.front().offset;
}

//---------------------------------------------------------------------------
// The tones.
//---------------------------------------------------------------------------
void ToneLaw( int tone, int perturb, double density[ 3 ], double tint[ 3 ] )
{
	for( int i = 0; i < 3; ++i )
		density[ i ] = tint[ i ] = 1.0;
	switch( tone )
	{
	case kSepia:
		if( perturb & kPerturbToneAsTint )
		{
			//The negative control: sepia as a coloured base, the colour of a
			//toned mid grey, brightest channel 1.
			double lo = kSepiaDensity[ 0 ];
			for( double k : kSepiaDensity )
				lo = std::min( lo, k );
			for( int i = 0; i < 3; ++i )
				tint[ i ] = std::pow( 10.0, -0.7447 * ( kSepiaDensity[ i ] - lo ) );
		}
		else
			for( int i = 0; i < 3; ++i )
				density[ i ] = kSepiaDensity[ i ];
		break;
	case kAmber:
		for( int i = 0; i < 3; ++i )
			tint[ i ] = kAmberTint[ i ];
		break;
	case kBlue:
		for( int i = 0; i < 3; ++i )
			tint[ i ] = kBlueTint[ i ];
		break;
	case kGreen:
		for( int i = 0; i < 3; ++i )
			tint[ i ] = kGreenTint[ i ];
		break;
	default:
		break;
	}
}

} // namespace handleman::model
