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

#ifndef _FFTCONVOLVER_MATRIXCONVOLVER_H
#define _FFTCONVOLVER_MATRIXCONVOLVER_H

#include "AudioFFT.h"
#include "Utilities.h"

#include <cstddef>
#include <vector>


namespace fftconvolver
{

/**
* @class MatrixConnection
* @brief One causal connection of an input channel to an output channel
*
* The output channel is the sum of the causal convolutions of all connections
* ending at that channel. Several connections with the same input/output pair
* are accumulated (not replaced). An impulse response of length 0 contributes
* nothing.
*/
struct MatrixConnection
{
  size_t input;       ///< Index of the input channel
  size_t output;      ///< Index of the output channel
  const Sample* ir;   ///< Impulse response samples (may be nullptr if irLen == 0)
  size_t irLen;       ///< Number of impulse response samples
};


/**
* @class MatrixConvolver
* @brief Multi-input/multi-output partitioned FFT convolution matrix
*
* The convolver implements a sparse convolution matrix with an arbitrary number
* of input and output channels: Each output channel is the sum of the causal
* convolutions of all connections ending at that channel. It uses the same
* short-head / long-tail two-stage partition strategy as TwoStageFFTConvolver,
* but the FFT work is shared across the whole matrix:
*
* - Each input channel performs only one forward FFT per partition (at each
*   partition scale), no matter how many connections depart from it.
*
* - The frequency-domain contributions of all connections ending at the same
*   output channel are accumulated, and the output gets only one inverse FFT
*   per partition.
*
* As the existing single-channel convolvers, the MatrixConvolver has zero extra
* block latency and performs no heap allocation/deallocation during process().
*
* Channel layout is planar: process() receives one sample array per channel,
* and all channels process the same number of frames per call. The calls may
* use arbitrary frame counts. An output array is allowed to alias a different
* input channel's array (in-place operation), as all inputs are snapshotted
* internally before any output is written. Output arrays must not overlap each
* other.
*/
class MatrixConvolver
{
public:
  /**
  * @brief Number of FFT calls performed by the convolver
  *
  * The counters are separated by partition scale and direction, so the
  * frequency-domain sharing can be verified: Forward FFTs scale with the
  * number of active input channels and inverse FFTs with the number of
  * active output channels - never with the number of connections.
  */
  struct FFTStats
  {
    size_t headForward = 0;   ///< Forward FFTs at the head partition scale
    size_t headInverse = 0;   ///< Inverse FFTs at the head partition scale
    size_t tailForward = 0;   ///< Forward FFTs at the tail partition scale
    size_t tailInverse = 0;   ///< Inverse FFTs at the tail partition scale
  };

  MatrixConvolver();
  ~MatrixConvolver();

  /**
  * @brief Initializes the convolution matrix
  *
  * The impulse response data is fully copied and transformed during init(),
  * so the connection table and the response arrays can be released or modified
  * after a successful call. A failed initialization leaves the convolver reset.
  *
  * @param numInputs Number of input channels (must be > 0)
  * @param numOutputs Number of output channels (must be > 0)
  * @param headBlockSize Head partition size (rounded up to power of 2)
  * @param tailBlockSize Tail partition size (rounded up to power of 2,
  *                      power-of-2 head size must not exceed the tail size)
  * @param connections The connection table (may be nullptr if numConnections == 0)
  * @param numConnections Number of connections
  * @return true: Success - false: Failed (illegal channel counts or indices,
  *         illegal block sizes, null response data or non-finite coefficients)
  */
  bool init(size_t numInputs,
            size_t numOutputs,
            size_t headBlockSize,
            size_t tailBlockSize,
            const MatrixConnection* connections,
            size_t numConnections);

  /**
  * @brief Convolves one planar block of all input channels
  *
  * All arrays contain the same number of frames. Output channels without any
  * connection are filled with zeros. Arrays of different output channels must
  * not overlap, but an output array may alias an input array of any channel.
  * A call with len == 0 does nothing and in particular does not advance the
  * internal history (the pointers may be nullptr in that case).
  *
  * @param inputs Array of numInputs input sample arrays
  * @param outputs Array of numOutputs output sample arrays
  * @param len Number of frames per channel
  */
  void process(const Sample* const* inputs, Sample* const* outputs, size_t len);

  /**
  * @brief Resets the convolver, discards all connections and history
  *
  * A subsequent init() starts with empty history - no tail of previously
  * processed data survives.
  */
  void reset();

  /**
  * @brief Returns the FFT call counters since the last resetFFTStats()
  */
  FFTStats fftStats() const;

  /**
  * @brief Resets the FFT call counters
  */
  void resetFFTStats();

private:
  struct EdgeState
  {
    size_t input;
    size_t output;
    std::vector<SplitComplex*> headIR;   ///< IR[0, T), partitioned at head size
    std::vector<SplitComplex*> tail0IR;  ///< IR[T, 2T), partitioned at head size
    std::vector<SplitComplex*> tailIR;   ///< IR[2T, ...), partitioned at tail size
  };

  struct InputState
  {
    InputState() : headUsed(false), tailUsed(false) {}

    bool headUsed;
    bool tailUsed;
    SampleBuffer headInputBuffer;             ///< H sample accumulation buffer
    std::vector<SplitComplex*> headSegments;  ///< Frequency ring (head scale)
    SampleBuffer tailInputBuffer;             ///< T sample accumulation buffer
    std::vector<SplitComplex*> tailSegments;  ///< Frequency ring (tail scale)
  };

  struct OutputState
  {
    OutputState() : headActive(false), tailLongActive(false) {}

    bool headActive;        ///< Has at least one head or tail-0 connection
    bool tailLongActive;    ///< Has at least one long-tail connection

    std::vector<size_t> edges;                  ///< Indices of incoming edges
    SampleBuffer headOverlap;                   ///< H overlap samples
    SampleBuffer tailOverlap[2];                ///< Parity slots for 2-block-late overlap
    SampleBuffer tailLongDelay[2];              ///< Parity slots for 1-block-late tail result
  };

  size_t _numInputs;
  size_t _numOutputs;
  size_t _headBlockSize;
  size_t _tailBlockSize;
  size_t _headFftSize;
  size_t _tailFftSize;
  size_t _headComplexSize;
  size_t _tailComplexSize;
  size_t _headSegCount;
  size_t _tailSegCount;

  audiofft::AudioFFT _fftHead;
  audiofft::AudioFFT _fftTail;
  SampleBuffer _headFftBuffer;   ///< Shared 2H real scratch buffer
  SampleBuffer _tailFftBuffer;   ///< Shared 2T real scratch buffer
  SplitComplex _headAcc;         ///< Shared head-scale frequency accumulator
  SplitComplex _tailAcc;         ///< Shared tail-scale frequency accumulator

  std::vector<InputState*> _inputs;
  std::vector<OutputState*> _outputs;
  std::vector<EdgeState*> _edges;

  size_t _headFill;
  size_t _tailFill;
  size_t _headCurrent;
  size_t _tailCurrent;
  size_t _headBlockCount;
  size_t _tailBlockCount;

  FFTStats _stats;

  // Prevent uncontrolled usage
  MatrixConvolver(const MatrixConvolver&);
  MatrixConvolver& operator=(const MatrixConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard
