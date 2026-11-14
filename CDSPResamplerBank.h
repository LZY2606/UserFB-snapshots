//$ nobt
//$ nocpp

/**
 * @file CDSPResamplerBank.h
 *
 * @brief Plane multi-channel sample rate converter with a shared scheduling
 * and interpolation control core.
 *
 * This file includes the r8b::CDSPResamplerBank class that performs sample
 * rate conversion of several synchronized channels at once. Unlike an array
 * of independent r8b::CDSPResampler objects, the bank drives all channels from
 * a single frame clock: the fractional interpolation position, filter phase
 * selection, polynomial coefficient evaluation, half-band odd/even stepping,
 * latency subtraction and block boundaries are computed exactly once per
 * output frame and then applied to each channel's independent history.
 *
 * r8brain-free-src Copyright (c) 2013-2026 Aleksey Vaneev
 *
 * See the "LICENSE" file for license.
 */

#ifndef R8B_CDSPRESAMPLERBANK_INCLUDED
#define R8B_CDSPRESAMPLERBANK_INCLUDED

#include "CDSPResampler.h"

/**
 * @brief Upper bound on the channel count accepted by CDSPResamplerBank.
 *
 * The processing kernels themselves have no channel limit, but fixed-size
 * stack pointer tables are used inside process() so that no heap memory is
 * allocated during processing.
 */

#define R8B_BANK_MAX_CHANNELS 32

namespace r8b {

/**
 * @brief Internal control-work counters of CDSPResamplerBank.
 *
 * The counters are incremented at the points where the corresponding control
 * work actually happens, independently of the number of channels being
 * processed. Because the bank performs this work once per shared frame clock,
 * equal input scheduling produces equal counts for any channel count.
 */

struct CDSPResamplerBankCounters
{
	/**
	 * @brief Number of fractional interpolator phase advances (one per
	 * produced fractional-stage output frame), across both whole-numbered and
	 * polynomial-interpolation stepping.
	 */

	long long PhaseAdvances;

	/**
	 * @brief Number of fractional filter phase selections with polynomial
	 * coefficient evaluation. Stays zero while the whole-numbered stepping
	 * filter bank is in use or when the pipeline has no fractional
	 * interpolator.
	 */

	long long FracCoeffEvals;

	/**
	 * @brief Number of half-band stage scheduling decisions (ring-buffer fill
	 * steps, direct-convolution switches and odd/even parity fills).
	 */

	long long HBSchedules;

	CDSPResamplerBankCounters()
	{
		reset();
	}

	void reset()
	{
		PhaseAdvances = 0;
		FracCoeffEvals = 0;
		HBSchedules = 0;
	}
};

// ---------------------------------------------------------------------------
// Internal bank pipeline stages
// ---------------------------------------------------------------------------

/**
 * @brief Abstract base class of a shared-scheduling bank pipeline stage.
 *
 * A stage keeps all scalar scheduling state in *this* shared object while the
 * per-channel audio history resides in separately allocated channel-state
 * blocks. All stage memory (shared objects, channel states, intermediate
 * buffers) is allocated while the owning bank is being constructed: process()
 * and clear() never allocate nor free heap memory.
 */

class CBankStage : public R8B_BASECLASS
{
	R8BNOCTOR( CBankStage )

public:
	/**
	 * @brief Offset, in frames, at which the valid output of the latest
	 * process() call starts inside the supplied output buffers. Non-zero only
	 * when a stage consumes output latency by trimming the front of its output
	 * buffer. Identical for all channels because latency is driven by the
	 * shared frame clock.
	 */

	int LastOutOffset;

	CBankStage()
		: LastOutOffset( 0 )
	{
	}

	virtual ~CBankStage()
	{
	}

	/**
	 * @brief Returns fractional latency, matching CDSPProcessor's function.
	 */

	virtual double getLatencyFrac() const = 0;

	/**
	 * @brief Returns the size, in bytes, of one channel's state block.
	 */

	virtual int getChannelStateBytes() const = 0;

	/**
	 * @brief Placement-constructs one channel's state block.
	 */

	virtual void initChannel( void* const state ) = 0;

	/**
	 * @brief Destroys a channel's state block previously constructed by
	 * initChannel().
	 */

	virtual void freeChannel( void* const state ) = 0;

	/**
	 * @brief Resets the shared scheduling state and all channels' history.
	 */

	virtual void clear( const int ChCount, void* const* const states ) = 0;

	/**
	 * @brief Processes one common block of `l` frames on all channels.
	 *
	 * @param ChCount Number of channels.
	 * @param states Per-channel state blocks.
	 * @param ip Per-channel input buffer pointers.
	 * @param l Common input frame count.
	 * @param op Per-channel output buffer pointers, each sized for
	 * getMaxOutLen( l ) frames.
	 * @param ctr Control-work counters to increment where work happens.
	 * @return The common output frame count, identical for all channels.
	 */

	virtual int process( const int ChCount, void* const* const states,
		double* const* const ip, const int l, double* const* const op,
		CDSPResamplerBankCounters& ctr ) = 0;

	/**
	 * @brief Returns the maximal output length for the given input length.
	 */

	virtual int getMaxOutLen( const int MaxInLen ) const = 0;
};

/**
 * @brief Shared fractional interpolator stage.
 *
 * The ring-buffer fill, fractional position, phase selection and polynomial
 * coefficient evaluation are all driven by a shared clock: per output frame
 * the phase is advanced and the active filter's coefficients are resolved
 * exactly once, then applied to every channel's independent ring buffer.
 */

class CBankFracStage : public CBankStage
{
public:
	CBankFracStage( const double aSrcSampleRate,
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
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual int getChannelStateBytes() const
	{
		return( sizeof( CChannel ));
	}

	virtual void initChannel( void* const state )
	{
		:: new( state ) CChannel();
	}

	virtual void freeChannel( void* const state )
	{
		((CChannel*) state) -> ~CChannel();
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return( (int) ceil( MaxInLen * DstSampleRate / SrcSampleRate ) + 1 );
	}

	virtual void clear( const int ChCount, void* const* const states )
	{
		LastOutOffset = 0;
		LatencyLeft = Latency;
		BufLeft = 0;
		WritePos = 0;
		ReadPos = flb;

		for( int c = 0; c < ChCount; c++ )
		{
			CChannel* const ch = (CChannel*) states[ c ];

			memset( &ch -> Buf[ ReadPos ], 0,
				(size_t) ( BufLen - flb ) * sizeof( ch -> Buf[ 0 ]));
		}

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

	virtual int process( const int ChCount, void* const* const states,
		double* const* const ip, const int l0, double* const* const op,
		CDSPResamplerBankCounters& ctr )
	{
		R8BASSERT( l0 >= 0 );
		LastOutOffset = 0;

		double* inp[ R8B_BANK_MAX_CHANNELS ];
		R8BASSERT( ChCount <= R8B_BANK_MAX_CHANNELS );

		for( int c = 0; c < ChCount; c++ )
		{
			inp[ c ] = ip[ c ];
		}

		int l = l0;

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= l )
			{
				LatencyLeft -= l;
				return( 0 );
			}

			l -= LatencyLeft;

			for( int c = 0; c < ChCount; c++ )
			{
				inp[ c ] += LatencyLeft;
			}

			LatencyLeft = 0;
		}

		int OutCount = 0;

		while( l > 0 )
		{
			// Copy new input samples to every channel's ring buffer, using the
			// shared write position.

			const int b = min( l, min( BufLen - WritePos, flb - BufLeft ));

			for( int c = 0; c < ChCount; c++ )
			{
				CChannel* const ch = (CChannel*) states[ c ];
				double* const wp1 = ch -> Buf + WritePos;

				memcpy( wp1, inp[ c ], (size_t) b * sizeof( wp1[ 0 ]));

				const int ec = flo - WritePos;

				if( ec > 0 )
				{
					memcpy( wp1 + BufLen, inp[ c ],
						(size_t) min( b, ec ) * sizeof( wp1[ 0 ]));
				}

				inp[ c ] += b;
			}

			WritePos = ( WritePos + b ) & BufLenMask;
			l -= b;
			BufLeft += b;

			if( IsWhole )
			{
				OutCount += convolveWhole( ChCount, states, op, OutCount,
					ctr );
			}
			else
			{
				OutCount += convolvePoly( ChCount, states, op, OutCount,
					ctr );
			}
		}

		if( !IsWhole && (int) InPosShift > 1000 )
		{
			// Reset the interpolation position counter to achieve a higher
			// sample-timing precision.

			InPosInt = 0;
			InPosShift = InPosFrac / SrcSampleRate * DstSampleRate;
		}

		return( OutCount );
	}

private:
	static const int BufLenBits = 8;
	static const int BufLen = 1 << BufLenBits;
	static const int BufLenMask = BufLen - 1;

	struct CChannel
	{
		double Buf[ BufLen + 29 ]; ///< Ring buffer including overrun
			///< protection for maximal filter length.
	};

	double SrcSampleRate;
	double DstSampleRate;
	double InitFracPos;
	int InitFracPosW;
	int Latency;
	double LatencyFrac;
	int FilterLen;
	int fll;
	int fl2;
	int flo;
	int flb;
	int InStep;
	int OutStep;
	int LatencyLeft;
	int BufLeft;
	int WritePos;
	int ReadPos;
	int InPosFracW;
	int InPosInt;
	double InPosFrac;
	double FracStep;
	double InPosShift;

	#if !R8B_FLTTEST
	CRefKeeper< CDSPFracDelayFilterBank > FilterBank;
	#else // !R8B_FLTTEST
	CPtrKeeper< CDSPFracDelayFilterBank > FilterBank;
	#endif // !R8B_FLTTEST

	bool IsWhole;

	/**
	 * @brief Whole-numbered stepping convolution (0th-order filter bank).
	 * The phase advances once per output frame, the filter pointer is resolved
	 * once per frame and then applied to every channel.
	 */

	int convolveWhole( const int ChCount, void* const* const states,
		double* const* const op, const int OutStart,
		CDSPResamplerBankCounters& ctr )
	{
		const CDSPFracDelayFilterBank& fb = *FilterBank;
		const int istep = InStep;
		const int ostep = OutStep;
		int fpos = InPosFracW;
		int rpos = ReadPos;
		int bl = BufLeft - fl2;
		int oc = 0;

		while( bl > 0 )
		{
			const double* const ftp = &fb[ fpos ];
			const int fltlen = FilterLen;

			for( int c = 0; c < ChCount; c++ )
			{
				const CChannel* const ch = (const CChannel*) states[ c ];
				const double* const rp = ch -> Buf + rpos;
				double s = 0.0;

				for( int i = 0; i < fltlen; i++ )
				{
					s += ftp[ i ] * rp[ i ];
				}

				op[ c ][ OutStart + oc ] = s;
			}

			oc++;
			ctr.PhaseAdvances++;

			fpos += istep;
			const int PosIncr = fpos / ostep;
			fpos -= PosIncr * ostep;

			rpos = ( rpos + PosIncr ) & BufLenMask;
			bl -= PosIncr;
		}

		BufLeft = bl + fl2;
		ReadPos = rpos;
		InPosFracW = fpos;

		return( oc );
	}

	/**
	 * @brief 2nd-order polynomial fractional interpolation. The fractional
	 * filter phase and the polynomial coefficients actually in use are
	 * evaluated once per output frame, then applied to each channel's history.
	 */

	int convolvePoly( const int ChCount, void* const* const states,
		double* const* const op, const int OutStart,
		CDSPResamplerBankCounters& ctr )
	{
		const CDSPFracDelayFilterBank& fb = *FilterBank;
		const int fltlen = FilterLen;
		const double ffracs = fb.getFilterFracs();
		const double fs = FracStep;
		int ipos = InPosInt;
		double fpos = InPosFrac;
		double psh = InPosShift;
		int rpos = ReadPos;
		int bl = BufLeft - fl2;
		int oc = 0;

		while( bl > 0 )
		{
			double x = fpos * ffracs;
			const int fti = (int) x; // Function table index.
			const double* ftp = &fb[ fti ];
			x -= fti; // Coefficient for interpolation between adjacent
				// fractional delay filters.
			const double x2d = x * x;

			ctr.FracCoeffEvals++;

			for( int c = 0; c < ChCount; c++ )
			{
				const CChannel* const ch = (const CChannel*) states[ c ];
				const double* const rp = ch -> Buf + rpos;
				const double* fp = ftp;
				double s = 0.0;

				for( int i = 0; i < fltlen; i++ )
				{
					s += ( fp[ 0 ] + fp[ 1 ] * x + fp[ 2 ] * x2d ) * rp[ i ];
					fp += 3;
				}

				op[ c ][ OutStart + oc ] = s;
			}

			oc++;
			ctr.PhaseAdvances++;

			psh += 1.0;

			const double NextInPos = psh * fs;
			const int NextInPosInt = (int) NextInPos;
			const int PosIncr = NextInPosInt - ipos;

			fpos = NextInPos - NextInPosInt;
			ipos = NextInPosInt;

			bl -= PosIncr;
			rpos = ( rpos + PosIncr ) & BufLenMask;
		}

		BufLeft = bl + fl2;
		ReadPos = rpos;
		InPosInt = ipos;
		InPosFrac = fpos;
		InPosShift = psh;

		return( oc );
	}
};

/**
 * @brief Shared half-band 2X upsampler stage.
 *
 * Odd/even output production, ring-buffer fill and the direct-convolution
 * switch are scheduled once from the shared frame clock; the sparse
 * convolutions run per channel over independent ring histories.
 */

class CBankHBUpStage : public CBankStage
{
public:
	CBankHBUpStage( const double ReqAtten, const int SteepIndex,
		const bool IsThird, const double PrevLatency,
		const bool aDoConsumeLatency = true )
		: DoConsumeLatency( aDoConsumeLatency )
	{
		const double* fltp0;
		int fltt;
		double att;

		if( IsThird )
		{
			CDSPHBUpsampler :: getHBFilterThird( ReqAtten, SteepIndex, fltp0,
				fltt, att );
		}
		else
		{
			CDSPHBUpsampler :: getHBFilter( ReqAtten, SteepIndex, fltp0, fltt,
				att );
		}

		R8BASSERT( fltt <= FltBufLen );
		memcpy( FilterTaps, fltp0, (size_t) fltt * sizeof( double ));

		FilterLen = fltt;
		convfn = getConvFns()[ fltt - 1 ];

		fll = fltt - 1;
		fl2 = fltt;
		flo = fll + fl2;

		LatencyFrac = PrevLatency * 2.0;
		Latency = (int) LatencyFrac;
		LatencyFrac -= Latency;

		R8BASSERT( Latency >= 0 );

		if( DoConsumeLatency )
		{
			flb = BufLen - fll;
		}
		else
		{
			Latency += fl2 + fl2;
			flb = BufLen - flo;
		}
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual int getChannelStateBytes() const
	{
		return( sizeof( CChannel ));
	}

	virtual void initChannel( void* const state )
	{
		CChannel* const ch = :: new( state ) CChannel();

		ch -> fltp = align_ptr( ch -> FltBuf, 16 );
		memcpy( ch -> fltp, FilterTaps,
			(size_t) FilterLen * sizeof( double ));

		ch -> BufRP = ch -> Buf + fll;
	}

	virtual void freeChannel( void* const state )
	{
		((CChannel*) state) -> ~CChannel();
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return( MaxInLen * 2 );
	}

	virtual void clear( const int ChCount, void* const* const states )
	{
		LastOutOffset = 0;

		if( DoConsumeLatency )
		{
			LatencyLeft = Latency;
			BufLeft = 0;
		}
		else
		{
			LatencyLeft = 0;
			BufLeft = fl2;
		}

		WritePos = 0;
		ReadPos = flb & BufLenMask;

		for( int c = 0; c < ChCount; c++ )
		{
			CChannel* const ch = (CChannel*) states[ c ];

			memset( &ch -> Buf[ ReadPos ], 0,
				(size_t) ( BufLen - flb ) * sizeof( ch -> Buf[ 0 ]));
		}
	}

	virtual int process( const int ChCount, void* const* const states,
		double* const* const ip, const int l0, double* const* const op,
		CDSPResamplerBankCounters& ctr )
	{
		R8BASSERT( l0 >= 0 );
		LastOutOffset = 0;

		double* inp[ R8B_BANK_MAX_CHANNELS ];
		R8BASSERT( ChCount <= R8B_BANK_MAX_CHANNELS );

		for( int c = 0; c < ChCount; c++ )
		{
			inp[ c ] = ip[ c ];
		}

		int l = l0;
		int OutCount = 0;
		int ac = 0;

		while( l > 0 && ac < flo )
		{
			const int b = min( min( l, flo - ac ),
				min( BufLen - WritePos, flb - BufLeft ));

			for( int c = 0; c < ChCount; c++ )
			{
				CChannel* const ch = (CChannel*) states[ c ];
				double* const wp1 = ch -> Buf + WritePos;

				memcpy( wp1, inp[ c ], (size_t) b * sizeof( wp1[ 0 ]));

				const int ec = flo - WritePos;

				if( ec > 0 )
				{
					memcpy( wp1 + BufLen, inp[ c ],
						(size_t) min( b, ec ) * sizeof( wp1[ 0 ]));
				}

				inp[ c ] += b;
			}

			WritePos = ( WritePos + b ) & BufLenMask;
			l -= b;
			BufLeft += b;
			ac += b;

			// Shared production schedule for the ring-buffered region.

			int c = BufLeft - fl2;
			int cbTotal = 0;
			int prodOut = 0;
			int scheduleReadPos = ReadPos;
			int scheduleBufLeft = BufLeft;

			while( c > 0 )
			{
				const int cb = min( c, BufLen - scheduleReadPos );

				for( int chidx = 0; chidx < ChCount; chidx++ )
				{
					CChannel* const ch = (CChannel*) states[ chidx ];
					double* const outp =
						op[ chidx ] + OutCount + prodOut;

					convfn( outp, outp + cb * 2, ch -> fltp,
						ch -> BufRP + scheduleReadPos );
				}

				scheduleReadPos =
					( scheduleReadPos + cb ) & BufLenMask;
				scheduleBufLeft -= cb;
				c -= cb;
				cbTotal += cb;
				prodOut += cb * 2;
			}

			ReadPos = scheduleReadPos;
			BufLeft = scheduleBufLeft;
			OutCount += cbTotal * 2;
			ctr.HBSchedules++;
		}

		if( l > 0 )
		{
			R8BASSERT( ac == flo );

			// The convolution window is wholly within the input: convolve
			// directly, without the ring buffer.

			for( int c = 0; c < ChCount; c++ )
			{
				CChannel* const ch = (CChannel*) states[ c ];
				double* const outp = op[ c ] + OutCount;

				convfn( outp, outp + l * 2, ch -> fltp, inp[ c ] - fl2 );
			}

			OutCount += l * 2;

			for( int c = 0; c < ChCount; c++ )
			{
				CChannel* const ch = (CChannel*) states[ c ];

				// Rebase the ring to the last `flo` input samples. The
				// direct segment occupies `l` samples starting at inp[c], so
				// its end is inp[c] + l (matching the mono implementation
				// which advances the input pointer before rebasing).

				memcpy( ch -> Buf, inp[ c ] + l - flo,
					(size_t) flo * sizeof( ch -> Buf[ 0 ]));

				inp[ c ] += l;
			}

			ReadPos = 0;
			WritePos = flo;
			BufLeft = fl2;
			ctr.HBSchedules++;
		}

		int ol = OutCount;

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= ol )
			{
				LatencyLeft -= ol;
				return( 0 );
			}

			ol -= LatencyLeft;
			LastOutOffset = LatencyLeft;
			LatencyLeft = 0;
		}

		return( ol );
	}

private:
	typedef void( *CConvolveFn )( double* op, double* const opend,
		const double* const flt, const double* rp );

	// Scalar portable implementations of the sparse symmetric half-band
	// upsampling convolutions (identical arithmetic to the non-SIMD branch of
	// CDSPHBUpsampler.inc).

	template< int fltt >
	static void convN( double* op, double* const opend,
		const double* const flt, const double* rp )
	{
		while( op != opend )
		{
			op[ 0 ] = rp[ 0 ]; // Even output is the direct input sample.

			double s = 0.0;

			for( int k = 0; k < fltt; k++ )
			{
				s += flt[ k ] * ( rp[ k + 1 ] + rp[ -k ]);
			}

			op[ 1 ] = s;
			op += 2;
			rp++;
		}
	}

	static const CConvolveFn* getConvFns()
	{
		static const CConvolveFn Fns[ 14 ] = {
			&CBankHBUpStage :: convN< 1 >,
			&CBankHBUpStage :: convN< 2 >,
			&CBankHBUpStage :: convN< 3 >,
			&CBankHBUpStage :: convN< 4 >,
			&CBankHBUpStage :: convN< 5 >,
			&CBankHBUpStage :: convN< 6 >,
			&CBankHBUpStage :: convN< 7 >,
			&CBankHBUpStage :: convN< 8 >,
			&CBankHBUpStage :: convN< 9 >,
			&CBankHBUpStage :: convN< 10 >,
			&CBankHBUpStage :: convN< 11 >,
			&CBankHBUpStage :: convN< 12 >,
			&CBankHBUpStage :: convN< 13 >,
			&CBankHBUpStage :: convN< 14 >
		};

		return( Fns );
	}

	static const int BufLenBits = 7;
	static const int BufLen = 1 << BufLenBits;
	static const int BufLenMask = BufLen - 1;
	static const int FltBufLen = 14;

	struct CChannel
	{
		double Buf[ BufLen + ( FltBufLen * 2 - 1 )];
		double FltBuf[ FltBufLen + 2 ];
		const double* BufRP;
		double* fltp;
	};

	double FilterTaps[ FltBufLen ]; ///< Copy of the selected filter taps, used
		///< for per-channel initialization.
	int FilterLen;
	CConvolveFn convfn;
	double LatencyFrac;
	int Latency;
	int fll;
	int fl2;
	int flo;
	int flb;
	int LatencyLeft;
	int BufLeft;
	int WritePos;
	int ReadPos;
	bool DoConsumeLatency;
};

/**
 * @brief Shared half-band 2X downsampler stage.
 *
 * The odd/even parity fill of the two ring buffers and the output production
 * are scheduled once from the shared frame clock; the sparse convolutions run
 * per channel.
 */

class CBankHBDownStage : public CBankStage
{
public:
	CBankHBDownStage( const double ReqAtten, const int SteepIndex,
		const bool IsThird, const double PrevLatency )
	{
		const double* fltp0;
		int fltt;
		double att;

		if( IsThird )
		{
			CDSPHBUpsampler :: getHBFilterThird( ReqAtten, SteepIndex, fltp0,
				fltt, att );
		}
		else
		{
			CDSPHBUpsampler :: getHBFilter( ReqAtten, SteepIndex, fltp0, fltt,
				att );
		}

		R8BASSERT( fltt <= FltBufLen );
		memcpy( FilterTaps, fltp0, (size_t) fltt * sizeof( double ));

		FilterLen = fltt;
		convfn = getConvFns()[ fltt - 1 ];

		fll = fltt;
		fl2 = fltt - 1;
		flo = fll + fl2;
		flb = BufLen - fll;

		LatencyFrac = PrevLatency * 0.5;
		Latency = (int) LatencyFrac;
		LatencyFrac -= Latency;

		R8BASSERT( Latency >= 0 );
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual int getChannelStateBytes() const
	{
		return( sizeof( CChannel ));
	}

	virtual void initChannel( void* const state )
	{
		CChannel* const ch = :: new( state ) CChannel();

		ch -> fltp = align_ptr( ch -> FltBuf, 16 );
		memcpy( ch -> fltp, FilterTaps,
			(size_t) FilterLen * sizeof( double ));

		ch -> BufRP = ch -> Buf + fll;
	}

	virtual void freeChannel( void* const state )
	{
		((CChannel*) state) -> ~CChannel();
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return(( MaxInLen + 1 ) >> 1 );
	}

	virtual void clear( const int ChCount, void* const* const states )
	{
		LastOutOffset = 0;
		LatencyLeft = Latency;
		BufLeft = 0;
		WritePos1 = 0;
		WritePos2 = 0;
		ReadPos = flb;

		for( int c = 0; c < ChCount; c++ )
		{
			CChannel* const ch = (CChannel*) states[ c ];

			memset( &ch -> Buf[ ReadPos ], 0,
				(size_t) ( BufLen - flb ) * sizeof( ch -> Buf[ 0 ]));

			memset( &ch -> Buf[ BufLenPad + ReadPos ], 0,
				(size_t) ( BufLen - flb ) * sizeof( ch -> Buf[ 0 ]));
		}
	}

	virtual int process( const int ChCount, void* const* const states,
		double* const* const ip, const int l0, double* const* const op,
		CDSPResamplerBankCounters& ctr )
	{
		R8BASSERT( l0 >= 0 );
		LastOutOffset = 0;

		double* inp[ R8B_BANK_MAX_CHANNELS ];
		R8BASSERT( ChCount <= R8B_BANK_MAX_CHANNELS );

		for( int c = 0; c < ChCount; c++ )
		{
			inp[ c ] = ip[ c ];
		}

		int l = l0;
		int OutCount = 0;

		while( l > 0 )
		{
			if( WritePos1 != WritePos2 )
			{
				// Shared parity decision: put a single sample to buffer 2.

				for( int c = 0; c < ChCount; c++ )
				{
					CChannel* const ch = (CChannel*) states[ c ];
					double* const wp2 = ch -> Buf + BufLenPad + WritePos2;

					*wp2 = inp[ c ][ 0 ];

					if( WritePos2 < flo )
					{
						wp2[ BufLen ] = inp[ c ][ 0 ];
					}

					inp[ c ]++;
				}

				WritePos2 = WritePos1;
				l--;
				BufLeft++;
			}

			const int b1 = min(( l + 1 ) >> 1,
				min( BufLen - WritePos1, flb - BufLeft ));
			const int b2 = b1 - ( b1 * 2 > l );
			const int ec = flo - WritePos1;

			for( int c = 0; c < ChCount; c++ )
			{
				CChannel* const ch = (CChannel*) states[ c ];
				double* const wp1 = ch -> Buf + WritePos1;
				double* const wp2 = ch -> Buf + BufLenPad + WritePos1;
				double* ipp = inp[ c ];

				for( int i = 0; i < b2; i++ )
				{
					wp1[ i ] = ipp[ 0 ];
					wp2[ i ] = ipp[ 1 ];
					ipp += 2;
				}

				if( b1 != b2 )
				{
					wp1[ b2 ] = *ipp;
					ipp++;
				}

				if( ec > 0 )
				{
					memcpy( wp1 + BufLen, wp1,
						(size_t) min( b1, ec ) * sizeof( wp1[ 0 ]));

					memcpy( wp2 + BufLen, wp2,
						(size_t) min( b2, ec ) * sizeof( wp2[ 0 ]));
				}

				inp[ c ] = ipp;
			}

			WritePos1 = ( WritePos1 + b1 ) & BufLenMask;
			WritePos2 = ( WritePos2 + b2 ) & BufLenMask;
			l -= b1 + b2;
			BufLeft += b2;

			// Shared production schedule.

			int c = BufLeft - fl2;
			int cbTotal = 0;
			int prodOut = 0;
			int scheduleReadPos = ReadPos;
			int scheduleBufLeft = BufLeft;

			while( c > 0 )
			{
				const int cb = min( c, BufLen - scheduleReadPos );

				for( int chidx = 0; chidx < ChCount; chidx++ )
				{
					CChannel* const ch = (CChannel*) states[ chidx ];
					double* const outp =
						op[ chidx ] + OutCount + prodOut;

					convfn( outp, outp + cb, ch -> fltp,
						ch -> BufRP + scheduleReadPos );
				}

				scheduleReadPos =
					( scheduleReadPos + cb ) & BufLenMask;
				scheduleBufLeft -= cb;
				c -= cb;
				cbTotal += cb;
				prodOut += cb;
			}

			ReadPos = scheduleReadPos;
			BufLeft = scheduleBufLeft;
			OutCount += cbTotal;
			ctr.HBSchedules++;
		}

		int ol = OutCount;

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= ol )
			{
				LatencyLeft -= ol;
				return( 0 );
			}

			ol -= LatencyLeft;
			LastOutOffset = LatencyLeft;
			LatencyLeft = 0;
		}

		return( ol );
	}

private:
	typedef void( *CConvolveFn )( double* op, double* const opend,
		const double* const flt, const double* rp1 );

	// Scalar portable sparse symmetric half-band downsampling convolutions,
	// matching the non-SIMD branch of CDSPHBDownsampler.inc.

	template< int fltt >
	static void convN( double* op, double* const opend,
		const double* const flt, const double* rp1 )
	{
		while( op != opend )
		{
			const double* const rp = rp1 + BufLenPad - 1;
			double s = rp1[ 0 ];

			for( int k = 0; k < fltt; k++ )
			{
				s += flt[ k ] * ( rp[ k + 1 ] + rp[ -k ]);
			}

			*op = s;
			op++;
			rp1++;
		}
	}

	static const CConvolveFn* getConvFns()
	{
		static const CConvolveFn Fns[ 14 ] = {
			&CBankHBDownStage :: convN< 1 >,
			&CBankHBDownStage :: convN< 2 >,
			&CBankHBDownStage :: convN< 3 >,
			&CBankHBDownStage :: convN< 4 >,
			&CBankHBDownStage :: convN< 5 >,
			&CBankHBDownStage :: convN< 6 >,
			&CBankHBDownStage :: convN< 7 >,
			&CBankHBDownStage :: convN< 8 >,
			&CBankHBDownStage :: convN< 9 >,
			&CBankHBDownStage :: convN< 10 >,
			&CBankHBDownStage :: convN< 11 >,
			&CBankHBDownStage :: convN< 12 >,
			&CBankHBDownStage :: convN< 13 >,
			&CBankHBDownStage :: convN< 14 >
		};

		return( Fns );
	}

	static const int BufLenBits = 10;
	static const int BufLen = 1 << BufLenBits;
	static const int BufLenMask = BufLen - 1;
	static const int FltBufLen = 14;
	static const int BufLenPad = BufLen + ( FltBufLen * 2 - 1 );

	struct CChannel
	{
		double Buf[ BufLenPad * 2 ];
		double FltBuf[ FltBufLen + 2 ];
		const double* BufRP;
		double* fltp;
	};

	double FilterTaps[ FltBufLen ];
	int FilterLen;
	CConvolveFn convfn;
	double LatencyFrac;
	int Latency;
	int fll;
	int fl2;
	int flo;
	int flb;
	int LatencyLeft;
	int BufLeft;
	int WritePos1;
	int WritePos2;
	int ReadPos;
};

/**
 * @brief Shared overlap-save block convolver stage.
 *
 * Block boundaries, latency subtraction and whole-numbered up/down sampling
 * skips are scheduled once from the shared frame clock. The FFT itself runs
 * independently per channel, as permitted by the bank's design.
 */

class CBankBlockStage : public CBankStage
{
public:
	CBankBlockStage( CDSPFIRFilter& aFilter, const int aUpFactor,
		const int aDownFactor, const double PrevLatency = 0.0,
		const bool aDoConsumeLatency = true )
		: Filter( &aFilter )
		, UpFactor( aUpFactor )
		, DownFactor( aDownFactor )
		, BlockLen2( 2 << Filter -> getBlockLenBits() )
		, DoConsumeLatency( aDoConsumeLatency )
	{
		R8BASSERT( UpFactor > 0 );
		R8BASSERT( DownFactor > 0 );
		R8BASSERT( PrevLatency >= 0.0 );

		int fftinBits;
		UpShift = getBitOccupancy( UpFactor ) - 1;

		if(( 1 << UpShift ) == UpFactor )
		{
			fftinBits = Filter -> getBlockLenBits() + 1 - UpShift;
			PrevInputLen = ( Filter -> getKernelLen() - 1 + UpFactor - 1 ) /
				UpFactor;

			InputLen = BlockLen2 - PrevInputLen * UpFactor;
		}
		else
		{
			UpShift = -1;
			fftinBits = Filter -> getBlockLenBits() + 1;
			PrevInputLen = Filter -> getKernelLen() - 1;
			InputLen = BlockLen2 - PrevInputLen;
		}

		OutOffset = ( Filter -> isZeroPhase() ? Filter -> getLatency() : 0 );
		LatencyFrac = Filter -> getLatencyFrac() + PrevLatency * UpFactor;
		Latency = (int) LatencyFrac;
		const int InLatency = Latency + Filter -> getLatency() - OutOffset;
		LatencyFrac -= Latency;
		LatencyFrac /= DownFactor;

		Latency += InputLen + Filter -> getLatency();

		int fftoutBits;
		InputDelay = 0;
		DownSkipInit = 0;
		DownShift = getBitOccupancy( DownFactor ) - 1;

		if(( 1 << DownShift ) == DownFactor )
		{
			fftoutBits = Filter -> getBlockLenBits() + 1 - DownShift;

			if( DownFactor > 1 )
			{
				if( UpShift > 0 )
				{
					R8BASSERT( UpShift == 0 );
				}
				else
				{
					const int ilc = InputLen & ( DownFactor - 1 );
					PrevInputLen += ilc;
					InputLen -= ilc;
					Latency -= ilc;

					const int lc = InLatency & ( DownFactor - 1 );

					if( lc > 0 )
					{
						InputDelay = DownFactor - lc;
					}

					if( !DoConsumeLatency )
					{
						Latency /= DownFactor;
					}
				}
			}
		}
		else
		{
			fftoutBits = Filter -> getBlockLenBits() + 1;
			DownShift = -1;

			if( !DoConsumeLatency && DownFactor > 1 )
			{
				DownSkipInit = Latency % DownFactor;
				Latency /= DownFactor;
			}
		}

		R8BASSERT( Latency >= 0 );

		FftInBits = fftinBits;
		FftOutBits = fftoutBits;
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual int getChannelStateBytes() const
	{
		return( sizeof( CChannel ));
	}

	virtual void initChannel( void* const state )
	{
		CChannel* const ch = :: new( state ) CChannel();

		ch -> fftin.init( FftInBits );

		if( FftOutBits == FftInBits )
		{
			ch -> fftout = &ch -> fftin;
		}
		else
		{
			ch -> ffto2.init( FftOutBits );
			ch -> fftout = &ch -> ffto2;
		}

		ch -> WorkBlocks.alloc( BlockLen2 * 2 + PrevInputLen );
		ch -> CurInput = &ch -> WorkBlocks[ 0 ];
		ch -> CurOutput = &ch -> WorkBlocks[ BlockLen2 ];
		ch -> PrevInput = &ch -> WorkBlocks[ BlockLen2 * 2 ];
	}

	virtual void freeChannel( void* const state )
	{
		((CChannel*) state) -> ~CChannel();
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return(( MaxInLen * UpFactor + DownFactor - 1 ) / DownFactor );
	}

	virtual void clear( const int ChCount, void* const* const states )
	{
		LastOutOffset = 0;

		if( DoConsumeLatency )
		{
			LatencyLeft = Latency;
		}
		else
		{
			LatencyLeft = 0;
		}

		for( int c = 0; c < ChCount; c++ )
		{
			CChannel* const ch = (CChannel*) states[ c ];

			memset( &ch -> PrevInput[ 0 ], 0,
				(size_t) PrevInputLen * sizeof( ch -> PrevInput[ 0 ]));

			if( !DoConsumeLatency )
			{
				if( DownShift > 0 )
				{
					memset( &ch -> CurOutput[ 0 ], 0,
						(size_t) ( BlockLen2 >> DownShift ) *
						sizeof( ch -> CurOutput[ 0 ]));
				}
				else
				{
					memset( &ch -> CurOutput[ BlockLen2 - OutOffset ], 0,
						(size_t) OutOffset *
						sizeof( ch -> CurOutput[ 0 ]));

					memset( &ch -> CurOutput[ 0 ], 0,
						(size_t) ( InputLen - OutOffset ) *
						sizeof( ch -> CurOutput[ 0 ]));
				}
			}

			memset( ch -> CurInput, 0,
				(size_t) InputDelay * sizeof( ch -> CurInput[ 0 ]));
		}

		InDataLeft = InputLen - InputDelay;
		UpSkip = 0;
		DownSkip = DownSkipInit;
	}

	virtual int process( const int ChCount, void* const* const states,
		double* const* const ip, const int l0, double* const* const op,
		CDSPResamplerBankCounters& )
	{
		R8BASSERT( l0 >= 0 );
		LastOutOffset = 0;

		double* inp[ R8B_BANK_MAX_CHANNELS ];
		R8BASSERT( ChCount <= R8B_BANK_MAX_CHANNELS );

		for( int c = 0; c < ChCount; c++ )
		{
			inp[ c ] = ip[ c ];
		}

		// Shared scheduling scalars. The up/down skips and latency subtraction
		// evolve once per block boundary; per-channel runs use local copies of
		// these values so all channels follow the identical schedule.

		int l = l0 * UpFactor;
		int sharedInDataLeft = InDataLeft;
		int sharedUpSkip = UpSkip;
		int sharedDownSkip = DownSkip;
		int sharedLatencyLeft = LatencyLeft;
		int OutCount = 0;

		// Per-channel output cursors: every FFT block produced inside one
		// process() call appends to the previous block instead of overwriting
		// the start of the output buffer.

		double* outcur[ R8B_BANK_MAX_CHANNELS ];

		for( int c = 0; c < ChCount; c++ )
		{
			outcur[ c ] = op[ c ];
		}

		while( l > 0 )
		{
			const int Offs = InputLen - sharedInDataLeft;

			if( l < sharedInDataLeft )
			{
				sharedInDataLeft -= l;

				for( int c = 0; c < ChCount; c++ )
				{
					CChannel* const ch = (CChannel*) states[ c ];
					double* ipc = inp[ c ];
					int localUpSkip = sharedUpSkip;
					int localDownSkip = sharedDownSkip;
					int localLatencyLeft = sharedLatencyLeft;
					double* outp = outcur[ c ];
					int localOut = 0;

					if( UpShift >= 0 )
					{
						memcpy( &ch -> CurInput[ Offs >> UpShift ], ipc,
							(size_t) ( l >> UpShift ) * sizeof( double ));
					}
					else
					{
						copyUpsample( ipc, &ch -> CurInput[ Offs ], l,
							localUpSkip );
					}

					copyToOutput( ch, Offs - OutOffset, outp, l, localOut,
						localLatencyLeft, localDownSkip );
				}

				sharedUpSkip = advanceUpSkip( l, sharedUpSkip );

				// Output count and the post-call latency/downsampling state
				// are schedule-defined and identical for all channels; obtain
				// them from a scalar dry run which is the single authoritative
				// source of the shared schedule.

				int dryLat = sharedLatencyLeft;
				int dryDown = sharedDownSkip;
				int dryOut = 0;
				copyToOutputSchedule( Offs - OutOffset, l, dryOut, dryLat,
					dryDown );
				sharedLatencyLeft = dryLat;
				sharedDownSkip = dryDown;

				for( int c = 0; c < ChCount; c++ )
				{
					outcur[ c ] += dryOut;
				}

				OutCount += dryOut;
				break;
			}

			const int b = sharedInDataLeft;
			l -= b;
			sharedInDataLeft = InputLen;

			for( int c = 0; c < ChCount; c++ )
			{
				CChannel* const ch = (CChannel*) states[ c ];
				double* ipc = inp[ c ];
				int localUpSkip = sharedUpSkip;
				int localDownSkip = sharedDownSkip;
				int localLatencyLeft = sharedLatencyLeft;
				double* outp = outcur[ c ];
				int localOut = 0;
				int ilu;

				if( UpShift >= 0 )
				{
					const int bu = b >> UpShift;
					memcpy( &ch -> CurInput[ Offs >> UpShift ], ipc,
						(size_t) bu * sizeof( double ));

					ipc += bu;
					ilu = InputLen >> UpShift;
				}
				else
				{
					copyUpsample( ipc, &ch -> CurInput[ Offs ], b,
						localUpSkip );
					ilu = InputLen;
				}

				const size_t pil =
					(size_t) PrevInputLen * sizeof( double );

				memcpy( &ch -> CurInput[ ilu ], ch -> PrevInput, pil );
				memcpy( ch -> PrevInput,
					&ch -> CurInput[ ilu - PrevInputLen ], pil );

				realfft_t* CurInputFFT =
					ch -> fftin -> forward( ch -> CurInput );
				realfft_t dsn = 0;

				if( UpShift > 0 || DownShift > 0 )
				{
					CurInputFFT = ch -> fftin -> reorderForward(
						CurInputFFT,
						( ( ch -> fftin -> getLen() >
							(*ch -> fftout) -> getLen() ) ?
						ch -> fftin -> getWorkBuf() :
						(*ch -> fftout) -> getWorkBuf() ));

					if( UpShift > 0 )
					{
						mirrorInputSpectrum( CurInputFFT );
					}

					if( DownShift > 0 )
					{
						const int z = BlockLen2 >> DownShift;
						dsn = Filter -> calcDownShiftNyquist(
							CurInputFFT[ z ], CurInputFFT[ z + 1 ]);
					}

					CurInputFFT = (*ch -> fftout) -> reorderInverse(
						CurInputFFT, ch -> CurInput );
				}

				if( Filter -> isZeroPhase() )
				{
					(*ch -> fftout) -> multiplyBlocksZP(
						Filter -> getKernelBlock(), CurInputFFT );
				}
				else
				{
					(*ch -> fftout) -> multiplyBlocks(
						Filter -> getKernelBlock(), CurInputFFT );
				}

				if( DownShift > 0 )
				{
					(*ch -> fftout) -> setBinNyquist( CurInputFFT, dsn );
				}

				(*ch -> fftout) -> inverse( CurInputFFT );
				copyToOutput( ch, Offs - OutOffset, outp, b, localOut,
					localLatencyLeft, localDownSkip );

				inp[ c ] = ipc;

				double* const tmp = ch -> CurInput;
				ch -> CurInput = ch -> CurOutput;
				ch -> CurOutput = tmp;
			}

			// Advance the shared schedule once, using a scalar dry run.

			sharedUpSkip = advanceUpSkip( b, sharedUpSkip );

			int dryLat = sharedLatencyLeft;
			int dryDown = sharedDownSkip;
			int dryOut = 0;
			copyToOutputSchedule( Offs - OutOffset, b, dryOut, dryLat,
				dryDown );
			sharedLatencyLeft = dryLat;
			sharedDownSkip = dryDown;

			for( int c = 0; c < ChCount; c++ )
			{
				outcur[ c ] += dryOut;
			}

			OutCount += dryOut;
		}

		UpSkip = sharedUpSkip;
		DownSkip = sharedDownSkip;
		LatencyLeft = sharedLatencyLeft;
		InDataLeft = sharedInDataLeft;

		return( OutCount );
	}

private:
	struct CChannel
	{
		CDSPRealFFTKeeper fftin;
		CDSPRealFFTKeeper ffto2;
		CDSPRealFFTKeeper* fftout;
		CFixedBuffer< double > WorkBlocks;
		double* PrevInput;
		double* CurInput;
		double* CurOutput;

		CChannel()
			: fftout( R8B_NULL )
			, PrevInput( R8B_NULL )
			, CurInput( R8B_NULL )
			, CurOutput( R8B_NULL )
		{
		}
	};

	CRefKeeper< CDSPFIRFilter > Filter;
	int UpFactor;
	int DownFactor;
	int BlockLen2;
	int OutOffset;
	int PrevInputLen;
	int InputLen;
	double LatencyFrac;
	int Latency;
	int UpShift;
	int DownShift;
	int InputDelay;
	int InDataLeft;
	int LatencyLeft;
	int UpSkip;
	int DownSkip;
	int DownSkipInit;
	int FftInBits;
	int FftOutBits;
	bool DoConsumeLatency;

	/**
	 * @brief Fills `l0` upsampled samples, using a caller-owned upsampling
	 * skip counter so identical schedules can be replayed per channel.
	 */

	void copyUpsample( double*& ip0, double* op, int l0,
		int& aUpSkip ) const
	{
		int b = min( aUpSkip, l0 );

		if( b != 0 )
		{
			aUpSkip -= b;
			l0 -= b;

			*op = 0.0;
			op++;

			while( --b != 0 )
			{
				*op = 0.0;
				op++;
			}
		}

		double* ip = ip0;
		const int upf = UpFactor;
		int l = l0 / upf;
		int lz = l0 - l * upf;

		if( upf == 3 )
		{
			while( l != 0 )
			{
				op[ 0 ] = *ip;
				op[ 1 ] = 0.0;
				op[ 2 ] = 0.0;
				ip++;
				op += upf;
				l--;
			}
		}
		else
		if( upf == 5 )
		{
			while( l != 0 )
			{
				op[ 0 ] = *ip;
				op[ 1 ] = 0.0;
				op[ 2 ] = 0.0;
				op[ 3 ] = 0.0;
				op[ 4 ] = 0.0;
				ip++;
				op += upf;
				l--;
			}
		}
		else
		{
			const size_t zc = (size_t) ( upf - 1 ) * sizeof( op[ 0 ]);

			while( l != 0 )
			{
				*op = *ip;
				ip++;

				memset( op + 1, 0, zc );
				op += upf;
				l--;
			}
		}

		if( lz != 0 )
		{
			*op = *ip;
			ip++;
			op++;

			aUpSkip = upf - lz;

			while( --lz != 0 )
			{
				*op = 0.0;
				op++;
			}
		}

		ip0 = ip;
	}

	/**
	 * @brief Computes the upsampling skip counter after `l0` upsampled
	 * samples, without touching channel data. This is the shared schedule.
	 */

	int advanceUpSkip( const int l0, const int aUpSkip ) const
	{
		if( UpShift >= 0 )
		{
			return( aUpSkip );
		}

		int skip = aUpSkip;
		int l = l0;
		int b = min( skip, l );

		if( b != 0 )
		{
			skip -= b;
			l -= b;
		}

		const int upf = UpFactor;
		const int whole = l / upf;
		int lz = l - whole * upf;

		if( lz != 0 )
		{
			skip = upf - lz;
		}

		return( skip );
	}

	/**
	 * @brief Performs latency subtraction and downsampling data movement for
	 * one channel, using caller-owned scheduling counters.
	 */

	void copyToOutput( CChannel* const ch, int Offs, double*& op0, int b,
		int& l0, int& aLatencyLeft, int& aDownSkip ) const
	{
		if( Offs < 0 )
		{
			if( Offs + b <= 0 )
			{
				Offs += BlockLen2;
			}
			else
			{
				copyToOutput( ch, Offs + BlockLen2, op0, -Offs, l0,
					aLatencyLeft, aDownSkip );
				b += Offs;
				Offs = 0;
			}
		}

		if( aLatencyLeft != 0 )
		{
			if( aLatencyLeft >= b )
			{
				aLatencyLeft -= b;
				return;
			}

			Offs += aLatencyLeft;
			b -= aLatencyLeft;
			aLatencyLeft = 0;
		}

		const int df = DownFactor;

		if( DownShift > 0 )
		{
			int Skip = Offs & ( df - 1 );

			if( Skip > 0 )
			{
				Skip = df - Skip;
				b -= Skip;
				Offs += Skip;
			}

			if( b > 0 )
			{
				b = ( b + df - 1 ) >> DownShift;
				memcpy( op0, &ch -> CurOutput[ Offs >> DownShift ],
					(size_t) b * sizeof( op0[ 0 ]));

				op0 += b;
				l0 += b;
			}
		}
		else
		{
			if( df > 1 )
			{
				const double* ip = &ch -> CurOutput[ Offs + aDownSkip ];
				int l = ( b + df - 1 - aDownSkip ) / df;
				aDownSkip += l * df - b;

				double* op = op0;
				l0 += l;
				op0 += l;

				while( l > 0 )
				{
					*op = *ip;
					ip += df;
					op++;
					l--;
				}
			}
			else
			{
				memcpy( op0, &ch -> CurOutput[ Offs ],
					(size_t) b * sizeof( op0[ 0 ]));

				op0 += b;
				l0 += b;
			}
		}
	}

	/**
	 * @brief Scalar dry run of copyToOutput(): advances only the shared
	 * latency and downsampling skip counters and reports the produced output
	 * count. This is the single source of the shared output schedule.
	 */

	void copyToOutputSchedule( int Offs, int b, int& l0, int& aLatencyLeft,
		int& aDownSkip ) const
	{
		if( Offs < 0 )
		{
			if( Offs + b <= 0 )
			{
				Offs += BlockLen2;
			}
			else
			{
				copyToOutputSchedule( Offs + BlockLen2, -Offs, l0,
					aLatencyLeft, aDownSkip );
				b += Offs;
				Offs = 0;
			}
		}

		if( aLatencyLeft != 0 )
		{
			if( aLatencyLeft >= b )
			{
				aLatencyLeft -= b;
				return;
			}

			Offs += aLatencyLeft;
			b -= aLatencyLeft;
			aLatencyLeft = 0;
		}

		const int df = DownFactor;

		if( DownShift > 0 )
		{
			int Skip = Offs & ( df - 1 );

			if( Skip > 0 )
			{
				Skip = df - Skip;
				b -= Skip;
				Offs += Skip;
			}

			if( b > 0 )
			{
				l0 += ( b + df - 1 ) >> DownShift;
			}
		}
		else
		{
			if( df > 1 )
			{
				int l = ( b + df - 1 - aDownSkip ) / df;
				aDownSkip += l * df - b;
				l0 += l;
			}
			else
			{
				l0 += b;
			}
		}
	}

	template< typename T >
	void mirrorInputSpectrum( T* const p ) const
	{
		const int bl1 = BlockLen2 >> UpShift;
		const int bl2 = bl1 * 2;
		int i;

		for( i = bl1 + 2; i < bl2; i += 2 )
		{
			p[ i ] = p[ bl2 - i ];
			p[ i + 1 ] = -p[ bl2 - i + 1 ];
		}

		p[ bl1 ] = p[ 1 ];
		p[ bl1 + 1 ] = (T) 0;
		p[ 1 ] = p[ 0 ];

		for( i = 1; i < UpShift; i++ )
		{
			const int z = bl1 << i;
			memcpy( &p[ z ], p, (size_t) z * sizeof( p[ 0 ]));
			p[ z + 1 ] = (T) 0;
		}
	}
};

// ---------------------------------------------------------------------------
// CDSPResamplerBank
// ---------------------------------------------------------------------------

/**
 * @brief Plane multi-channel sample rate converter with a shared scheduling
 * and interpolation control core.
 *
 * The class converts any number of synchronized channels (1, 2 and 6 are the
 * supported common cases) using the same sample rate, filter and block-size
 * parameters as r8b::CDSPResampler. All channels are driven by a single frame
 * clock: fractional phase advances, polynomial coefficient evaluation and
 * half-band scheduling happen once per output frame and are merely applied to
 * each channel's independent history.
 *
 * All workspace memory is allocated in the constructor and is bounded by the
 * channel count, the maximal input block length and the filter lengths.
 * process() and clear() perform no heap allocation or deallocation.
 *
 * The output buffers returned by process() remain valid until the next
 * process() or clear() call. The supplied input and output regions must not
 * overlap.
 */

class CDSPResamplerBank : public R8B_BASECLASS
{
	R8BNOCTOR( CDSPResamplerBank )

public:
	/**
	 * @brief Initializes the multi-channel resampler.
	 *
	 * Parameter semantics match r8b::CDSPResampler.
	 *
	 * @param aChannelCount Number of independent synchronized channels, at
	 * least 1 and at most R8B_BANK_MAX_CHANNELS.
	 * @param SrcSampleRate Source signal's sample rate.
	 * @param DstSampleRate Destination signal's sample rate.
	 * @param aMaxInLen Maximal input buffer length in frames per process()
	 * call.
	 * @param ReqTransBand Required transition band, in percent.
	 * @param ReqAtten Required stop-band attenuation, in decibel.
	 * @param ReqPhase Required filter's phase response.
	 */

	CDSPResamplerBank( const int aChannelCount,
		const double SrcSampleRate, const double DstSampleRate,
		const int aMaxInLen, const double ReqTransBand = 2.0,
		const double ReqAtten = 206.91,
		const EDSPFilterPhaseResponse ReqPhase = fprLinearPhase )
		: ChannelCount( aChannelCount )
		, MaxInLen( aMaxInLen )
		, MaxOutLen( aMaxInLen )
		, StageCount( 0 )
		, IsPassthrough( false )
		, PtrA( R8B_NULL )
		, PtrB( R8B_NULL )
	{
		R8BASSERT( ChannelCount >= 1 );
		R8BASSERT( ChannelCount <= R8B_BANK_MAX_CHANNELS );
		R8BASSERT( SrcSampleRate > 0.0 );
		R8BASSERT( DstSampleRate > 0.0 );
		R8BASSERT( MaxInLen > 0 );
		R8BASSERT( ReqTransBand > 0.0 );
		R8BASSERT( ReqAtten > 0.0 );

		if( SrcSampleRate == DstSampleRate )
		{
			// Equal sample rates: input is returned unchanged, no pipeline is
			// built.

			IsPassthrough = true;
			allocateWorkspace();
			return;
		}

		buildPipeline( SrcSampleRate, DstSampleRate, ReqTransBand, ReqAtten,
			ReqPhase );

		allocateWorkspace();

		for( int s = 0; s < StageCount; s++ )
		{
			for( int c = 0; c < ChannelCount; c++ )
			{
				Stages[ s ] -> initChannel( ChannelStates[ s ][ c ]);
			}
		}

		clear();
	}

	virtual ~CDSPResamplerBank()
	{
		if( IsPassthrough )
		{
			return;
		}

		for( int s = 0; s < StageCount; s++ )
		{
			for( int c = 0; c < ChannelCount; c++ )
			{
				Stages[ s ] -> freeChannel( ChannelStates[ s ][ c ]);
			}

			delete Stages[ s ];
		}
	}

	/**
	 * @brief Returns the number of channels this bank was constructed for.
	 */

	int getChannelCount() const
	{
		return( ChannelCount );
	}

	/**
	 * @brief Returns the maximal output frame count a process() call can
	 * produce.
	 */

	int getMaxOutLen() const
	{
		return( MaxOutLen );
	}

	/**
	 * @brief Clears all channels' audio history and resets the shared frame
	 * clock, returning the object to its after-construction state. No heap
	 * memory is allocated or freed.
	 */

	void clear()
	{
		Counters.reset();

		if( IsPassthrough )
		{
			return;
		}

		for( int s = 0; s < StageCount; s++ )
		{
			Stages[ s ] -> clear( ChannelCount,
				(void* const*) ChannelStates[ s ]);
		}
	}

	/**
	 * @brief Performs multi-channel sample rate conversion.
	 *
	 * @param ip0 Array of `ChannelCount` input buffer pointers, each holding at
	 * least `l` frames. The buffers must not overlap any internal output
	 * storage.
	 * @param l Common number of input frames, at most `MaxInLen`. May be zero,
	 * in which case no audio state is advanced.
	 * @param[out] op0 Array of `ChannelCount` pointers that receive the
	 * internal output buffers. The returned buffers remain valid until the
	 * next process() or clear() call and must be consumed before then.
	 * @return The common number of output frames, identical for all channels.
	 */

	int process( double* const* ip0, const int l, double** op0 )
	{
		R8BASSERT( l >= 0 && l <= MaxInLen );

		if( IsPassthrough )
		{
			for( int c = 0; c < ChannelCount; c++ )
			{
				op0[ c ] = ip0[ c ];
			}

			return( l );
		}

		if( l == 0 )
		{
			// Zero-length calls do not advance audio.

			for( int c = 0; c < ChannelCount; c++ )
			{
				op0[ c ] = StageOutputs[ StageCount - 1 ][ c ];
			}

			return( 0 );
		}

		for( int c = 0; c < ChannelCount; c++ )
		{
			PtrA[ c ] = ip0[ c ];
		}

		int curLen = l;

		for( int s = 0; s < StageCount; s++ )
		{
			double** const inPtrs = ( s & 1 ) ? PtrB : PtrA;
			double** const outPtrs = ( s & 1 ) ? PtrA : PtrB;

			for( int c = 0; c < ChannelCount; c++ )
			{
				outPtrs[ c ] = StageOutputs[ s ][ c ];
			}

			curLen = Stages[ s ] -> process( ChannelCount,
				(void* const*) ChannelStates[ s ], inPtrs, curLen, outPtrs,
				Counters );

			// Communicate a front-trim (latency consumption) to the next
			// stage's input table.

			const int offs = Stages[ s ] -> LastOutOffset;

			if( offs != 0 )
			{
				for( int c = 0; c < ChannelCount; c++ )
				{
					outPtrs[ c ] = StageOutputs[ s ][ c ] + offs;
				}
			}
		}

		double* const* const finalPtrs =
			( StageCount & 1 ) ? PtrB : PtrA;

		for( int c = 0; c < ChannelCount; c++ )
		{
			op0[ c ] = finalPtrs[ c ];
		}

		return( curLen );
	}

	/**
	 * @brief Returns the accumulated control-work counters.
	 */

	const CDSPResamplerBankCounters& getCounters() const
	{
		return( Counters );
	}

	/**
	 * @brief Resets the control-work counters without touching audio state.
	 */

	void resetCounters()
	{
		Counters.reset();
	}

private:
	static const int R8B_BANK_MAX_STAGES = 16;

	int ChannelCount;
	int MaxInLen;
	int MaxOutLen;
	int StageCount;
	bool IsPassthrough;
	CBankStage* Stages[ R8B_BANK_MAX_STAGES ];
	CDSPResamplerBankCounters Counters;

	CFixedBuffer< unsigned char > ChannelStateStorage; ///< All per-channel
		///< state blocks of all stages.
	CFixedBuffer< double > StageOutputStorage; ///< All intermediate and final
		///< stage output buffers.
	CFixedBuffer< unsigned char* > StatePtrStorage; ///< Channel state pointer
		///< tables for all stages.
	CFixedBuffer< double* > OutputPtrStorage; ///< Stage output pointer tables
		///< for all stages.
	CFixedBuffer< double* > WorkPtrStorage; ///< Two flip-flop per-channel
		///< pointer tables used in process().

	unsigned char** ChannelStates[ R8B_BANK_MAX_STAGES ];
	double** StageOutputs[ R8B_BANK_MAX_STAGES ];
	double** PtrA;
	double** PtrB;

	void addStage( CBankStage* const stage, double& LatencyFrac )
	{
		R8BASSERT( StageCount < R8B_BANK_MAX_STAGES );

		Stages[ StageCount ] = stage;
		MaxOutLen = stage -> getMaxOutLen( MaxOutLen );
		LatencyFrac = stage -> getLatencyFrac();
		StageCount++;
	}

	/**
	 * @brief Builds the same processor topology as CDSPResampler for the same
	 * parameters, but as shared-scheduling bank stages.
	 */

	void buildPipeline( const double SrcSampleRate,
		const double DstSampleRate, const double ReqTransBand,
		const double ReqAtten, const EDSPFilterPhaseResponse ReqPhase )
	{
		double LatencyFrac = 0.0;

		// Try some common efficient ratios requiring only a single step.

		static const int CommonRatioCount = 5;
		static const int CommonRatios[ CommonRatioCount ][ 2 ] = {
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
				addStage( new CBankBlockStage(
					CDSPFIRFilterCache :: getLPFilter(
					1.0 / ( num > den ? num : den ), ReqTransBand,
					ReqAtten, ReqPhase, num, den ), num, den, LatencyFrac ),
					LatencyFrac );

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
				addStage( new CBankBlockStage(
					CDSPFIRFilterCache :: getLPFilter( 1.0 / i, ReqTransBand,
					ReqAtten, ReqPhase, i, 1 ), i, 1, LatencyFrac ),
					LatencyFrac );

				const bool IsThirdFirst = ( i == 3 );

				for( int j = 0; j < c; j++ )
				{
					addStage( new CBankHBUpStage( ReqAtten, j,
						( IsThirdFirst || j > 1 ), LatencyFrac ),
						LatencyFrac );
				}

				return;
			}
		}

		if( DstSampleRate * 2.0 > SrcSampleRate )
		{
			// Upsampling or fractional downsampling down to 2X.

			const double NormFreq = ( DstSampleRate > SrcSampleRate ? 0.5 :
				0.5 * DstSampleRate / SrcSampleRate );

			addStage( new CBankBlockStage(
				CDSPFIRFilterCache :: getLPFilter( NormFreq, ReqTransBand,
				ReqAtten, ReqPhase, 2.0, 1 ), 2, 1, LatencyFrac ),
				LatencyFrac );

			const double tbw = 0.0175;
			const double ThreshSampleRate = SrcSampleRate /
				( 1.0 - tbw * ReqTransBand );

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
				c = 0;
			}

			if( c > 0 )
			{
				addStage( new CBankFracStage(
					SrcSampleRate2 * div, DstSampleRate, ReqAtten, false,
					LatencyFrac ), LatencyFrac );

				double tb = ( 1.0 - SrcSampleRate * div / DstSampleRate ) /
					tbw;

				if( tb > CDSPFIRFilter :: getLPMaxTransBand() )
				{
					tb = CDSPFIRFilter :: getLPMaxTransBand();
				}

				addStage( new CBankBlockStage(
					CDSPFIRFilterCache :: getLPFilter( 0.5, tb, ReqAtten,
					ReqPhase, 2, 1 ), 2, 1, LatencyFrac ), LatencyFrac );

				for( int j = 1; j < c; j++ )
				{
					addStage( new CBankHBUpStage( ReqAtten, j - 1,
						( j > 2 ), LatencyFrac ), LatencyFrac );
				}
			}
			else
			{
				addStage( new CBankFracStage( SrcSampleRate2,
					DstSampleRate, ReqAtten, false, LatencyFrac ),
					LatencyFrac );
			}

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
			addStage( new CBankHBDownStage( ReqAtten, c - 1 - i,
				( IsThirdFirst || i < c - 2 ), LatencyFrac ), LatencyFrac );
		}

		addStage( new CBankBlockStage(
			CDSPFIRFilterCache :: getLPFilter( NormFreq, ReqTransBand,
			ReqAtten, ReqPhase, FinGain, downf ), 1, downf, LatencyFrac ),
			LatencyFrac );

		if( UseInterp )
		{
			addStage( new CBankFracStage( SrcSampleRate,
				DstSampleRate * SrcSRDiv, ReqAtten, IsThirdFirst,
				LatencyFrac ), LatencyFrac );
		}
	}

	/**
	 * @brief Allocates all bounded workspace: per-channel state blocks,
	 * per-stage output buffers and pointer tables.
	 */

	void allocateWorkspace()
	{
		int StateBytes = 0;
		int OutputDoubles = 0;

		for( int s = 0; s < StageCount; s++ )
		{
			const int chbytes = Stages[ s ] -> getChannelStateBytes();
			const int slot = ( chbytes + 63 ) & ~63;

			StateBytes += slot * ChannelCount;

			OutputDoubles +=
				Stages[ s ] -> getMaxOutLen( MaxInLenForStage( s )) *
				ChannelCount;
		}

		ChannelStateStorage.alloc( StateBytes > 0 ? StateBytes : 1 );
		StageOutputStorage.alloc( OutputDoubles > 0 ? OutputDoubles : 1 );
		StatePtrStorage.alloc( StageCount * ChannelCount + 1 );
		OutputPtrStorage.alloc( StageCount * ChannelCount + 1 );
		WorkPtrStorage.alloc( ChannelCount * 2 );

		PtrA = &WorkPtrStorage[ 0 ];
		PtrB = &WorkPtrStorage[ ChannelCount ];

		unsigned char* sp = ChannelStateStorage;
		double* op = StageOutputStorage;

		for( int s = 0; s < StageCount; s++ )
		{
			ChannelStates[ s ] = &StatePtrStorage[ s * ChannelCount ];
			StageOutputs[ s ] = &OutputPtrStorage[ s * ChannelCount ];

			const int chbytes = Stages[ s ] -> getChannelStateBytes();
			const int slot = ( chbytes + 63 ) & ~63;
			const int cap = Stages[ s ] -> getMaxOutLen( MaxInLenForStage( s ));

			for( int c = 0; c < ChannelCount; c++ )
			{
				ChannelStates[ s ][ c ] = sp;
				sp += slot;

				StageOutputs[ s ][ c ] = op;
				op += cap;
			}
		}
	}

	int MaxInLenForStage( const int stage ) const
	{
		// Output buffer sizing follows the same getMaxOutLen() chain as the
		// mono resampler: the first stage sees MaxInLen, later stages see the
		// maximal output length of their predecessors.

		int len = MaxInLen;

		for( int s = 0; s < stage; s++ )
		{
			len = Stages[ s ] -> getMaxOutLen( len );
		}

		return( len );
	}
};

} // namespace r8b

#endif // R8B_CDSPRESAMPLERBANK_INCLUDED
