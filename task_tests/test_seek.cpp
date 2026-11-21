// Tests for CDSPResampler::seekToOutput().
//
// The tests register three groups (suffix, phase, bounded) via the
// task_cases/task_case_count interface declared in task_support/check.h.
// Each test compares the output obtained after a seek against the
// sequential conversion of the same source, either sample-by-sample
// (suffix) or through a periodic-source shift relation (phase/bounded).

#include "CDSPResampler.h"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace r8b;

namespace {

// A source signal consisting of two non-periodic tones plus an impulse.

double sourceAt( const std::vector< double >& src, const int64_t i )
{
	if( i < 0 || i >= (int64_t) src.size() )
	{
		return( 0.0 );
	}

	return( src[ (size_t) i ]);
}

std::vector< double > makeSource( const int n )
{
	std::vector< double > s( (size_t) n );

	for( int i = 0; i < n; i++ )
	{
		s[ (size_t) i ] = 0.4 * std::sin( i * 0.071 ) +
			0.17 * std::sin( i * 0.113 ) + 0.05 * std::sin( i * 0.9 + 1.3 );
	}

	if( n > 1000 )
	{
		s[ 1000 ] += 0.3;
	}

	return( s );
}

// Periodic source with the given period, used for the phase checks.

struct PeriodicSource
{
	int64_t Period;

	explicit PeriodicSource( const int64_t p )
		: Period( p )
	{
	}

	double at( const int64_t i ) const
	{
		if( i < 0 )
		{
			return( 0.0 );
		}

		const int64_t r = i % Period;

		return( 0.4 * std::sin((double) r * 2.0 * R8B_PI / Period ) +
			0.2 * std::sin((double) r * 6.0 * R8B_PI / Period + 1.0 ));
	}
};

// Reader state for the suffix tests: serves a supplied source array.

struct ArrayReader
{
	const std::vector< double >* Src;
	int64_t Reads;
	int64_t Calls;
	int64_t MaxCount;
	bool SawNegative;

	explicit ArrayReader( const std::vector< double >& s )
		: Src( &s )
		, Reads( 0 )
		, Calls( 0 )
		, MaxCount( 0 )
		, SawNegative( false )
	{
	}
};

int readArray( void* const user, const int64_t first, double* const dst,
	const int count )
{
	ArrayReader* const r = (ArrayReader*) user;
	r -> Reads += count;
	r -> Calls++;
	r -> MaxCount = std::max( r -> MaxCount, (int64_t) count );

	if( first < 0 )
	{
		r -> SawNegative = true;
	}

	for( int i = 0; i < count; i++ )
	{
		const int64_t a = first + i;
		dst[ i ] = ( a >= 0 && a < (int64_t) r -> Src -> size() ?
			(*r -> Src)[ (size_t) a ] : 0.0 );
	}

	return( count );
}

// Reader state for the periodic phase tests: generates source on the fly.

struct PeriodicReader
{
	PeriodicSource Src;
	int64_t Reads;
	int64_t Calls;
	int64_t MaxCount;
	bool SawNegative;

	explicit PeriodicReader( const int64_t p )
		: Src( p )
		, Reads( 0 )
		, Calls( 0 )
		, MaxCount( 0 )
		, SawNegative( false )
	{
	}
};

int readPeriodic( void* const user, const int64_t first, double* const dst,
	const int count )
{
	PeriodicReader* const r = (PeriodicReader*) user;
	r -> Reads += count;
	r -> Calls++;
	r -> MaxCount = std::max( r -> MaxCount, (int64_t) count );

	if( first < 0 )
	{
		r -> SawNegative = true;
	}

	for( int i = 0; i < count; i++ )
	{
		dst[ i ] = r -> Src.at( first + i );
	}

	return( count );
}

// A reader that always fails.

int readFail( void*, const int64_t, double*, const int )
{
	return( 0 );
}

// Sequentially converts the source, returning at least `limit` output
// samples.

std::vector< double > convertSequential( const int srcRate,
	const int dstRate, const int block, const std::vector< double >& src,
	const int64_t limit )
{
	CDSPResampler r( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );
	std::vector< double > out;

	for( size_t i = 0; i < src.size() &&
		(int64_t) out.size() < limit; )
	{
		const int n = (int) std::min< size_t >( (size_t) block,
			src.size() - i );

		double* op = R8B_NULL;
		const int m = r.process( const_cast< double* >( src.data() + i ), n,
			op );

		if( m > 0 )
		{
			out.insert( out.end(), op, op + m );
		}

		i += (size_t) n;
	}

	return( out );
}

// Runs seekToOutput() and then continues processing with the returned cursor,
// collecting `count` output samples. Returns the collected samples.

std::vector< double > seekAndCollect( CDSPResampler& r, const int64_t frame,
	const int block, const std::vector< double >& src,
	ArrayReader& reader, const int64_t count )
{
	const int64_t cur = r.seekToOutput( frame, readArray, &reader );
	std::vector< double > out;
	std::vector< double > buf( (size_t) block );

	double* op = R8B_NULL;

	// A zero-length call must neither advance state nor emit retained
	// samples.

	int m = r.process( R8B_NULL, 0, op );

	if( m != 0 )
	{
		return( out ); // Signalled by an empty result; checked by callers.
	}

	int64_t pos = cur;

	while( (int64_t) out.size() < count )
	{
		const int n = block;

		for( int i = 0; i < n; i++ )
		{
			buf[ (size_t) i ] = sourceAt( src, pos + i );
		}

		op = R8B_NULL;
		m = r.process( &buf[ 0 ], n, op );

		if( m > 0 )
		{
			out.insert( out.end(), op, op + m );
		}

		pos += n;
	}

	out.resize( (size_t) count );

	return( out );
}

// Shared suffix comparison helper. Returns 0 on success.

int suffixCase( const int srcRate, const int dstRate, const int block,
	const int64_t frame, const int64_t count )
{
	const int n = (int) ( frame * (int64_t) srcRate / (int64_t) dstRate ) +
		4 * block + 65536;
	const std::vector< double > src = makeSource( n );

	ArrayReader reader( src );
	CDSPResampler r( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );
	const std::vector< double > got = seekAndCollect( r, frame, block, src,
		reader, count );

	TASK_CHECK( (int64_t) got.size() == count );

	const std::vector< double > want = convertSequential( srcRate, dstRate,
		block, src, frame + count );

	TASK_CHECK( (int64_t) want.size() >= frame + count );

	double maxErr = 0.0;

	for( int64_t j = 0; j < count; j++ )
	{
		maxErr = std::max( maxErr,
			std::fabs( got[ (size_t) j ] - want[ (size_t)( frame + j ) ]));
	}

	TASK_CHECK( maxErr <= 1e-9 );

	return( 0 );
}

// ---------------------------------------------------------------------------
// suffix: seek-then-process must match the sequential tail exactly.

int suffix44100to48000_127()
{
	return( suffixCase( 44100, 48000, 127, 100000, 2048 ));
}

int suffix44100to48000_7()
{
	return( suffixCase( 44100, 48000, 7, 100000, 2048 ));
}

int suffix44100to48000_1()
{
	return( suffixCase( 44100, 48000, 1, 100000, 1024 ));
}

int suffix192000to44100_127()
{
	return( suffixCase( 192000, 44100, 127, 100000, 2048 ));
}

int suffix192000to44100_7()
{
	return( suffixCase( 192000, 44100, 7, 100000, 1024 ));
}

int suffix32000to96000_127()
{
	return( suffixCase( 32000, 96000, 127, 100000, 2048 ));
}

int suffix32000to96000_1()
{
	return( suffixCase( 32000, 96000, 1, 100000, 1024 ));
}

int suffix32000to96000_7()
{
	return( suffixCase( 32000, 96000, 7, 100000, 1024 ));
}

int suffix192000to44100_1()
{
	return( suffixCase( 192000, 44100, 1, 100000, 1024 ));
}

int suffix48000to48000_1()
{
	return( suffixCase( 48000, 48000, 1, 100000, 1024 ));
}

int suffix48000to48000_127()
{
	return( suffixCase( 48000, 48000, 127, 100000, 2048 ));
}

int suffix48000to48000_7()
{
	return( suffixCase( 48000, 48000, 7, 100000, 1024 ));
}

// Backward and forward interleaved seeks, each followed by a suffix check.

int suffixInterleaved()
{
	const int srcRate = 44100;
	const int dstRate = 48000;
	const int block = 127;
	const int64_t maxFrame = 150000;
	const int n = (int) ( maxFrame * (int64_t) srcRate / dstRate ) +
		4 * block + 65536;
	const std::vector< double > src = makeSource( n );
	const std::vector< double > want = convertSequential( srcRate, dstRate,
		block, src, maxFrame + 4096 );

	const int64_t frames[] = { 100000, 50000, 150000, 100000, 20000 };
	CDSPResampler r( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );

	for( int k = 0; k < 5; k++ )
	{
		ArrayReader reader( src );
		const std::vector< double > got = seekAndCollect( r, frames[ k ],
			block, src, reader, 1024 );

		TASK_CHECK( (int64_t) got.size() == 1024 );

		double maxErr = 0.0;

		for( int j = 0; j < 1024; j++ )
		{
			maxErr = std::max( maxErr, std::fabs( got[ (size_t) j ] -
				want[ (size_t)( frames[ k ] + j ) ]));
		}

		TASK_CHECK( maxErr <= 1e-9 );
	}

	return( 0 );
}

// ---------------------------------------------------------------------------
// phase: the periodic shift relation and reader-range rules.

// Verifies out_{f}(j) == out_{f + k*b}(j) for a source with period `a`.

int phaseShiftCase( const int srcRate, const int dstRate, const int block,
	const int64_t frame, const int64_t k )
{
	int64_t a = srcRate;
	int64_t b = dstRate;
	int64_t x = a;
	int64_t y = b;

	while( y != 0 )
	{
		const int64_t t = x % y;
		x = y;
		y = t;
	}

	a /= x;
	b /= x;

	const int64_t shifted = frame + k * b;

	TASK_CHECK( shifted <= ( (int64_t) 1 << 40 ));

	PeriodicReader r1( a );
	PeriodicReader r2( a );

	CDSPResampler s1( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );
	CDSPResampler s2( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );

	const int64_t cur1 = s1.seekToOutput( frame, readPeriodic, &r1 );
	const int64_t cur2 = s2.seekToOutput( shifted, readPeriodic, &r2 );

	// The cursor must sit at an integral source period boundary relative to
	// the requested frame and must not be negative.

	TASK_CHECK( cur1 >= 0 );
	TASK_CHECK( cur2 >= 0 );
	TASK_CHECK( r1.SawNegative == false );
	TASK_CHECK( r2.SawNegative == false );

	const int64_t L = s1.getInputRequiredForOutput( 1 );
	const int64_t budget = 16 * L + 16384;

	TASK_CHECK( r1.Reads <= budget );
	TASK_CHECK( r2.Reads <= budget );
	TASK_CHECK( r1.MaxCount <= block );
	TASK_CHECK( r2.MaxCount <= block );

	std::vector< double > b1( (size_t) block );
	std::vector< double > b2( (size_t) block );

	int64_t p1 = cur1;
	int64_t p2 = cur2;

	double* op = R8B_NULL;

	// Drain any retained pre-roll samples.

	if( s1.process( R8B_NULL, 0, op ) != 0 ||
		s2.process( R8B_NULL, 0, op ) != 0 )
	{
		TASK_CHECK( false );
	}

	for( int64_t j = 0; j < 2048; )
	{
		const int n = block;

		for( int i = 0; i < n; i++ )
		{
			b1[ (size_t) i ] = r1.Src.at( p1 + i );
			b2[ (size_t) i ] = r2.Src.at( p2 + i );
		}

		double* op1 = R8B_NULL;
		double* op2 = R8B_NULL;
		const int m1 = s1.process( &b1[ 0 ], n, op1 );
		const int m2 = s2.process( &b2[ 0 ], n, op2 );

		const int common = std::min( m1, m2 );

		for( int i = 0; i < common && j + i < 2048; i++ )
		{
			TASK_CHECK( std::fabs( op1[ i ] - op2[ i ] ) <= 1e-9 );
		}

		TASK_CHECK( m1 == m2 );

		p1 += n;
		p2 += n;
		j += n;
	}

	return( 0 );
}

int phase47999to48001()
{
	return( phaseShiftCase( 47999, 48001, 127, 100000, 1 ));
}

int phase47999to48001_large()
{
	return( phaseShiftCase( 47999, 48001, 127,
		(int64_t) 4294967297LL, 2 ));
}

int phase47999to48001_huge()
{
	return( phaseShiftCase( 47999, 48001, 127,
		(int64_t) 1099510577776LL, 3 ));
}

int phase44100to48000_large()
{
	return( phaseShiftCase( 44100, 48000, 127,
		(int64_t) 4294967297LL, 5 ));
}

// Verifies the invalid-configuration error behaviour.

int phaseInvalidArguments()
{
	CDSPResampler r( 44100, 48000, 127 );

	bool threw = false;

	try
	{
		r.seekToOutput( -1, readFail, R8B_NULL );
	}
	catch( const std::invalid_argument& )
	{
		threw = true;
	}

	TASK_CHECK( threw );

	threw = false;

	try
	{
		r.seekToOutput( ( (int64_t) 1 << 40 ) + 1, readFail, R8B_NULL );
	}
	catch( const std::invalid_argument& )
	{
		threw = true;
	}

	TASK_CHECK( threw );

	// A non-integral sample rate is rejected by seekToOutput, although
	// sequential conversion remains available.

	CDSPResampler n( 44100.5, 48000.0, 127 );

	threw = false;

	try
	{
		n.seekToOutput( 100, readFail, R8B_NULL );
	}
	catch( const std::invalid_argument& )
	{
		threw = true;
	}

	TASK_CHECK( threw );

	std::vector< double > in( 20000, 0.5 );
	std::vector< double > out;

	for( size_t i = 0; i < in.size(); )
	{
		const int c = (int) std::min< size_t >( 127, in.size() - i );
		double* op = R8B_NULL;
		const int m = n.process( in.data() + i, c, op );

		if( m > 0 )
		{
			out.insert( out.end(), op, op + m );
		}

		i += (size_t) c;
	}

	TASK_CHECK( !out.empty() );

	return( 0 );
}

// A failing reader must be reported as a std::runtime_error.

int phaseReaderFailure()
{
	CDSPResampler r( 44100, 48000, 127 );

	bool threw = false;

	try
	{
		r.seekToOutput( 100000, readFail, R8B_NULL );
	}
	catch( const std::runtime_error& )
	{
		threw = true;
	}

	TASK_CHECK( threw );

	return( 0 );
}

// ---------------------------------------------------------------------------
// bounded: read volume and PCM-space limits, plus clear()/re-seek behaviour.

int boundedReadBudget()
{
	const int srcRate = 44100;
	const int dstRate = 48000;
	const int block = 127;
	const int64_t frame = 100000;

	const std::vector< double > src = makeSource(
		(int) ( frame * (int64_t) srcRate / dstRate ) + 4 * block + 65536 );

	ArrayReader reader( src );
	CDSPResampler r( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );

	const int64_t cur = r.seekToOutput( frame, readArray, &reader );

	TASK_CHECK( cur >= 0 );
	TASK_CHECK( reader.SawNegative == false );

	const int64_t L = r.getInputRequiredForOutput( 1 );
	const int64_t budget = 16 * L + 16384;

	TASK_CHECK( reader.Reads <= budget );
	TASK_CHECK( reader.MaxCount <= block );

	// Extra PCM space: the retained output must not exceed the budget or
	// the maximal output block.

	TASK_CHECK( reader.Reads <= budget );

	return( 0 );
}

int boundedHighUpsampling()
{
	// At an extreme upsampling ratio the pre-roll must still respect the
	// read budget and the input-chunk limit.

	const int srcRate = 8000;
	const int dstRate = 384000;
	const int block = 1;
	const int64_t frame = 100000;

	PeriodicReader reader( srcRate );
	CDSPResampler r( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );

	const int64_t cur = r.seekToOutput( frame, readPeriodic, &reader );

	TASK_CHECK( cur >= 0 );

	const int64_t L = r.getInputRequiredForOutput( 1 );
	const int64_t budget = 16 * L + 16384;

	TASK_CHECK( reader.Reads <= budget );
	TASK_CHECK( reader.MaxCount <= block );
	TASK_CHECK( reader.SawNegative == false );

	return( 0 );
}

int boundedLargeFrame()
{
	// A frame above 2^32 must be handled with work independent of the frame
	// magnitude.

	const int srcRate = 47999;
	const int dstRate = 48001;
	const int block = 127;
	const int64_t frame = 1099510577776LL; // > 2^40 - 2^28

	PeriodicReader reader( srcRate );
	CDSPResampler r( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );

	const int64_t cur = r.seekToOutput( frame, readPeriodic, &reader );

	TASK_CHECK( cur >= 0 );

	const int64_t L = r.getInputRequiredForOutput( 1 );
	const int64_t budget = 16 * L + 16384;

	TASK_CHECK( reader.Reads <= budget );
	TASK_CHECK( reader.MaxCount <= block );
	TASK_CHECK( reader.SawNegative == false );

	// Same frame, repeated seek: work must remain bounded.

	PeriodicReader reader2( srcRate );
	const int64_t cur2 = r.seekToOutput( frame, readPeriodic, &reader2 );

	TASK_CHECK( cur2 == cur );
	TASK_CHECK( reader2.Reads <= budget );

	return( 0 );
}

int boundedZeroAndClear()
{
	const int srcRate = 44100;
	const int dstRate = 48000;
	const int block = 127;

	// A zero-length process() after a seek must emit nothing and must not
	// advance the state.

	const std::vector< double > src = makeSource( 300000 );

	ArrayReader reader( src );
	CDSPResampler r( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );
	const int64_t cur = r.seekToOutput( 100000, readArray, &reader );

	double* op = R8B_NULL;

	for( int i = 0; i < 8; i++ )
	{
		TASK_CHECK( r.process( R8B_NULL, 0, op ) == 0 );
	}

	// The first real process() call must still produce the correct first
	// sample, i.e. the zero-length calls did not consume retained samples.

	std::vector< double > buf( (size_t) block );

	for( int i = 0; i < block; i++ )
	{
		buf[ (size_t) i ] = sourceAt( src, cur + i );
	}

	const int m = r.process( &buf[ 0 ], block, op );

	TASK_CHECK( m > 0 );

	const std::vector< double > want = convertSequential( srcRate, dstRate,
		block, src, 100001 );

	TASK_CHECK( std::fabs( op[ 0 ] - want[ (size_t) 100000 ] ) <= 1e-9 );

	// clear() must restore the from-zero behaviour.

	r.clear();

	double first = 1.0;
	double* op1 = R8B_NULL;
	const int m1 = r.process( &first, 1, op1 );

	CDSPResampler fresh( srcRate, dstRate, block, 2.0, 206.91,
		fprLinearPhase );
	double first2 = 1.0;
	double* op2 = R8B_NULL;
	const int m2 = fresh.process( &first2, 1, op2 );

	TASK_CHECK( m1 == m2 );

	// Equal sample rates: the cursor equals the requested frame and the
	// next process() returns the input unchanged.

	CDSPResampler same( 48000, 48000, 127, 2.0, 206.91, fprLinearPhase );
	const int64_t sc = same.seekToOutput( 12345, readFail, R8B_NULL );

	TASK_CHECK( sc == 12345 );

	return( 0 );
}

} // namespace

// The test driver in task_support/driver.c expects these two symbols at
// global scope. The `extern` declarations give them external linkage.

extern const task_case task_cases[];
extern const size_t task_case_count;

const task_case task_cases[] =
{
	{ "suffix", "44100to48000_127", &suffix44100to48000_127 },
	{ "suffix", "44100to48000_7", &suffix44100to48000_7 },
	{ "suffix", "44100to48000_1", &suffix44100to48000_1 },
	{ "suffix", "192000to44100_127", &suffix192000to44100_127 },
	{ "suffix", "192000to44100_7", &suffix192000to44100_7 },
	{ "suffix", "192000to44100_1", &suffix192000to44100_1 },
	{ "suffix", "32000to96000_127", &suffix32000to96000_127 },
	{ "suffix", "32000to96000_7", &suffix32000to96000_7 },
	{ "suffix", "32000to96000_1", &suffix32000to96000_1 },
	{ "suffix", "48000to48000_127", &suffix48000to48000_127 },
	{ "suffix", "48000to48000_7", &suffix48000to48000_7 },
	{ "suffix", "48000to48000_1", &suffix48000to48000_1 },
	{ "suffix", "interleaved", &suffixInterleaved },

	{ "phase", "47999to48001", &phase47999to48001 },
	{ "phase", "47999to48001_large", &phase47999to48001_large },
	{ "phase", "47999to48001_huge", &phase47999to48001_huge },
	{ "phase", "44100to48000_large", &phase44100to48000_large },
	{ "phase", "invalid_arguments", &phaseInvalidArguments },
	{ "phase", "reader_failure", &phaseReaderFailure },

	{ "bounded", "read_budget", &boundedReadBudget },
	{ "bounded", "high_upsampling", &boundedHighUpsampling },
	{ "bounded", "large_frame", &boundedLargeFrame },
	{ "bounded", "zero_and_clear", &boundedZeroAndClear }
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ]);

