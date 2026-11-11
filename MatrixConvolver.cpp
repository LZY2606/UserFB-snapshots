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

#include "MatrixConvolver.h"

#include <algorithm>
#include <cassert>
#include <cmath>


namespace fftconvolver
{

namespace
{
  bool IsPowerOf2(size_t val)
  {
    return val > 0 && (val & (val - 1)) == 0;
  }

  void DeleteComplexVector(std::vector<SplitComplex*>& segments)
  {
    for (size_t i=0; i<segments.size(); ++i)
    {
      delete segments[i];
    }
    segments.clear();
  }

  // Effective impulse response length after removing the trailing zeros
  // (same policy as the single-channel convolvers)
  size_t EffectiveIrLen(const Sample* ir, size_t irLen)
  {
    while (irLen > 0 && ::fabs(ir[irLen-1]) < 0.000001f)
    {
      --irLen;
    }
    return irLen;
  }
}


MatrixConvolver::MatrixConvolver() :
  _numInputs(0),
  _numOutputs(0),
  _headBlockSize(0),
  _tailBlockSize(0),
  _headFftSize(0),
  _tailFftSize(0),
  _headComplexSize(0),
  _tailComplexSize(0),
  _headSegCount(0),
  _tailSegCount(0),
  _fftHead(),
  _fftTail(),
  _headFftBuffer(),
  _tailFftBuffer(),
  _headAcc(),
  _tailAcc(),
  _inputs(),
  _outputs(),
  _edges(),
  _headFill(0),
  _tailFill(0),
  _headCurrent(0),
  _tailCurrent(0),
  _headBlockCount(0),
  _tailBlockCount(0),
  _stats()
{
}


MatrixConvolver::~MatrixConvolver()
{
  reset();
}


MatrixConvolver::FFTStats MatrixConvolver::fftStats() const
{
  return _stats;
}


void MatrixConvolver::resetFFTStats()
{
  _stats = FFTStats();
}


void MatrixConvolver::reset()
{
  for (size_t e=0; e<_edges.size(); ++e)
  {
    EdgeState* edge = _edges[e];
    DeleteComplexVector(edge->headIR);
    DeleteComplexVector(edge->tail0IR);
    DeleteComplexVector(edge->tailIR);
    delete edge;
  }
  _edges.clear();

  for (size_t i=0; i<_inputs.size(); ++i)
  {
    DeleteComplexVector(_inputs[i]->headSegments);
    DeleteComplexVector(_inputs[i]->tailSegments);
    delete _inputs[i];
  }
  _inputs.clear();

  for (size_t o=0; o<_outputs.size(); ++o)
  {
    delete _outputs[o];
  }
  _outputs.clear();

  _numInputs = 0;
  _numOutputs = 0;
  _headBlockSize = 0;
  _tailBlockSize = 0;
  _headFftSize = 0;
  _tailFftSize = 0;
  _headComplexSize = 0;
  _tailComplexSize = 0;
  _headSegCount = 0;
  _tailSegCount = 0;
  _fftHead.init(0);
  _fftTail.init(0);
  _headFftBuffer.clear();
  _tailFftBuffer.clear();
  _headAcc.clear();
  _tailAcc.clear();
  _headFill = 0;
  _tailFill = 0;
  _headCurrent = 0;
  _tailCurrent = 0;
  _headBlockCount = 0;
  _tailBlockCount = 0;
  _stats = FFTStats();
}


bool MatrixConvolver::init(size_t numInputs,
                           size_t numOutputs,
                           size_t headBlockSize,
                           size_t tailBlockSize,
                           const MatrixConnection* connections,
                           size_t numConnections)
{
  reset();

  if (numInputs == 0 || numOutputs == 0)
  {
    return false;
  }

  if (headBlockSize == 0 || tailBlockSize == 0)
  {
    return false;
  }

  const size_t headSize = NextPowerOf2(headBlockSize);
  const size_t tailSize = NextPowerOf2(tailBlockSize);
  if (!IsPowerOf2(headSize) || !IsPowerOf2(tailSize) || headSize > tailSize)
  {
    return false;
  }

  // Validate the complete connection table before anything is allocated
  if (numConnections > 0 && connections == nullptr)
  {
    return false;
  }
  for (size_t c=0; c<numConnections; ++c)
  {
    const MatrixConnection& conn = connections[c];
    if (conn.input >= numInputs || conn.output >= numOutputs)
    {
      return false;
    }
    if (conn.irLen > 0 && conn.ir == nullptr)
    {
      return false;
    }
    for (size_t i=0; i<conn.irLen; ++i)
    {
      if (!std::isfinite(conn.ir[i]))
      {
        return false;
      }
    }
  }

  _numInputs = numInputs;
  _numOutputs = numOutputs;
  _headBlockSize = headSize;
  _tailBlockSize = tailSize;
  _headFftSize = 2 * headSize;
  _tailFftSize = 2 * tailSize;
  _headComplexSize = audiofft::AudioFFT::ComplexSize(_headFftSize);
  _tailComplexSize = audiofft::AudioFFT::ComplexSize(_tailFftSize);

  const size_t tail0Ratio = _tailBlockSize / _headBlockSize; // Q = T/H

  // One edge per non-empty connection (duplicate connections get separate edges
  // and are therefore accumulated rather than overwritten).
  // edgeForConnection[c] holds the edge index created for connection c, or -1.
  std::vector<long> edgeForConnection(numConnections, -1);

  size_t maxHeadSegCount = 1;
  size_t maxTail0Count = 0;
  size_t maxTailSegCount = 1;
  bool anyTailLong = false;

  for (size_t c=0; c<numConnections; ++c)
  {
    const MatrixConnection& conn = connections[c];
    const size_t irLen = EffectiveIrLen(conn.ir, conn.irLen);
    if (irLen == 0)
    {
      continue; // Empty response => no contribution
    }

    EdgeState* edge = new EdgeState();
    edge->input = conn.input;
    edge->output = conn.output;
    edgeForConnection[c] = static_cast<long>(_edges.size());

    // Head portion: IR[0, T), partitioned at the head size
    const size_t headIrLen = std::min(irLen, _tailBlockSize);
    const size_t headCount = static_cast<size_t>(::ceil(static_cast<float>(headIrLen) /
                                                        static_cast<float>(_headBlockSize)));
    for (size_t i=0; i<headCount; ++i)
    {
      edge->headIR.push_back(new SplitComplex(_headComplexSize));
    }
    maxHeadSegCount = std::max(maxHeadSegCount, headCount);

    // First tail portion: IR[T, 2T), partitioned at the head size
    if (irLen > _tailBlockSize)
    {
      const size_t tail0IrLen = std::min(irLen - _tailBlockSize, _tailBlockSize);
      const size_t tail0Count = static_cast<size_t>(::ceil(static_cast<float>(tail0IrLen) /
                                                            static_cast<float>(_headBlockSize)));
      for (size_t i=0; i<tail0Count; ++i)
      {
        edge->tail0IR.push_back(new SplitComplex(_headComplexSize));
      }
      maxTail0Count = std::max(maxTail0Count, tail0Count);
    }

    // Long tail portion: IR[2T, ...), partitioned at the tail size
    if (irLen > 2 * _tailBlockSize)
    {
      const size_t tailIrLen = irLen - (2 * _tailBlockSize);
      const size_t tailCount = static_cast<size_t>(::ceil(static_cast<float>(tailIrLen) /
                                                          static_cast<float>(_tailBlockSize)));
      for (size_t i=0; i<tailCount; ++i)
      {
        edge->tailIR.push_back(new SplitComplex(_tailComplexSize));
      }
      anyTailLong = true;
      maxTailSegCount = std::max(maxTailSegCount, tailCount);
    }

    _edges.push_back(edge);
  }

  if (!_edges.empty())
  {
    _fftHead.init(_headFftSize);
    _headFftBuffer.resize(_headFftSize);
    _headAcc.resize(_headComplexSize);

    if (anyTailLong)
    {
      _fftTail.init(_tailFftSize);
      _tailFftBuffer.resize(_tailFftSize);
      _tailAcc.resize(_tailComplexSize);
    }

    // Transform all IR partitions. The caller's response arrays are only read
    // here; after init() the convolver holds independent copies.
    for (size_t c=0; c<numConnections; ++c)
    {
      const MatrixConnection& conn = connections[c];
      const long edgePos = edgeForConnection[c];
      if (edgePos < 0)
      {
        continue;
      }
      EdgeState* edge = _edges[static_cast<size_t>(edgePos)];
      const size_t irLen = EffectiveIrLen(conn.ir, conn.irLen);

      for (size_t i=0; i<edge->headIR.size(); ++i)
      {
        const size_t headIrLen = std::min(irLen, _tailBlockSize);
        const size_t remaining = headIrLen - (i * _headBlockSize);
        const size_t sizeCopy = std::min(remaining, _headBlockSize);
        CopyAndPad(_headFftBuffer, &conn.ir[i * _headBlockSize], sizeCopy);
        _fftHead.fft(_headFftBuffer.data(), edge->headIR[i]->re(), edge->headIR[i]->im());
      }

      if (!edge->tail0IR.empty())
      {
        const Sample* tail0Data = conn.ir + _tailBlockSize;
        const size_t tail0IrLen = std::min(irLen - _tailBlockSize, _tailBlockSize);
        for (size_t i=0; i<edge->tail0IR.size(); ++i)
        {
          const size_t remaining = tail0IrLen - (i * _headBlockSize);
          const size_t sizeCopy = std::min(remaining, _headBlockSize);
          CopyAndPad(_headFftBuffer, &tail0Data[i * _headBlockSize], sizeCopy);
          _fftHead.fft(_headFftBuffer.data(), edge->tail0IR[i]->re(), edge->tail0IR[i]->im());
        }
      }

      if (!edge->tailIR.empty())
      {
        const Sample* tailData = conn.ir + (2 * _tailBlockSize);
        const size_t tailIrLen = irLen - (2 * _tailBlockSize);
        for (size_t i=0; i<edge->tailIR.size(); ++i)
        {
          const size_t remaining = tailIrLen - (i * _tailBlockSize);
          const size_t sizeCopy = std::min(remaining, _tailBlockSize);
          CopyAndPad(_tailFftBuffer, &tailData[i * _tailBlockSize], sizeCopy);
          _fftTail.fft(_tailFftBuffer.data(), edge->tailIR[i]->re(), edge->tailIR[i]->im());
        }
      }
    }
  }

  // The head frequency ring must retain every audio block that any head or
  // tail-0 partition can reach: a tail-0 partition s looks back Q + s blocks.
  _headSegCount = std::max(maxHeadSegCount, tail0Ratio + maxTail0Count);
  _headSegCount = std::max(_headSegCount, size_t(1));
  // The long-tail stage reads audio blocks aged 1 .. tailSegCount, so the
  // ring needs one slot more than the number of tail partitions.
  _tailSegCount = maxTailSegCount + 1;

  _inputs.resize(_numInputs);
  _outputs.resize(_numOutputs);
  for (size_t i=0; i<_numInputs; ++i)
  {
    _inputs[i] = new InputState();
  }
  for (size_t o=0; o<_numOutputs; ++o)
  {
    _outputs[o] = new OutputState();
  }

  // Mark the active channels and collect the incoming edges of each output
  for (size_t e=0; e<_edges.size(); ++e)
  {
    EdgeState* edge = _edges[e];
    InputState& input = *_inputs[edge->input];
    OutputState& output = *_outputs[edge->output];

    if (!edge->headIR.empty() || !edge->tail0IR.empty())
    {
      input.headUsed = true;
      output.headActive = true;
    }
    if (!edge->tailIR.empty())
    {
      input.tailUsed = true;
      output.tailLongActive = true;
    }
    output.edges.push_back(e);
  }

  for (size_t i=0; i<_numInputs; ++i)
  {
    InputState& input = *_inputs[i];
    if (input.headUsed)
    {
      input.headInputBuffer.resize(_headBlockSize);
      input.headInputBuffer.setZero();
      for (size_t s=0; s<_headSegCount; ++s)
      {
        SplitComplex* segment = new SplitComplex(_headComplexSize);
        segment->setZero();
        input.headSegments.push_back(segment);
      }
    }
    if (input.tailUsed)
    {
      input.tailInputBuffer.resize(_tailBlockSize);
      input.tailInputBuffer.setZero();
      for (size_t s=0; s<_tailSegCount; ++s)
      {
        SplitComplex* segment = new SplitComplex(_tailComplexSize);
        segment->setZero();
        input.tailSegments.push_back(segment);
      }
    }
  }

  for (size_t o=0; o<_numOutputs; ++o)
  {
    OutputState& output = *_outputs[o];
    if (output.headActive)
    {
      output.headOverlap.resize(_headBlockSize);
      output.headOverlap.setZero();
    }
    if (output.tailLongActive)
    {
      for (size_t p=0; p<2; ++p)
      {
        output.tailOverlap[p].resize(_tailBlockSize);
        output.tailOverlap[p].setZero();
        output.tailLongDelay[p].resize(_tailBlockSize);
        output.tailLongDelay[p].setZero();
      }
    }
  }

  _headFill = 0;
  _tailFill = 0;
  _headCurrent = 0;
  _tailCurrent = 0;
  _headBlockCount = 0;
  _tailBlockCount = 0;
  _stats = FFTStats();

  return true;
}


void MatrixConvolver::process(const Sample* const* inputs, Sample* const* outputs, size_t len)
{
  if (len == 0)
  {
    // A zero-length call must not advance any history
    return;
  }

  if (_edges.empty())
  {
    // No effective connections: every output is silent
    for (size_t o=0; o<_numOutputs; ++o)
    {
      ::memset(outputs[o], 0, len * sizeof(Sample));
    }
    return;
  }

  const size_t tail0Ratio = _tailBlockSize / _headBlockSize;

  size_t processed = 0;
  while (processed < len)
  {
    const size_t processing = std::min(len - processed, _headBlockSize - _headFill);
    const size_t blockPos = _headFill;

    // Snapshot every active input channel's current slice before any output
    // buffer can overwrite it through cross-channel in-place aliasing.
    for (size_t ch=0; ch<_numInputs; ++ch)
    {
      InputState& input = *_inputs[ch];
      if (input.headUsed)
      {
        ::memcpy(input.headInputBuffer.data() + blockPos,
                 inputs[ch] + processed,
                 processing * sizeof(Sample));
      }
      if (input.tailUsed)
      {
        ::memcpy(input.tailInputBuffer.data() + _tailFill,
                 inputs[ch] + processed,
                 processing * sizeof(Sample));
      }
    }

    const bool headBlockCompletes = (blockPos + processing == _headBlockSize);
    const bool tailBlockCompletes = (_tailFill + processing == _tailBlockSize);

    // Shared head-scale forward FFT: exactly one per active input per sub-call,
    // no matter how many connections depart from the input. Partial blocks are
    // recomputed on every sub-call, exactly as in FFTConvolver.
    for (size_t ch=0; ch<_numInputs; ++ch)
    {
      InputState& input = *_inputs[ch];
      if (!input.headUsed)
      {
        continue;
      }
      CopyAndPad(_headFftBuffer, input.headInputBuffer.data(), _headBlockSize);
      _fftHead.fft(_headFftBuffer.data(),
                   input.headSegments[_headCurrent]->re(),
                   input.headSegments[_headCurrent]->im());
      ++_stats.headForward;
    }

    // Shared tail-scale forward FFT: one per active input per completed tail block
    if (tailBlockCompletes)
    {
      for (size_t ch=0; ch<_numInputs; ++ch)
      {
        InputState& input = *_inputs[ch];
        if (!input.tailUsed)
        {
          continue;
        }
        CopyAndPad(_tailFftBuffer, input.tailInputBuffer.data(), _tailBlockSize);
        _fftTail.fft(_tailFftBuffer.data(),
                     input.tailSegments[_tailCurrent]->re(),
                     input.tailSegments[_tailCurrent]->im());
        ++_stats.tailForward;
      }
    }

    // Parity slots of the long-tail results consumed during the current tail block
    const size_t currentTailBlock = _tailBlockCount;
    const size_t delayParity = (currentTailBlock > 0) ? ((currentTailBlock - 1) & 1) : 0;
    const size_t overlapParity = currentTailBlock & 1;

    // Per output: frequency-domain accumulation of all incoming edges followed
    // by a single inverse FFT.
    for (size_t o=0; o<_numOutputs; ++o)
    {
      OutputState& output = *_outputs[o];

      if (!output.headActive && !output.tailLongActive)
      {
        // Output without any connection
        ::memset(outputs[o] + processed, 0, processing * sizeof(Sample));
        continue;
      }

      Sample* outPtr = outputs[o] + processed;

      if (output.headActive)
      {
        _headAcc.setZero();

        for (size_t ei=0; ei<output.edges.size(); ++ei)
        {
          const EdgeState* edge = _edges[output.edges[ei]];
          const InputState& input = *_inputs[edge->input];

          // Head partitions: audio blocks aged 0 .. headIR.size()-1
          for (size_t s=0; s<edge->headIR.size(); ++s)
          {
            const size_t audioIndex = (_headCurrent + s) % _headSegCount;
            ComplexMultiplyAccumulate(_headAcc,
                                      *input.headSegments[audioIndex],
                                      *edge->headIR[s]);
          }

          // Tail-0 partitions: head-scale blocks aged another Q blocks
          // (the IR starts at offset T). They fold into the same accumulator,
          // so no additional inverse FFT is required.
          for (size_t s=0; s<edge->tail0IR.size(); ++s)
          {
            const size_t age = tail0Ratio + s;
            const size_t audioIndex = (_headCurrent + age) % _headSegCount;
            ComplexMultiplyAccumulate(_headAcc,
                                      *input.headSegments[audioIndex],
                                      *edge->tail0IR[s]);
          }
        }

        _fftHead.ifft(_headFftBuffer.data(), _headAcc.re(), _headAcc.im());
        ++_stats.headInverse;

        if (output.tailLongActive)
        {
          const SampleBuffer& delayBlock = output.tailLongDelay[delayParity];
          const SampleBuffer& overlapBlock = output.tailOverlap[overlapParity];
          for (size_t i=0; i<processing; ++i)
          {
            outPtr[i] = _headFftBuffer.data()[blockPos + i]
                        + output.headOverlap[blockPos + i]
                        + delayBlock[_tailFill + i]
                        + overlapBlock[_tailFill + i];
          }
        }
        else
        {
          for (size_t i=0; i<processing; ++i)
          {
            outPtr[i] = _headFftBuffer.data()[blockPos + i]
                        + output.headOverlap[blockPos + i];
          }
        }

        if (headBlockCompletes)
        {
          // The shared FFT buffer is overwritten for each output, so the
          // overlap has to be saved here for this output only.
          ::memcpy(output.headOverlap.data(),
                   _headFftBuffer.data() + _headBlockSize,
                   _headBlockSize * sizeof(Sample));
        }
      }
      else
      {
        // Tail-only output: no head-scale inverse FFT needed
        const SampleBuffer& delayBlock = output.tailLongDelay[delayParity];
        const SampleBuffer& overlapBlock = output.tailOverlap[overlapParity];
        for (size_t i=0; i<processing; ++i)
        {
          outPtr[i] = delayBlock[_tailFill + i] + overlapBlock[_tailFill + i];
        }
      }
    }

    // Head block bookkeeping
    if (headBlockCompletes)
    {
      for (size_t ch=0; ch<_numInputs; ++ch)
      {
        InputState& input = *_inputs[ch];
        if (input.headUsed)
        {
          input.headInputBuffer.setZero();
        }
      }
      _headCurrent = (_headCurrent > 0) ? (_headCurrent - 1) : (_headSegCount - 1);
      ++_headBlockCount;
      _headFill = 0;
    }
    else
    {
      _headFill += processing;
    }

    // Tail block bookkeeping: one shared inverse FFT per active long-tail output
    if (tailBlockCompletes)
    {
      const size_t parity = currentTailBlock & 1;

      for (size_t o=0; o<_numOutputs; ++o)
      {
        OutputState& output = *_outputs[o];
        if (!output.tailLongActive)
        {
          continue;
        }

        _tailAcc.setZero();
        for (size_t ei=0; ei<output.edges.size(); ++ei)
        {
          const EdgeState* edge = _edges[output.edges[ei]];
          if (edge->tailIR.empty())
          {
            continue;
          }
          const InputState& input = *_inputs[edge->input];
          // Tail partition s multiplies the audio block aged s+1, which gives
          // the required 2T input/output delay of the long-tail stage.
          for (size_t s=0; s<edge->tailIR.size(); ++s)
          {
            const size_t audioIndex = (_tailCurrent + s + 1) % _tailSegCount;
            ComplexMultiplyAccumulate(_tailAcc,
                                      *input.tailSegments[audioIndex],
                                      *edge->tailIR[s]);
          }
        }

        _fftTail.ifft(_tailFftBuffer.data(), _tailAcc.re(), _tailAcc.im());
        ++_stats.tailInverse;

        // First half is consumed in the next tail block, the second half two
        // tail blocks later. The parity slots are free at those points.
        ::memcpy(output.tailLongDelay[parity].data(),
                 _tailFftBuffer.data(),
                 _tailBlockSize * sizeof(Sample));
        ::memcpy(output.tailOverlap[parity].data(),
                 _tailFftBuffer.data() + _tailBlockSize,
                 _tailBlockSize * sizeof(Sample));
      }

      for (size_t ch=0; ch<_numInputs; ++ch)
      {
        InputState& input = *_inputs[ch];
        if (input.tailUsed)
        {
          input.tailInputBuffer.setZero();
        }
      }
      _tailCurrent = (_tailCurrent > 0) ? (_tailCurrent - 1) : (_tailSegCount - 1);
      ++_tailBlockCount;
      _tailFill = 0;
    }
    else
    {
      _tailFill += processing;
    }

    processed += processing;
  }
}

} // End of namespace fftconvolver
