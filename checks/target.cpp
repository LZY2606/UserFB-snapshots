/**
 * @file target.cpp
 *
 * @brief Verification program for the r8b::CDSPResamplerBank planar
 * multi-channel resampler.
 *
 * The program takes one of four group names as its argument:
 *
 * - "equivalence" - numeric equality against independent r8b::CDSPResampler
 *   instances, over all required ratios, phases, channel counts and input
 *   block arrangements.
 * - "schedule" - control-work counters: half-band scheduling and fractional
 *   coefficient evaluation are non-zero on the paths that use them and do not
 *   depend on the channel count.
 * - "boundaries" - fragmented block arrangements, block-split output shift
 *   check and clear()-then-reuse equivalence to a freshly constructed object.
 * - "realtime" - asserts that process() and clear() perform no heap
 *   allocation or deallocation, and checks zero-length and equal-rate
 *   behavior.
 *
 * All input signals are self-generated PCM; no audio files, third-party
 * packages or devices are used.
 *
 * r8brain-free-src Copyright (c) 2013-2026 Aleksey Vaneev
 *
 * See the "LICENSE" file for license.
 */

#include "CDSPResamplerBank.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

using namespace r8b;

namespace {

const double ReqTransBand = 2.0;
const double ReqAtten = 120.0;
const int MaxInLen = 256;

int CheckCount = 0;

void check( const bool cond, const char* const what )
{
	if( !cond )
	{
		std::fprintf( stderr, "CHECK FAILED: %s\n", what );
		std::exit( 1 );
	}

	CheckCount++;
}

/**
 * @brief Generates a distinct, deterministic per-channel signal.
 *
 * Each channel carries two sines of channel-dependent frequency/phase and a
 * channel-specific impulse. When at least two channels are present channel 1
 * is the exact polarity inversion of channel 0, providing a left/right
 * anti-phase pair.
 */

double genSample( const int ch, const long long frame, const int chCount )
{
	if( chCount >= 2 && ch == 1 )
	{
		return( -genSample( 0, frame, chCount ));
	}

	const double f1 = 0.131 + 0.011 * ch;
	const double f2 = 0.047 + 0.007 * ch;
	double v = 0.8 * std::sin( frame * f1 + ch * 1.7 ) +
		0.3 * std::sin( frame * f2 + ch * 0.9 );

	if( frame == ch * 7 + 3 )
	{
		v += ( ch % 2 ? -1.0 : 1.0 );
	}

	return( v );
}

struct RatioDef
{
	double Src;
	double Dst;
	EDSPFilterPhaseResponse Phase;
	const char* Name;
};

const RatioDef EquivRatios[] = {
	{ 44100.0, 48000.0, fprLinearPhase, "44100->48000 linear" },
	{ 48000.0, 16000.0, fprLinearPhase, "48000->16000 linear" },
	{ 32000.0, 96000.0, fprLinearPhase, "32000->96000 linear" },
	{ 44056.3, 47999.7, fprLinearPhase, "44056.3->47999.7 linear" },
	{ 44100.0, 48000.0, fprMinPhase, "44100->48000 min-phase" },
	{ 44056.3, 47999.7, fprMinPhase, "44056.3->47999.7 min-phase" },
	{ 48000.0, 192000.0, fprLinearPhase, "48000->192000 half-band up" },
	{ 192000.0, 48000.0, fprLinearPhase, "192000->48000 half-band down" },
	{ 48000.0, 48000.0, fprLinearPhase, "48000->48000 passthrough" }
};

const int BlockSizes[] = { 1, 7, 31, 256 };
const int ChannelCounts[] = { 1, 2, 6 };

/**
 * @brief Runs one bank and a matching set of mono resamplers over the same
 * block schedule, comparing every output sample and frame count.
 */

void runEquivalence( const RatioDef& rd, const int chCount,
	const int blockSize )
{
	const long long totalFrames = 6000;

	CDSPResamplerBank bank( chCount, rd.Src, rd.Dst, MaxInLen,
		ReqTransBand, ReqAtten, rd.Phase );

	std::vector< CDSPResampler* > mono;
	int c;

	for( c = 0; c < chCount; c++ )
	{
		mono.push_back( new CDSPResampler( rd.Src, rd.Dst, MaxInLen,
			ReqTransBand, ReqAtten, rd.Phase ));
	}

	std::vector< std::vector< double > > bankOut( chCount );
	std::vector< std::vector< double > > monoOut( chCount );

	long long pos = 0;

	while( pos < totalFrames )
	{
		const int n = (int) std::min( (long long) blockSize,
			totalFrames - pos );

		std::vector< std::vector< double > > in( chCount,
			std::vector< double >( n ));
		std::vector< double* > ip( chCount );
		std::vector< double* > op( chCount );

		for( c = 0; c < chCount; c++ )
		{
			for( int i = 0; i < n; i++ )
			{
				in[ c ][ i ] = genSample( c, pos + i, chCount );
			}

			ip[ c ] = &in[ c ][ 0 ];
		}

		// The returned pointers are consumed (copied out) before the next
		// process() call, as required.

		const int bm = bank.process( &ip[ 0 ], n, &op[ 0 ]);

		for( c = 0; c < chCount; c++ )
		{
			double* mp;
			const int mm = mono[ c ] -> process( ip[ c ], n, mp );

			check( mm == bm, "per-call output frame count matches mono" );

			for( int i = 0; i < bm; i++ )
			{
				bankOut[ c ].push_back( op[ c ][ i ]);
				monoOut[ c ].push_back( mp[ i ]);
			}
		}

		pos += n;
	}

	for( c = 0; c < chCount; c++ )
	{
		check( bankOut[ c ].size() == monoOut[ c ].size(),
			"total output frame count matches mono" );
		check( !bankOut[ c ].empty(), "some output was produced" );

		double err = 0.0;

		for( size_t i = 0; i < bankOut[ c ].size(); i++ )
		{
			err = std::fmax( err, std::fabs(
				bankOut[ c ][ i ] - monoOut[ c ][ i ]));
		}

		check( std::isfinite( err ) && err <= 1e-10,
			"per-sample error within 1e-10 of mono" );
	}

	for( c = 0; c < chCount; c++ )
	{
		delete mono[ c ];
	}
}

int groupEquivalence()
{
	for( size_t r = 0; r < sizeof( EquivRatios ) / sizeof( EquivRatios[ 0 ]);
		r++ )
	{
		for( size_t b = 0;
			b < sizeof( BlockSizes ) / sizeof( BlockSizes[ 0 ]); b++ )
		{
			for( size_t k = 0;
				k < sizeof( ChannelCounts ) / sizeof( ChannelCounts[ 0 ]);
				k++ )
			{
				runEquivalence( EquivRatios[ r ], ChannelCounts[ k ],
					BlockSizes[ b ]);
			}
		}
	}

	std::printf( "PASS equivalence checks=%d\n", CheckCount );
	return( 0 );
}

/**
 * @brief Runs a bank for a fixed frame schedule and returns its counters.
 */

CDSPBankCounters runCounters( const double src, const double dst,
	const int chCount, const int blockSize, const long long totalFrames )
{
	CDSPResamplerBank bank( chCount, src, dst, MaxInLen, ReqTransBand,
		ReqAtten );

	long long pos = 0;

	while( pos < totalFrames )
	{
		const int n = (int) std::min( (long long) blockSize,
			totalFrames - pos );

		std::vector< std::vector< double > > in( chCount,
			std::vector< double >( n ));
		std::vector< double* > ip( chCount );
		std::vector< double* > op( chCount );

		for( int c = 0; c < chCount; c++ )
		{
			for( int i = 0; i < n; i++ )
			{
				in[ c ][ i ] = genSample( c, pos + i, chCount );
			}

			ip[ c ] = &in[ c ][ 0 ];
		}

		bank.process( &ip[ 0 ], n, &op[ 0 ]);
		pos += n;
	}

	return( bank.getCounters());
}

int groupSchedule()
{
	// Half-band scheduling must be positive on both half-band directions and
	// independent of the channel count.

	const struct { double Src; double Dst; const char* Name; } hbCases[] = {
		{ 48000.0, 192000.0, "half-band up" },
		{ 192000.0, 48000.0, "half-band down" }
	};

	for( size_t h = 0; h < sizeof( hbCases ) / sizeof( hbCases[ 0 ]); h++ )
	{
		CDSPBankCounters first;

		for( size_t k = 0;
			k < sizeof( ChannelCounts ) / sizeof( ChannelCounts[ 0 ]); k++ )
		{
			const CDSPBankCounters ct = runCounters( hbCases[ h ].Src,
				hbCases[ h ].Dst, ChannelCounts[ k ], 31, 3000 );

			if( k == 0 )
			{
				check( ct.HalfBandSchedules > 0,
					"half-band scheduling actually happened" );
				first = ct;
			}
			else
			{
				check( ct.HalfBandSchedules == first.HalfBandSchedules,
					"half-band schedule count independent of channel count" );
			}
		}
	}

	// Fractional polynomial coefficient evaluation must be non-zero on the
	// true fractional (non-whole-stepping) ratio and channel-invariant.

	CDSPBankCounters polyFirst;

	for( size_t k = 0;
		k < sizeof( ChannelCounts ) / sizeof( ChannelCounts[ 0 ]); k++ )
	{
		const CDSPBankCounters ct = runCounters( 44056.3, 47999.7,
			ChannelCounts[ k ], 31, 3000 );

		if( k == 0 )
		{
			check( ct.FracCoeffEvals > 0,
				"fractional coefficient evaluation actually happened" );
			check( ct.PhaseAdvances > 0,
				"fractional phase advances actually happened" );
			polyFirst = ct;
		}
		else
		{
			check( ct.FracCoeffEvals == polyFirst.FracCoeffEvals,
				"coefficient eval count independent of channel count" );
			check( ct.PhaseAdvances == polyFirst.PhaseAdvances,
				"phase advance count independent of channel count" );
		}
	}

	// On a whole-numbered stepping ratio polynomial evaluation is not used:
	// the zero here is corroborated by the positive count above, not treated
	// as evidence on its own.

	for( size_t k = 0;
		k < sizeof( ChannelCounts ) / sizeof( ChannelCounts[ 0 ]); k++ )
	{
		const CDSPBankCounters ct = runCounters( 44100.0, 48000.0,
			ChannelCounts[ k ], 31, 3000 );

		check( ct.FracCoeffEvals == 0,
			"whole-stepping path performs no polynomial evaluation" );
		check( ct.PhaseAdvances > 0,
			"whole-stepping path still advances the shared phase" );
		check( ct.HalfBandSchedules == 0,
			"non-half-band chain performs no half-band scheduling" );
	}

	std::printf( "PASS schedule checks=%d\n", CheckCount );
	return( 0 );
}

/**
 * @brief Collects a bank's full output for one block arrangement.
 */

std::vector< std::vector< double > > collectBank(
	CDSPResamplerBank& bank, const int chCount, const int blockSize,
	const long long totalFrames )
{
	std::vector< std::vector< double > > out( chCount );
	long long pos = 0;

	while( pos < totalFrames )
	{
		const int n = (int) std::min( (long long) blockSize,
			totalFrames - pos );

		std::vector< std::vector< double > > in( chCount,
			std::vector< double >( n ));
		std::vector< double* > ip( chCount );
		std::vector< double* > op( chCount );

		for( int c = 0; c < chCount; c++ )
		{
			for( int i = 0; i < n; i++ )
			{
				in[ c ][ i ] = genSample( c, pos + i, chCount );
			}

			ip[ c ] = &in[ c ][ 0 ];
		}

		const int m = bank.process( &ip[ 0 ], n, &op[ 0 ]);

		for( int c = 0; c < chCount; c++ )
		{
			for( int i = 0; i < m; i++ )
			{
				out[ c ].push_back( op[ c ][ i ]);
			}
		}

		pos += n;
	}

	return( out );
}

int groupBoundaries()
{
	const RatioDef rd = { 192000.0, 48000.0, fprLinearPhase,
		"half-band down fragments" };
	const int chCount = 6;
	const long long totalFrames = 4096;

	// Every required block arrangement must match a mono instance exactly.

	for( size_t b = 0;
		b < sizeof( BlockSizes ) / sizeof( BlockSizes[ 0 ]); b++ )
	{
		runEquivalence( rd, chCount, BlockSizes[ b ]);
	}

	// Splitting the same input into different block sizes must not shift the
	// output: all arrangements render bit-identical streams.

	std::vector< std::vector< std::vector< double > > > renders;

	for( size_t b = 0;
		b < sizeof( BlockSizes ) / sizeof( BlockSizes[ 0 ]); b++ )
	{
		CDSPResamplerBank bank( chCount, rd.Src, rd.Dst, MaxInLen,
			ReqTransBand, ReqAtten, rd.Phase );

		renders.push_back( collectBank( bank, chCount, BlockSizes[ b ],
			totalFrames ));
	}

	for( size_t b = 1; b < renders.size(); b++ )
	{
		for( int c = 0; c < chCount; c++ )
		{
			check( renders[ b ][ c ].size() == renders[ 0 ][ c ].size(),
				"block split keeps output frame count stable" );

			double err = 0.0;

			for( size_t i = 0; i < renders[ 0 ][ c ].size(); i++ )
			{
				err = std::fmax( err, std::fabs(
					renders[ b ][ c ][ i ] - renders[ 0 ][ c ][ i ]));
			}

			check( err == 0.0, "block split introduces no output shift" );
		}
	}

	// clear() must return the object to its after-construction state: a
	// cleared-and-reused bank equals a brand new object fed the same stream.

	const RatioDef cr = { 44056.3, 47999.7, fprMinPhase, "clear reuse" };

	CDSPResamplerBank reused( chCount, cr.Src, cr.Dst, MaxInLen,
		ReqTransBand, ReqAtten, cr.Phase );
	CDSPResamplerBank fresh( chCount, cr.Src, cr.Dst, MaxInLen,
		ReqTransBand, ReqAtten, cr.Phase );

	collectBank( reused, chCount, 7, totalFrames ); // Fill with history.
	reused.clear();

	const std::vector< std::vector< double > > reusedOut =
		collectBank( reused, chCount, 31, totalFrames );
	const std::vector< std::vector< double > > freshOut =
		collectBank( fresh, chCount, 31, totalFrames );

	for( int c = 0; c < chCount; c++ )
	{
		check( reusedOut[ c ].size() == freshOut[ c ].size(),
			"cleared object output length matches new object" );

		double err = 0.0;

		for( size_t i = 0; i < freshOut[ c ].size(); i++ )
		{
			err = std::fmax( err, std::fabs(
				reusedOut[ c ][ i ] - freshOut[ c ][ i ]));
		}

		check( err == 0.0,
			"cleared-and-reused object equals a new object" );
	}

	std::printf( "PASS boundaries checks=%d\n", CheckCount );
	return( 0 );
}

// ---- Heap allocation monitoring for the "realtime" group -----------------

long long NewCount = 0;
long long DeleteCount = 0;
long long ActiveCount = 0;

} // namespace

void* operator new( size_t size )
{
	void* const p = std::malloc( size ? size : 1 );

	if( p == 0 )
	{
		throw std::bad_alloc();
	}

	NewCount++;
	ActiveCount++;
	return( p );
}

void* operator new[]( size_t size )
{
	void* const p = std::malloc( size ? size : 1 );

	if( p == 0 )
	{
		throw std::bad_alloc();
	}

	NewCount++;
	ActiveCount++;
	return( p );
}

void operator delete( void* p ) noexcept
{
	if( p != 0 )
	{
		std::free( p );
		DeleteCount++;
		ActiveCount--;
	}
}

void operator delete[]( void* p ) noexcept
{
	if( p != 0 )
	{
		std::free( p );
		DeleteCount++;
		ActiveCount--;
	}
}

void operator delete( void* p, size_t ) noexcept
{
	if( p != 0 )
	{
		std::free( p );
		DeleteCount++;
		ActiveCount--;
	}
}

void operator delete[]( void* p, size_t ) noexcept
{
	if( p != 0 )
	{
		std::free( p );
		DeleteCount++;
		ActiveCount--;
	}
}

namespace {

int groupRealtime()
{
	const double rates[][ 2 ] = {
		{ 44100.0, 48000.0 },
		{ 48000.0, 16000.0 },
		{ 32000.0, 96000.0 },
		{ 44056.3, 47999.7 },
		{ 48000.0, 192000.0 },
		{ 192000.0, 48000.0 },
		{ 48000.0, 48000.0 }
	};

	const int chCount = 6;

	for( size_t r = 0; r < sizeof( rates ) / sizeof( rates[ 0 ]); r++ )
	{
		CDSPResamplerBank bank( chCount, rates[ r ][ 0 ], rates[ r ][ 1 ],
			MaxInLen, ReqTransBand, ReqAtten );

		for( size_t b = 0;
			b < sizeof( BlockSizes ) / sizeof( BlockSizes[ 0 ]); b++ )
		{
			const int blockSize = BlockSizes[ b ];

			std::vector< std::vector< double > > in( chCount,
				std::vector< double >( blockSize ));
			std::vector< double* > ip( chCount );
			std::vector< double* > op( chCount );

			for( int c = 0; c < chCount; c++ )
			{
				for( int i = 0; i < blockSize; i++ )
				{
					in[ c ][ i ] = genSample( c, i, chCount );
				}

				ip[ c ] = &in[ c ][ 0 ];
			}

			const long long beforeNew = NewCount;
			const long long beforeDel = DeleteCount;
			const long long beforeActive = ActiveCount;

			const int m = bank.process( &ip[ 0 ], blockSize, &op[ 0 ]);
			bank.clear();

			check( NewCount == beforeNew,
				"process()/clear() allocate no heap memory" );
			check( DeleteCount == beforeDel,
				"process()/clear() free no heap memory" );
			check( ActiveCount == beforeActive,
				"live heap allocation count is unchanged" );
			check( m >= 0, "process returns a non-negative frame count" );
		}

		// Zero-length calls must not advance audio.

		std::vector< double* > ip( chCount );
		std::vector< double* > op( chCount );
		std::vector< std::vector< double > > in( chCount,
			std::vector< double >( MaxInLen ));

		for( int c = 0; c < chCount; c++ )
		{
			ip[ c ] = &in[ c ][ 0 ];
		}

		const long long beforeNew = NewCount;
		const int zm = bank.process( &ip[ 0 ], 0, &op[ 0 ]);

		check( zm == 0, "zero-length call produces no output" );
		check( NewCount == beforeNew,
			"zero-length call allocates no memory" );

		// A zero-length call followed by normal processing must equal a run
		// without it (no state advancement).

		CDSPResamplerBank other( chCount, rates[ r ][ 0 ], rates[ r ][ 1 ],
			MaxInLen, ReqTransBand, ReqAtten );

		const auto a = collectBank( bank, chCount, 31, 1500 );
		const auto z = collectBank( other, chCount, 31, 1500 );

		for( int c = 0; c < chCount; c++ )
		{
			check( a[ c ].size() == z[ c ].size(),
				"zero-length call does not alter frame timing" );

			double err = 0.0;

			for( size_t i = 0; i < a[ c ].size(); i++ )
			{
				err = std::fmax( err, std::fabs( a[ c ][ i ] - z[ c ][ i ]));
			}

			check( err == 0.0,
				"zero-length call does not alter output samples" );
		}
	}

	// Equal-rate passthrough returns the input pointers and frame count.

	{
		CDSPResamplerBank bank( chCount, 48000.0, 48000.0, MaxInLen,
			ReqTransBand, ReqAtten );

		std::vector< std::vector< double > > in( chCount,
			std::vector< double >( 37 ));
		std::vector< double* > ip( chCount );
		std::vector< double* > op( chCount );

		for( int c = 0; c < chCount; c++ )
		{
			in[ c ][ 0 ] = genSample( c, 5, chCount );
			ip[ c ] = &in[ c ][ 0 ];
		}

		const int m = bank.process( &ip[ 0 ], 37, &op[ 0 ]);

		check( m == 37, "equal-rate call returns all frames" );

		for( int c = 0; c < chCount; c++ )
		{
			check( op[ c ] == ip[ c ],
				"equal-rate call returns the input pointer" );
		}
	}

	std::printf( "PASS realtime checks=%d\n", CheckCount );
	return( 0 );
}

} // namespace

int main( int argc, char** argv )
{
	if( argc != 2 )
	{
		std::fprintf( stderr,
			"usage: %s equivalence|schedule|boundaries|realtime\n",
			argv[ 0 ] );
		return( 2 );
	}

	const char* const group = argv[ 1 ];

	if( std::strcmp( group, "equivalence" ) == 0 )
	{
		return( groupEquivalence());
	}

	if( std::strcmp( group, "schedule" ) == 0 )
	{
		return( groupSchedule());
	}

	if( std::strcmp( group, "boundaries" ) == 0 )
	{
		return( groupBoundaries());
	}

	if( std::strcmp( group, "realtime" ) == 0 )
	{
		return( groupRealtime());
	}

	std::fprintf( stderr, "unknown group: %s\n", group );
	return( 2 );
}
