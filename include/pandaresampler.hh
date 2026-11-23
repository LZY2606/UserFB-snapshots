// This Source Code Form is licensed MPL-2.0: http://mozilla.org/MPL/2.0
#ifndef __PANDA_RESAMPLER_HH__
#define __PANDA_RESAMPLER_HH__

#include <vector>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

/* ------------------------ configuration ---------------------------- */

// uncomment this to use header only mode
// #define PANDA_RESAMPLER_HEADER_ONLY

/* ------------------------------------------------------------------- */

/** \file pandaresampler.hh
 * \brief This header contains the public API for PandaResampler
 */

namespace PandaResampler {

typedef unsigned int uint;

static inline bool
check (bool value, const char *file, int line, const char *func, const char *what)
{
  if (!value)
    fprintf (stderr, "%s:%d:%s: PANDA_RESAMPLER_CHECK FAILED: %s\n", file, line, func, what);
  return value;
}

#define PANDA_RESAMPLER_CHECK(expr) (PandaResampler::check (expr, __FILE__, __LINE__, __func__, #expr))

/**
 * \brief Array class using aligned memory allocation
 *
 * \ref PandaResampler::Resampler2 works best on blocks of 16-byte aligned floats. One
 * way to ensure proper alignment is using this class:
 * \code
 * AlignedArray<float> samples (256);
 * \endcode
 */
template<class T>
class AlignedArray {
  unsigned char *unaligned_mem;
  T *data;
  size_t n_elements;

  void
  allocate_aligned_data()
  {
    /* for SSE we need 16-byte alignment, but we also ensure that no false
     * sharing will occur (at begin and end of data)
     */
    const size_t cache_line_size = 64;

    unaligned_mem = (unsigned char *) malloc (n_elements * sizeof (T) + 2 * (cache_line_size - 1));
    unsigned char *aligned_mem = unaligned_mem;
    if ((ptrdiff_t) aligned_mem % cache_line_size)
      aligned_mem += cache_line_size - (ptrdiff_t) aligned_mem % cache_line_size;

    data = reinterpret_cast<T *> (aligned_mem);
  }
public:
  AlignedArray (size_t n_elements) :
    n_elements (n_elements)
  {
    allocate_aligned_data();
    for (size_t i = 0; i < n_elements; i++)
      new (data + i) T();
  }
  AlignedArray (const std::vector<T>& elements) :
    AlignedArray (elements.size())
  {
    std::copy (elements.begin(), elements.end(), data);
  }
  ~AlignedArray()
  {
    /* C++ destruction order: last allocated element is deleted first */
    while (n_elements)
      data[--n_elements].~T();
    free (unaligned_mem);
  }
  T&
  operator[] (size_t pos)
  {
    return data[pos];
  }
  const T&
  operator[] (size_t pos) const
  {
    return data[pos];
  }
  size_t size () const
  {
    return n_elements;
  }
  T* begin()
  {
    return &data[0];
  }
  T* end()
  {
    return &data[n_elements];
  }
};

/**
 * \brief Interface for factor 2 resampling classes
 */
class Resampler2 {
  class Impl
  {
  public:
    virtual void   process_block (const float *input, uint n_input_samples, float *output) = 0;
    virtual uint   order() const = 0;
    virtual double delay() const = 0;
    virtual void   reset() = 0;
    virtual bool   sse_enabled() const = 0;
    /* PeriodicPlan support: number of scalar IIR polyphase coefficients and
     * the production coefficient values actually stored in the filter. Non-IIR
     * implementations report 0 and leave the buffer untouched.
     */
    virtual uint   iir_ncoefs() const { return 0; }
    virtual void   iir_export_coefs (double *) const { }
    virtual
    ~Impl()
    {
    }
  };
  std::unique_ptr<Impl> impl_x2;
  std::unique_ptr<Impl> impl_x4;
  std::unique_ptr<Impl> impl_x8;
  uint                  ratio_;

  template<uint ORDER, bool USE_SSE>
  class Upsampler2;
  template<uint ORDER, bool USE_SSE>
  class Downsampler2;
  template<uint ORDER>
  class IIRUpsampler2;
  template<uint ORDER>
  class IIRDownsampler2;
  template<uint ORDER>
  class IIRUpsampler2SSE;
  template<uint ORDER>
  class IIRDownsampler2SSE;
public:
  enum Mode {
    UP,
    DOWN
  };
  /**
   * \brief Precision level (quality/speed tradeoff) for the resampler
   */
  enum Precision {
    PREC_LINEAR = 1,     /* linear interpolation */
    PREC_48DB = 8,
    PREC_72DB = 12,
    PREC_96DB = 16,
    PREC_120DB = 20,
    PREC_144DB = 24
  };
  enum Filter {
    FILTER_IIR,
    FILTER_FIR,
  };
protected:
  Mode      mode_;
  Precision precision_;
  bool      use_sse_if_available_;
  Filter    filter_;
public:
  /**
   * creates a resampler instance fulfilling a given specification
   */
  Resampler2 (Mode      mode,
              uint      ratio,
              Precision precision,
              bool      use_sse_if_available = true,
              Filter    filter = FILTER_FIR);
  /**
   * returns true if an optimized SSE version of the Resampler is available
   */
  static bool        sse_available();
  /**
   * test internal filter implementation
   */
  static bool        test_filter_impl (bool verbose);
  /**
   * finds a precision which is appropriate for at least the specified number of bits
   */
  static Precision   find_precision_for_bits (uint bits);
  /**
   * returns a human-readable name for a given precision
   */
  static const char  *precision_name (Precision precision);
  /**
   * resample a data block
   */
  void
  process_block (const float *input, uint n_input_samples, float *output)
  {
    if (ratio_ == 2)
      {
        impl_x2->process_block (input, n_input_samples, output);
      }
    else if (ratio_ == 1)
      {
        std::copy (input, input + n_input_samples, output);
      }
    else
      {
        while (n_input_samples)
          {
            const uint block_size = 1024;
            const uint n_todo_samples = std::min (block_size, n_input_samples);

            float tmp[block_size * 4];
            float tmp2[block_size * 4];

            if (mode_ == UP)
              {
                if (ratio_ == 4)
                  {
                    impl_x2->process_block (input, n_todo_samples, tmp);
                    impl_x4->process_block (tmp, n_todo_samples * 2, output);
                  }
                else /* ratio_ == 8 */
                  {
                    impl_x2->process_block (input, n_todo_samples, tmp);
                    impl_x4->process_block (tmp, n_todo_samples * 2, tmp2);
                    impl_x8->process_block (tmp2, n_todo_samples * 4, output);
                  }
                output += n_todo_samples * ratio_;
              }
            else /* (mode_ == DOWN) */
              {
                if (ratio_ == 4)
                  {
                    impl_x4->process_block (input, n_todo_samples, tmp);
                    impl_x2->process_block (tmp, n_todo_samples / 2, output);
                  }
                else /* ratio_ == 8 */
                  {
                    impl_x8->process_block (input, n_todo_samples, tmp);
                    impl_x4->process_block (tmp, n_todo_samples / 2, tmp2);
                    impl_x2->process_block (tmp2, n_todo_samples / 4, output);
                  }
                output += n_todo_samples / ratio_;
              }
            input += n_todo_samples;
            n_input_samples -= n_todo_samples;
          }
      }
  }
  /**
   * return FIR filter order
   */
  uint
  order() const
  {
    return impl_x2->order(); // FIXME
  }
  /**
   * Return the delay introduced by the resampler. This delay is guaranteed to
   * be >= 0.0, and for factor 2 resampling always a multiple of 0.5 (1.0 for
   * upsampling).
   *
   * The return value can also be thought of as index into the output signal,
   * where the first input sample can be found.
   *
   * Beware of fractional delays, for instance for downsampling, a delay() of
   * 10.5 means that the first input sample would be found by interpolating
   * output[10] and output[11], and the second input sample equates output[11].
   */
  double
  delay() const
  {
    double d = 0;
    if (mode_ == UP)
      {
        if (ratio_ >= 2)
          d += impl_x2->delay();
        if (ratio_ >= 4)
          d += d + impl_x4->delay();
        if (ratio_ >= 8)
          d += d + impl_x8->delay();
      }
    else /* mode_ == DOWN */
      {
        if (ratio_ >= 2)
          d += impl_x2->delay();
        if (ratio_ >= 4)
          d += impl_x4->delay() / 2;
        if (ratio_ >= 8)
          d += impl_x8->delay() / 4;
      }
    return d;
  }
  /**
   * clear internal history, reset resampler state to zero values
   */
  void
  reset()
  {
    if (ratio_ >= 2)
      impl_x2->reset();
    if (ratio_ >= 4)
      impl_x4->reset();
    if (ratio_ >= 8)
      impl_x8->reset();
  }
  /**
   * return whether the resampler is using sse optimized code
   */
  bool
  sse_enabled() const
  {
    return impl_x2->sse_enabled();
  }
  /**
   * Scalar IIR coefficient introspection for a factor-2 stage (stage_ratio is
   * 2, 4 or 8). Returns the number of polyphase coefficients, or 0 if this is
   * not an IIR resampler or the stage does not exist. Additive helper; it does
   * not change any processing behavior.
   */
  uint  iir_stage_ncoefs (uint stage_ratio) const;
  /**
   * Copy the IIR polyphase coefficients of \a stage_ratio into \a out, which
   * must hold iir_stage_ncoefs() doubles. No-op when the count is 0.
   */
  void  iir_stage_coefs (uint stage_ratio, double *out) const;
protected:
  /* Creates implementation from filter coefficients and Filter implementation class
   *
   * Since up- and downsamplers use different (scaled) coefficients, its possible
   * to specify a scaling factor. Usually 2 for upsampling and 1 for downsampling.
   */
  template<class Filter> static inline Impl*
  create_impl_with_coeffs (const double *d,
	                   uint          order,
	                   double        scaling)
  {
    float taps[order];
    for (uint i = 0; i < order; i++)
      taps[i] = d[i] * scaling;

    Resampler2::Impl *filter = new Filter (taps);
    if (!PANDA_RESAMPLER_CHECK (order == filter->order()))
      return nullptr;

    return filter;
  }
  /* creates the actual implementation; specifying USE_SSE=true will use
   * SSE instructions, USE_SSE=false will use FPU instructions
   *
   * Don't use this directly - it's only to be used by
   * bseblockutils.cc's anonymous Impl classes.
   */
  template<bool USE_SSE> inline Impl*
  create_impl (uint stage_ratio);

  template<bool USE_SSE> inline Impl*
  create_impl_iir (uint stage_ratio);

  template<class CArray>
  inline Impl*
  create_impl_iir_with_coeffs (const CArray& carray, double group_delay);

  void
  init_stage (std::unique_ptr<Impl>& impl,
              uint                   stage_ratio);

  friend class PeriodicPlan;
};

/**
 * \brief Periodically steady-stated, multi-channel IIR resampling plan
 *
 * A PeriodicPlan resamples a finite, looping source buffer (planar PCM) with
 * the same scalar IIR filters used by Resampler2 (FILTER_IIR, SSE disabled).
 * Unlike a freshly constructed Resampler2 - whose IIR memories start at zero
 * and therefore need many repetitions of the loop before their transient has
 * decayed - a PeriodicPlan computes the exact all-pass filter state that the
 * infinite repetition of the source loop would have reached in steady state.
 * The very first rendered sample therefore already equals the stable periodic
 * response; there is no zero-state ramp-up, no warm-up and no cross-fade.
 *
 * Only the scalar (FPU) IIR path is supported: ratio must be 2, 4 or 8 and
 * precision must be one of PREC_48DB, PREC_96DB or PREC_144DB.
 *
 * The construction cost is a finite, deterministic periodic boundary
 * calculation (it grows with the loop length, ratio and filter order, but not
 * with any convergence/iteration count). render() always runs the real IIR
 * recursion with the production coefficients and state topology; it never
 * allocates or frees heap memory and never plays back a precomputed table.
 *
 * The causal group delay of the underlying filters is left untouched (no
 * extra group-delay compensation), and the conversion phase is identical to
 * running Resampler2::process_block() starting at source frame 0.
 */
class PeriodicPlan {
public:
  /**
   * \param mode       UP or DOWN resampling
   * \param ratio      2, 4 or 8
   * \param precision  PREC_48DB, PREC_96DB or PREC_144DB
   * \param channels   number of planar channels, 1..8
   * \param source     array of \a channels planar float buffers, each holding
   *                   the same number N of frames, 1 <= N <= 65536; the data
   *                   is copied
   *
   * \throws std::invalid_argument for non-finite samples, zero/out-of-range
   * channels or ratio/precision, an empty or too long loop, unequal channel
   * lengths being impossible by construction, or null pointers. A failed
   * construction leaves every other, independently used converter untouched.
   */
  PeriodicPlan (Resampler2::Mode       mode,
                uint                   ratio,
                Resampler2::Precision  precision,
                uint                   channels,
                const float * const   *source,
                size_t                 frames);
  ~PeriodicPlan();

  PeriodicPlan (const PeriodicPlan&) = delete;
  PeriodicPlan& operator= (const PeriodicPlan&) = delete;

  /**
   * \brief length, in output frames, of one fully closed output period
   *
   * For upsampling this is N*ratio. For downsampling by R this is
   * N/gcd(N,R): the decimation lattice and the filter histories only close
   * together after R/gcd(N,R) source loops, which is taken into account
   * internally.
   */
  uint64_t period() const;

  /**
   * \brief render \a frames output frames, planar, continuing the stream
   *
   * Output is independent of how the request is split into blocks and of the
   * length of the final remainder. A zero-length call does not advance the
   * stream. No heap memory is allocated or freed during the call.
   *
   * \throws std::invalid_argument if output or any of its channel pointers is
   * null (the stream position is left unchanged in that case).
   */
  void render (float *const *output, size_t frames);

  /**
   * \brief position the next output sample at global output frame \a frame
   *
   * The already solved periodic start state is restored and the stream is
   * advanced within a single period, so the cost does not grow with the
   * absolute frame number. Positions above 2^53 are reduced modulo the period
   * with integer arithmetic and are never converted through double first.
   */
  void seek (uint64_t frame);

  /**
   * \brief return to the solved periodic start (equivalent to seek(0))
   */
  void reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} /* namespace PandaResampler */

// Make sure implementation is included in header-only mode
#if defined(PANDA_RESAMPLER_HEADER_ONLY) && !defined(PANDA_RESAMPLER_SOURCE)
#	define PANDA_RESAMPLER_SOURCE "pandaresampler/pandaresampler.cc"
#	include PANDA_RESAMPLER_SOURCE
#endif

/**
 *
\mainpage PandaResampler

\section intro_sec Introduction

PandaResampler is a fast factor 2 resampler using SSE instructions. The
PandaResampler::Resampler2 class provides the API.

\section simple_example A Simple Example

\code

#include "pandaresampler.hh"

#include <cmath>

using PandaResampler::Resampler2;

class Saturation
{
  static constexpr int OVERSAMPLE = 8;
  static constexpr auto PREC      = Resampler2::PREC_72DB;
  static constexpr float DRIVE    = 3;

  Resampler2 ups { Resampler2::UP, OVERSAMPLE, PREC };
  Resampler2 downs { Resampler2::DOWN, OVERSAMPLE, PREC };
public:
  void
  reset()
  {
    ups.reset();
    downs.reset();
  }
  void
  process (const float *in, size_t n_samples, float *out)
  {
    float tmp[n_samples * OVERSAMPLE];

    ups.process_block (in, n_samples, tmp);
    for (size_t i = 0; i < n_samples * OVERSAMPLE; i++)
      tmp[i] = std::tanh (tmp[i] * DRIVE);
    downs.process_block (tmp, n_samples * OVERSAMPLE, out);
  }
};

\endcode

*/


#endif /* __PANDA_RESAMPLER_HH__ */
