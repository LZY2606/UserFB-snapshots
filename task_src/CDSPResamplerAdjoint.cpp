#include "CDSPResamplerAdjoint.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

// The adjoint planner reuses the exact chain builder and coefficient caches.
// Access to the planned linked stages is needed only during construction.
#define private public
#define protected public
#include "task_support/rational_reference.h"
#undef private
#undef protected

namespace r8b {
namespace {

using i128 = __int128_t;

std::int64_t floor_div( i128 a, i128 b )
{
	i128 q = a / b;
	const i128 r = a % b;

	if( r != 0 && ( ( r < 0 ) != ( b < 0 ) ) )
	{
		--q;
	}

	return( (std::int64_t) q );
}

std::int64_t ceil_div( i128 a, i128 b )
{
	return( -floor_div( -a, b ) );
}

std::uint64_t rational_M( std::uint64_t N, std::uint64_t S, std::uint64_t D )
{
	return( ( N / S ) * D + ( ( N % S ) * D + S - 1 ) / S );
}

struct Stage
{
	enum Type
	{
		Block,
		HBUp,
		HBDown,
		Frac
	};

	Type type = Frac;

	// Generic integer mapping parameters.
	std::int64_t up = 1;
	std::int64_t down = 1;
	std::int64_t center = 0;
	std::int64_t taps = 0; // HB tap count / Block kernel length marker.

	// Fractional stage data.
	std::uint64_t S = 1;
	std::uint64_t D = 1;
	double alpha = 0.0;
	bool whole = false;
	int filter_len = 0;
	int fll = 0;
	int fl2 = 0;
	int in_step = 0;
	int out_step = 1;
	int init_w = 0;
	int fracs = 0;

	// Block FIR, HB coefficients, or fractional coefficient table.
	std::vector< double > coef;
};

// Conservative monotone upper bound on the largest input coordinate touched
// by output coordinate k.
std::int64_t stage_upper( const Stage& s, std::int64_t k )
{
	switch( s.type )
	{
		case Stage::Block:
			return floor_div( (i128) s.center + (i128) s.down * k, s.up );

		case Stage::HBUp:
			return floor_div( k, 2 ) + s.taps;

		case Stage::HBDown:
			return 2 * k + 2 * s.taps - 1;

		case Stage::Frac:
		{
			if( s.whole )
			{
				const std::int64_t ic = floor_div(
					(i128) s.init_w + (i128) k * s.in_step, s.out_step );
				return( ic + s.fl2 );
			}

			const std::int64_t q = floor_div( (i128) k * s.S, s.D );
			return( q + 1 + s.fl2 );
		}
	}

	return( k );
}

// Conservative monotone lower bound on the smallest input coordinate touched
// by output coordinate k.
std::int64_t stage_lower( const Stage& s, std::int64_t k )
{
	switch( s.type )
	{
		case Stage::Block:
			return ceil_div( (i128) s.center + (i128) s.down * k -
				( s.taps - 1 ), s.up );

		case Stage::HBUp:
			return floor_div( k, 2 ) - s.taps;

		case Stage::HBDown:
			return 2 * k - 2 * s.taps - 1;

		case Stage::Frac:
		{
			if( s.whole )
			{
				const std::int64_t ic = floor_div(
					(i128) s.init_w + (i128) k * s.in_step, s.out_step );
				return( ic - s.fll );
			}

			const std::int64_t q = floor_div( (i128) k * s.S, s.D );
			return( q - s.fl2 - 2 );
		}
	}

	return( k );
}

template< class Add >
void for_each_tap( const Stage& s, std::uint64_t j, Add add )
{
	switch( s.type )
	{
		case Stage::Block:
		{
			const std::int64_t base = s.center + s.down * (std::int64_t) j;
			const std::int64_t imin = ceil_div( (i128) base -
				( s.taps - 1 ), s.up );
			const std::int64_t imax = floor_div( base, s.up );

			for( std::int64_t i = imin; i <= imax; ++i )
			{
				const std::int64_t ti = base - s.up * i;

				if( ti >= 0 && ti < s.taps )
				{
					add( i, s.coef[ (size_t) ti ] );
				}
			}
			break;
		}

		case Stage::HBUp:
		{
			const std::uint64_t r = j / 2;

			if(( j & 1U ) == 0 )
			{
				add( (std::int64_t) r, 1.0 );
			}
			else
			{
				for( std::int64_t k = 0; k < s.taps; ++k )
				{
					const double h = s.coef[ (size_t) k ];
					add( (std::int64_t) r + k + 1, h );
					add( (std::int64_t) r - k, h );
				}
			}
			break;
		}

		case Stage::HBDown:
		{
			add( 2 * (std::int64_t) j, 1.0 );

			for( std::int64_t k = 0; k < s.taps; ++k )
			{
				const double h = s.coef[ (size_t) k ];
				add( 2 * (std::int64_t) j + 2 * k + 1, h );
				add( 2 * (std::int64_t) j - 2 * k - 1, h );
			}
			break;
		}

		case Stage::Frac:
		{
			double c[ 32 ];

			if( s.whole )
			{
				const i128 num = (i128) s.init_w + (i128) j * s.in_step;
				const std::uint64_t ic = (std::uint64_t) floor_div(
					num, s.out_step );
				const int fpos = (int)( num % s.out_step );
				const double* row = &s.coef[ (size_t) fpos * s.filter_len ];

				for( int t = 0; t < s.filter_len; ++t )
				{
					c[ t ] = row[ t ];
				}

				for( int t = 0; t < s.filter_len; ++t )
				{
					add( (std::int64_t) ic + t - s.fll, c[ t ] );
				}
			}
			else
			{
				const task_reference::Point pt = task_reference::phase_at(
					j, s.S, s.D, s.alpha );
				double x = pt.fraction * (double) s.fracs;
				int fti = (int) x;

				if( fti < 0 )
				{
					fti = 0;
				}

				if( fti > s.fracs )
				{
					fti = s.fracs;
				}

				double u = x - (double) fti;
				const double u2 = u * u;
				const double* row = &s.coef[ (size_t) fti *
					(size_t) s.filter_len * 3 ];

				for( int t = 0; t < s.filter_len; ++t )
				{
					c[ t ] = row[ 3 * t ] + row[ 3 * t + 1 ] * u +
						row[ 3 * t + 2 ] * u2;
				}

				const std::int64_t ic = (std::int64_t) pt.integer;

				for( int t = 0; t < s.filter_len; ++t )
				{
					add( ic + t - s.fll, c[ t ] );
				}
			}
			break;
		}
	}
}

std::vector< double > make_block_kernel( CDSPBlockConvolver& b )
{
	CDSPFIRFilter& f = *b.Filter;

	// Rebuild the exact prepared linear-phase FIR coefficients in their
	// ordinary centered time-domain order.  The FFT representation is not
	// needed because the real-arithmetic operator is the required reference.
	double pwr, hl, fo1;
	CDSPFIRFilter::calcDesignParams( f.ReqTransBand * 0.01, f.ReqAtten,
		pwr, hl, fo1 );

	double win[ 2 ] = { 125.0, pwr };
	CDSPSincFilterGen sinc;
	sinc.Len2 = 0.25 * hl / f.ReqNormFreq;
	sinc.Freq1 = 0.0;
	sinc.Freq2 = R8B_PI * ( 1.0 - fo1 ) * f.ReqNormFreq;
	sinc.initBand( CDSPSincFilterGen :: wftKaiser, win, true );

	std::vector< double > raw( (size_t) sinc.KernelLen );
	sinc.generateBand( raw.data(),
		&CDSPSincFilterGen :: calcWindowKaiser );

	double sum = 0.0;

	for( double v : raw )
	{
		sum += v;
	}

	const double scale = f.ReqGain / sum;

	for( double& v : raw )
	{
		v *= scale;
	}

	return( raw );
}

} // namespace

struct CDSPResamplerAdjoint::Impl
{
	std::uint32_t src_rate = 0;
	std::uint32_t dst_rate = 0;
	std::uint64_t N = 0;
	std::uint64_t M = 0;
	int channels = 1;
	int max_block = 1;

	std::uint64_t residual_cursor = 0;
	std::uint64_t source_cursor = 0;
	std::int64_t emit_next = -1;
	bool done = false;

	std::vector< Stage > stages;
	std::int64_t ring_width = 1;
	std::int64_t kernel_width = 1;
	std::vector< double > source_ring;
	std::vector< std::int64_t > ring_tag;
	std::vector< double > kern_a;
	std::vector< double > kern_b;

	void reset_state()
	{
		residual_cursor = M;
		source_cursor = N;
		emit_next = (std::int64_t) N - 1;
		done = ( M == 0 && N == 0 );

		std::fill( ring_tag.begin(), ring_tag.end(), -1 );
		std::fill( source_ring.begin(), source_ring.end(), 0.0 );
	}

	std::int64_t composite_upper( std::int64_t j ) const
	{
		std::int64_t k = j;

		for( auto it = stages.rbegin(); it != stages.rend(); ++it )
		{
			k = stage_upper( *it, k );
		}

		return( k );
	}

	void compose_row( std::uint64_t j )
	{
		// kern_a contains [a_lo, a_lo + a_len).
		std::int64_t a_lo = (std::int64_t) j;
		int a_len = 1;
		kern_a[ 0 ] = 1.0;

		for( auto it = stages.rbegin(); it != stages.rend(); ++it )
		{
			const Stage& st = *it;
			const std::int64_t a_hi = a_lo + a_len - 1;
			const std::int64_t b_lo = stage_lower( st, a_lo );
			const std::int64_t b_hi = stage_upper( st, a_hi );
			const std::int64_t b_len64 = b_hi - b_lo + 1;
			const int b_len = (int) std::min<std::int64_t>( b_len64,
				kernel_width );

			std::fill( kern_b.begin(), kern_b.begin() + b_len, 0.0 );

			for( int e = 0; e < a_len; ++e )
			{
				const double av = kern_a[ (size_t) e ];

				if( av == 0.0 )
				{
					continue;
				}

				const std::int64_t out_index = a_lo + e;

				// Negative coordinates before a stage are its cleared
				// pre-history, and therefore cannot propagate earlier.
				if( out_index < 0 )
				{
					continue;
				}

				for_each_tap( st, (std::uint64_t) out_index,
					[ & ]( std::int64_t q, double c )
					{
						const std::int64_t pos = q - b_lo;

						if( pos >= 0 && pos < b_len )
						{
							kern_b[ (size_t) pos ] += av * c;
						}
					} );
			}

			kern_a.swap( kern_b );
			a_lo = b_lo;
			a_len = b_len;
		}

		// Store the resulting row location in the first two otherwise unused
		// prefix slots is awkward; keep it in member state for apply_row().
		row_lo = a_lo;
		row_len = a_len;
	}

	std::int64_t row_lo = 0;
	int row_len = 0;

	void apply_row( const double* const* residual, std::uint64_t /*absolute_j*/,
		int array_index )
	{
		for( int ch = 0; ch < channels; ++ch )
		{
			const double g = residual[ ch ][ array_index ];
			double* ring = &source_ring[ (size_t) ch *
				(size_t) ring_width ];

			for( int e = 0; e < row_len; ++e )
			{
				const std::int64_t idx = row_lo + e;

				if( idx < 0 || idx >= (std::int64_t) N )
				{
					continue;
				}

				const std::int64_t slot = idx % ring_width;

				if( ring_tag[ (size_t) slot ] != idx )
				{
					ring_tag[ (size_t) slot ] = idx;

					for( int c2 = 0; c2 < channels; ++c2 )
					{
						source_ring[ (size_t) c2 * (size_t) ring_width +
							(size_t) slot ] = 0.0;
					}
				}

				ring[ (size_t) slot ] += g * kern_a[ (size_t) e ];
			}
		}
	}

	void finalize_through( std::int64_t future_upper )
	{
		while( emit_next > future_upper && emit_next >= 0 )
		{
			// The value is already in the tagged ring.  Advancing the
			// frontier marks it complete; delivery copies it in ascending
			// absolute order later.
			--emit_next;
		}
	}

	std::uint64_t available() const
	{
		if( emit_next + 1 >= (std::int64_t) source_cursor )
		{
			return( 0 );
		}

		return( source_cursor - (std::uint64_t)( emit_next + 1 ) );
	}

	void copy_output( double* const* source, int capacity, int& produced )
	{
		const std::uint64_t avail = available();
		const int p = (int) std::min<std::uint64_t>( avail,
			(std::uint64_t) capacity );

		if( p == 0 )
		{
			return;
		}

		const std::uint64_t first = source_cursor - (std::uint64_t) p;

		for( int ch = 0; ch < channels; ++ch )
		{
			for( int k = 0; k < p; ++k )
			{
				const std::uint64_t idx = first + (std::uint64_t) k;
				const std::int64_t slot = (std::int64_t)( idx %
					(std::uint64_t) ring_width );
				const size_t off = (size_t) ch * (size_t) ring_width +
					(size_t) slot;

				source[ ch ][ k ] =
					( ring_tag[ (size_t) slot ] == (std::int64_t) idx ) ?
					source_ring[ off ] : 0.0;
			}
		}

		source_cursor = first;
		produced = p;
	}
};

CDSPResamplerAdjoint::CDSPResamplerAdjoint( std::uint32_t srcRate,
	std::uint32_t dstRate, std::uint64_t N, int channels, int maxBlock,
	double transBand, double atten )
{
	if( srcRate < 8000 || srcRate > 384000 ||
		dstRate < 8000 || dstRate > 384000 ||
		N > ( 1ULL << 40 ) ||
		( channels != 1 && channels != 2 && channels != 6 ) ||
		maxBlock <= 0 ||
		!( transBand >= CDSPFIRFilter :: getLPMinTransBand() &&
			transBand <= CDSPFIRFilter :: getLPMaxTransBand() ) ||
		!( atten >= CDSPFIRFilter :: getLPMinAtten() &&
			atten <= CDSPFIRFilter :: getLPMaxAtten() ) )
	{
		throw std::invalid_argument( "CDSPResamplerAdjoint: invalid argument" );
	}

	std::unique_ptr< Impl > impl( new Impl );
	impl->src_rate = srcRate;
	impl->dst_rate = dstRate;
	impl->N = N;
	impl->channels = channels;
	impl->max_block = maxBlock;
	impl->M = rational_M( N, srcRate, dstRate );

	if( N != 0 )
	{
		TaskRationalResampler planner( srcRate, dstRate, 127,
			transBand, atten, fprLinearPhase );

		for( CDSPProcessor* node = planner.StepFirst; node != nullptr;
			node = node->Next )
		{
			Stage st;

			if( auto* q = dynamic_cast< CDSPBlockConvolver* >( node ) )
			{
				st.type = Stage::Block;
				st.up = q->UpFactor;
				st.down = q->DownFactor;
				st.center = q->Filter->getLatency();
				st.coef = make_block_kernel( *q );
				st.taps = (std::int64_t) st.coef.size();
			}
			else if( auto* q = dynamic_cast< CDSPHBUpsampler* >( node ) )
			{
				st.type = Stage::HBUp;
				const int count = q->fll + 1; // constructor: fll = fltt - 1
				st.taps = count;
				st.coef.resize( (size_t) count );
				std::memcpy( st.coef.data(), q->fltp,
					(size_t) count * sizeof( double ) );
			}
			else if( auto* q = dynamic_cast< CDSPHBDownsampler* >( node ) )
			{
				st.type = Stage::HBDown;
				const int count = q->fll; // constructor: fll = fltt
				st.taps = count;
				st.coef.resize( (size_t) count );
				std::memcpy( st.coef.data(), q->fltp,
					(size_t) count * sizeof( double ) );
			}
			else if( auto* q =
				dynamic_cast< TaskRationalFracInterpolator* >( node ) )
			{
				st.type = Stage::Frac;
				st.S = (std::uint64_t) q->SrcSampleRate;
				st.D = (std::uint64_t) q->DstSampleRate;
				st.alpha = q->InitFracPos;
				st.whole = q->IsWhole;
				st.filter_len = q->FilterLen;
				st.fll = q->fll;
				st.fl2 = q->fl2;
				st.in_step = q->InStep;
				st.out_step = q->OutStep;
				st.init_w = q->InitFracPosW;
				st.fracs = q->FilterBank->getFilterFracs();

				CDSPFracDelayFilterBank& fb = *q->FilterBank;

				if( st.whole )
				{
					const int rows = st.out_step;
					st.coef.resize( (size_t) rows * st.filter_len );

					for( int r = 0; r < rows; ++r )
					{
						const double* src = &fb[ r ];
						std::memcpy( &st.coef[ (size_t) r *
							st.filter_len ], src,
							(size_t) st.filter_len * sizeof( double ) );
					}
				}
				else
				{
					const int rows = st.fracs + 1;
					st.coef.resize( (size_t) rows * st.filter_len * 3 );

					for( int r = 0; r < rows; ++r )
					{
						const double* src = &fb[ r ];
						std::memcpy( &st.coef[ (size_t) r *
							st.filter_len * 3 ], src,
							(size_t) st.filter_len * 3 * sizeof( double ) );
					}
				}
			}
			else
			{
				throw std::invalid_argument(
					"CDSPResamplerAdjoint: unsupported processing stage" );
			}

			impl->stages.push_back( std::move( st ) );
		}
	}

	// Bound the tagged modulo ring.  Its live span is one row support plus
	// at most two maxBlock-sized cursor/pending windows.
	std::int64_t lo = 0;
	std::int64_t hi = 0;

	for( auto it = impl->stages.rbegin(); it != impl->stages.rend(); ++it )
	{
		const Stage& st = *it;
		const std::int64_t nlo = stage_lower( st, lo );
		const std::int64_t nhi = stage_upper( st, hi );
		lo = nlo;
		hi = nhi;
	}

	const std::int64_t support_bound = hi - lo + 64;
	impl->kernel_width = std::max<std::int64_t>( 64, support_bound + 64 );

	// The live source window contains one composite row plus at most one
	// pending output block and one newly accumulated maxBlock-sized block.
	// The largest legal sample-rate ratio is 48 source frames per residual.
	impl->ring_width = impl->kernel_width +
		64LL * (std::int64_t) maxBlock + 1024;

	const size_t W = (size_t) impl->ring_width;
	const size_t K = (size_t) impl->kernel_width;
	impl->source_ring.assign( (size_t) channels * W, 0.0 );
	impl->ring_tag.assign( W, -1 );
	impl->kern_a.assign( K, 0.0 );
	impl->kern_b.assign( K, 0.0 );
	impl->reset_state();

	p = impl.release();
}

CDSPResamplerAdjoint::~CDSPResamplerAdjoint()
{
	delete p;
}

AdjointResult CDSPResamplerAdjoint::process( const double* const* residual,
	int frames, double* const* source, int capacity )
{
	if( frames < 0 || frames > p->max_block ||
		capacity < 0 || capacity > p->max_block ||
		(std::uint64_t) frames > p->residual_cursor )
	{
		throw std::invalid_argument(
			"CDSPResamplerAdjoint: invalid process count" );
	}

	if( frames > 0 )
	{
		if( residual == nullptr )
		{
			throw std::invalid_argument(
				"CDSPResamplerAdjoint: null residual array" );
		}

		for( int ch = 0; ch < p->channels; ++ch )
		{
			if( residual[ ch ] == nullptr )
			{
				throw std::invalid_argument(
					"CDSPResamplerAdjoint: null residual channel" );
			}
		}
	}

	if( capacity > 0 )
	{
		if( source == nullptr )
		{
			throw std::invalid_argument(
				"CDSPResamplerAdjoint: null source array" );
		}

		for( int ch = 0; ch < p->channels; ++ch )
		{
			if( source[ ch ] == nullptr )
			{
				throw std::invalid_argument(
					"CDSPResamplerAdjoint: null source channel" );
			}
		}
	}

	if( frames > 0 && capacity > 0 )
	{
		for( int ch = 0; ch < p->channels; ++ch )
		{
			const double* a = residual[ ch ];
			const double* b = a + frames;
			double* c = source[ ch ];
			const double* d = c + capacity;

			if( std::less<const double*>()( a, d ) &&
				std::less<const double*>()( c, b ) )
			{
				throw std::invalid_argument(
					"CDSPResamplerAdjoint: overlapping buffers" );
			}
		}
	}

	AdjointResult result{ 0, 0, p->source_cursor, false };

	if( capacity == 0 )
	{
		result.first = p->source_cursor;
		result.done = p->done;
		return( result );
	}

	if( p->done )
	{
		result.first = 0;
		result.done = true;
		return( result );
	}

	const std::uint64_t avail = p->available();

	// If the caller's whole capacity can be filled from completed samples,
	// apply back-pressure before accepting more residual.
	if( avail >= (std::uint64_t) capacity )
	{
		p->copy_output( source, capacity, result.produced );
		result.first = p->source_cursor;
		result.done = ( p->residual_cursor == 0 && p->source_cursor == 0 );
		return( result );
	}

	std::uint64_t old_cursor = p->residual_cursor;

	for( int take = 0; take < frames; ++take )
	{
		const std::uint64_t j = old_cursor - 1 - (std::uint64_t) take;
		const int array_index = frames - 1 - take;

		p->compose_row( j );
		p->apply_row( residual, j, array_index );

		if( j == 0 )
		{
			p->finalize_through( -1 );
		}
		else
		{
			p->finalize_through( p->composite_upper( (std::int64_t) j - 1 ) );
		}
	}

	if( frames > 0 )
	{
		p->residual_cursor -= (std::uint64_t) frames;
		result.consumed = frames;
	}

	p->copy_output( source, capacity, result.produced );
	result.first = p->source_cursor;
	p->done = ( p->residual_cursor == 0 && p->source_cursor == 0 );
	result.done = p->done;

	return( result );
}

std::uint64_t CDSPResamplerAdjoint::getResidualFrames() const
{
	return( p->M );
}

std::uint64_t CDSPResamplerAdjoint::getNextResidualFrame() const
{
	return( p->residual_cursor );
}

std::uint64_t CDSPResamplerAdjoint::getNextSourceFrame() const
{
	return( p->source_cursor );
}

void CDSPResamplerAdjoint::clear()
{
	p->reset_state();
}

} // namespace r8b
