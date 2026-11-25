// Regression tests for r8b::CDSPResamplerAdjoint.
// These tests construct and drive the real adjoint API; the forward rational
// reference is used only as the independent numerical oracle.
#include "CDSPResamplerAdjoint.h"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <random>
#include <vector>

#define private public
#define protected public
#include "task_support/rational_reference.h"
#undef private
#undef protected

using r8b::AdjointResult;
using r8b::CDSPResamplerAdjoint;

namespace {

bool g_track_alloc = false;
unsigned long long g_alloc_count = 0;
unsigned long long g_alloc_bytes = 0;

void count_alloc( std::size_t n )
{
	if( g_track_alloc )
	{
		++g_alloc_count;
		g_alloc_bytes += (unsigned long long) n;
	}
}

} // namespace

void* operator new( std::size_t n )
{
	count_alloc( n );
	void* p = std::malloc( n ? n : 1 );

	if( p == nullptr )
	{
		throw std::bad_alloc();
	}

	return( p );
}

void* operator new[]( std::size_t n )
{
	count_alloc( n );
	void* p = std::malloc( n ? n : 1 );

	if( p == nullptr )
	{
		throw std::bad_alloc();
	}

	return( p );
}

void* operator new( std::size_t n, std::align_val_t a )
{
	count_alloc( n );
	void* p = nullptr;

	if( posix_memalign( &p, (std::size_t) a, n ? n : 1 ) != 0 )
	{
		throw std::bad_alloc();
	}

	return( p );
}

void* operator new[]( std::size_t n, std::align_val_t a )
{
	count_alloc( n );
	void* p = nullptr;

	if( posix_memalign( &p, (std::size_t) a, n ? n : 1 ) != 0 )
	{
		throw std::bad_alloc();
	}

	return( p );
}

void operator delete( void* p ) noexcept
{
	std::free( p );
}

void operator delete[]( void* p ) noexcept
{
	std::free( p );
}

void operator delete( void* p, std::size_t ) noexcept
{
	std::free( p );
}

void operator delete[]( void* p, std::size_t ) noexcept
{
	std::free( p );
}

void operator delete( void* p, std::align_val_t ) noexcept
{
	std::free( p );
}

void operator delete[]( void* p, std::size_t, std::align_val_t ) noexcept
{
	std::free( p );
}

namespace {

using u64 = std::uint64_t;

u64 M_of( u64 N, unsigned S, unsigned D )
{
	return( task_reference::output_frames( N, S, D ) );
}

std::vector< double > forward_reference( unsigned S, unsigned D,
	const std::vector< double >& x )
{
	r8b::TaskRationalResampler rr( S, D, 127, 2.0, 120.0,
		r8b::fprLinearPhase );
	const u64 M = M_of( x.size(), S, D );
	std::vector< double > y;
	std::size_t p = 0;
	int zero_calls = 0;

	while( (u64) y.size() < M )
	{
		double block[ 127 ] = {};
		int n = 127;

		if( p < x.size() )
		{
			n = (int) std::min<std::size_t>( 127, x.size() - p );
			std::copy_n( x.data() + p, n, block );
			p += (std::size_t) n;
		}
		else
		{
			++zero_calls;
		}

		double* op = nullptr;
		const int c = rr.process( block, n, op );
		const u64 need = M - (u64) y.size();
		const std::size_t take = (std::size_t) std::min<u64>( c, need );

		if( take )
		{
			y.insert( y.end(), op, op + take );
		}

		if( zero_calls > 100000 )
		{
			break;
		}
	}

	return( y );
}

std::vector< double > make_signal( int n, unsigned seed, bool residual )
{
	std::mt19937_64 rng( 0x9e3779b97f4a7c15ULL + seed * 1000003ULL );
	std::uniform_real_distribution< double > rnd( -1.0, 1.0 );
	std::vector< double > x( n );

	for( int i = 0; i < n; ++i )
	{
		const double wave = residual ?
			0.40 * std::sin( i * 0.017 + seed * 0.31 ) +
			0.30 * std::cos( i * 0.061 + seed * 0.77 ) :
			0.35 * std::sin( i * 0.013 + seed * 0.23 ) +
			0.25 * std::cos( i * 0.071 + seed * 0.61 );

		x[ i ] = wave + ( residual ? 0.20 : 0.15 ) * rnd( rng );
	}

	if( n > 0 && !residual )
	{
		x.front() += 0.45;
		x.back() -= 0.55;
	}

	return( x );
}

struct RunOptions
{
	int max_block = 127;
	std::vector< int > frame_plan{ 1, 7, 127 };
	std::vector< int > cap_plan{ 127, 3, 1, 13, 0, 7 };
	bool occasional_zero_frames = true;
	bool occasional_zero_capacity = true;
};

bool same_bits( const std::vector< std::vector< double > >& a,
	const std::vector< std::vector< double > >& b )
{
	if( a.size() != b.size() )
	{
		return( false );
	}

	for( std::size_t c = 0; c < a.size(); ++c )
	{
		if( a[ c ].size() != b[ c ].size() )
		{
			return( false );
		}

		for( std::size_t i = 0; i < a[ c ].size(); ++i )
		{
			u64 x, y;
			std::memcpy( &x, &a[ c ][ i ], 8 );
			std::memcpy( &y, &b[ c ][ i ], 8 );

			if( x != y )
			{
				return( false );
			}
		}
	}

	return( true );
}

bool run_full( unsigned S, unsigned D, u64 N, int channels,
	const std::vector< std::vector< double > >& g,
	const RunOptions& opt,
	std::vector< std::vector< double > >& result,
	CDSPResamplerAdjoint* reuse = nullptr )
{
	const u64 M = M_of( N, S, D );
	CDSPResamplerAdjoint local( S, D, N, channels, opt.max_block );
	CDSPResamplerAdjoint& adj = reuse ? *reuse : local;

	if( reuse )
	{
		adj.clear();
	}

	result.assign( (std::size_t) channels,
		std::vector< double >( (std::size_t) N, 0.0 ) );

	std::vector< std::vector< double > > outbuf( (std::size_t) channels,
		std::vector< double >( (std::size_t) opt.max_block, 0.0 ) );
	std::vector< const double* > rp( (std::size_t) channels, nullptr );
	std::vector< double* > sp( (std::size_t) channels, nullptr );

	for( int c = 0; c < channels; ++c )
	{
		sp[ c ] = outbuf[ (std::size_t) c ].data();
	}

	u64 q = adj.getNextResidualFrame();
	u64 s = adj.getNextSourceFrame();

	if( q != M || s != N || adj.getResidualFrames() != M )
	{
		return( false );
	}

	int calls = 0;
	u64 produced_total = 0;

	while( q != 0 || s != 0 )
	{
		int frames = 0;

		if( q > 0 )
		{
			frames = opt.frame_plan[ (std::size_t) calls %
				opt.frame_plan.size() ];

			if( opt.occasional_zero_frames && calls % 11 == 10 )
			{
				frames = 0;
			}

			frames = std::max( 0, std::min( frames,
				(int) std::min<u64>( opt.max_block, q ) ) );
		}

		int capacity = opt.cap_plan[ (std::size_t) calls %
			opt.cap_plan.size() ];
		capacity = std::max( 0, std::min( capacity, opt.max_block ) );

		if( opt.occasional_zero_capacity && calls % 13 == 12 )
		{
			capacity = 0;
		}

		const u64 old_q = q;
		const u64 old_s = s;

		if( frames > 0 )
		{
			for( int c = 0; c < channels; ++c )
			{
				rp[ c ] = g[ c ].data() + (std::size_t)( q - frames );
			}
		}

		AdjointResult r = adj.process(
			frames > 0 ? rp.data() : nullptr, frames,
			capacity > 0 ? sp.data() : nullptr, capacity );

		if( r.consumed < 0 || r.produced < 0 ||
			r.consumed > frames || r.produced > capacity ||
			(u64) r.first > old_s ||
			(u64) r.first + (u64) r.produced != old_s )
		{
			return( false );
		}

		const u64 nq = adj.getNextResidualFrame();
		const u64 ns = adj.getNextSourceFrame();

		if( nq != old_q - (u64) r.consumed || ns != (u64) r.first )
		{
			return( false );
		}

		if( capacity == 0 && ( r.consumed != 0 || r.produced != 0 ) )
		{
			return( false );
		}

		if( frames > 0 && capacity > 0 && r.consumed == 0 &&
			r.produced == 0 )
		{
			return( false );
		}

		if( r.produced > 0 )
		{
			for( int c = 0; c < channels; ++c )
			{
				std::copy_n( sp[ c ], r.produced,
					result[ c ].begin() + (std::size_t) r.first );
			}

			produced_total += (u64) r.produced;
		}

		q = nq;
		s = ns;

		const bool expected_done = ( q == 0 && s == 0 );

		if( r.done != expected_done )
		{
			return( false );
		}

		++calls;

		if( calls > 200000 )
		{
			return( false );
		}
	}

	if( produced_total != N )
	{
		return( false );
	}

	AdjointResult last = adj.process( nullptr, 0, sp.data(),
		opt.max_block );

	if( !last.done || last.consumed != 0 || last.produced != 0 ||
		last.first != 0 || adj.getNextResidualFrame() != 0 ||
		adj.getNextSourceFrame() != 0 )
	{
		return( false );
	}

	return( true );
}

bool check_duality( unsigned S, unsigned D, int N, int channels,
	unsigned seed )
{
	const u64 M = M_of( (u64) N, S, D );
	std::vector< std::vector< double > > x( channels );
	std::vector< std::vector< double > > g( channels );
	std::vector< std::vector< double > > y( channels );

	for( int c = 0; c < channels; ++c )
	{
		x[ c ] = make_signal( N, seed + 17U * (unsigned) c, false );
		g[ c ] = make_signal( (int) M, seed + 101U + 19U * (unsigned) c,
			true );
		y[ c ] = forward_reference( S, D, x[ c ] );

		if( y[ c ].size() != M )
		{
			return( false );
		}
	}

	RunOptions opt;
	opt.frame_plan = { 1, 7, 127, 7 };
	opt.cap_plan = { 5, 1, 17, 3, 127, 2 };
	std::vector< std::vector< double > > z;

	if( !run_full( S, D, (u64) N, channels, g, opt, z ) )
	{
		std::fprintf( stderr, "run_full failed %u->%u N=%d ch=%d M=%llu\n",
			S, D, N, channels, (unsigned long long) M );
		return( false );
	}

	for( int c = 0; c < channels; ++c )
	{
		double fg = 0.0;
		double xz = 0.0;
		double ny2 = 0.0;
		double ng2 = 0.0;

		for( u64 j = 0; j < M; ++j )
		{
			fg += y[ c ][ (std::size_t) j ] *
				g[ c ][ (std::size_t) j ];
			ny2 += y[ c ][ (std::size_t) j ] *
				y[ c ][ (std::size_t) j ];
			ng2 += g[ c ][ (std::size_t) j ] *
				g[ c ][ (std::size_t) j ];
		}

		for( int i = 0; i < N; ++i )
		{
			xz += x[ c ][ i ] * z[ c ][ i ];

			if( !std::isfinite( z[ c ][ i ] ) )
			{
				return( false );
			}
		}

		const double tol = 1e-9 * std::max( 1.0,
			std::sqrt( ny2 ) * std::sqrt( ng2 ) );

		if( std::fabs( fg - xz ) > tol )
		{
			std::fprintf( stderr,
				"duality mismatch %u->%u N=%d ch=%d c=%d err=%.12g tol=%.12g\n",
				S, D, N, channels, c, std::fabs( fg - xz ), tol );
			return( false );
		}
	}

	return( true );
}

} // namespace

static int Duality_FiveRatesMono()
{
	const unsigned rates[][ 2 ] = {
		{ 44100, 48000 }, { 48000, 192000 }, { 192000, 48000 },
		{ 47999, 48001 }, { 48000, 48000 }
	};
	const int Ns[] = { 1, 2, 3, 13, 127, 311 };

	for( const auto& r : rates )
	{
		for( int N : Ns )
		{
			TASK_CHECK( check_duality( r[ 0 ], r[ 1 ], N, 1,
				700U + (unsigned) N ) );
		}
	}

	return( 0 );
}

static int Duality_TwoChannels()
{
	const unsigned rates[][ 2 ] = {
		{ 44100, 48000 }, { 48000, 192000 }, { 192000, 48000 },
		{ 47999, 48001 }, { 48000, 48000 }
	};
	const int Ns[] = { 2, 17, 129 };

	for( const auto& r : rates )
	{
		for( int N : Ns )
		{
			TASK_CHECK( check_duality( r[ 0 ], r[ 1 ], N, 2,
				900U + (unsigned) N ) );
		}
	}

	return( 0 );
}

static int Duality_SixChannels()
{
	const unsigned rates[][ 2 ] = {
		{ 44100, 48000 }, { 48000, 192000 }, { 192000, 48000 },
		{ 47999, 48001 }, { 48000, 48000 }
	};
	const int Ns[] = { 1, 5, 33, 129 };

	for( const auto& r : rates )
	{
		for( int N : Ns )
		{
			TASK_CHECK( check_duality( r[ 0 ], r[ 1 ], N, 6,
				1100U + (unsigned) N ) );
		}
	}

	return( 0 );
}

static int Duality_ChannelSeparation()
{
	const unsigned S = 47999, D = 48001;
	const int N = 73, ch = 6, target = 4;
	const u64 M = M_of( N, S, D );
	std::vector< std::vector< double > > g( ch );

	for( int c = 0; c < ch; ++c )
	{
		g[ c ] = make_signal( (int) M, 300U + (unsigned) c, true );
	}

	RunOptions opt;
	opt.frame_plan = { 1, 7, 127 };
	opt.cap_plan = { 3, 17, 1 };
	std::vector< std::vector< double > > zmulti;
	TASK_CHECK( run_full( S, D, N, ch, g, opt, zmulti ) );

	std::vector< std::vector< double > > gm{ g[ target ] };
	std::vector< std::vector< double > > zmono;
	TASK_CHECK( run_full( S, D, N, 1, gm, opt, zmono ) );
	TASK_CHECK( same_bits( std::vector< std::vector< double > >{
		zmulti[ target ] }, zmono ) );

	// A residual only in target must leave every other channel at exact zero.
	std::vector< std::vector< double > > zero_g( ch,
		std::vector< double >( (std::size_t) M, 0.0 ) );
	zero_g[ target ] = g[ target ];
	std::vector< std::vector< double > > zsep;
	TASK_CHECK( run_full( S, D, N, ch, zero_g, opt, zsep ) );

	for( int c = 0; c < ch; ++c )
	{
		if( c == target )
		{
			u64 diff = 0;

			for( int i = 0; i < N; ++i )
			{
				u64 a, b;
				std::memcpy( &a, &zsep[ c ][ i ], 8 );
				std::memcpy( &b, &zmono[ 0 ][ i ], 8 );
				diff += ( a != b );
			}

			TASK_CHECK( diff == 0 );
		}
		else
		{
			for( int i = 0; i < N; ++i )
			{
				TASK_CHECK( zsep[ c ][ i ] == 0.0 );
			}
		}
	}

	return( 0 );
}

static int ReverseStream_BitExactSchedules()
{
	const unsigned rates[][ 2 ] = {
		{ 44100, 48000 }, { 48000, 192000 }, { 192000, 48000 },
		{ 47999, 48001 }, { 48000, 48000 }
	};

	for( const auto& r : rates )
	{
		const int N = 211;
		const u64 M = M_of( N, r[ 0 ], r[ 1 ] );
		std::vector< std::vector< double > > g{
			make_signal( (int) M, 401U + r[ 0 ], true ) };

		RunOptions ref;
		ref.frame_plan = { 127 };
		ref.cap_plan = { 127 };
		ref.occasional_zero_frames = false;
		ref.occasional_zero_capacity = false;
		std::vector< std::vector< double > > zref;
		TASK_CHECK( run_full( r[ 0 ], r[ 1 ], N, 1, g, ref, zref ) );

		RunOptions one;
		one.frame_plan = { 1 };
		one.cap_plan = { 1, 1, 2, 1, 3 };
		std::vector< std::vector< double > > z1;
		TASK_CHECK( run_full( r[ 0 ], r[ 1 ], N, 1, g, one, z1 ) );
		TASK_CHECK( same_bits( zref, z1 ) );

		RunOptions seven;
		seven.frame_plan = { 7 };
		seven.cap_plan = { 1, 7, 2, 4, 7, 0, 13 };
		std::vector< std::vector< double > > z7;
		TASK_CHECK( run_full( r[ 0 ], r[ 1 ], N, 1, g, seven, z7 ) );
		TASK_CHECK( same_bits( zref, z7 ) );

		RunOptions mixed;
		mixed.frame_plan = { 1, 7, 127, 0, 7, 1 };
		mixed.cap_plan = { 127, 1, 0, 3, 1, 1, 127, 5, 0, 7 };
		std::vector< std::vector< double > > zm;
		TASK_CHECK( run_full( r[ 0 ], r[ 1 ], N, 1, g, mixed, zm ) );
		TASK_CHECK( same_bits( zref, zm ) );
	}

	return( 0 );
}

static int ReverseStream_ClearReuseAndIntervals()
{
	const unsigned S = 44100, D = 48000;
	const int N = 137, max_block = 127, ch = 2;
	const u64 M = M_of( N, S, D );
	std::vector< std::vector< double > > g( ch );

	for( int c = 0; c < ch; ++c )
	{
		g[ c ] = make_signal( (int) M, 600U + (unsigned) c, true );
	}

	CDSPResamplerAdjoint adj( S, D, N, ch, max_block );
	TASK_CHECK( adj.getResidualFrames() == M );
	TASK_CHECK( adj.getNextResidualFrame() == M );
	TASK_CHECK( adj.getNextSourceFrame() == (u64) N );

	// Drive a partial, back-pressured interval and then clear it.
	{
		const int frames = 31;
		const int capacity = 2;
		std::vector< std::vector< double > > ob( ch,
			std::vector< double >( max_block ) );
		std::vector< const double* > rp( ch );
		std::vector< double* > sp( ch );

		for( int c = 0; c < ch; ++c )
		{
			rp[ c ] = g[ c ].data() + M - frames;
			sp[ c ] = ob[ c ].data();
		}

		AdjointResult a = adj.process( rp.data(), frames, sp.data(),
			capacity );
		TASK_CHECK( a.consumed == 0 || a.consumed == frames );
		TASK_CHECK( a.produced <= capacity );
		TASK_CHECK( adj.getNextResidualFrame() == M - a.consumed );
		TASK_CHECK( (u64) a.first + a.produced == N );
	}

	adj.clear();
	TASK_CHECK( adj.getNextResidualFrame() == M );
	TASK_CHECK( adj.getNextSourceFrame() == (u64) N );

	RunOptions opt;
	opt.frame_plan = { 1, 7, 127 };
	opt.cap_plan = { 3, 1, 17, 0, 127 };
	std::vector< std::vector< double > > z;
	TASK_CHECK( run_full( S, D, N, ch, g, opt, z, &adj ) );

	RunOptions ref = opt;
	std::vector< std::vector< double > > zfresh;
	TASK_CHECK( run_full( S, D, N, ch, g, ref, zfresh ) );
	TASK_CHECK( same_bits( z, zfresh ) );

	return( 0 );
}

static int Bounded_EmptySameRateAndValidation()
{
	// Empty fragments are immediately complete and accept null pointers.
	{
		CDSPResamplerAdjoint a( 44100, 48000, 0, 1, 127 );
		TASK_CHECK( a.getResidualFrames() == 0 );
		TASK_CHECK( a.getNextResidualFrame() == 0 );
		TASK_CHECK( a.getNextSourceFrame() == 0 );
		AdjointResult r = a.process( nullptr, 0, nullptr, 0 );
		TASK_CHECK( r.done && r.consumed == 0 && r.produced == 0 &&
			r.first == 0 );
	}

	// Same rate is identity; the reverse assembly must recover every bit.
	{
		const int N = 151;
		std::vector< std::vector< double > > g{
			make_signal( N, 812U, true ) };
		RunOptions opt;
		opt.frame_plan = { 1, 7, 127 };
		opt.cap_plan = { 1, 127, 3, 0, 7 };
		std::vector< std::vector< double > > z;
		TASK_CHECK( run_full( 48000, 48000, N, 1, g, opt, z ) );
		TASK_CHECK( same_bits( g, z ) );
	}

	// Constructor validation.
	const std::vector< std::function< void() > > bad = {
		[] { CDSPResamplerAdjoint( 7999, 48000, 10, 1, 8 ); },
		[] { CDSPResamplerAdjoint( 48000, 384001, 10, 1, 8 ); },
		[] { CDSPResamplerAdjoint( 48000, 48000, ( 1ULL << 40 ) + 1, 1, 8 ); },
		[] { CDSPResamplerAdjoint( 48000, 48000, 10, 3, 8 ); },
		[] { CDSPResamplerAdjoint( 48000, 48000, 10, 1, 0 ); },
		[] { CDSPResamplerAdjoint( 48000, 48000, 10, 1, 8, 0.4 ); },
		[] { CDSPResamplerAdjoint( 48000, 48000, 10, 1, 8, 2.0, 48.0 ); }
	};

	for( const auto& fn : bad )
	{
		bool threw = false;

		try
		{
			fn();
		}
		catch( const std::invalid_argument& )
		{
			threw = true;
		}

		TASK_CHECK( threw );
	}

	// Illegal process counts do not move either cursor.
	CDSPResamplerAdjoint a( 44100, 48000, 100, 1, 32 );
	const u64 M = a.getResidualFrames();
	TASK_CHECK( M > 100 );
	const double* rp[ 1 ] = { nullptr };
	double* sp[ 1 ] = { nullptr };
	double residual[ 64 ] = {};
	double source[ 64 ] = {};
	rp[ 0 ] = residual;
	sp[ 0 ] = source;

	const int bad_frames[] = { -1, 33, 200 };
	const int bad_caps[] = { -1, 33 };

	for( int f : bad_frames )
	{
		bool threw = false;

		try
		{
			a.process( rp, f, sp, 1 );
		}
		catch( const std::invalid_argument& )
		{
			threw = true;
		}

		TASK_CHECK( threw );
		TASK_CHECK( a.getNextResidualFrame() == M );
		TASK_CHECK( a.getNextSourceFrame() == 100 );
	}

	for( int c : bad_caps )
	{
		bool threw = false;

		try
		{
			a.process( rp, 1, sp, c );
		}
		catch( const std::invalid_argument& )
		{
			threw = true;
		}

		TASK_CHECK( threw );
		TASK_CHECK( a.getNextResidualFrame() == M );
		TASK_CHECK( a.getNextSourceFrame() == 100 );
	}

	// Null pointers for positive counts are also invalid and non-mutating.
	{
		bool threw = false;

		try
		{
			a.process( nullptr, 1, sp, 1 );
		}
		catch( const std::invalid_argument& )
		{
			threw = true;
		}

		TASK_CHECK( threw );
		TASK_CHECK( a.getNextResidualFrame() == M );
	}

	return( 0 );
}

namespace {

std::vector< double > local_block_kernel()
{
	double pwr, hl, fo1;
	r8b::CDSPFIRFilter::calcDesignParams( 0.02, 120.0, pwr, hl, fo1 );
	double win[ 2 ] = { 125.0, pwr };
	r8b::CDSPSincFilterGen sinc;
	sinc.Len2 = 0.25 * hl / 0.5;
	sinc.Freq1 = 0.0;
	sinc.Freq2 = r8b::R8B_PI * 0.5 * ( 1.0 - fo1 );
	sinc.initBand( r8b::CDSPSincFilterGen::wftKaiser, win, true );
	std::vector< double > h( (std::size_t) sinc.KernelLen );
	sinc.generateBand( h.data(),
		&r8b::CDSPSincFilterGen::calcWindowKaiser );
	double sum = 0.0;

	for( double v : h )
	{
		sum += v;
	}

	for( double& v : h )
	{
		v *= 2.0 / sum;
	}

	return( h );
}

unsigned long long construction_bytes( u64 N )
{
	g_alloc_count = 0;
	g_alloc_bytes = 0;
	g_track_alloc = true;
	CDSPResamplerAdjoint a( 47999, 48001, N, 6, 127 );
	g_track_alloc = false;
	return( g_alloc_bytes );
}

} // namespace

static int Bounded_LongCursorLocalWindowAndAllocation()
{
	// Warm coefficient/FFT caches before measuring constructor workspace.
	{
		CDSPResamplerAdjoint warm( 47999, 48001, 1024, 6, 127 );
	}

	const unsigned long long small_bytes = construction_bytes( 1024 );
	const unsigned long long long_bytes = construction_bytes(
		( 1ULL << 33 ) + 123 );
	TASK_CHECK( small_bytes > 0 );
	TASK_CHECK( small_bytes == long_bytes );

	const unsigned S = 47999, D = 48001;
	const u64 N = ( 1ULL << 33 ) + 77;
	const u64 M = M_of( N, S, D );
	TASK_CHECK( N > ( 1ULL << 32 ) );
	TASK_CHECK( M > ( 1ULL << 32 ) );

	CDSPResamplerAdjoint a( S, D, N, 1, 127 );
	TASK_CHECK( a.getNextResidualFrame() == M );
	TASK_CHECK( a.getNextSourceFrame() == N );

	const int K = 2000;
	const u64 Q0 = M - (u64) K;
	std::vector< double > g( K );

	for( int t = 0; t < K; ++t )
	{
		const u64 j = Q0 + (u64) t;
		g[ t ] = 0.25 * std::sin( (double) j * 0.0000173 ) +
			0.10 * std::cos( (double) ( j % 1000003ULL ) * 0.001 );
	}

	// All process/clear heap traffic must be zero after construction.
	double residual_block[ 127 ];
	double source_block[ 127 ];
	const double* rp[ 1 ] = { residual_block };
	double* sp[ 1 ] = { source_block };
	std::vector< std::pair< u64, double > > delivered;
	delivered.reserve( 8192 );
	g_alloc_count = 0;
	g_track_alloc = true;

	u64 q = M;
	int processed = 0;

	while( processed < K )
	{
		const int take = std::min( 127, K - processed );
		const u64 abs_start = q - (u64) take;

		for( int k = 0; k < take; ++k )
		{
			residual_block[ k ] = g[ (std::size_t)( abs_start + (u64) k - Q0 ) ];
		}

		AdjointResult r = a.process( rp, take, sp, 127 );
		TASK_CHECK( r.consumed == take );
		TASK_CHECK( r.produced >= 0 && r.produced <= 127 );

		for( int k = 0; k < r.produced; ++k )
		{
			delivered.emplace_back( (u64) r.first + (u64) k,
				source_block[ k ] );
		}

		q -= (u64) r.consumed;
		processed += r.consumed;
	}

	TASK_CHECK( !delivered.empty() );
	const u64 source_before_clear = a.getNextSourceFrame();
	TASK_CHECK( source_before_clear < N );
	TASK_CHECK( source_before_clear > ( 1ULL << 32 ) );

	a.clear();
	g_track_alloc = false;
	TASK_CHECK( g_alloc_count == 0 );
	TASK_CHECK( q == Q0 );
	TASK_CHECK( q > ( 1ULL << 32 ) );
	TASK_CHECK( a.getNextResidualFrame() == M );
	TASK_CHECK( a.getNextSourceFrame() == N );

	// Pick a delivered source sample and check it against an independent
	// finite window built from integer quotient/remainder positions and the
	// real fractional/FIR coefficients.
	const auto pick = delivered[ delivered.size() / 2 ];
	const u64 ii = pick.first;
	TASK_CHECK( ii < N );
	const std::vector< double > h = local_block_kernel();
	const int center = ( (int) h.size() - 1 ) / 2;
	r8b::CDSPFracDelayFilterBank& fb =
		r8b::CDSPFracDelayFilterBankCache::getFilterBank(
			-1, 3, 8, 120.0, false, true );
	const int L = fb.getFilterLen();
	const int F = fb.getFilterFracs();
	TASK_CHECK( L == 16 );

	double expected = 0.0;

	for( u64 j = Q0; j < M; ++j )
	{
		const task_reference::Point pt = task_reference::phase_at(
			j, 95998, 48001, 0.0 );
		double x = pt.fraction * (double) F;
		int fti = (int) x;
		fti = std::max( 0, std::min( F, fti ) );
		const double u = x - (double) fti;
		const double u2 = u * u;
		const double* row = &fb[ fti ];
		const double gv = g[ (std::size_t)( j - Q0 ) ];

		for( int t = 0; t < L; ++t )
		{
			const double c = row[ 3 * t ] + row[ 3 * t + 1 ] * u +
				row[ 3 * t + 2 ] * u2;
			const long long p = (long long) pt.integer + t - 7;
			const long long hidx = (long long) center + p -
				2LL * (long long) ii;

			if( hidx >= 0 && hidx < (long long) h.size() )
			{
				expected += gv * c * h[ (std::size_t) hidx ];
			}
		}
	}

	const double tol = 1e-9 * std::max( 1.0, std::fabs( expected ) );
	TASK_CHECK( std::isfinite( pick.second ) );
	TASK_CHECK( std::fabs( pick.second - expected ) <= tol );

	return( 0 );
}

const struct task_case task_cases[] = {
	{ "Duality", "FiveRatesMono", Duality_FiveRatesMono },
	{ "Duality", "TwoChannels", Duality_TwoChannels },
	{ "Duality", "SixChannels", Duality_SixChannels },
	{ "Duality", "ChannelSeparation", Duality_ChannelSeparation },
	{ "ReverseStream", "BitExactSchedules", ReverseStream_BitExactSchedules },
	{ "ReverseStream", "ClearReuseAndIntervals",
		ReverseStream_ClearReuseAndIntervals },
	{ "Bounded", "EmptySameRateAndValidation",
		Bounded_EmptySameRateAndValidation },
	{ "Bounded", "LongCursorLocalWindowAndAllocation",
		Bounded_LongCursorLocalWindowAndAllocation },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
