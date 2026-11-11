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

#ifndef _FFTCONVOLVER_CROSSFADEFFTCONVOLVER_H
#define _FFTCONVOLVER_CROSSFADEFFTCONVOLVER_H

#include "FFTConvolver.h"
#include "Utilities.h"

#include <vector>


namespace fftconvolver
{

/**
* @class CrossfadeConvolver
* @brief Single-channel partitioned FFT convolver with seamless impulse response swapping
*
* The convolver keeps two internal convolution engines running in parallel while
* an impulse response change is in progress: The old engine produces the output
* for the previous response, the new engine produces the output for the new
* response (including the response of all input which arrived before the change),
* and the two outputs are linearly cross-faded.
*
* Both engines are fed the very same continuous input stream, starting at the
* same sample (the beginning of the stream or the last reset). Consequently the
* new engine is never cold-started at the change point: echoes of pre-change
* input which only appear with the longer new response are reproduced correctly.
*
* As the basic FFTConvolver, this class is suitable for real-time processing:
* process() neither allocates/frees heap memory nor uses locks, and it introduces
* no additional block latency. Arbitrary processing lengths (even 1) and change
* points within FFT partitions are supported; the produced output is independent
* of how the stream is split into processing calls.
*
* The host is expected to call setImpulse() synchronously between process()
* calls - no multithreading support is provided.
*/
class CrossfadeConvolver
{
public:
  CrossfadeConvolver();
  ~CrossfadeConvolver();

  /**
  * @brief Initializes the convolver
  * @param blockSize Block size internally used by the convolver (gets rounded
  *                  up to the next power of 2, as in FFTConvolver)
  * @param maxIRLen Maximum admissible impulse response length for this instance
  * @param ir The initial impulse response (may be NULL if irLen is 0)
  * @param irLen Length of the initial impulse response
  * @return true: Success - false: Failed
  */
  bool init(size_t blockSize, size_t maxIRLen, const Sample* ir, size_t irLen);

  /**
  * @brief Switches the impulse response, starting at the next processed sample
  *
  * The change takes place after the already processed samples, i.e. the next
  * output sample is the first one of the cross-fade. For fadeSamples F > 0,
  * the samples j=0..F-1 of the new region are
  *   (1-a) * oldConvolution[j] + a * newConvolution[j], a = (j+1)/F,
  * afterwards only the new convolution is output. For F = 0, the new
  * convolution is output directly from the next sample on.
  *
  * A new request while a cross-fade is still running is rejected (returns
  * false) and leaves the ongoing cross-fade untouched.
  *
  * @param ir The new impulse response (may be NULL if irLen is 0)
  * @param irLen Length of the new impulse response (must not exceed maxIRLen)
  * @param fadeSamples Length of the linear cross-fade in samples
  * @return true: Success - false: Rejected (cross-fade active, length too
  *         long, non-empty length with NULL pointer, non-finite coefficients
  *         or instance not initialized)
  */
  bool setImpulse(const Sample* ir, size_t irLen, size_t fadeSamples);

  /**
  * @brief Convolves the given input samples and immediately outputs the result
  *
  * Supports input == output (in-place processing). With len == 0, the pointers
  * may be NULL and no internal state is advanced.
  *
  * @param input The input samples
  * @param output The convolution result
  * @param len Number of input/output samples
  */
  void process(const Sample* input, Sample* output, size_t len);

  /**
  * @brief Resets the convolver, discards the impulse responses and all history
  *
  * After reset(), the instance has to be initialized again via init().
  * process() on an uninitialized convolver just outputs silence.
  */
  void reset();

private:
  // Checks the admissibility of an impulse response.
  bool validateIR(const Sample* ir, size_t irLen) const;

  // Appends the given samples to the bounded input history ring buffer.
  void appendHistory(const Sample* input, size_t len);

  // Replays the retained input history into the given freshly initialized
  // engine, so that its state reflects the whole stream instead of a cold
  // start at the change point.
  void primeEngine(size_t engineIdx);

  // Retires the old engine once the cross-fade has completed.
  void finishFade();

  bool _initialized;
  size_t _blockSize;   // Power-of-2 partition size
  size_t _maxIRLen;    // Maximum admissible impulse response length

  FFTConvolver _engine[2];
  size_t _active;      // Engine which currently defines the output
  size_t _target;      // Engine taking over (only meaningful while fading)
  bool _fading;
  size_t _fadePos;     // Already cross-faded samples
  size_t _fadeLen;     // Total cross-fade length in samples

  SampleBuffer _oldOut;
  SampleBuffer _newOut;
  SampleBuffer _inputCopy;
  SampleBuffer _primeOut;

  // Bounded ring buffer of the most recent input samples (at most _maxIRLen),
  // which keeps the input history alive even while a mute filter is active.
  SampleBuffer _history;
  size_t _histCap;
  size_t _histFill;
  size_t _histPos;

  // Persistent storage for the impulse response copies (class-owned, so later
  // modifications of the caller's arrays don't affect the sound). The buffers
  // are reused between changes and never grow beyond the maximum IR length.
  SampleBuffer _irStorage[2];

  // Prevent uncontrolled usage
  CrossfadeConvolver(const CrossfadeConvolver&);
  CrossfadeConvolver& operator=(const CrossfadeConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard
