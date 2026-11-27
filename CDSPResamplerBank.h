//$ nobt
//$ nocpp

/**
 * @file CDSPResamplerBank.h
 *
 * @brief Planar multi-channel sample rate converter with a shared scheduling
 * clock.
 *
 * This file includes the r8b::CDSPResamplerBank class: a planar, flat-layout
 * multi-channel resampler. Unlike an array of r8b::CDSPResampler objects,
 * where every channel advances its own fractional position, selects its own
 * interpolation filter phase and evaluates polynomial coefficients separately,
 * the bank performs all such control work exactly once per output frame, on a
 * common frame clock, and applies the result to each channel's independent
 * sample history. FFT transforms remain per-channel.
 *
 * r8brain-free-src Copyright (c) 2013-2026 Aleksey Vaneev
 *
 * See the "LICENSE" file for license.
 */

#ifndef R8B_CDSPRESAMPLERBANK_INCLUDED
#define R8B_CDSPRESAMPLERBANK_INCLUDED

#include "CDSPResampler.h"

namespace r8b {

/**
 * @brief Internal control-work counters of CDSPResamplerBank.
 *
 * These counters are incremented at the points where the corresponding
 * control work actually happens. They are not derived from the channel count.
 * With identical input frame arrangements the values do not depend on the
 * number of channels processed.
 */

struct CDSPBankCounters
{
	CDSPBankCounters()
		: PhaseAdvances( 0 )
		, FracCoeffEvals( 0 )
		, HalfBandSchedules( 0 )
	{
	}

	long long PhaseAdvances; ///< Fractional interpolator position advances,
		///< one per produced fractional-stage output frame.
	long long FracCoeffEvals; ///< Fractional polynomial (2nd-order) filter
		///< coefficient evaluations. Stays zero on the whole-numbered stepping
		///< path where polynomial interpolation is not used.
	long long HalfBandSchedules; ///< Half-band stage output scheduling runs
		///< (contiguous convolution dispatches on the common frame clock).
};

/**
 * @brief Base class for the bank's shared-schedule processing stages.
 *
 * Every stage processes `ChannelCount` independent histories in one call while
 * keeping all scheduling state on the common frame clock.
 */

class CDSPBankProcessor : public R8B_BASECLASS
{
	R8BNOCTOR( CDSPBankProcessor )

public:
	CDSPBankProcessor( const int aChannelCount, CDSPBankCounters& aCounters )
		: ChannelCount( aChannelCount )
		, Counters( aCounters )
	{
	}

	virtual ~CDSPBankProcessor()
	{
	}

	/**
	 * @brief Clears all per-channel histories and the shared scheduling state.
	 */

	virtual void clear() = 0;

	/**
	 * @brief Returns the maximal output length, per channel.
	 */

	virtual int getMaxOutLen( const int MaxInLen ) const = 0;

	/**
	 * @brief Returns fractional latency, in samples, left on the output.
	 */

	virtual double getLatencyFrac() const = 0;

	/**
	 * @brief Processes `l` common input frames of all channels.
	 *
	 * @param ip Array of ChannelCount input buffers, one per channel.
	 * @param l Common input frame count.
	 * @param[out] op Array of ChannelCount output buffer base pointers; on
	 * return receives the pointers to the valid output data.
	 * @return The common output frame count.
	 */

	virtual int process( double* const* ip, int l, double** op ) = 0;

protected:
	int ChannelCount; ///< Number of channels handled by this stage.
	CDSPBankCounters& Counters; ///< Shared control-work counters.
};

// ---------------------------------------------------------------------------
// CDSPBankFracInterpolator
// ---------------------------------------------------------------------------

/**
 * @brief Multi-channel fractional delay interpolator with one shared position.
 *
 * The fractional position, filter phase index and polynomial coefficients are
 * evaluated once per output frame and applied to every channel's ring buffer.
 */

class CDSPBankFracInterpolator : public CDSPBankProcessor
{
public:
	CDSPBankFracInterpolator( const int aChannelCount,
		CDSPBankCounters& aCounters, const double aSrcSampleRate,
		const double aDstSampleRate, const double ReqAtten,
		const bool IsThird, const double PrevLatency )
		: CDSPBankProcessor( aChannelCount, aCounters )
		, SrcSampleRate( aSrcSampleRate )
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

		FilterLen = FilterBank -> getFilterLen();
		fl2 = FilterLen >> 1;
		fll = fl2 - 1;
		flo = fll + fl2;
		flb = BufLen - fll;

		R8BASSERT(( 1 << BufLenBits ) >= FilterLen * 3 );

		Rings.alloc( ChannelCount * RingStride );
		RingPtr.alloc( ChannelCount );
		InCursor.alloc( ChannelCount );
		OutCursor.alloc( ChannelCount );
		BufLeftArr.alloc( ChannelCount );
		WritePosArr.alloc( ChannelCount );
		ReadPosArr.alloc( ChannelCount );

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			RingPtr[ ch ] = &Rings[ ch * RingStride ];
		}

		clear();
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return( (int) ceil( MaxInLen * DstSampleRate / SrcSampleRate ) + 1 );
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual void clear()
	{
		LatencyLeft = Latency;
		InPosFracW = InitFracPosW;
		InPosFrac = InitFracPos;
		InPosInt = 0;
		InPosShift = InitFracPos / SrcSampleRate * DstSampleRate;

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			BufLeftArr[ ch ] = 0;
			WritePosArr[ ch ] = 0;
			ReadPosArr[ ch ] = flb;

			memset( &RingPtr[ ch ][ flb ], 0,
				(size_t) ( BufLen - flb ) * sizeof( double ));
		}
	}

	virtual int process( double* const* ip0, int l0, double** op0 )
	{
		R8BASSERT( l0 >= 0 );

		int l = l0;

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			InCursor[ ch ] = ip0[ ch ];
		}

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= l )
			{
				LatencyLeft -= l;
				return( 0 );
			}

			l -= LatencyLeft;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				InCursor[ ch ] += LatencyLeft;
			}

			LatencyLeft = 0;
		}

		double** op = OutCursor;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			op[ ch ] = op0[ ch ];
		}

		while( l > 0 )
		{
			// All channels run on the same clock: the fill count is determined
			// by the shared positions (the per-channel values are identical).

			const int b = min( l, min( BufLen - WritePosArr[ 0 ],
				flb - BufLeftArr[ 0 ]));

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				double* const ring = RingPtr[ ch ];
				double* const wp1 = ring + WritePosArr[ ch ];

				memcpy( wp1, InCursor[ ch ], (size_t) b * sizeof( double ));

				const int ec = flo - WritePosArr[ ch ];

				if( ec > 0 )
				{
					memcpy( wp1 + BufLen, InCursor[ ch ],
						(size_t) min( b, ec ) * sizeof( double ));
				}

				InCursor[ ch ] += b;
				WritePosArr[ ch ] = ( WritePosArr[ ch ] + b ) & BufLenMask;
				BufLeftArr[ ch ] += b;
			}

			l -= b;

			if( IsWhole )
			{
				convolveWhole( op );
			}
			else
			{
				convolvePoly( op );
			}
		}

		if( !IsWhole && (int) InPosShift > 1000 )
		{
			// Reset the interpolation position counter to achieve a higher
			// sample-timing precision.

			InPosInt = 0;
			InPosShift = InPosFrac / SrcSampleRate * DstSampleRate;
		}

		return( (int) ( op[ 0 ] - op0[ 0 ]));
	}

private:
	static const int BufLenBits = 8; ///< Ring buffer length, Nth power of 2.
	static const int BufLen = 1 << BufLenBits;
	static const int BufLenMask = BufLen - 1;
	static const int RingStride = BufLen + 29; ///< Per-channel ring stride,
		///< including overrun protection.

	CFixedBuffer< double > Rings; ///< All channels' ring buffers, contiguous.
	CFixedBuffer< double* > RingPtr; ///< Per-channel ring pointers.
	CFixedBuffer< double* > InCursor; ///< Per-channel input cursors.
	CFixedBuffer< double* > OutCursor; ///< Per-channel output cursors.
	CFixedBuffer< int > BufLeftArr;
	CFixedBuffer< int > WritePosArr;
	CFixedBuffer< int > ReadPosArr;

	double SrcSampleRate;
	double DstSampleRate;
	double InitFracPos;
	int InitFracPosW;
	int Latency;
	int LatencyLeft;
	double LatencyFrac;
	int FilterLen;
	int fll;
	int fl2;
	int flo;
	int flb;
	int InStep;
	int OutStep;
	int InPosFracW;
	int InPosInt;
	double InPosFrac;
	double FracStep;
	double InPosShift;
	CRefKeeper< CDSPFracDelayFilterBank > FilterBank;
	bool IsWhole;

	/**
	 * @brief Whole-numbered stepping convolution, shared phase per frame.
	 */

	void convolveWhole( double** op )
	{
		const CDSPFracDelayFilterBank& fb = *FilterBank;
		const int istep = InStep;
		const int ostep = OutStep;
		int fpos = InPosFracW;
		int bl = BufLeftArr[ 0 ] - fl2;

		while( bl > 0 )
		{
			const double* const ftp = &fb[ fpos ];

			Counters.PhaseAdvances++;

			int ch;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				const double* const rp = RingPtr[ ch ] + ReadPosArr[ ch ];
				double s = 0.0;
				int i;

				for( i = 0; i < FilterLen; i++ )
				{
					s += ftp[ i ] * rp[ i ];
				}

				*op[ ch ] = s;
				op[ ch ]++;
			}

			fpos += istep;
			const int PosIncr = fpos / ostep;
			fpos -= PosIncr * ostep;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				ReadPosArr[ ch ] =
					( ReadPosArr[ ch ] + PosIncr ) & BufLenMask;
			}

			bl -= PosIncr;
		}

		for( int ch = 0; ch < ChannelCount; ch++ )
		{
			BufLeftArr[ ch ] = bl + fl2;
		}

		InPosFracW = fpos;
	}

	/**
	 * @brief Fractional polynomial interpolation, coefficients once per frame.
	 */

	void convolvePoly( double** op )
	{
		const CDSPFracDelayFilterBank& fb = *FilterBank;
		const int fltlen = FilterLen;
		const double ffracs = fb.getFilterFracs();
		const double fs = FracStep;
		int ipos = InPosInt;
		double fpos = InPosFrac;
		double psh = InPosShift;
		int bl = BufLeftArr[ 0 ] - fl2;

		while( bl > 0 )
		{
			double x = fpos * ffracs;
			const int fti = (int) x; // Function table index.
			const double* ftp = &fb[ fti ];
			x -= fti; // Interpolation factor between adjacent filters.
			const double x2d = x * x;

			Counters.PhaseAdvances++;
			Counters.FracCoeffEvals++;

			int ch;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				const double* const rp = RingPtr[ ch ] + ReadPosArr[ ch ];
				const double* tp = ftp;
				double s = 0.0;
				int i;

				for( i = 0; i < fltlen; i++ )
				{
					s += ( tp[ 0 ] + tp[ 1 ] * x + tp[ 2 ] * x2d ) * rp[ i ];
					tp += 3;
				}

				*op[ ch ] = s;
				op[ ch ]++;
			}

			psh += 1.0;

			const double NextInPos = psh * fs;
			const int NextInPosInt = (int) NextInPos;
			const int PosIncr = NextInPosInt - ipos;

			fpos = NextInPos - NextInPosInt;
			ipos = NextInPosInt;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				ReadPosArr[ ch ] =
					( ReadPosArr[ ch ] + PosIncr ) & BufLenMask;
			}

			bl -= PosIncr;
		}

		for( int ch = 0; ch < ChannelCount; ch++ )
		{
			BufLeftArr[ ch ] = bl + fl2;
		}

		InPosInt = ipos;
		InPosFrac = fpos;
		InPosShift = psh;
	}
};

// ---------------------------------------------------------------------------
// CDSPBankHBUpsampler
// ---------------------------------------------------------------------------

/**
 * @brief Multi-channel 2X half-band upsampler on a common frame clock.
 */

class CDSPBankHBUpsampler : public CDSPBankProcessor
{
public:
	CDSPBankHBUpsampler( const int aChannelCount, CDSPBankCounters& aCounters,
		const double ReqAtten, const int SteepIndex, const bool IsThird,
		const double PrevLatency )
		: CDSPBankProcessor( aChannelCount, aCounters )
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

		fltp = align_ptr( FltBuf, 16 );
		memcpy( fltp, fltp0, (size_t) fltt * sizeof( double ));

		flttCount = fltt;
		fll = fltt - 1;
		fl2 = fltt;
		flo = fll + fl2;
		flb = BufLen - fll;

		LatencyFrac = PrevLatency * 2.0;
		Latency = (int) LatencyFrac;
		LatencyFrac -= Latency;

		R8BASSERT( Latency >= 0 );

		Rings.alloc( ChannelCount * RingStride );
		RingPtr.alloc( ChannelCount );
		InCursor.alloc( ChannelCount );
		OutCursor.alloc( ChannelCount );
		BufLeftArr.alloc( ChannelCount );
		WritePosArr.alloc( ChannelCount );
		ReadPosArr.alloc( ChannelCount );

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			RingPtr[ ch ] = &Rings[ ch * RingStride ];
		}

		clear();
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return( MaxInLen * 2 );
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual void clear()
	{
		LatencyLeft = Latency;

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			BufLeftArr[ ch ] = 0;
			WritePosArr[ ch ] = 0;
			ReadPosArr[ ch ] = flb & BufLenMask;

			memset( &RingPtr[ ch ][ ReadPosArr[ ch ]], 0,
				(size_t) ( BufLen - flb ) * sizeof( double ));
		}
	}

	virtual int process( double* const* ip0, int l0, double** op0 )
	{
		R8BASSERT( l0 >= 0 );

		int l = l0;
		int ac = 0;
		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			InCursor[ ch ] = ip0[ ch ];
			OutCursor[ ch ] = op0[ ch ];
		}

		while( l > 0 && ac < flo )
		{
			const int b = min( min( l, flo - ac ),
				min( BufLen - WritePosArr[ 0 ],
				flb - BufLeftArr[ 0 ]));

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				double* const ring = RingPtr[ ch ];
				double* const wp1 = ring + WritePosArr[ ch ];

				memcpy( wp1, InCursor[ ch ], (size_t) b * sizeof( double ));

				const int ec = flo - WritePosArr[ ch ];

				if( ec > 0 )
				{
					memcpy( wp1 + BufLen, InCursor[ ch ],
						(size_t) min( b, ec ) * sizeof( double ));
				}

				InCursor[ ch ] += b;
				WritePosArr[ ch ] = ( WritePosArr[ ch ] + b ) & BufLenMask;
				BufLeftArr[ ch ] += b;
			}

			l -= b;
			ac += b;

			int c = BufLeftArr[ 0 ] - fl2;

			while( c > 0 )
			{
				const int cb = min( c, BufLen - ReadPosArr[ 0 ]);

				Counters.HalfBandSchedules++;

				for( ch = 0; ch < ChannelCount; ch++ )
				{
					convolve( OutCursor[ ch ],
						OutCursor[ ch ] + cb * 2,
						RingPtr[ ch ] + fll + ReadPosArr[ ch ]);

					OutCursor[ ch ] += cb * 2;
				}

				for( ch = 0; ch < ChannelCount; ch++ )
				{
					ReadPosArr[ ch ] =
						( ReadPosArr[ ch ] + cb ) & BufLenMask;
					BufLeftArr[ ch ] -= cb;
				}

				c -= cb;
			}
		}

		if( l > 0 )
		{
			R8BASSERT( ac == flo );

			Counters.HalfBandSchedules++;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				convolve( OutCursor[ ch ], OutCursor[ ch ] + l * 2,
					InCursor[ ch ] - fl2 );

				OutCursor[ ch ] += l * 2;

				// Rebase the ring buffer to the last `flo` input samples,
				// i.e. those ending at the input cursor advanced by `l`.

				memcpy( RingPtr[ ch ], InCursor[ ch ] + l - flo,
					(size_t) flo * sizeof( double ));

				ReadPosArr[ ch ] = 0;
				WritePosArr[ ch ] = flo;
				BufLeftArr[ ch ] = fl2;
			}
		}

		int ol = (int) ( OutCursor[ 0 ] - op0[ 0 ]);

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= ol )
			{
				LatencyLeft -= ol;
				return( 0 );
			}

			ol -= LatencyLeft;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				op0[ ch ] += LatencyLeft;
			}

			LatencyLeft = 0;
		}

		return( ol );
	}

private:
	static const int BufLenBits = 7;
	static const int BufLen = 1 << BufLenBits;
	static const int BufLenMask = BufLen - 1;
	static const int FltBufLen = 14;
	static const int RingStride = BufLen + ( FltBufLen * 2 - 1 );

	CFixedBuffer< double > Rings;
	CFixedBuffer< double* > RingPtr;
	CFixedBuffer< double* > InCursor;
	CFixedBuffer< double* > OutCursor;
	CFixedBuffer< int > BufLeftArr;
	CFixedBuffer< int > WritePosArr;
	CFixedBuffer< int > ReadPosArr;
	double FltBuf[ FltBufLen + 2 ];
	double* fltp;
	int flttCount;
	int fll;
	int fl2;
	int flo;
	int flb;
	double LatencyFrac;
	int Latency;
	int LatencyLeft;

	/**
	 * @brief Scalar half-band upsampling kernel, one input -> two outputs.
	 */

	void convolve( double* op, double* const opend, const double* rp )
	{
		while( op != opend )
		{
			double s = 0.0;
			int k;

			for( k = 0; k < flttCount; k++ )
			{
				s += fltp[ k ] * ( rp[ k + 1 ] + rp[ -k ]);
			}

			op[ 0 ] = rp[ 0 ];
			op[ 1 ] = s;
			op += 2;
			rp++;
		}
	}
};

// ---------------------------------------------------------------------------
// CDSPBankHBDownsampler
// ---------------------------------------------------------------------------

/**
 * @brief Multi-channel 2X half-band downsampler on a common frame clock.
 */

class CDSPBankHBDownsampler : public CDSPBankProcessor
{
public:
	CDSPBankHBDownsampler( const int aChannelCount,
		CDSPBankCounters& aCounters, const double ReqAtten,
		const int SteepIndex, const bool IsThird,
		const double PrevLatency )
		: CDSPBankProcessor( aChannelCount, aCounters )
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

		fltp = align_ptr( FltBuf, 16 );
		memcpy( fltp, fltp0, (size_t) fltt * sizeof( double ));

		flttCount = fltt;
		fll = fltt;
		fl2 = fltt - 1;
		flo = fll + fl2;
		flb = BufLen - fll;

		LatencyFrac = PrevLatency * 0.5;
		Latency = (int) LatencyFrac;
		LatencyFrac -= Latency;

		R8BASSERT( Latency >= 0 );

		Rings.alloc( ChannelCount * RingStride * 2 );
		RingPtr.alloc( ChannelCount * 2 );
		InCursor.alloc( ChannelCount );
		OutCursor.alloc( ChannelCount );
		BufLeftArr.alloc( ChannelCount );
		WritePos1Arr.alloc( ChannelCount );
		WritePos2Arr.alloc( ChannelCount );
		ReadPosArr.alloc( ChannelCount );

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			RingPtr[ ch * 2 ] = &Rings[ ch * RingStride * 2 ];
			RingPtr[ ch * 2 + 1 ] =
				&Rings[ ch * RingStride * 2 + BufLenPad ];
		}

		clear();
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return(( MaxInLen + 1 ) >> 1 );
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual void clear()
	{
		LatencyLeft = Latency;

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			BufLeftArr[ ch ] = 0;
			WritePos1Arr[ ch ] = 0;
			WritePos2Arr[ ch ] = 0;
			ReadPosArr[ ch ] = flb;

			memset( &RingPtr[ ch * 2 ][ flb ], 0,
				(size_t) ( BufLen - flb ) * sizeof( double ));

			memset( &RingPtr[ ch * 2 + 1 ][ flb ], 0,
				(size_t) ( BufLen - flb ) * sizeof( double ));
		}
	}

	virtual int process( double* const* ip0, int l0, double** op0 )
	{
		R8BASSERT( l0 >= 0 );

		int l = l0;
		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			InCursor[ ch ] = ip0[ ch ];
			OutCursor[ ch ] = op0[ ch ];
		}

		while( l > 0 )
		{
			if( WritePos1Arr[ 0 ] != WritePos2Arr[ 0 ] )
			{
				// If previous fill was asymmetrical, put a single sample to
				// the second buffer.

				for( ch = 0; ch < ChannelCount; ch++ )
				{
					double* const buf2 = RingPtr[ ch * 2 + 1 ];
					double* const wp2 = buf2 + WritePos2Arr[ ch ];
					*wp2 = *InCursor[ ch ];

					if( WritePos2Arr[ ch ] < flo )
					{
						wp2[ BufLen ] = *InCursor[ ch ];
					}

					InCursor[ ch ]++;
					WritePos2Arr[ ch ] = WritePos1Arr[ ch ];
					BufLeftArr[ ch ]++;
				}

				l--;
			}

			const int b1 = min(( l + 1 ) >> 1,
				min( BufLen - WritePos1Arr[ 0 ],
				flb - BufLeftArr[ 0 ]));
			const int b2 = b1 - ( b1 * 2 > l );

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				double* const buf1 = RingPtr[ ch * 2 ];
				double* const buf2 = RingPtr[ ch * 2 + 1 ];
				double* const wp1 = buf1 + WritePos1Arr[ ch ];
				double* const wp2 = buf2 + WritePos1Arr[ ch ];
				double* ip = InCursor[ ch ];
				int i;

				for( i = 0; i < b2; i++ )
				{
					wp1[ i ] = ip[ 0 ];
					wp2[ i ] = ip[ 1 ];
					ip += 2;
				}

				if( b1 != b2 )
				{
					// After the pairs loop `ip` points at the odd sample.

					wp1[ b2 ] = *ip;
					ip++;
				}

				const int ec = flo - WritePos1Arr[ ch ];

				if( ec > 0 )
				{
					memcpy( wp1 + BufLen, wp1,
						(size_t) min( b1, ec ) * sizeof( double ));

					memcpy( wp2 + BufLen, wp2,
						(size_t) min( b2, ec ) * sizeof( double ));
				}

				WritePos1Arr[ ch ] =
					( WritePos1Arr[ ch ] + b1 ) & BufLenMask;
				WritePos2Arr[ ch ] =
					( WritePos2Arr[ ch ] + b2 ) & BufLenMask;
				BufLeftArr[ ch ] += b2;

				InCursor[ ch ] += b1 + b2;
			}

			l -= b1 + b2;

			int c = BufLeftArr[ 0 ] - fl2;

			while( c > 0 )
			{
				const int cb = min( c, BufLen - ReadPosArr[ 0 ]);

				Counters.HalfBandSchedules++;

				for( ch = 0; ch < ChannelCount; ch++ )
				{
					const double* const rp1 =
						RingPtr[ ch * 2 ] + fll + ReadPosArr[ ch ];

					convolve( OutCursor[ ch ], OutCursor[ ch ] + cb, rp1 );
					OutCursor[ ch ] += cb;
					ReadPosArr[ ch ] =
						( ReadPosArr[ ch ] + cb ) & BufLenMask;
					BufLeftArr[ ch ] -= cb;
				}

				c -= cb;
			}
		}

		int ol = (int) ( OutCursor[ 0 ] - op0[ 0 ]);

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= ol )
			{
				LatencyLeft -= ol;
				return( 0 );
			}

			ol -= LatencyLeft;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				op0[ ch ] += LatencyLeft;
			}

			LatencyLeft = 0;
		}

		return( ol );
	}

private:
	static const int BufLenBits = 10;
	static const int BufLen = 1 << BufLenBits;
	static const int BufLenMask = BufLen - 1;
	static const int FltBufLen = 14;
	static const int BufLenPad = BufLen + ( FltBufLen * 2 - 1 );
	static const int RingStride = BufLenPad;

	CFixedBuffer< double > Rings;
	CFixedBuffer< double* > RingPtr;
	CFixedBuffer< double* > InCursor;
	CFixedBuffer< double* > OutCursor;
	CFixedBuffer< int > BufLeftArr;
	CFixedBuffer< int > WritePos1Arr;
	CFixedBuffer< int > WritePos2Arr;
	CFixedBuffer< int > ReadPosArr;
	double FltBuf[ FltBufLen + 2 ];
	double* fltp;
	int flttCount;
	int fll;
	int fl2;
	int flo;
	int flb;
	double LatencyFrac;
	int Latency;
	int LatencyLeft;

	/**
	 * @brief Scalar half-band downsampling kernel, two inputs -> one output.
	 *
	 * `rp1` points into the first ring buffer; its positive-offset counterpart
	 * lies in the second ring buffer at offset `BufLenPad - 1`.
	 */

	void convolve( double* op, double* const opend, const double* rp1in )
	{
		const double* rp1 = rp1in;

		while( op != opend )
		{
			const double* const rp = rp1 + BufLenPad - 1;
			double s = rp1[ 0 ];
			int k;

			for( k = 0; k < flttCount; k++ )
			{
				s += fltp[ k ] * ( rp[ k + 1 ] + rp[ -k ]);
			}

			*op = s;
			op++;
			rp1++;
		}
	}
};

// ---------------------------------------------------------------------------
// CDSPBankBlockConvolver
// ---------------------------------------------------------------------------

/**
 * @brief Multi-channel single-block overlap-save convolver.
 *
 * The block-fill clock, up/down sampling skips and latency deduction are
 * shared; FFT transforms run independently per channel.
 */

class CDSPBankBlockConvolver : public CDSPBankProcessor
{
public:
	CDSPBankBlockConvolver( const int aChannelCount,
		CDSPBankCounters& aCounters, CDSPFIRFilter& aFilter,
		const int aUpFactor, const int aDownFactor,
		const double PrevLatency = 0.0,
		const bool aDoConsumeLatency = true )
		: CDSPBankProcessor( aChannelCount, aCounters )
		, Filter( &aFilter )
		, UpFactor( aUpFactor )
		, DownFactor( aDownFactor )
		, BlockLen2( 2 << Filter -> getBlockLenBits() )
		, DoConsumeLatency( aDoConsumeLatency )
	{
		// fftin and fftout are assigned below; ffto2 stays null when the input
		// and output FFT share one keeper.

		ffto2 = R8B_NULL;

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

		fftin = new CDSPRealFFTKeeper( fftinBits );

		if( fftoutBits == fftinBits )
		{
			fftout = fftin;
		}
		else
		{
			ffto2 = new CDSPRealFFTKeeper( fftoutBits );
			fftout = ffto2;
		}

		const int WorkStride = BlockLen2 * 2 + PrevInputLen;
		WorkBlocks.alloc( ChannelCount * WorkStride );

		CurInput.alloc( ChannelCount );
		CurOutput.alloc( ChannelCount );
		PrevInput.alloc( ChannelCount );
		InDataLeft.alloc( ChannelCount );
		LatencyLeftArr.alloc( ChannelCount );
		UpSkipArr.alloc( ChannelCount );
		DownSkipArr.alloc( ChannelCount );
		OutCursor.alloc( ChannelCount );
		ipCur.alloc( ChannelCount );

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			double* const base = &WorkBlocks[ ch * WorkStride ];
			CurInput[ ch ] = &base[ 0 ];
			CurOutput[ ch ] = &base[ BlockLen2 ];
			PrevInput[ ch ] = &base[ BlockLen2 * 2 ];
		}

		clear();
	}

	virtual ~CDSPBankBlockConvolver()
	{
		delete fftin;

		if( ffto2 != R8B_NULL )
		{
			delete ffto2;
		}
	}

	virtual int getMaxOutLen( const int MaxInLen ) const
	{
		R8BASSERT( MaxInLen >= 0 );

		return(( MaxInLen * UpFactor + DownFactor - 1 ) / DownFactor );
	}

	virtual double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	virtual void clear()
	{
		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			memset( PrevInput[ ch ], 0,
				(size_t) PrevInputLen * sizeof( double ));

			if( DoConsumeLatency )
			{
				LatencyLeftArr[ ch ] = Latency;
			}
			else
			{
				LatencyLeftArr[ ch ] = 0;

				if( DownShift > 0 )
				{
					memset( CurOutput[ ch ], 0,
						(size_t) ( BlockLen2 >> DownShift ) *
						sizeof( double ));
				}
				else
				{
					memset( &CurOutput[ ch ][ BlockLen2 - OutOffset ], 0,
						(size_t) OutOffset * sizeof( double ));

					memset( CurOutput[ ch ], 0,
						(size_t) ( InputLen - OutOffset ) *
						sizeof( double ));
				}
			}

			memset( CurInput[ ch ], 0,
				(size_t) InputDelay * sizeof( double ));

			InDataLeft[ ch ] = InputLen - InputDelay;
			UpSkipArr[ ch ] = 0;
			DownSkipArr[ ch ] = DownSkipInit;
		}
	}

	virtual int process( double* const* ip0, int l0in, double** op0 )
	{
		R8BASSERT( l0in >= 0 );

		int l = l0in * UpFactor;
		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			OutCursor[ ch ] = op0[ ch ];
			ipCur[ ch ] = ip0[ ch ];
		}

		while( l > 0 )
		{
			const int Offs = InputLen - InDataLeft[ 0 ];

			if( l < InDataLeft[ 0 ])
			{
				for( ch = 0; ch < ChannelCount; ch++ )
				{
					InDataLeft[ ch ] -= l;

					if( UpShift >= 0 )
					{
						memcpy( &CurInput[ ch ][ Offs >> UpShift ],
							ipCur[ ch ],
							(size_t) ( l >> UpShift ) * sizeof( double ));
					}
					else
					{
						copyUpsample( ch, CurInput[ ch ] + Offs, l );
					}

					copyToOutput( ch, Offs - OutOffset, l );
				}

				break;
			}

			const int b = InDataLeft[ 0 ];
			l -= b;

			int ilu;

			if( UpShift >= 0 )
			{
				const int bu = b >> UpShift;
				ilu = InputLen >> UpShift;

				for( ch = 0; ch < ChannelCount; ch++ )
				{
					memcpy( &CurInput[ ch ][ Offs >> UpShift ],
						ipCur[ ch ], (size_t) bu * sizeof( double ));

					ipCur[ ch ] += bu;
					InDataLeft[ ch ] = InputLen;
				}
			}
			else
			{
				ilu = InputLen;

				for( ch = 0; ch < ChannelCount; ch++ )
				{
					copyUpsample( ch, CurInput[ ch ] + Offs, b );
					InDataLeft[ ch ] = InputLen;
				}
			}

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				double* const cur = CurInput[ ch ];

				memcpy( &cur[ ilu ], PrevInput[ ch ],
					(size_t) PrevInputLen * sizeof( double ));

				memcpy( PrevInput[ ch ], &cur[ ilu - PrevInputLen ],
					(size_t) PrevInputLen * sizeof( double ));

				realfft_t* CurInputFFT = ( *fftin ) -> forward( cur );
				realfft_t dsn = 0;

				if( UpShift > 0 || DownShift > 0 )
				{
					CurInputFFT = ( *fftin ) -> reorderForward( CurInputFFT,
						( ( *fftin ) -> getLen() > ( *fftout ) -> getLen() ?
						( *fftin ) -> getWorkBuf() :
						( *fftout ) -> getWorkBuf() ));

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

					CurInputFFT = ( *fftout ) -> reorderInverse( CurInputFFT,
						cur );
				}

				if( Filter -> isZeroPhase() )
				{
					( *fftout ) -> multiplyBlocksZP(
						Filter -> getKernelBlock(), CurInputFFT );
				}
				else
				{
					( *fftout ) -> multiplyBlocks(
						Filter -> getKernelBlock(), CurInputFFT );
				}

				if( DownShift > 0 )
				{
					( *fftout ) -> setBinNyquist( CurInputFFT, dsn );
				}

				( *fftout ) -> inverse( CurInputFFT );

				copyToOutput( ch, Offs - OutOffset, b );

				double* const tmp = CurInput[ ch ];
				CurInput[ ch ] = CurOutput[ ch ];
				CurOutput[ ch ] = tmp;
			}
		}

		return( (int) ( OutCursor[ 0 ] - op0[ 0 ]));
	}

private:
	CRefKeeper< CDSPFIRFilter > Filter;
	CDSPRealFFTKeeper* fftin;
	CDSPRealFFTKeeper* ffto2;
	CDSPRealFFTKeeper* fftout;
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
	int DownSkipInit;

	CFixedBuffer< double > WorkBlocks;
	CFixedBuffer< double* > CurInput;
	CFixedBuffer< double* > CurOutput;
	CFixedBuffer< double* > PrevInput;
	CFixedBuffer< double* > ipCur;
	CFixedBuffer< double* > OutCursor;
	CFixedBuffer< int > InDataLeft;
	CFixedBuffer< int > LatencyLeftArr;
	CFixedBuffer< int > UpSkipArr;
	CFixedBuffer< int > DownSkipArr;
	bool DoConsumeLatency;

	/**
	 * @brief Copies samples to the output block while inserting zero samples
	 * for whole-numbered upsampling.
	 */

	void copyUpsample( const int ch, double* op, int l0 )
	{
		int& UpSkip = UpSkipArr[ ch ];
		double*& ip = ipCur[ ch ];

		int b = min( UpSkip, l0 );

		if( b != 0 )
		{
			UpSkip -= b;
			l0 -= b;

			*op = 0.0;
			op++;

			while( --b != 0 )
			{
				*op = 0.0;
				op++;
			}
		}

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
			const size_t zc = (size_t) ( upf - 1 ) * sizeof( double );

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

			UpSkip = upf - lz;

			while( --lz != 0 )
			{
				*op = 0.0;
				op++;
			}
		}
	}

	/**
	 * @brief Copies processed samples from the channel's output block,
	 * consuming latency and applying whole-numbered downsampling.
	 */

	void copyToOutput( const int ch, int Offs, int b )
	{
		if( Offs < 0 )
		{
			if( Offs + b <= 0 )
			{
				Offs += BlockLen2;
			}
			else
			{
				copyToOutput( ch, Offs + BlockLen2, -Offs );
				b += Offs;
				Offs = 0;
			}
		}

		int& LatencyLeft = LatencyLeftArr[ ch ];

		if( LatencyLeft != 0 )
		{
			if( LatencyLeft >= b )
			{
				LatencyLeft -= b;
				return;
			}

			Offs += LatencyLeft;
			b -= LatencyLeft;
			LatencyLeft = 0;
		}

		double*& op = OutCursor[ ch ];
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
				memcpy( op, &CurOutput[ ch ][ Offs >> DownShift ],
					(size_t) b * sizeof( double ));

				op += b;
			}
		}
		else
		{
			if( df > 1 )
			{
				int& DownSkip = DownSkipArr[ ch ];
				const double* ip = &CurOutput[ ch ][ Offs + DownSkip ];
				int l = ( b + df - 1 - DownSkip ) / df;
				DownSkip += l * df - b;

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
				memcpy( op, &CurOutput[ ch ][ Offs ],
					(size_t) b * sizeof( double ));

				op += b;
			}
		}
	}

	/**
	 * @brief Mirrors the input spectrum, equivalent to zero-stuffing.
	 */

	void mirrorInputSpectrum( realfft_t* const p )
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
		p[ bl1 + 1 ] = (realfft_t) 0;
		p[ 1 ] = p[ 0 ];

		for( i = 1; i < UpShift; i++ )
		{
			const int z = bl1 << i;
			memcpy( &p[ z ], p, (size_t) z * sizeof( realfft_t ));
			p[ z + 1 ] = (realfft_t) 0;
		}
	}
};

// ---------------------------------------------------------------------------
// CDSPResamplerBank
// ---------------------------------------------------------------------------

/**
 * @brief Planar multi-channel sample rate converter with shared scheduling.
 *
 * All channels share one frame clock: fractional position advances, filter
 * phase selection, polynomial coefficient evaluation, half-band odd/even
 * scheduling, latency deduction and block boundaries happen once and are
 * applied to each channel's independent history. The FFT transforms of the
 * block convolution stages remain independent per channel.
 *
 * All heap memory is allocated in the constructor. process() and clear() do
 * not allocate or free heap memory.
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
	 * @param aChannelCount Number of channels, a positive value.
	 * @param SrcSampleRate Source signal's sample rate.
	 * @param DstSampleRate Destination signal's sample rate.
	 * @param aMaxInLen Maximal input buffer length, in frames.
	 * @param ReqTransBand Required transition band, in percent.
	 * @param ReqAtten Required stop-band attenuation, in decibel.
	 * @param ReqPhase Required filter's phase response.
	 */

	CDSPResamplerBank( const int aChannelCount, const double SrcSampleRate,
		const double DstSampleRate, const int aMaxInLen,
		const double ReqTransBand = 2.0, const double ReqAtten = 206.91,
		const EDSPFilterPhaseResponse ReqPhase = fprLinearPhase )
		: ChannelCount( aChannelCount )
		, MaxInLen( aMaxInLen )
		, CurMaxOutLen( aMaxInLen )
		, LatencyFrac( 0.0 )
		, StageCount( 0 )
		, CurTmpBuf( 0 )
	{
		R8BASSERT( ChannelCount >= 1 );
		R8BASSERT( SrcSampleRate > 0.0 );
		R8BASSERT( DstSampleRate > 0.0 );
		R8BASSERT( MaxInLen > 0 );
		R8BASSERT( ReqTransBand > 0.0 );
		R8BASSERT( ReqAtten > 0.0 );

		TmpBufCapacities[ 0 ] = 0;
		TmpBufCapacities[ 1 ] = 0;
		ipCur.alloc( ChannelCount );

		if( SrcSampleRate == DstSampleRate )
		{
			return;
		}

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
				addStage( new CDSPBankBlockConvolver( ChannelCount, Counters,
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
				addStage( new CDSPBankBlockConvolver( ChannelCount, Counters,
					CDSPFIRFilterCache :: getLPFilter( 1.0 / i, ReqTransBand,
					ReqAtten, ReqPhase, i, 1 ), i, 1, LatencyFrac ));

				const bool IsThirdFirst = ( i == 3 );

				for( i = 0; i < c; i++ )
				{
					addStage( new CDSPBankHBUpsampler( ChannelCount, Counters,
						ReqAtten, i, ( IsThirdFirst || i > 1 ),
						LatencyFrac ));
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

			addStage( new CDSPBankBlockConvolver( ChannelCount, Counters,
				CDSPFIRFilterCache :: getLPFilter( NormFreq, ReqTransBand,
				ReqAtten, ReqPhase, 2.0, 1 ), 2, 1, LatencyFrac ));

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
				addStage( new CDSPBankFracInterpolator( ChannelCount, Counters,
					SrcSampleRate2 * div, DstSampleRate, ReqAtten, false,
					LatencyFrac ));

				double tb = ( 1.0 - SrcSampleRate * div / DstSampleRate ) /
					tbw;

				if( tb > CDSPFIRFilter :: getLPMaxTransBand() )
				{
					tb = CDSPFIRFilter :: getLPMaxTransBand();
				}

				addStage( new CDSPBankBlockConvolver( ChannelCount, Counters,
					CDSPFIRFilterCache :: getLPFilter( 1.0 / 2, tb,
					ReqAtten, ReqPhase, 2, 1 ), 2, 1, LatencyFrac ));

				int k;

				for( k = 1; k < c; k++ )
				{
					addStage( new CDSPBankHBUpsampler( ChannelCount, Counters,
						ReqAtten, k - 1, ( k > 2 ), LatencyFrac ));
				}
			}
			else
			{
				addStage( new CDSPBankFracInterpolator( ChannelCount, Counters,
					SrcSampleRate2, DstSampleRate, ReqAtten, false,
					LatencyFrac ));
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

		int k;

		for( k = 0; k < c; k++ )
		{
			addStage( new CDSPBankHBDownsampler( ChannelCount, Counters,
				ReqAtten, c - 1 - k, ( IsThirdFirst || k < c - 2 ),
				LatencyFrac ));
		}

		addStage( new CDSPBankBlockConvolver( ChannelCount, Counters,
			CDSPFIRFilterCache :: getLPFilter( NormFreq, ReqTransBand,
			ReqAtten, ReqPhase, FinGain, downf ), 1, downf, LatencyFrac ));

		if( UseInterp )
		{
			addStage( new CDSPBankFracInterpolator( ChannelCount, Counters,
				SrcSampleRate, DstSampleRate * SrcSRDiv, ReqAtten,
				IsThirdFirst, LatencyFrac ));
		}

		createTmpBuffers();
	}

	virtual ~CDSPResamplerBank()
	{
		int i;

		for( i = 0; i < StageCount; i++ )
		{
			delete Stages[ i ];
		}
	}

	/**
	 * @brief Returns the maximal input buffer length per channel, in frames.
	 */

	int getMaxInLen() const
	{
		return( MaxInLen );
	}

	/**
	 * @brief Returns the maximal output buffer length per channel.
	 */

	int getMaxOutLen() const
	{
		return( CurMaxOutLen );
	}

	/**
	 * @brief Returns fractional latency, in samples.
	 */

	double getLatencyFrac() const
	{
		return( LatencyFrac );
	}

	/**
	 * @brief Returns the control-work counters.
	 */

	const CDSPBankCounters& getCounters() const
	{
		return( Counters );
	}

	/**
	 * @brief Resets the control-work counters without touching audio history.
	 */

	void resetCounters()
	{
		Counters = CDSPBankCounters();
	}

	/**
	 * @brief Clears all accumulated audio history, returning the object to
	 * its after-construction state. Control-work counters are not reset.
	 */

	void clear()
	{
		int i;

		for( i = 0; i < StageCount; i++ )
		{
			Stages[ i ] -> clear();
		}
	}

	/**
	 * @brief Performs multi-channel sample rate conversion.
	 *
	 * @param ip0 Array of ChannelCount input buffers, one per channel. Input
	 * and output buffers never overlap.
	 * @param l Common number of input frames, not exceeding MaxInLen.
	 * @param[out] op0 Array receiving ChannelCount pointers to internal output
	 * buffers, valid until the next process() or clear() call.
	 * @return The common number of output frames. A zero-length call does not
	 * advance any audio state. If source and destination sample rates are
	 * equal, the input pointers are returned unchanged.
	 */

	int process( double* const* ip0, int l, double** op0 )
	{
		R8BASSERT( l >= 0 && l <= MaxInLen );

		if( l == 0 )
		{
			// Zero-length calls do not advance audio.

			int ch;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				op0[ ch ] = ( StageCount == 0 ? ip0[ ch ] :
					TmpBufs[ 0 ][ ch ]);
			}

			return( 0 );
		}

		if( StageCount == 0 )
		{
			// Equal sample rates: pass through unchanged.

			int ch;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				op0[ ch ] = ip0[ ch ];
			}

			return( l );
		}

		int ch;

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			ipCur[ ch ] = ip0[ ch ];
		}

		int tp = 0;
		int i;

		for( i = 0; i < StageCount; i++ )
		{
			double** const op = TmpBufs[ tp ];

			l = Stages[ i ] -> process( ipCur, l, op );

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				ipCur[ ch ] = op[ ch ];
			}

			tp ^= 1;
		}

		for( ch = 0; ch < ChannelCount; ch++ )
		{
			op0[ ch ] = ipCur[ ch ];
		}

		return( l );
	}

private:
	static const int MaxStages = 8; ///< Maximal processor chain length.

	int ChannelCount;
	int MaxInLen;
	int CurMaxOutLen;
	double LatencyFrac;
	CDSPBankCounters Counters;
	CDSPBankProcessor* Stages[ MaxStages ];
	int StageCount;
	CFixedBuffer< double > TmpBufAll; ///< Both temporary buffer pools.
	CFixedBuffer< double* > TmpPtrAll; ///< Per-channel tmp pointer storage.
	double** TmpBufs[ 2 ]; ///< The two alternating per-channel buffer pools.
	int TmpBufCapacities[ 2 ];
	int CurTmpBuf;
	CFixedBuffer< double* > ipCur; ///< Per-chain input cursors.

	/**
	 * @brief Adds a stage, tracking output buffer capacities.
	 */

	void addStage( CDSPBankProcessor* const Proc )
	{
		R8BASSERT( StageCount < MaxStages );

		Stages[ StageCount ] = Proc;
		StageCount++;

		LatencyFrac = Proc -> getLatencyFrac();
		CurMaxOutLen = Proc -> getMaxOutLen( CurMaxOutLen );

		if( CurMaxOutLen > TmpBufCapacities[ CurTmpBuf ])
		{
			TmpBufCapacities[ CurTmpBuf ] = CurMaxOutLen;
		}

		CurTmpBuf ^= 1;
	}

	/**
	 * @brief Allocates the alternating per-channel temporary buffers.
	 */

	void createTmpBuffers()
	{
		const int ol = TmpBufCapacities[ 0 ] + TmpBufCapacities[ 1 ];

		if( ol > 0 )
		{
			TmpBufAll.alloc( ChannelCount * ol );
			TmpPtrAll.alloc( ChannelCount * 2 );
			TmpBufs[ 0 ] = &TmpPtrAll[ 0 ];
			TmpBufs[ 1 ] = &TmpPtrAll[ ChannelCount ];

			int ch;

			for( ch = 0; ch < ChannelCount; ch++ )
			{
				TmpBufs[ 0 ][ ch ] =
					&TmpBufAll[ ch * ol ];
				TmpBufs[ 1 ][ ch ] =
					&TmpBufAll[ ch * ol + TmpBufCapacities[ 0 ]];
			}
		}
	}
};

} // namespace r8b

#endif // R8B_CDSPRESAMPLERBANK_INCLUDED
