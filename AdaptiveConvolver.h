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

#include "AudioFFT.h"
#include "Utilities.h"

#include <vector>


namespace fftconvolver
{

/**
* @class AdaptiveConvolver
* @brief Causal partitioned frequency-domain NLMS for multi-reference crosstalk cancellation
*
* Each reference channel learns an adaptive filter of L causal taps which is
* stored in P=ceil(L/B) frequency-domain partitions of B taps each. The
* processing is strictly block aligned from the stream start and introduces a
* fixed delay of B frames: the outputs for frame t correspond to the inputs of
* frame t-B (the first B output frames are zeros).
*
* All memory is allocated in init(); process()/setLearning() never allocate.
*/
class AdaptiveConvolver
{
public:
  AdaptiveConvolver();
  ~AdaptiveConvolver();

  /**
  * @brief Initializes the adaptive convolver
  * @param B Block size (power of 2, 8..1024)
  * @param L Number of causal taps learned per reference (1..65536)
  * @param channels Number of reference channels (1..6)
  * @param mu Adaptation step size (0 < mu <= 0.5)
  * @param beta Power averaging factor (0 < beta <= 1)
  * @param delta Power floor (delta > 0 and finite)
  * @return true: Success - false: Invalid configuration
  */
  bool init(size_t B, size_t L, size_t channels, float mu, float beta, float delta);

  /**
  * @brief Processes an arbitrary-length portion of the stream
  *
  * Computation and learning only advance in B-frame blocks aligned from the
  * start of the stream; incomplete blocks are buffered without learning.
  * predicted[t] + residual[t] emitted here correspond to desired[t-B].
  * The residual buffer may alias the desired buffer; all other buffers must
  * not overlap. A length of 0 performs no change and null pointers are allowed.
  *
  * @param references Plane-arranged reference channels (channels x len)
  * @param desired Target frames (len)
  * @param predicted Predicted crosstalk output (len)
  * @param residual Residual output desired-delayed minus predicted (len)
  * @param len Number of frames in this call
  */
  void process(const Sample* const* references,
               const Sample* desired,
               Sample* predicted,
               Sample* residual,
               size_t len);

  /**
  * @brief Enables/disables filter adaptation
  *
  * Only succeeds at a block boundary, i.e. when the number of accumulated
  * input frames is a multiple of B; otherwise the learning state is untouched.
  * While disabled, H updates stop but the input rings, output clock and power
  * estimation keep advancing.
  *
  * @param enabled New learning state
  * @return true: State changed - false: Currently inside a partial block
  */
  bool setLearning(bool enabled);

  /**
  * @brief Exports the current L taps of one reference channel
  *
  * Does not advance the streaming state.
  *
  * @param channel Reference channel index
  * @param taps Output buffer of at least L samples
  */
  void getFilter(size_t channel, Sample* taps) const;

  /**
  * @brief Clears coefficients, power estimates, audio history and queued output
  *
  * The configuration is retained and learning is switched back on; the next
  * stream starts timing from zero again.
  */
  void reset();

private:
  // Processes one complete B-frame block (input buffers must be full)
  void processBlock();

  // Frees all allocated storage (used by init() and the destructor)
  void release();

  size_t filterIndex(size_t channel, size_t partition) const
  {
    return channel * _partitions + partition;
  }

  size_t _blockSize;
  size_t _filterLen;
  size_t _channels;
  size_t _partitions;
  size_t _segSize;
  size_t _fftComplexSize;
  float _mu;
  float _beta;
  float _delta;
  bool _learning;
  bool _configured;

  mutable audiofft::AudioFFT _fft;
  SampleBuffer _fftBuffer;

  std::vector<PartitionSpectrumRing> _referenceSpectra; // One ring per channel
  std::vector<SplitComplex*> _filters;                  // H[channel][partition], flat
  SampleBuffer _power;                                  // Per-bin power S
  SplitComplex _conv;
  SplitComplex _error;
  SplitComplex _gradient;

  SampleBuffer _previousReferences; // channels * B, preceding reference block
  SampleBuffer _blockReferences;    // channels * B, accumulating reference block
  SampleBuffer _blockDesired;       // B, accumulating target block
  SampleBuffer _stagedPredicted;    // B predicted frames of the finished block
  SampleBuffer _stagedResidual;     // B residual frames of the finished block
  SampleBuffer _queuePredicted;     // 2B output ring (at most B frames queued)
  SampleBuffer _queueResidual;      // 2B output ring (at most B frames queued)

  size_t _inputFill;    // Frames accumulated in the input block (0..B-1)
  size_t _outWrite;     // Block-aligned write position in the output rings
  size_t _outRead;      // Read position in the output rings
  size_t _outCount;     // Queued frames not yet emitted (0..B)

  // Mutable scratch storage for the state-less getFilter() export
  mutable SampleBuffer _exportBuffer;
  mutable SplitComplex _exportSpectrum;

  // Prevent uncontrolled usage
  AdaptiveConvolver(const AdaptiveConvolver&);
  AdaptiveConvolver& operator=(const AdaptiveConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard
