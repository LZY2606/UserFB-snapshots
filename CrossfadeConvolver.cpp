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

#include "CrossfadeConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>


namespace fftconvolver
{

CrossfadeConvolver::CrossfadeConvolver() :
  _initialized(false),
  _blockSize(0),
  _maxIRLen(0),
  _engine(),
  _active(0),
  _target(1),
  _fading(false),
  _fadePos(0),
  _fadeLen(0),
  _oldOut(),
  _newOut(),
  _inputCopy(),
  _primeOut(),
  _history(),
  _histCap(0),
  _histFill(0),
  _histPos(0),
  _irStorage()
{
}


CrossfadeConvolver::~CrossfadeConvolver()
{
  reset();
}


void CrossfadeConvolver::reset()
{
  _initialized = false;
  _blockSize = 0;
  _maxIRLen = 0;
  _engine[0].reset();
  _engine[1].reset();
  _active = 0;
  _target = 1;
  _fading = false;
  _fadePos = 0;
  _fadeLen = 0;
  _oldOut.clear();
  _newOut.clear();
  _inputCopy.clear();
  _primeOut.clear();
  _history.clear();
  _histCap = 0;
  _histFill = 0;
  _histPos = 0;
  _irStorage[0].clear();
  _irStorage[1].clear();
}


bool CrossfadeConvolver::validateIR(const Sample* ir, size_t irLen) const
{
  if (irLen > _maxIRLen)
  {
    return false;
  }

  if (irLen > 0 && ir == 0)
  {
    return false;
  }

  for (size_t i = 0; i < irLen; ++i)
  {
    if (!std::isfinite(ir[i]))
    {
      return false;
    }
  }

  return true;
}


void CrossfadeConvolver::appendHistory(const Sample* input, size_t len)
{
  if (_histCap == 0 || len == 0)
  {
    return;
  }

  // Only the most recent _histCap samples matter: skip any excess first.
  size_t start = 0;
  if (len > _histCap)
  {
    start = len - _histCap;
    len = _histCap;
  }

  for (size_t i = start; i < start + len; ++i)
  {
    _history[_histPos] = input[i];
    _histPos = (_histPos + 1) % _histCap;
    if (_histFill < _histCap)
    {
      ++_histFill;
    }
  }
}


void CrossfadeConvolver::primeEngine(size_t engineIdx)
{
  if (_histFill == 0)
  {
    return;
  }

  // Linearize the ring buffer (oldest sample first) and replay it.
  const size_t start = (_histFill < _histCap) ? 0 : _histPos;
  for (size_t i = 0; i < _histFill; ++i)
  {
    _inputCopy[i] = _history[(start + i) % _histCap];
  }
  _engine[engineIdx].process(_inputCopy.data(), _primeOut.data(), _histFill);
}


void CrossfadeConvolver::finishFade()
{
  const size_t oldEngine = _active;

  // The old engine keeps its allocations on purpose: freeing here would
  // violate the no-allocation/no-free contract of process(). It gets
  // re-initialized on the next setImpulse() or released in reset().
  _active = _target;
  _target = oldEngine;
  _fading = false;
  _fadePos = 0;
  _fadeLen = 0;
}


bool CrossfadeConvolver::init(size_t blockSize, size_t maxIRLen, const Sample* ir, size_t irLen)
{
  reset();

  if (blockSize == 0)
  {
    return false;
  }

  _blockSize = NextPowerOf2(blockSize);
  _maxIRLen = maxIRLen;

  if (!validateIR(ir, irLen))
  {
    reset();
    return false;
  }

  _active = 0;
  _target = 1;
  _fading = false;
  _fadePos = 0;
  _fadeLen = 0;

  // All persistent storage is acquired here once. process() and setImpulse()
  // only reuse it, so the audio path never allocates or frees heap memory.
  // The scratch capacity covers both the longest admissible response and one
  // internal partition, so arbitrary process() lengths can be served in
  // fixed-size sub-chunks.
  const size_t scratchCap = std::max(_blockSize, _maxIRLen);
  if (scratchCap > 0)
  {
    _irStorage[0].resize(_maxIRLen);
    _irStorage[1].resize(_maxIRLen);
    _oldOut.resize(scratchCap);
    _newOut.resize(scratchCap);
    _inputCopy.resize(scratchCap);
    _primeOut.resize(scratchCap);
    if (_maxIRLen > 0)
    {
      _history.resize(_maxIRLen);
    }
  }
  _histCap = _maxIRLen;
  _histFill = 0;
  _histPos = 0;

  // Own copy of the initial impulse response.
  if (irLen > 0)
  {
    ::memcpy(_irStorage[_active].data(), ir, irLen * sizeof(Sample));
  }

  // The initial engine covers the whole stream from the first sample on.
  if (!_engine[_active].init(_blockSize,
                             (irLen > 0) ? _irStorage[_active].data() : 0,
                             irLen))
  {
    reset();
    return false;
  }

  _initialized = true;
  return true;
}


bool CrossfadeConvolver::setImpulse(const Sample* ir, size_t irLen, size_t fadeSamples)
{
  if (!_initialized)
  {
    return false;
  }

  // A cross-fade is still in progress: reject without touching anything.
  if (_fading)
  {
    return false;
  }

  if (!validateIR(ir, irLen))
  {
    return false;
  }

  const size_t newEngine = _target;

  // Own copy of the new impulse response. The storage is reused, so the
  // occupied memory stays bounded regardless of how often responses change.
  if (irLen > 0)
  {
    ::memcpy(_irStorage[newEngine].data(), ir, irLen * sizeof(Sample));
  }

  // Build the new engine for the complete stream so far. Replaying the
  // retained input history gives the same state as continuous processing
  // from sample 0 - pre-change echoes of a long response are preserved.
  if (!_engine[newEngine].init(_blockSize,
                               (irLen > 0) ? _irStorage[newEngine].data() : 0,
                               irLen))
  {
    return false;
  }
  primeEngine(newEngine);

  _fading = true;
  _fadePos = 0;
  _fadeLen = fadeSamples;
  return true;
}


void CrossfadeConvolver::process(const Sample* input, Sample* output, size_t len)
{
  if (len == 0)
  {
    // No state is advanced; the pointers may be NULL.
    return;
  }

  if (!_initialized)
  {
    ::memset(output, 0, len * sizeof(Sample));
    return;
  }

  // Keep the input history even while a mute filter is active.
  appendHistory(input, len);

  if (!_fading)
  {
    // FFTConvolver copies the input into its own buffers first, so an
    // in-place call (input == output) is safe here.
    _engine[_active].process(input, output, len);
    return;
  }

  // Cross-fade path: run both engines on identical input, in fixed-size
  // sub-chunks so that no allocation is needed regardless of len.
  const size_t scratchCap = std::max(_blockSize, _maxIRLen);
  size_t off = 0;
  while (off < len)
  {
    if (!_fading)
    {
      // The fade finished in an earlier sub-chunk: hand the remainder to
      // the now active engine.
      _engine[_active].process(input + off, output + off, len - off);
      return;
    }

    const size_t step = std::min(scratchCap, len - off);
    const Sample* src = input + off;

    // A private copy decouples the engines from an in-place call, since the
    // first engine would otherwise overwrite its own input.
    if (input == output)
    {
      ::memcpy(_inputCopy.data(), src, step * sizeof(Sample));
      src = _inputCopy.data();
    }

    _engine[_active].process(src, _oldOut.data(), step);
    _engine[_target].process(src, _newOut.data(), step);

    if (_fadeLen == 0)
    {
      // Hard switch: the new convolution from the very next sample on.
      ::memcpy(output + off, _newOut.data(), step * sizeof(Sample));
      finishFade();
      off += step;
      continue;
    }

    // Linear cross-fade: a = (j+1)/F with the global fade position.
    size_t i = 0;
    while (i < step && _fadePos < _fadeLen)
    {
      const Sample a =
          static_cast<Sample>(static_cast<double>(_fadePos + 1) /
                              static_cast<double>(_fadeLen));
      output[off + i] =
          (Sample(1.0) - a) * _oldOut[i] + a * _newOut[i];
      ++_fadePos;
      ++i;

      if (_fadePos == _fadeLen)
      {
        finishFade();
      }
    }

    // Fade completed within this sub-chunk: the rest is the new engine only.
    for (; i < step; ++i)
    {
      output[off + i] = _newOut[i];
    }

    off += step;
  }
}

} // End of namespace fftconvolver
