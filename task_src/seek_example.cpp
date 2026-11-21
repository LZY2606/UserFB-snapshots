// Seek example: resampling a long recording starting at an arbitrary output
// frame.
//
// This translation unit contains the device-free example logic. It is a
// plain function, not a `main`, so it can be linked together with other
// task sources. The small runner in examples/seek_example.cpp calls
// `r8b_seek_example()`; the same function can also be called from an
// existing application, unit test, or a device callback.
//
// The example shows how to use CDSPResampler::seekToOutput() to begin
// resampling at an arbitrary output frame without converting (or reading)
// the whole source prefix, and how to consume the source cursor it returns.

#include "CDSPResampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace r8b;

namespace {

// The source signal is generated on demand, so the example never has to
// allocate a buffer for the whole recording. In a real application this
// function would read from a decoded audio file, a memory-mapped buffer,
// or a decoder API instead.
//
// The reader is called with absolute source sample positions. Negative
// positions cannot occur (seekToOutput() only passes non-negative
// positions), but treating them as silence is a safe convention: a seek
// near the start of a stream pre-rolls "before" sample 0, and those
// positions are silence.

int readSource( void* const user, const int64_t first, double* const dst,
	const int count )
{
	const int64_t period = *(const int64_t*) user;

	for( int i = 0; i < count; i++ )
	{
		const int64_t pos = first + i;
		double v = 0.0;

		if( pos >= 0 )
		{
			const int64_t r = pos % period;
			v = 0.4 * std::sin( (double) r * 0.071 ) +
				0.17 * std::sin( (double) r * 0.113 );
		}

		dst[ i ] = v;
	}

	return( count );
}

} // namespace

// Returns 0 on success, non-zero if the seek result does not match the
// sequential conversion.

int r8b_seek_example()
{
	const int srcRate = 48000;
	const int dstRate = 44100;
	const int block = 256; // Maximal input block length.
	const int64_t seekFrame = 123456789; // Arbitrary output frame.

	CDSPResampler resampler( srcRate, dstRate, block, 2.0, 206.91,
		fprLinearPhase );

	int64_t period = 48000; // Source period used by the synthetic reader.

	// Position the resampler so that the next process() call produces the
	// sample at output frame `seekFrame`. The returned cursor is the
	// absolute source sample position from which the caller must continue
	// feeding source data. It is not necessarily the "obvious"
	// floor(seekFrame * srcRate / dstRate) value: the resampler starts a
	// little earlier so that its filters have the history they need, and
	// returns the position just after that pre-roll.
	//
	// Only a bounded window of source samples around the requested frame is
	// read, regardless of how far into the stream `seekFrame` points.

	int64_t cursor = resampler.seekToOutput( seekFrame, readSource,
		&period );

	std::printf( "seekToOutput(%lld) returned source cursor %lld\n",
		(long long) seekFrame, (long long) cursor );

	// A zero-length process() call is a no-op: it does not advance the
	// state and does not emit samples retained by the pre-roll.

	double* out = nullptr;

	if( resampler.process( nullptr, 0, out ) != 0 )
	{
		std::printf( "unexpected output from a zero-length call\n" );
		return( 1 );
	}

	// Feed source samples starting at `cursor`. Any output samples that the
	// seek pre-roll produced at or after `seekFrame` are returned first,
	// then the freshly computed samples follow, so the output stream is
	// continuous and free of gaps or duplicates.

	std::vector< double > inBuf( (size_t) block );
	std::vector< double > got;
	const int want = 512;

	while( (int) got.size() < want )
	{
		readSource( &period, cursor, inBuf.data(), block );

		const int n = resampler.process( inBuf.data(), block, out );

		if( n > 0 )
		{
			got.insert( got.end(), out, out + n );
		}

		cursor += block;
	}

	// Verify against a sequential conversion. A real application would not
	// need this: it is here only to show that the seek is exact.

	CDSPResampler seq( srcRate, dstRate, block, 2.0, 206.91, fprLinearPhase );
	std::vector< double > seqOut;
	std::vector< double > seqBuf( (size_t) block );
	int64_t seqPos = 0;

	while( (int64_t) seqOut.size() < seekFrame + want )
	{
		readSource( &period, seqPos, seqBuf.data(), block );

		const int n = seq.process( seqBuf.data(), block, out );

		if( n > 0 )
		{
			seqOut.insert( seqOut.end(), out, out + n );
		}

		seqPos += block;
	}

	double maxErr = 0.0;

	for( int i = 0; i < want; i++ )
	{
		maxErr = std::max( maxErr, std::fabs( got[ (size_t) i ] -
			seqOut[ (size_t)( seekFrame + i ) ]));
	}

	std::printf( "max error vs. sequential conversion: %.3g\n", maxErr );

	return( maxErr <= 1e-9 ? 0 : 1 );
}
