#ifndef R8B_CDSPRESAMPLERADJOINT_INCLUDED
#define R8B_CDSPRESAMPLERADJOINT_INCLUDED

#include <cstdint>

namespace r8b {

/**
 * @brief Result of one reverse-time adjoint processing call.
 *
 * The cursor fields use the same absolute frame numbering as
 * CDSPResamplerAdjoint.  Intervals returned by process() are contiguous when
 * the calls are assembled from right to left.
 */
struct AdjointResult
{
	int consumed; ///< Number of rearmost residual frames accepted.
	int produced; ///< Number of source-domain contributions produced.
	std::uint64_t first; ///< First absolute source frame in the output.
	bool done; ///< True only after all residual and source frames are done.
};

/**
 * @brief Adjoint (transpose) of the bounded rational-clock resampling operator.
 *
 * The processor receives residuals in the destination sample domain and
 * accumulates their transposed contributions in the source sample domain.
 * It does not require the original recording and does not call a previously
 * constructed forward resampler.
 *
 * Time flows backwards through process(): the first residual frames supplied
 * are the rightmost frames of the complete residual.  Each channel uses the
 * same finite linear operator independently.
 */
class CDSPResamplerAdjoint
{
public:
	/**
	 * @brief Constructs an adjoint for one finite source/destination pair.
	 *
	 * @param srcRate Integer source rate, 8000..384000.
	 * @param dstRate Integer destination rate, 8000..384000.
	 * @param N Number of source frames, at most 2^40.
	 * @param channels Number of independent channels: 1, 2, or 6.
	 * @param maxBlock Maximum legal process() frame/capacity count.
	 * @param transBand r8brain transition band percentage.
	 * @param atten Required stop-band attenuation in decibels.
	 *
	 * @throw std::invalid_argument if any configuration value is illegal.
	 */
	CDSPResamplerAdjoint( std::uint32_t srcRate, std::uint32_t dstRate,
		std::uint64_t N, int channels, int maxBlock,
		double transBand = 2.0, double atten = 120.0 );

	~CDSPResamplerAdjoint();

	CDSPResamplerAdjoint( const CDSPResamplerAdjoint& ) = delete;
	CDSPResamplerAdjoint& operator=( const CDSPResamplerAdjoint& ) = delete;

	/**
	 * @brief Processes a reverse-order residual block.
	 *
	 * `residual` contains @p frames ascending samples per channel and is the
	 * interval ending at getNextResidualFrame().  Only the rearmost
	 * AdjointResult::consumed frames may be accepted.  Accepted samples are
	 * processed from the highest absolute index downwards.
	 *
	 * @param residual Per-channel residual arrays; may be null only when
	 *  @p frames is zero.
	 * @param frames Number of supplied residual frames, 0..maxBlock and no
	 *  greater than the remaining residual count.
	 * @param source Per-channel output arrays; may be null only when
	 *  @p capacity is zero.
	 * @param capacity Number of writable frames in each output array.
	 * @return Cursor and interval information for this call.
	 *
	 * @throw std::invalid_argument for negative/oversized counts, counts
	 *  beyond the remaining residual, or invalid required pointers.  The
	 *  object state is unchanged when an exception is thrown.
	 */
	AdjointResult process( const double* const* residual, int frames,
		double* const* source, int capacity );

	/// @brief Number of destination-domain frames, M=ceil(N*dst/src).
	std::uint64_t getResidualFrames() const;

	/// @brief Right endpoint of the residual interval not yet received.
	std::uint64_t getNextResidualFrame() const;

	/// @brief Right endpoint of the source interval not yet delivered.
	std::uint64_t getNextSourceFrame() const;

	/// @brief Clears accumulated contributions and restores initial cursors.
	void clear();

private:
	struct Impl;
	Impl* p;
};

} // namespace r8b

#endif // R8B_CDSPRESAMPLERADJOINT_INCLUDED
