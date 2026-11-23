// r8brain-free-src Copyright (c) 2013-2026 Aleksey Vaneev. MIT: ../LICENSE.
// Forward numerical reference only; not an implementation of the requested adjoint.
// Class body derived from CDSPFracInterpolator.h.
#pragma once
#include "../CDSPResampler.h"
#include "rational_clock.h"
namespace r8b {
class TaskRationalFracInterpolator : public CDSPProcessor
{
public:
	/**
	 * @brief Initalizes the interpolator. It is important to call the
	 * getMaxOutLen() function afterwards to obtain the optimal output buffer
	 * length.
	 *
	 * @param aSrcSampleRate Source sample rate.
	 * @param aDstSampleRate Destination sample rate.
	 * @param ReqAtten Required filter attentuation.
	 * @param IsThird `true` if one-third filter is required.
	 * @param PrevLatency Latency, in samples (any non-negative value), which
	 * was left in the output signal by a previous process. This latency will
	 * be consumed completely.
	 */

	TaskRationalFracInterpolator( const double aSrcSampleRate,
		const double aDstSampleRate, const double ReqAtten,
		const bool IsThird, const double PrevLatency )
		: SrcSampleRate( aSrcSampleRate )
		, DstSampleRate( aDstSampleRate )
		, FracStep( aSrcSampleRate / aDstSampleRate )
	{
		R8BASSERT( SrcSampleRate > 0.0 );
		R8BASSERT( DstSampleRate > 0.0 );
		R8BASSERT( PrevLatency >= 0.0 );
		R8BASSERT( BufLenBits >= 5 );

		InitFracPos = PrevLatency;
		Latency = (int) InitFracPos;
		InitFracPos -= Latency;

		R8BASSERT( Latency >= 0 );

	#if R8B_FLTTEST

		IsWhole = false;
		LatencyFrac = 0.0;
		FilterBank = new CDSPFracDelayFilterBank( -1, 3, 8, ReqAtten,
			IsThird );

	#else // R8B_FLTTEST

		IsWhole = getWholeStepping( SrcSampleRate, DstSampleRate, InStep,
			OutStep );

		if( IsWhole )
		{
			const double spos = InitFracPos * OutStep;
			InitFracPosW = (int) spos;
			LatencyFrac = ( spos - InitFracPosW ) / InStep;

			FilterBank = &CDSPFracDelayFilterBankCache :: getFilterBank(
				OutStep, 1, 2, ReqAtten, IsThird, false );
		}
		else
		{
			LatencyFrac = 0.0;
			FilterBank = &CDSPFracDelayFilterBankCache :: getFilterBank(
				-1, 3, 8, ReqAtten, IsThird, true );
		}

	#endif // R8B_FLTTEST

		FilterLen = FilterBank -> getFilterLen();
		fl2 = FilterLen >> 1;
		fll = fl2 - 1;
		flo = fll + fl2;
		flb = BufLen - fll;

		R8BASSERT(( 1 << BufLenBits ) >= FilterLen * 3 );

		static const CConvolveFn FltConvFn0[ 13 ] = {
			&TaskRationalFracInterpolator :: convolve0< 6 >,
			&TaskRationalFracInterpolator :: convolve0< 8 >,
			&TaskRationalFracInterpolator :: convolve0< 10 >,
			&TaskRationalFracInterpolator :: convolve0< 12 >,
			&TaskRationalFracInterpolator :: convolve0< 14 >,
			&TaskRationalFracInterpolator :: convolve0< 16 >,
			&TaskRationalFracInterpolator :: convolve0< 18 >,
			&TaskRationalFracInterpolator :: convolve0< 20 >,
			&TaskRationalFracInterpolator :: convolve0< 22 >,
			&TaskRationalFracInterpolator :: convolve0< 24 >,
			&TaskRationalFracInterpolator :: convolve0< 26 >,
			&TaskRationalFracInterpolator :: convolve0< 28 >,
			&TaskRationalFracInterpolator :: convolve0< 30 >
		};

		convfn = ( IsWhole ? FltConvFn0[ fl2 - 3 ] :
			&TaskRationalFracInterpolator :: convolve2 );

		R8BCONSOLE( "TaskRationalFracInterpolator: src=%.2f dst=%.2f taps=%i "
			"fracs=%i whole=%i third=%i step=%.6f\n", SrcSampleRate,
			DstSampleRate, FilterLen, ( IsWhole ? OutStep :
			FilterBank -> getFilterFracs() ), (int) IsWhole, (int) IsThird,
			aSrcSampleRate / aDstSampleRate );

		clear();
	}

	virtual int getInLenBeforeOutPos( int ReqOutPos ) const
	{
		if( Next != R8B_NULL )
		{
			ReqOutPos = Next -> getInLenBeforeOutPos( ReqOutPos );
		}

		const int ilat = fl2 + Latency;

		if( IsWhole )
		{
			return( ilat + (int) (( InitFracPosW +
				(double) ReqOutPos * InStep ) / OutStep +
				LatencyFrac * InStep / OutStep ));
		}

		return( ilat + (int) ( InitFracPos + ReqOutPos * SrcSampleRate /
			DstSampleRate ));
	}

	virtual int getLatency() const
	{
		return( 0 );
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return( (int) ceil( MaxInLen * DstSampleRate / SrcSampleRate ) + 1 );
	}

	virtual void clear()
	{
		ReferenceOutput = 0;
		LatencyLeft = Latency;
		BufLeft = 0;
		WritePos = 0;
		ReadPos = flb; // Set "read" position to account for filter's
			// latency at zero fractional delay.

		memset( &Buf[ ReadPos ], 0,
			(size_t) ( BufLen - flb ) * sizeof( Buf[ 0 ]));

		if( IsWhole )
		{
			InPosFracW = InitFracPosW;
		}
		else
		{
			InPosFrac = InitFracPos;
			InPosInt = 0;
			InPosShift = InitFracPos / SrcSampleRate * DstSampleRate;
		}
	}

	virtual int process( double* ip, int l, double*& op0 )
	{
		R8BASSERT( l >= 0 );
		R8BASSERT( ip != op0 || l == 0 || SrcSampleRate > DstSampleRate );

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= l )
			{
				LatencyLeft -= l;
				return( 0 );
			}

			l -= LatencyLeft;
			ip += LatencyLeft;
			LatencyLeft = 0;
		}

		double* op = op0;

		while( l > 0 )
		{
			// Copy new input samples to the ring buffer.

			const int b = min( l, min( BufLen - WritePos, flb - BufLeft ));

			double* const wp1 = Buf + WritePos;
			memcpy( wp1, ip, (size_t) b * sizeof( wp1[ 0 ]));
			const int ec = flo - WritePos;

			if( ec > 0 )
			{
				memcpy( wp1 + BufLen, ip,
					(size_t) min( b, ec ) * sizeof( wp1[ 0 ]));
			}

			ip += b;
			WritePos = ( WritePos + b ) & BufLenMask;
			l -= b;
			BufLeft += b;

			// Produce as many output samples as possible.

			op = ( *this.*convfn )( op );
		}

		return( (int) ( op - op0 ));
	}

private:
	std::uint64_t ReferenceOutput = 0;
	static const int BufLenBits = 8; ///< The length of the ring buffer,
		///< expressed as Nth power of 2. This value can be reduced if it is
		///< known that only short input buffers will be passed to the
		///< interpolator. The minimum value of this parameter is 5, and
		///< `1 << BufLenBits` should be at least 3 times larger than the
		///< FilterLen. However, this condition can be easily met if the input
		///< signal is suitably downsampled first before the interpolation is
		///< performed.
	static const int BufLen = 1 << BufLenBits; ///< The length of the ring
		///< buffer. The actual length is longer, to permit "beyond bounds"
		///< positioning.
	static const int BufLenMask = BufLen - 1; ///< Mask used for quick buffer
		///< position wrapping.
	double Buf[ BufLen + 29 ]; ///< The ring buffer, including overrun
		///< protection for maximal filter length.
	double SrcSampleRate; ///< Source sample rate.
	double DstSampleRate; ///< Destination sample rate.
	double InitFracPos; ///< Initial fractional position, in samples, in the
		///< range [0; 1).
	int InitFracPosW; ///< Initial fractional position for whole-number
		///< stepping.
	int Latency; ///< Initial latency that should be removed from the input.
	double LatencyFrac; ///< Left-over fractional latency on output (always
		///< zero for non-whole stepping).
	int FilterLen; ///< Filter length, in taps. Even value.
	int fll; ///< Input latency (left-hand filter length).
	int fl2; ///< Right-side (half) filter length.
	int flo; ///< Overrun length.
	int flb; ///< Initial buffer read position.
	int InStep; ///< Input whole-number stepping.
	int OutStep; ///< Output whole-number stepping (corresponds to filter bank
		///< size).
	int LatencyLeft; ///< Input latency left to remove.
	int BufLeft; ///< The number of samples left in the buffer to process.
	int WritePos; ///< The current buffer write position. Incremented together
		///< with the `BufLeft` variable.
	int ReadPos; ///< The current buffer read position.
	int InPosFracW; ///< Interpolation position (fractional part) for
		///< whole-number stepping. Corresponds to the index into the filter
		///< bank.
	int InPosInt; ///< Interpolation position (integer part).
	double InPosFrac; ///< Interpolation position (fractional part).
	double FracStep; ///< Fractional sample-timing step.
	double InPosShift; ///< Interpolation position's shift.

	#if !R8B_FLTTEST
	CRefKeeper< CDSPFracDelayFilterBank > FilterBank; ///< Filter bank in use,
		///< may be whole-number stepping filter bank or static bank.
	#else // !R8B_FLTTEST
	CPtrKeeper< CDSPFracDelayFilterBank > FilterBank;
	#endif // !R8B_FLTTEST

	bool IsWhole; ///< `true` if whole-number stepping is in use.

	typedef double*( TaskRationalFracInterpolator ::* CConvolveFn )( double* op ); ///<
		///< Convolution function type.
	CConvolveFn convfn; ///< Convolution function in use.

	/**
	 * @brief Convolution function for 0th order resampling.
	 *
	 * @param[out] op Output buffer.
	 * @tparam fltlen Filter length, in taps.
	 * @return Advanced "op" value.
	 */

	template< int fltlen >
	double* convolve0( double* op )
	{
		const CDSPFracDelayFilterBank& fb = *FilterBank;
		const int istep = InStep;
		const int ostep = OutStep;
		int fpos = InPosFracW;
		int rpos = ReadPos;
		int bl = BufLeft - fl2;

		while( bl > 0 )
		{
			const double* const ftp = &fb[ fpos ];
			const double* const rp = Buf + rpos;
			int i;

		#if defined( R8B_SSE2 ) && !defined( __INTEL_COMPILER )

			__m128d s = _mm_setzero_pd();

			for( i = 0; i < fltlen; i += 2 )
			{
				const __m128d m = _mm_mul_pd( _mm_load_pd( ftp + i ),
					_mm_loadu_pd( rp + i ));

				s = _mm_add_pd( s, m );
			}

			_mm_storel_pd( op, _mm_add_pd( s, _mm_shuffle_pd( s, s, 1 )));

		#elif defined( R8B_NEON )

			float64x2_t s = vdupq_n_f64( 0.0 );

			for( i = 0; i < fltlen; i += 2 )
			{
				s = vmlaq_f64( s, vld1q_f64( ftp + i ), vld1q_f64( rp + i ));
			}

			*op = vaddvq_f64( s );

		#else // SIMD

			double s = 0.0;

			for( i = 0; i < fltlen; i++ )
			{
				s += ftp[ i ] * rp[ i ];
			}

			*op = s;

		#endif // SIMD

			op++;

			fpos += istep;
			const int PosIncr = fpos / ostep;
			fpos -= PosIncr * ostep;

			rpos = ( rpos + PosIncr ) & BufLenMask;
			bl -= PosIncr;
		}

		BufLeft = bl + fl2;
		ReadPos = rpos;
		InPosFracW = fpos;

		return( op );
	}

	/**
	 * @brief Convolution function for 2nd order resampling.
	 *
	 * @param[out] op Output buffer.
	 * @return Advanced "op" value.
	 */

	double* convolve2( double* op )
	{
		const CDSPFracDelayFilterBank& fb = *FilterBank;
		const int fltlen = FilterLen;
		const double ffracs = fb.getFilterFracs();
		auto position = task_reference::phase_at(ReferenceOutput,
            static_cast<std::uint64_t>(SrcSampleRate),
            static_cast<std::uint64_t>(DstSampleRate), InitFracPos);
		double fpos = InPosFrac;
		int rpos = ReadPos;
		int bl = BufLeft - fl2;

		while( bl > 0 )
		{
			double x = fpos * ffracs;
			const int fti = (int) x; // Function table index.
			const double* ftp = &fb[ fti ];
			x -= fti; // Coefficient for interpolation between adjacent
				// fractional delay filters.
			const double* const rp = Buf + rpos;
			const double x2d = x * x;
			int i;

		#if defined( R8B_SSE2 ) && defined( R8B_SIMD_ISH )

			const __m128d x1 = _mm_set1_pd( x );
			const __m128d x2 = _mm_set1_pd( x2d );
			__m128d s = _mm_setzero_pd();

			for( i = 0; i < fltlen; i += 2 )
			{
				const __m128d ftp2 = _mm_load_pd( ftp + 2 );
				const __m128d xx1 = _mm_mul_pd( ftp2, x1 );
				const __m128d ftp4 = _mm_load_pd( ftp + 4 );
				const __m128d xx2 = _mm_mul_pd( ftp4, x2 );
				const __m128d ftp0 = _mm_load_pd( ftp );
				ftp += 6;

				const __m128d rpi = _mm_loadu_pd( rp + i );
				const __m128d xxs = _mm_add_pd( ftp0, _mm_add_pd( xx1, xx2 ));

				s = _mm_add_pd( s, _mm_mul_pd( rpi, xxs ));
			}

			_mm_storel_pd( op, _mm_add_pd( s, _mm_shuffle_pd( s, s, 1 )));

		#elif defined( R8B_NEON ) && defined( R8B_SIMD_ISH )

			const float64x2_t x1 = vdupq_n_f64( x );
			const float64x2_t x2 = vdupq_n_f64( x2d );
			float64x2_t s = vdupq_n_f64( 0.0 );

			for( i = 0; i < fltlen; i += 2 )
			{
				const float64x2_t ftp2 = vld1q_f64( ftp + 2 );
				const float64x2_t xx1 = vmulq_f64( ftp2, x1 );
				const float64x2_t ftp4 = vld1q_f64( ftp + 4 );
				const float64x2_t xx2 = vmulq_f64( ftp4, x2 );
				const float64x2_t ftp0 = vld1q_f64( ftp );
				ftp += 6;

				const float64x2_t rpi = vld1q_f64( rp + i );
				const float64x2_t xxs = vaddq_f64( ftp0,
					vaddq_f64( xx1, xx2 ));

				s = vmlaq_f64( s, rpi, xxs );
			}

			*op = vaddvq_f64( s );

		#else // SIMD

			double s = 0.0;

			for( i = 0; i < fltlen; i++ )
			{
				s += ( ftp[ 0 ] + ftp[ 1 ] * x + ftp[ 2 ] * x2d ) * rp[ i ];
				ftp += 3;
			}

			*op = s;

		#endif // SIMD

			op++;
            ++ReferenceOutput;
            const auto next = task_reference::phase_at(ReferenceOutput,
                static_cast<std::uint64_t>(SrcSampleRate),
                static_cast<std::uint64_t>(DstSampleRate), InitFracPos);
            const int PosIncr = static_cast<int>(next.integer - position.integer);
            fpos = next.fraction;
            position = next;

			bl -= PosIncr;
			rpos = ( rpos + PosIncr ) & BufLenMask;
		}

		BufLeft = bl + fl2;
		ReadPos = rpos;
		InPosFrac = fpos;

		return( op );
	}
};
}
// Reuse the unmodified production chain builder and all other DSP stages.
#undef R8B_CDSPRESAMPLER_INCLUDED
#define CDSPFracInterpolator TaskRationalFracInterpolator
#define CDSPResampler TaskRationalResampler
#define CDSPResampler16 TaskRationalResampler16
#define CDSPResampler16IR TaskRationalResampler16IR
#define CDSPResampler24 TaskRationalResampler24
#include "../CDSPResampler.h"
#undef CDSPFracInterpolator
#undef CDSPResampler
#undef CDSPResampler16
#undef CDSPResampler16IR
#undef CDSPResampler24
