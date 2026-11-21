//$ nobt
//$ nocpp

/**
 * @file CDSPResampler.h
 *
 * @brief The master sample rate converter (resampler) class.
 *
 * This file includes the master sample rate converter (resampler) class that
 * combines all elements of this library into a single front-end class.
 *
 * r8brain-free-src Copyright (c) 2013-2026 Aleksey Vaneev
 *
 * See the "LICENSE" file for license.
 */

#ifndef R8B_CDSPRESAMPLER_INCLUDED
#define R8B_CDSPRESAMPLER_INCLUDED

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "CDSPHBDownsampler.h"
#include "CDSPHBUpsampler.h"
#include "CDSPBlockConvolver.h"
#include "CDSPFracInterpolator.h"

namespace r8b {

/**
 * @brief The master sample rate converter (resampler) class.
 *
 * This class can be considered the "master" sample rate converter (resampler)
 * class since it combines all functionality of this library into a single
 * front-end class that performs sample rate conversion to/from any sample
 * rate, including non-integer sample rates.
 *
 * Note that objects of this class can be constructed on the stack as it has a
 * small member data size. The default template parameters of this class are
 * suited for 27-bit fixed point resampling.
 *
 * WARNING: Since FFT and filter object caches are global static variables,
 * objects of the resampler class should not be declared as global variables
 * to avoid causing crashes on exit; they should instead be allocated on the
 * heap or stack. Additionally, under pre-C++11 standards only, the first-ever
 * resampler object must be constructed in a non-concurrent context (i.e.,
 * not concurrently).
 *
 * Use the CDSPResampler16 class for 16-bit resampling.
 *
 * Use the CDSPResampler16IR class for 16-bit impulse response resampling.
 *
 * Use the CDSPResampler24 class for 24-bit resampling (including 32-bit
 * floating point resampling).
 */

class CDSPResampler : public CDSPProcessor
{
public:
	/**
	 * @brief Source sample reader callback used by the seekToOutput()
	 * function.
	 *
	 * The callback must return `count` samples starting at the absolute
	 * source sample position `first` (negative positions are treated as
	 * silence by the seek function). The callback must return `count` on
	 * success; any other return value is reported as a `std::runtime_error`.
	 * `count` never exceeds the `MaxInLen` value supplied to the constructor,
	 * and `first` is never negative.
	 *
	 * The callback is only valid for the duration of the seekToOutput()
	 * call: the resampler does not retain the pointer or call it later.
	 */

	typedef int ( * ReadAt )( void* user, int64_t first, double* dst,
		int count );

	/**
	 * @brief Initalizes the resampler object.
	 *
	 * Note that increasing the transition band and decreasing attenuation
	 * reduces the filter length, this in turn reduces the "input before
	 * output" delay. However, the filter length has only a minor influence on
	 * the overall resampling speed.
	 *
	 * It should be noted that the `ReqAtten` specifies the minimal difference
	 * between the loudest input signal component and the produced aliasing
	 * artifacts during resampling. For example, if `ReqAtten=100` was
	 * specified when performing 2x upsampling, the analysis of the resulting
	 * signal may display high-frequency components which are quieter than
	 * the loudest part of the input signal by only 100 decibel meaning
	 * the high-frequency part did not become "magically" completely silent
	 * after resampling. You have to specify a higher `ReqAtten` value if you
	 * need a totally clean high-frequency content. On the other hand, it may
	 * not be reasonable to have a high-frequency content cleaner than
	 * the input signal itself: if the input signal is 16-bit, setting
	 * `ReqAtten` to 180 will make its high-frequency content 24-bit, but
	 * the original part of the signal will remain 16-bit.
	 *
	 * @param SrcSampleRate Source signal's sample rate. Both sample rates can
	 * be specified as a ratio: e.g., `SrcSampleRate = 1.0`,
	 * `DstSampleRate = 2.0`.
	 * @param DstSampleRate Destination signal's sample rate. The "power of 2"
	 * ratios between the source and destination sample rates force resampler
	 * to use several fast "power of 2" resampling steps, without using
	 * fractional interpolation at all.
	 * @param aMaxInLen The maximal planned length of the input buffer (in
	 * samples) that will be passed to the resampler. The resampler relies on
	 * this value as it allocates intermediate buffers. Input buffers longer
	 * than this value should never be supplied to the resampler. Note that
	 * upsampling produces more samples than was provided on input, so at
	 * higher upsampling ratios it is advisable to use smaller `aMaxInLen`
	 * values to reduce memory footprint. When downsampling, a larger
	 * `aMaxInLen` is suggested in order to increase downsampling performance.
	 * @param ReqTransBand Required transition band, in percent of the
	 * spectral space of the input signal (or the output signal if
	 * downsampling is performed) between filter's -3 dB point and the Nyquist
	 * frequency. The range is from CDSPFIRFilter::getLPMinTransBand() to
	 * CDSPFIRFilter::getLPMaxTransBand(), inclusive. When upsampling 88200 or
	 * 96000 audio to higher sample rates the ReqTransBand can be considerably
	 * increased, up to 30. The selection of ReqTransBand depends on the level
	 * of desire to preserve the high-frequency content. While values 0.5 to 2
	 * are extremely "greedy" settings, not necessary in most cases, values 2
	 * to 3 can be used in most cases. Values 3 to 4 are relaxed settings, but
	 * they still offer a flat frequency response up to 21kHz with 44.1k
	 * source or destination sample rate.
	 * @param ReqAtten Required stop-band attenuation in decibel, in the
	 * range CDSPFIRFilter::getLPMinAtten() to CDSPFIRFilter::getLPMaxAtten(),
	 * inclusive. The actual attenuation may be 0.40-4.46 dB higher. The
	 * general formula for selecting the `ReqAtten` is `6.02 * Bits + 40`,
	 * where `Bits` is the bit resolution (e.g., 16, 24), 40 is an added
	 * resolution for dynamic signals; this value can be decreased to 20 to
	 * 10, if the signal being resampled is non-dynamic (e.g., an impulse
	 * response or filter, with a non-steep frequency response).
	 * @param ReqPhase Required filter's phase response. Note that this
	 * setting does not affect interpolator's phase response which is always
	 * linear-phase. Also note that if the "power of 2" resampling was engaged
	 * by the resampler together with the minimum-phase response, the audio
	 * stream may become fractionally delayed, depending on the minimum-phase
	 * filter's actual fractional delay. Linear-phase filters do not have
	 * fractional delay.
	 * @see EDSPFilterPhaseResponse
	 */

	CDSPResampler( const double SrcSampleRate, const double DstSampleRate,
		const int aMaxInLen, const double ReqTransBand = 2.0,
		const double ReqAtten = 206.91,
		const EDSPFilterPhaseResponse ReqPhase = fprLinearPhase )
		: MaxInLen( aMaxInLen )
		, CurMaxOutLen( aMaxInLen )
		, LatencyFrac( 0.0 )
		, SeekSrcRate( SrcSampleRate )
		, SeekDstRate( DstSampleRate )
		, SeekPhase( ReqPhase )
		, SeekMode( false )
	{
		R8BASSERT( SrcSampleRate > 0.0 );
		R8BASSERT( DstSampleRate > 0.0 );
		R8BASSERT( MaxInLen > 0 );
		R8BASSERT( ReqTransBand > 0.0 );
		R8BASSERT( ReqAtten > 0.0 );

		R8BCONSOLE( "* CDSPResampler: src=%.1f dst=%.1f len=%i tb=%.1f "
			"att=%.2f ph=%i\n", SrcSampleRate, DstSampleRate, aMaxInLen,
			ReqTransBand, ReqAtten, (int) ReqPhase );

		if( SrcSampleRate == DstSampleRate )
		{
			return;
		}

		TmpBufCapacities[ 0 ] = 0;
		TmpBufCapacities[ 1 ] = 0;
		CurTmpBuf = 0;

		// Try some common efficient ratios requiring only a single step.

		const int CommonRatioCount = 5;
		const int CommonRatios[ CommonRatioCount ][ 2 ] = {
			{ 1, 2 },
			{ 1, 3 },
			{ 2, 3 },
			{ 3, 2 },
			{ 3, 4 }
		};

		int i;

		for( i = 0; i < CommonRatioCount; i++ )
		{
			const int num = CommonRatios[ i ][ 0 ];
			const int den = CommonRatios[ i ][ 1 ];

			if( SrcSampleRate * num == DstSampleRate * den )
			{
				addProcessor( new CDSPBlockConvolver(
					CDSPFIRFilterCache :: getLPFilter(
					1.0 / ( num > den ? num : den ), ReqTransBand,
					ReqAtten, ReqPhase, num, den ), num, den, LatencyFrac ));

				createTmpBuffers();
				return;
			}
		}

		// Try whole-number power-of-2 or 3*power-of-2 upsampling.

		for( i = 2; i <= 3; i++ )
		{
			bool WasFound = false;
			double sm = i;
			int c = 0;

			while( true )
			{
				const double NewSR = SrcSampleRate * sm;

				if( NewSR == DstSampleRate )
				{
					WasFound = true;
					break;
				}

				if( NewSR > DstSampleRate )
				{
					break;
				}

				sm *= 2.0;
				c++;
			}

			if( WasFound )
			{
				addProcessor( new CDSPBlockConvolver(
					CDSPFIRFilterCache :: getLPFilter( 1.0 / i, ReqTransBand,
					ReqAtten, ReqPhase, i, 1 ), i, 1, LatencyFrac ));

				const bool IsThirdFirst = ( i == 3 );

				for( i = 0; i < c; i++ )
				{
					addProcessor( new CDSPHBUpsampler( ReqAtten, i,
						( IsThirdFirst || i > 1 ), LatencyFrac ));
				}

				createTmpBuffers();
				return;
			}
		}

		if( DstSampleRate * 2.0 > SrcSampleRate )
		{
			// Upsampling or fractional downsampling down to 2X.

			const double NormFreq = ( DstSampleRate > SrcSampleRate ? 0.5 :
				0.5 * DstSampleRate / SrcSampleRate );

			addProcessor( new CDSPBlockConvolver(
				CDSPFIRFilterCache :: getLPFilter( NormFreq, ReqTransBand,
				ReqAtten, ReqPhase, 2.0, 1 ), 2, 1, LatencyFrac ));

			// Try intermediate interpolated resampling with subsequent 2X
			// or 3X upsampling.

			const double tbw = 0.0175; // Intermediate filter's transition
				// band extension coefficient.
			const double ThreshSampleRate = SrcSampleRate /
				( 1.0 - tbw * ReqTransBand ); // Make sure intermediate
				// filter's transition band is not steeper than ReqTransBand
				// (this keeps the latency under control).

			int c = 0;
			double div = 1.0;

			while( true )
			{
				const double ndiv = div * 2.0;

				if( DstSampleRate < ThreshSampleRate * ndiv )
				{
					break;
				}

				div = ndiv;
				c++;
			}

			const double SrcSampleRate2 = SrcSampleRate * 2.0;
			int tmp1;
			int tmp2;

			if( c == 1 && getWholeStepping( SrcSampleRate2, DstSampleRate,
				tmp1, tmp2 ))
			{
				// Do not use intermediate interpolation if whole stepping is
				// available as it performs very fast.

				c = 0;
			}

			if( c > 0 )
			{
				// Add steps using intermediate interpolation.

				const int num = 2;

				addProcessor( new CDSPFracInterpolator( SrcSampleRate2 * div,
					DstSampleRate, ReqAtten, false, LatencyFrac ));

				double tb = ( 1.0 - SrcSampleRate * div / DstSampleRate ) /
					tbw; // Divide TransBand by a constant that assures a
					// linear response in the pass-band.

				if( tb > CDSPFIRFilter :: getLPMaxTransBand() )
				{
					tb = CDSPFIRFilter :: getLPMaxTransBand();
				}

				addProcessor( new CDSPBlockConvolver(
					CDSPFIRFilterCache :: getLPFilter( 1.0 / num, tb,
					ReqAtten, ReqPhase, num, 1 ), num, 1, LatencyFrac ));

				for( i = 1; i < c; i++ )
				{
					addProcessor( new CDSPHBUpsampler( ReqAtten, i - 1,
						( i > 2 ), LatencyFrac ));
				}
			}
			else
			{
				addProcessor( new CDSPFracInterpolator( SrcSampleRate2,
					DstSampleRate, ReqAtten, false, LatencyFrac ));
			}

			createTmpBuffers();
			return;
		}

		// Use downsampling steps, including power-of-2 downsampling.

		double CheckSR = DstSampleRate * 4.0;
		double SrcSRDiv = 1.0;
		int c = 0;

		while( CheckSR <= SrcSampleRate )
		{
			c++;
			CheckSR *= 2.0;
			SrcSRDiv *= 2.0;
		}

		const double FinGain = 1.0 / SrcSRDiv;

		double NormFreq = 0.5;
		bool UseInterp = true;
		bool IsThirdFirst = false;
		int downf;

		for( downf = 2; downf <= 3; downf++ )
		{
			if( DstSampleRate * SrcSRDiv * downf == SrcSampleRate )
			{
				NormFreq = 1.0 / downf;
				UseInterp = false;
				IsThirdFirst = ( downf == 3 );
				break;
			}
		}

		if( UseInterp )
		{
			downf = 1;
			NormFreq = DstSampleRate * SrcSRDiv / SrcSampleRate;
			IsThirdFirst = ( NormFreq * 3.0 <= 1.0 );
		}

		for( i = 0; i < c; i++ )
		{
			// Use fixed, very relaxed 2X downsampling filters, that at the
			// final stage only guarantee stop-band between 0.75 and 1.0 of
			// Nyquist. 0.5-0.75 range will be aliased to 0.25-0.5 range which
			// will then be filtered out by the final filter.

			addProcessor( new CDSPHBDownsampler( ReqAtten, c - 1 - i,
				( IsThirdFirst || i < c - 2 ), LatencyFrac ));
		}

		addProcessor( new CDSPBlockConvolver(
			CDSPFIRFilterCache :: getLPFilter( NormFreq, ReqTransBand,
			ReqAtten, ReqPhase, FinGain, downf ), 1, downf, LatencyFrac ));

		if( UseInterp )
		{
			addProcessor( new CDSPFracInterpolator( SrcSampleRate,
				DstSampleRate * SrcSRDiv, ReqAtten, IsThirdFirst,
				LatencyFrac ));
		}

		createTmpBuffers();
	}

	virtual int getInLenBeforeOutPos( int ReqOutPos ) const
	{
		R8BASSERT( ReqOutPos >= 0 );

		if( Next != R8B_NULL )
		{
			ReqOutPos = Next -> getInLenBeforeOutPos( ReqOutPos );
		}

		if( StepFirst != R8B_NULL )
		{
			ReqOutPos = StepFirst -> getInLenBeforeOutPos( ReqOutPos );
		}

		return( ReqOutPos );
	}

	/**
	 * @brief Positions the resampler at the specified output frame and
	 * pre-rolls its internal state using a pull-style source reader.
	 *
	 * After the call, the next process() call returns an output stream whose
	 * first returned sample is the sample that a conversion started at source
	 * sample 0 and fed sequentially would have produced at output position
	 * `frame`. The caller should continue supplying source samples starting
	 * from the cursor value returned by this function.
	 *
	 * The work performed by this function does not grow with `frame`: only a
	 * short window of source samples around the requested position is read.
	 * Source positions before 0 are treated as silence. If the resampler was
	 * previously positioned, the prior state is discarded and replaced.
	 *
	 * This function is currently supported only for linear-phase filters and
	 * integral source and destination sample rates in the range 8000 to
	 * 384000, inclusive, with `frame` in the range 0 to 2^40, inclusive.
	 * Calling this function for any other configuration raises a
	 * `std::invalid_argument` exception; sequential conversion (without
	 * calling this function) remains available for such configurations.
	 *
	 * If `frame` is 0, the resampler is simply cleared and the returned
	 * cursor is 0. If the source and destination sample rates are equal, the
	 * returned cursor equals `frame` and no source is read.
	 *
	 * Any output samples produced by the pre-roll that lie at or after
	 * `frame` are retained and returned by the following process() call
	 * before any newly computed samples, so that no output sample is lost or
	 * duplicated.
	 *
	 * The `reader` callback is used only during this call; no pointer to it
	 * is retained and it is never called afterwards.
	 *
	 * @param frame Requested output frame position.
	 * @param reader Source sample reader callback.
	 * @param user Opaque pointer passed to `reader`.
	 * @return The absolute source sample position from which the next
	 * process() call should be fed.
	 */

	virtual int64_t seekToOutput( const int64_t frame, const ReadAt reader,
		void* const user )
	{
		if( frame < 0 )
		{
			throw std::invalid_argument(
				"CDSPResampler::seekToOutput: negative output frame." );
		}

		if( frame > ( (int64_t) 1 << 40 ))
		{
			throw std::invalid_argument(
				"CDSPResampler::seekToOutput: output frame out of range." );
		}

		if( reader == R8B_NULL )
		{
			throw std::invalid_argument(
				"CDSPResampler::seekToOutput: null reader." );
		}

		if( SeekPhase != fprLinearPhase )
		{
			throw std::invalid_argument(
				"CDSPResampler::seekToOutput: only linear-phase filters "
				"are supported." );
		}

		const double S = SeekSrcRate;
		const double D = SeekDstRate;

		if( !( S >= 8000.0 && S <= 384000.0 && D >= 8000.0 &&
			D <= 384000.0 && floor( S ) == S && floor( D ) == D ))
		{
			throw std::invalid_argument(
				"CDSPResampler::seekToOutput: unsupported sample rates." );
		}

		clear();

		if( S == D )
		{
			SeekMode = true;
			return( frame );
		}

		if( frame == 0 )
		{
			SeekMode = true;
			return( 0 );
		}

		const int64_t SrcRate = (int64_t) S;
		const int64_t DstRate = (int64_t) D;
		const int64_t g = seekGCD( SrcRate, DstRate );
		const int64_t a = SrcRate / g; // Source period, in samples.
		const int64_t b = DstRate / g; // Output period, in samples.

		// Compute the source position that corresponds to the requested
		// output frame (rounded down). Up to 2^40 * 384000 fits into int64.

		const int64_t Xf = frame * a / b;
		const int64_t L = getInputRequiredForOutput( 1 );
		const int64_t W = 4 * L + 1024; // Pre-roll window, in samples.

		int64_t Rstart = Xf - W;

		if( Rstart < 0 )
		{
			Rstart = 0;
		}

		// Align the pre-roll start to a whole number of source periods so
		// that the resulting output stream is shifted by an integer number
		// of output frames (`K0`). The gap between the aligned position and
		// `Rstart` is fed as silence; it is outside the filter's support of
		// the requested output frame.

		const int64_t m = Rstart / a;
		const int64_t P = m * a;
		const int64_t K0 = m * b;
		int64_t ZeroCount = Rstart - P;

		// Bound the source-chunk size so that both the source staging buffer
		// and the retained output PCM stay within the seek budget and the
		// maximal input block length.

		const int64_t Budget = 16 * L + 16384;
		int64_t ChunkCap = (int64_t) ((double) Budget * S / D) + 1;

		if( ChunkCap < 1 )
		{
			ChunkCap = 1;
		}

		if( ChunkCap > MaxInLen )
		{
			ChunkCap = MaxInLen;
		}

		if( ChunkCap > 4096 )
		{
			ChunkCap = 4096;
		}

		const int Chunk = (int) ChunkCap;
		std::vector< double > Buf( (size_t) Chunk );
		int64_t Pos = Rstart;
		int64_t Produced = 0;

		// Notes on the `Produced` counter: it counts all output samples
		// generated by this pre-roll (not only the retained ones). Output
		// sample number `K0 + Produced` lies at the requested output frame
		// when `K0 + Produced == frame`.

		int i;

		for( i = 0; i < Chunk; i++ )
		{
			Buf[ (size_t) i ] = 0.0;
		}

		while( ZeroCount > 0 )
		{
			const int n = (int) ( ZeroCount < Chunk ? ZeroCount : Chunk );
			double* op = R8B_NULL;
			const int r = process( &Buf[ 0 ], n, op );

			retainSeekOutput( op, r, K0, frame, Produced );
			ZeroCount -= n;
		}

		while( K0 + Produced <= frame )
		{
			// Request only as much source data as is expected to be
			// required to reach the requested frame. This keeps the number
			// of overshoot samples (and hence the retained PCM) small even
			// at high upsampling ratios.

			const int64_t Remain = frame - ( K0 + Produced );
			const double EstD = (double) Remain * S / D + 4.0;
			int n;

			if( EstD < (double) Chunk )
			{
				n = (int) EstD;

				if( n < 1 )
				{
					n = 1;
				}
			}
			else
			{
				n = Chunk;
			}

			const int rc = reader( user, Pos, &Buf[ 0 ], n );

			if( rc != n )
			{
				throw std::runtime_error(
					"CDSPResampler::seekToOutput: source reader failed." );
			}

			double* op = R8B_NULL;
			const int r = process( &Buf[ 0 ], n, op );

			retainSeekOutput( op, r, K0, frame, Produced );
			Pos += n;
		}

		SeekMode = true;
		return( Pos );
	}

	/**
	 * @brief Returns the number of input samples required to advance to
	 * the specified output sample position (so that the next process() call
	 * passes this output position), starting at the cleared or
	 * after-construction state of *this* object.
	 *
	 * This function works by iteratively passing 1 sample at a time until the
	 * overall output length passes the specified value. This is a relatively
	 * CPU-consuming operation. This function should be called after the
	 * clear() function call or after object's construction. The function
	 * itself calls the clear() function before return.
	 *
	 * Note that this function can be considered a legacy function, and is now
	 * used for testing purposes. It is advised to use "instant" (much faster)
	 * getInLenBeforeOutPos() and getInputRequiredForOutput() functions
	 * instead.
	 *
	 * @param ReqOutPos The required output position. Must be a non-negative
	 * value.
	 * @return The number of input samples required.
	 */

	int getInLenBeforeOutStart( const int ReqOutPos = 0 )
	{
		R8BASSERT( ReqOutPos >= 0 );

		int inc = 0;
		int outc = 0;

		while( true )
		{
			double ins = 0.0;
			double* op;
			outc += process( &ins, 1, op );

			if( outc > ReqOutPos )
			{
				clear();
				return( inc );
			}

			inc++;
		}
	}

	/**
	 * @brief Returns the number of input samples required to produce at
	 * least the specified number of output samples, starting at the cleared
	 * or after-construction state of *this* object.
	 *
	 * @param ReqOutSamples The number of output samples required. If a
	 * non-positive value was specified, the function returns 0.
	 * @return The number of input samples required.
	 */

	int getInputRequiredForOutput( const int ReqOutSamples ) const
	{
		if( ReqOutSamples < 1 )
		{
			return( 0 );
		}

		return( getInLenBeforeOutPos( ReqOutSamples - 1 ) + 1 );
	}

	virtual int getLatency() const
	{
		return( 0 );
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	/**
	 * @brief This implementation ignores the supplied parameter and returns
	 * the maximal output buffer length that depends on the `MaxInLen`
	 * supplied to the constructor.
	 */

	virtual int getMaxOutLen( const int/* MaxInLen */) const
	{
		return( CurMaxOutLen );
	}

	/**
	 * @brief Clears (resets) the state of *this* object and returns it to
	 * the state after construction.
	 *
	 * All input data accumulated in the internal buffer so far will be
	 * discarded.
	 *
	 * This function makes it possible to use *this* object for converting
	 * separate streams from the same source sample rate to the same
	 * destination sample rate without reconstructing the object. It is more
	 * efficient to clear the state of the resampler object than to destroy it
	 * and create a new object.
	 */

	virtual void clear()
	{
		SeekMode = false;
		SeekPending.clear();

		CDSPProcessor* ps = StepFirst;

		while( ps != R8B_NULL )
		{
			ps -> clear();
			ps = ps -> Next;
		}
	}

	/**
	 * @brief Performs sample rate conversion.
	 *
	 * If the source and destination sample rates are equal, the resampler
	 * will do nothing and will simply return the input buffer unchanged.
	 *
	 * You do not need to allocate an intermediate output buffer for use with
	 * this function. If required, the resampler will allocate a suitable
	 * intermediate output buffer itself.
	 *
	 * @param ip0 Input buffer. This buffer is never used as output buffer by
	 * this function. This pointer may be returned in `op0` if no resampling
	 * is happening (source sample rate equals destination sample rate).
	 * @param l The number of samples available in the input buffer. Should
	 * not exceed the `MaxInLen` supplied in the constructor.
	 * @param[out] op0 This variable receives the pointer to the resampled
	 * data. On function's return, this pointer points to *this* object's
	 * internal buffer. In real-time applications it is suggested to pass this
	 * pointer to the next output audio block and consume any data left from
	 * the previous output audio block first before calling the process()
	 * function again. The buffer pointed to by the `op0` on return is owned
	 * by the resampler, so it should not be freed by the caller.
	 * @return The number of samples available in the `op0` output buffer. If
	 * the data from the output buffer `op0` is going to be written to a
	 * bigger output buffer, it is suggested to check the returned number of
	 * samples so that no overflow of the bigger output buffer happens.
	 */

	virtual int process( double* ip0, int l, double*& op0 )
	{
		R8BASSERT( l >= 0 && l <= MaxInLen );

		if( l == 0 && SeekMode )
		{
			// A zero-length process() call performed after seekToOutput()
			// must neither advance the internal state nor emit samples that
			// were retained by the seek operation but not yet returned.

			op0 = ip0;
			return( 0 );
		}

		const int n = processChain( ip0, l, op0 );

		if( SeekPending.empty() )
		{
			return( n );
		}

		// Prepend the output samples retained by seekToOutput() that were
		// produced by the pre-roll but not yet returned to the caller.

		const size_t pn = SeekPending.size();
		SeekOutBuf.resize( pn + (size_t) n );

		size_t i;

		for( i = 0; i < pn; i++ )
		{
			SeekOutBuf[ i ] = SeekPending[ i ];
		}

		for( i = 0; i < (size_t) n; i++ )
		{
			SeekOutBuf[ pn + i ] = op0[ i ];
		}

		SeekPending.clear();
		op0 = &SeekOutBuf[ 0 ];

		return( (int) ( pn + (size_t) n ));
	}

	/**
	 * @brief Performs resampling of an input sample buffer of the specified
	 * length in the "one-shot" mode.
	 *
	 * This function can be useful when impulse response or time-series
	 * resampling is required.
	 *
	 * @param ip Input buffer pointer.
	 * @param iplen Length of the input buffer in samples.
	 * @param[out] op Output buffer pointer.
	 * @param oplen Length of the output buffer in samples.
	 * @tparam Tin Input buffer's element type.
	 * @tparam Tout Output buffer's element type.
	 */

	template< typename Tin, typename Tout >
	void oneshot( Tin* ip, int iplen, Tout* op, int oplen )
	{
		CFixedBuffer< double > Buf( MaxInLen );
		bool IsZero = false;

		while( oplen > 0 )
		{
			int rc;
			double* p;
			int i;

			if( iplen == 0 )
			{
				rc = MaxInLen;
				p = &Buf[ 0 ];

				if( !IsZero )
				{
					IsZero = true;
					memset( p, 0, MaxInLen * sizeof( p[ 0 ]));
				}
			}
			else
			{
				rc = min( iplen, MaxInLen );
				p = getOneshotBuf( ip, Buf, rc );
				ip += rc;
				iplen -= rc;
			}

			double* op0;
			int wc = process( p, rc, op0 );
			wc = min( oplen, wc );

			for( i = 0; i < wc; i++ )
			{
				op[ i ] = (Tout) op0[ i ];
			}

			op += wc;
			oplen -= wc;
		}

		clear();
	}

private:
	CPtrKeeper< CDSPProcessor > StepFirst; ///< The first processor in the
		///< singly-linked list.
	CDSPProcessor* StepLast; ///< The last processor in the singly-linked
		///< list.
	int MaxInLen; ///< Maximal input length.
	CFixedBuffer< double > TmpBufAll; ///< Buffer containing both temporary
		///< buffers.
	double* TmpBufs[ 2 ]; ///< Temporary output buffers.
	int TmpBufCapacities[ 2 ]; ///< Capacities of temporary buffers, updated
		///< during processing steps building.
	int CurTmpBuf; ///< Current temporary buffer.
	int CurMaxOutLen; ///< Current maximal output length.
	double LatencyFrac; ///< Current fractional latency. After object's
		///< construction, equals to the remaining fractional latency in the
		///< output.
	double SeekSrcRate; ///< Source sample rate, kept for seekToOutput().
	double SeekDstRate; ///< Destination sample rate, kept for seekToOutput().
	EDSPFilterPhaseResponse SeekPhase; ///< Filter's phase response, kept for
		///< the seekToOutput() range checks.
	bool SeekMode; ///< `true` if seekToOutput() was used and the object has
		///< not been cleared since. Enables strict zero-length process()
		///< semantics and the pending-sample queue.
	std::vector< double > SeekPending; ///< Output samples already produced by
		///< the pre-roll of a seekToOutput() call that lie at or after the
		///< requested output frame, but were not returned to the caller yet.
	std::vector< double > SeekOutBuf; ///< Holds the concatenation of pending
		///< samples and freshly processed samples, when required.

	/**
	 * @brief Adds processor, updates `MaxOutLen` variable and adjusts length
	 * of temporary internal buffers.
	 *
	 * @param Proc Processor to add. This pointer is inherited and will be
	 * destroyed on *this* object's destruction.
	 */

	void addProcessor( CDSPProcessor* const Proc )
	{
		if( StepFirst != R8B_NULL )
		{
			StepLast -> Next = Proc;
		}
		else
		{
			StepFirst = Proc;
		}

		StepLast = Proc;

		LatencyFrac = Proc -> getLatencyFrac();
		CurMaxOutLen = Proc -> getMaxOutLen( CurMaxOutLen );

		if( CurMaxOutLen > TmpBufCapacities[ CurTmpBuf ])
		{
			TmpBufCapacities[ CurTmpBuf ] = CurMaxOutLen;
		}

		CurTmpBuf ^= 1;
	}

	/**
	 * @brief Creates temporary buffers.
	 */
	void createTmpBuffers()
	{
		const int ol = TmpBufCapacities[ 0 ] + TmpBufCapacities[ 1 ];

		if( ol > 0 )
		{
			TmpBufAll.alloc( ol );
			TmpBufs[ 0 ] = &TmpBufAll[ 0 ];
			TmpBufs[ 1 ] = &TmpBufAll[ TmpBufCapacities[ 0 ]];
		}

		R8BCONSOLE( "* CDSPResampler: init done\n" );
	}

	/**
	 * @brief Runs the processor chain without the seek pending-sample
	 * handling.
	 *
	 * @param ip0 Input buffer pointer.
	 * @param l Input sample count.
	 * @param[out] op0 Output buffer pointer.
	 * @return The number of output samples produced.
	 */

	int processChain( double* ip0, int l, double*& op0 )
	{
		double* ip = ip0;
		int tp = 0;
		CDSPProcessor* ps = StepFirst;

		while( ps != R8B_NULL )
		{
			double* op = TmpBufs[ tp ];
			l = ps -> process( ip, l, op );
			ps = ps -> Next;
			ip = op;
			tp ^= 1;
		}

		op0 = ip;
		return( l );
	}

	/**
	 * @brief Returns the greatest common divisor of two positive integers.
	 *
	 * @param a First value.
	 * @param b Second value.
	 */

	static int64_t seekGCD( int64_t a, int64_t b )
	{
		while( b != 0 )
		{
			const int64_t t = a % b;
			a = b;
			b = t;
		}

		return( a );
	}

	/**
	 * @brief Retains output samples produced by the seekToOutput() pre-roll
	 * that lie at or after the requested output frame.
	 *
	 * @param op Output buffer pointer.
	 * @param Count Output sample count.
	 * @param K0 Absolute output frame of the first produced sample of the
	 * pre-roll.
	 * @param frame Requested output frame.
	 * @param[in,out] Produced Number of output samples produced so far by the
	 * pre-roll; increased by `Count` on return.
	 */

	void retainSeekOutput( const double* const op, const int Count,
		const int64_t K0, const int64_t frame, int64_t& Produced )
	{
		if( Count <= 0 )
		{
			return;
		}

		const int64_t First = K0 + Produced;

		if( First + Count > frame )
		{
			int64_t Start = 0;

			if( First < frame )
			{
				Start = frame - First;
			}

			int i;

			for( i = (int) Start; i < Count; i++ )
			{
				SeekPending.push_back( op[ i ]);
			}
		}

		Produced += Count;
	}

	/**
	 * @brief Auxiliary function for input buffer type convrsion in the
	 * oneshot() function.
	 *
	 * @param ip Input buffer pointer.
	 * @return `ip` verbatim, without type conversion.
	 */

	static double* getOneshotBuf( double* const ip, double* const, const int )
	{
		return( ip );
	}

	/**
	 * @brief Auxiliary function for input buffer type conversion in the
	 * oneshot() function.
	 *
	 * @param ip Input buffer pointer.
	 * @param Buf Temporary buffer.
	 * @param rc Input sample count.
	 * @return `Buf` filled with input samples converted from `float`.
	 */

	static double* getOneshotBuf( float* const ip, double* const Buf,
		const int rc )
	{
		int i;

		for( i = 0; i < rc; i++ )
		{
			Buf[ i ] = ip[ i ];
		}

		return( Buf );
	}
};

/**
 * @brief The resampler class for 16-bit resampling.
 *
 * This class defines resampling parameters suitable for 16-bit resampling,
 * using linear-phase low-pass filter. See the r8b::CDSPResampler class for
 * details.
 */

class CDSPResampler16 : public CDSPResampler
{
public:
	/**
	 * @brief Initializes the 16-bit resampler. See the r8b::CDSPResampler
	 * class for details.
	 *
	 * @param SrcSampleRate Source signal's sample rate.
	 * @param DstSampleRate Destination signal's sample rate.
	 * @param aMaxInLen The maximal planned length of the input buffer (in
	 * samples) that will be passed to the resampler.
	 * @param ReqTransBand Required transition band, in percent.
	 */

	CDSPResampler16( const double SrcSampleRate, const double DstSampleRate,
		const int aMaxInLen, const double ReqTransBand = 2.0 )
		: CDSPResampler( SrcSampleRate, DstSampleRate, aMaxInLen, ReqTransBand,
			136.45, fprLinearPhase )
	{
	}
};

/**
 * @brief The resampler class for 16-bit impulse response resampling.
 *
 * This class defines resampling parameters suitable for 16-bit impulse
 * response resampling, using linear-phase low-pass filter. Impulse responses
 * are non-dynamic signals, and thus need resampler with a lesser SNR. See the
 * r8b::CDSPResampler class for details.
 */

class CDSPResampler16IR : public CDSPResampler
{
public:
	/**
	 * @brief Initializes the 16-bit impulse response resampler. See the
	 * r8b::CDSPResampler class for details.
	 *
	 * @param SrcSampleRate Source signal's sample rate.
	 * @param DstSampleRate Destination signal's sample rate.
	 * @param aMaxInLen The maximal planned length of the input buffer (in
	 * samples) that will be passed to the resampler.
	 * @param ReqTransBand Required transition band, in percent.
	 */

	CDSPResampler16IR( const double SrcSampleRate, const double DstSampleRate,
		const int aMaxInLen, const double ReqTransBand = 2.0 )
		: CDSPResampler( SrcSampleRate, DstSampleRate, aMaxInLen, ReqTransBand,
			109.56, fprLinearPhase )
	{
	}
};

/**
 * @brief The resampler class for 24-bit resampling.
 *
 * This class defines resampling parameters suitable for 24-bit resampling
 * (including 32-bit floating point resampling), using linear-phase low-pass
 * filter. See the r8b::CDSPResampler class for details.
 */

class CDSPResampler24 : public CDSPResampler
{
public:
	/**
	 * @brief Initializes the 24-bit resampler (including 32-bit floating
	 * point). See the r8b::CDSPResampler class for details.
	 *
	 * @param SrcSampleRate Source signal's sample rate.
	 * @param DstSampleRate Destination signal's sample rate.
	 * @param aMaxInLen The maximal planned length of the input buffer (in
	 * samples) that will be passed to the resampler.
	 * @param ReqTransBand Required transition band, in percent.
	 */

	CDSPResampler24( const double SrcSampleRate, const double DstSampleRate,
		const int aMaxInLen, const double ReqTransBand = 2.0 )
		: CDSPResampler( SrcSampleRate, DstSampleRate, aMaxInLen, ReqTransBand,
			180.15, fprLinearPhase )
	{
	}
};

} // namespace r8b

#endif // R8B_CDSPRESAMPLER_INCLUDED
