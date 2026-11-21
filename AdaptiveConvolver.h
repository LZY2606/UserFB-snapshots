// ==================================================================================
// Copyright (c) 2017 HiFi-LoFi
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ==================================================================================

#ifndef _FFTCONVOLVER_ADAPTIVECONVOLVER_H
#define _FFTCONVOLVER_ADAPTIVECONVOLVER_H

#include "PartitionedFFTCore.h"
#include "Utilities.h"

#include <vector>


namespace fftconvolver
{

/**
* @class AdaptiveConvolver
* @brief Block frequency domain NLMS adaptive filter with a causal constraint
*
* The adaptive filter learns, for every reference channel, L causal taps which
* predict a desired signal. Processing is done in blocks of B samples:
*
* - P = ceil(L/B) partitions of B taps each are kept per reference channel as
*   spectra H[c,p]. They are initialized with zeros.
*
* - For block m, X[c,m] is the 2B point FFT of the concatenation of the B
*   sample reference blocks m-1 and m. Before the stream starts, the history
*   is zero.
*
* - The prediction is the last B samples of
*   IFFT(sum over c,p of H[c,p] * X[c,m-p]), using the H values from before
*   the update. The 2B point IFFT uses the 1/(2B) normalization and includes
*   the DC and Nyquist bins.
*
* - e is the corresponding desired block minus the prediction and
*   E = FFT([B zeros, e]).
*
* - The power estimate S is updated first, per frequency bin:
*   S = (1-beta)*S + beta*sum(c,p) |X[c,m-p]|^2.
*
* - The gradient g = IFFT(conj(X[c,m-p]) * E / (delta+S)) is computed per
*   partition and reference channel. Only its first min(B, L-p*B) samples are
*   kept, everything else is zeroed. This time domain causal projection is the
*   only way coefficients are modified: H[c,p] += mu*FFT(g). The update
*   becomes effective for the next block.
*
* There is no leakage, no additional suppression and no double talk detection.
*
* Streaming semantics:
*
* - process() accepts any number of frames, but computation and learning only
*   progress in B sized blocks aligned to the start of the stream. Partial
*   input blocks are accumulated and never learned before they are complete.
*
* - The externally visible delay is exactly B frames: output positions t < B
*   are zero, afterwards predicted[t] and residual[t] hold the prediction and
*   the residual for desired[t-B]. To render the end of a stream, keep
*   feeding zero frames.
*
* - process() and setLearning() do not allocate or free heap memory and do not
*   lock. Memory usage does not grow with the stream length.
*/
class AdaptiveConvolver
{
public:
  AdaptiveConvolver();
  ~AdaptiveConvolver();

  /**
  * @brief Initializes the adaptive convolver
  * @param B Block size, power of 2 in [8, 1024]
  * @param L Number of causal taps per reference channel, in [1, 65536]
  * @param channels Number of reference channels, in [1, 6]
  * @param mu NLMS step size, 0 < mu <= 0.5
  * @param beta Power estimate smoothing factor, 0 < beta <= 1
  * @param delta Power estimate regularization, finite and > 0
  * @return true: Success - false: Invalid configuration
  */
  bool init(size_t B, size_t L, size_t channels, float mu, float beta, float delta);

  /**
  * @brief Processes one chunk of the stream
  * @param references Array of "channels" planar reference buffers
  * @param desired Desired signal (target channel, planar)
  * @param predicted Predicted crosstalk (the learned filter output for the
  *        reference signals), may be null to skip writing
  * @param residual Desired signal with the prediction removed; may alias
  *        desired
  * @param len Number of frames; may be zero, in which case all pointers may
  *        be null and nothing changes
  *
  * The predicted and residual buffers must not overlap each other or the
  * reference buffers. residual may be the same buffer as desired; in that
  * case every input frame is read before its output position is written.
  */
  void process(const Sample* const* references, const Sample* desired,
               Sample* predicted, Sample* residual, size_t len);

  /**
  * @brief Enables or disables coefficient adaptation
  * @param enabled true: keep learning - false: freeze H
  * @return true: Success - false: The accumulated input is not a whole
  *         number of blocks, so the switch cannot be changed. On failure the
  *         current setting is kept.
  *
  * While learning is disabled only the H update stops. The input ring, the
  * output timing and the power estimate continue to run.
  */
  bool setLearning(bool enabled);

  /**
  * @brief Copies the currently learned filter into taps
  * @param channel Reference channel index
  * @param taps Destination for L samples; the result is the concatenation of
  *        the first halves of all partitions (the first min(B,L) taps of
  *        partition 0, then the first min(B,L-B) taps of partition 1, etc.)
  *
  * Does not advance or otherwise modify any state. A null taps pointer or an
  * out of range channel is ignored.
  */
  void getFilter(size_t channel, Sample* taps) const;

  /**
  * @brief Clears the coefficients, the power estimate, the audio history and
  *        the queued output; keeps the configuration and re-enables learning
  */
  void reset();

private:
  bool _configured;
  size_t _blockSize;
  size_t _filterLength;
  size_t _channels;
  size_t _partitions;
  size_t _fftSize;
  size_t _fftComplexSize;
  float _mu;
  float _beta;
  float _delta;
  bool _learning;

  mutable audiofft::AudioFFT _fft;

  // Reference spectra history: one ring with _partitions slots per channel.
  // A stream therefore starts with P zero blocks per channel.
  std::vector<SpectrumRing*> _referenceHistory;

  // Learned filters, _channels * _partitions spectra
  std::vector<SplitComplex*> _filters;

  // Per block bookkeeping
  SampleBuffer _referenceBlock;    // _channels * B samples
  SampleBuffer _previousReference; // _channels * B samples
  SampleBuffer _desiredBlock;      // B samples (copy of the original desired)
  SampleBuffer _delayedDesired;    // B samples, desired[t-B]
  SampleBuffer _predictionBlock;   // B samples, prediction for desired[t-B]
  size_t _blockFill;
  size_t _totalFrames;

  // Scratch, all of size 2B (except the spectra)
  mutable SampleBuffer _fftTime;
  mutable SampleBuffer _readTime; // scratch for getFilter, keeps _fftTime untouched
  SplitComplex _predictionSpectrum;
  SplitComplex _desiredSpectrum;  // E
  SplitComplex _powerSpectrum;    // S
  SplitComplex _gradientSpectrum;
  SplitComplex _bracketSpectrum;

  AdaptiveConvolver(const AdaptiveConvolver&) = delete;
  AdaptiveConvolver& operator=(const AdaptiveConvolver&) = delete;

  void clearState();
  void processBlock();
  static bool isPowerOf2(size_t value);
};

} // End of namespace fftconvolver

#endif // Header guard
