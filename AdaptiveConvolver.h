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
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ==================================================================================

#ifndef _FFTCONVOLVER_ADAPTIVECONVOLVER_H
#define _FFTCONVOLVER_ADAPTIVECONVOLVER_H

#include "PartitionedConvolverCore.h"
#include "Utilities.h"

#include <cstddef>
#include <vector>


namespace fftconvolver
{

/**
* @class AdaptiveConvolver
* @brief Causal partitioned frequency-domain NLMS adaptive filter
*
* Cancels reference signals (e.g. clean accompaniment guides) that leak into a
* vocal track through an unknown short room response. Each reference channel
* gets an adaptive causal filter of L taps stored in P=ceil(L/B) frequency
* partitions. The adaptation is a multi-channel partitioned NLMS:
*
* - Block m's reference spectrum X[c,m] is the 2B-point FFT of two adjacent
*   B-frame reference blocks [block m-1 | block m], zero-padded before the
*   stream starts.
* - Prediction is the back B-frame of IFFT(sum_{c,p} H[c,p] * X[c,m-p]),
*   computed with the filters as they are before the block update.
* - The error block e is desired minus prediction; E = FFT([zeros, e]).
* - Per-bin power: S = (1-beta)*S + beta*sum_{c,p} |X[c,m-p]|^2.
* - The gradient IFFT(conj(X[c,m-p]) * E / (delta + S)) is passed through a
*   time-domain causal projection (only the first min(B, L-p*B) taps of each
*   partition survive) before being re-transformed and added to the filter.
*
* External delay is strictly B frames: the first B output frames are zero and
* predicted[t] + residual[t] afterwards corresponds to desired[t-B]. Arbitrary
* process() call lengths are supported; incomplete blocks are accumulated and
* no learning happens before a block is complete. No heap allocation or
* locking takes place inside process()/setLearning().
*/
class AdaptiveConvolver
{
public:
  AdaptiveConvolver();
  ~AdaptiveConvolver();

  /**
  * @brief Initializes the adaptive convolver
  * @param B Block size (power of 2, 8..1024)
  * @param L Number of causal filter taps per reference (1..65536)
  * @param channels Number of reference channels (1..6)
  * @param mu Adaptation step size (0 < mu <= 0.5)
  * @param beta Power averaging factor (0 < beta <= 1)
  * @param delta Power floor (> 0 and finite)
  * @return true: Success - false: Invalid configuration
  */
  bool init(size_t B, size_t L, size_t channels, float mu, float beta, float delta);

  /**
  * @brief Processes a chunk of interleaved-time, planar-channel audio
  *
  * @param references Array of @c channels planar reference buffers
  * @param desired Target track buffer (may equal @p residual for in-place use)
  * @param predicted Output: the predicted crosstalk
  * @param residual Output: the cleaned signal
  * @param len Number of frames in this call (0 is allowed)
  */
  void process(const Sample* const* references,
               const Sample* desired,
               Sample* predicted,
               Sample* residual,
               size_t len);

  /**
  * @brief Enables/disables filter adaptation
  *
  * Even while disabled, the input ring, output clock and power estimate keep
  * advancing; only the H update is suspended.
  *
  * @return true: Success - false: Called while a partial input block is
  *         accumulated (the switch state is then left unchanged)
  */
  bool setLearning(bool enabled);

  /**
  * @brief Exports the current L causal taps of one reference channel
  *
  * Does not advance the streaming state.
  */
  void getFilter(size_t channel, Sample* taps) const;

  /**
  * @brief Clears coefficients, power estimate, audio history and queued output
  *
  * Keeps the configuration, re-enables learning and restarts timing at zero.
  * No memory is released.
  */
  void reset();

  size_t blockSize() const { return _blockSize; }
  size_t filterLength() const { return _filterLength; }
  size_t channels() const { return _channels; }
  size_t partitionCount() const { return _partitionCount; }
  bool isLearning() const { return _learning; }

private:
  void processCompleteBlock();
  void clearStreamingState();

  size_t _blockSize;
  size_t _fftSize;
  size_t _filterLength;
  size_t _channels;
  size_t _partitionCount;
  size_t _fftComplexSize;
  float _mu;
  float _beta;
  float _delta;
  bool _learning;

  std::vector<PartitionedConvolverCore*> _cores;
  std::vector<SampleBuffer*> _referenceBlocks;
  SampleBuffer _desiredBlock;
  SampleBuffer _blockPrediction;
  SampleBuffer _blockResidual;
  SampleBuffer _power;
  SplitComplex _errorSpectrum;
  SplitComplex _gradientSpectrum;
  SampleBuffer _timeBuffer;

  // Strictly B-frame delayed output queue (starts up filled with B zeros)
  SampleBuffer _queuedPrediction;
  SampleBuffer _queuedResidual;
  size_t _queueReadPos;
  size_t _outputFill;

  size_t _inputFill;

  // Prevent uncontrolled usage
  AdaptiveConvolver(const AdaptiveConvolver&);
  AdaptiveConvolver& operator=(const AdaptiveConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard
