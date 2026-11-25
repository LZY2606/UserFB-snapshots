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

#include "AsyncFFTConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>


namespace fftconvolver
{

namespace
{
  bool IsPowerOfTwo(size_t val)
  {
    return val > 0 && (val & (val - 1)) == 0;
  }
}


AsyncFFTConvolver::AsyncFFTConvolver() :
  _headBlockSize(0),
  _tailBlockSize(0),
  _executor(0),
  _ir(),
  _head(),
  _plan(),
  _tailSegCount(0),
  _fftComplexSize(0),
  _inputFft(),
  _inputFftBuffer(),
  _ring(),
  _ringCurrent(0),
  _ringView(),
  _slot(),
  _localAccum(),
  _localFftBuffer(),
  _localFft(),
  _tailInput(),
  _tailInputFill(0),
  _nextJobPosition(0),
  _nextDuePosition(0),
  _records(),
  _recordHead(0),
  _recordTail(0),
  _overlap(),
  _tailOutput(),
  _tailOutputPos(0)
{
}


AsyncFFTConvolver::~AsyncFFTConvolver()
{
  reset();
}


bool AsyncFFTConvolver::validate(size_t headBlockSize, size_t tailBlockSize, size_t irLen) const
{
  if (!IsPowerOfTwo(headBlockSize) || !IsPowerOfTwo(tailBlockSize))
  {
    return false;
  }
  if (headBlockSize < 8 || headBlockSize > 256)
  {
    return false;
  }
  if (tailBlockSize < 64 || tailBlockSize > 4096)
  {
    return false;
  }
  if (tailBlockSize < 4 * headBlockSize)
  {
    return false;
  }
  if (irLen > 65536)
  {
    return false;
  }
  return true;
}


void AsyncFFTConvolver::teardown()
{
  // Control methods never run concurrently with process(); quiesce() waits for
  // every accepted background call to fully return before any job memory moves.
  if (_executor)
  {
    _executor->quiesce();
  }

  _head.reset();
  _plan.clear();
  _slot.clear();

  for (size_t i = 0; i < _ring.size(); ++i)
  {
    delete _ring[i];
  }
  _ring.clear();
  _ringView.clear();

  for (size_t i = 0; i < _records.size(); ++i)
  {
    delete [] _records[i].localResult;
  }
  _records.clear();

  _inputFftBuffer.clear();
  _localAccum.clear();
  _localFftBuffer.clear();
  _localFft.init(0);
  _tailInput.clear();
  _overlap.clear();
  _tailOutput.clear();

  _headBlockSize = 0;
  _tailBlockSize = 0;
  _tailSegCount = 0;
  _fftComplexSize = 0;
  _ringCurrent = 0;
  _tailInputFill = 0;
  _nextJobPosition = 0;
  _nextDuePosition = 0;
  _recordHead = 0;
  _recordTail = 0;
  _tailOutputPos = 0;
}


void AsyncFFTConvolver::reset()
{
  teardown();
  _executor = 0;
  _ir.clear();
}


void AsyncFFTConvolver::resetHistory()
{
  _head.reset();
  _head.init(_headBlockSize, _ir.data(), std::min(_ir.size(), 2 * _tailBlockSize));

  for (size_t i = 0; i < _ring.size(); ++i)
  {
    _ring[i]->setZero();
  }
  _ringCurrent = 0;

  for (size_t i = 0; i < _records.size(); ++i)
  {
    _records[i].state = JOB_EMPTY;
    _records[i].position = 0;
    _records[i].slotJob = false;
  }
  _recordHead = 0;
  _recordTail = 0;

  _slot.storeState(TailJobSlot::IDLE, std::memory_order_release);
  _tailInput.setZero();
  _tailInputFill = 0;
  _overlap.setZero();
  _tailOutput.setZero();
  _tailOutputPos = 0;
  _nextJobPosition = 0;
  _nextDuePosition = 2 * _tailBlockSize;
}


bool AsyncFFTConvolver::init(size_t headBlockSize,
                             size_t tailBlockSize,
                             const Sample* ir,
                             size_t irLen,
                             TailExecutor* executor)
{
  if (!validate(headBlockSize, tailBlockSize, irLen))
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

  // Private copy of the IR, taken before the existing configuration is touched
  // so that a failed preparation cannot alter the current sound.
  std::vector<Sample> newIr;
  if (irLen > 0)
  {
    newIr.assign(ir, ir + irLen);
  }

  // Build the tail plan into a temporary object first.
  TailPlan newPlan;
  size_t tailSegCount = 0;
  if (irLen > 2 * tailBlockSize)
  {
    const size_t tailIrLen = irLen - (2 * tailBlockSize);
    if (!newPlan.build(tailBlockSize, ir + (2 * tailBlockSize), tailIrLen))
    {
      return false;
    }
    tailSegCount = newPlan.segmentCount();
  }

  // Everything validated: safely reclaim the old configuration and its jobs.
  teardown();

  _executor = executor;
  _ir.swap(newIr);
  _headBlockSize = headBlockSize;
  _tailBlockSize = tailBlockSize;

  const size_t headIrLen = std::min(_ir.size(), 2 * _tailBlockSize);
  _head.init(_headBlockSize, _ir.data(), headIrLen);

  if (tailSegCount > 0)
  {
    _plan = std::move(newPlan);
    _tailSegCount = tailSegCount;
    _fftComplexSize = audiofft::AudioFFT::ComplexSize(2 * _tailBlockSize);

    _inputFft.init(2 * _tailBlockSize);
    _inputFftBuffer.resize(2 * _tailBlockSize);

    // Rotating ring of input spectra. One extra slot keeps the oldest block
    // alive until every job referencing it has been produced.
    _ring.reserve(_tailSegCount + 1);
    for (size_t i = 0; i < _tailSegCount + 1; ++i)
    {
      _ring.push_back(new SplitComplex(_fftComplexSize));
    }
    _ringView.resize(_tailSegCount);
    _ringCurrent = 0;

    _localAccum.resize(_fftComplexSize);
    _localFftBuffer.resize(2 * _tailBlockSize);
    _localFft.init(2 * _tailBlockSize);

    _tailInput.resize(_tailBlockSize);
    _overlap.resize(_tailBlockSize);
    _tailOutput.resize(_tailBlockSize);

    // The single background slot: at most one worker call may be in flight.
    _slot.configure(_tailBlockSize, _tailSegCount);

    // Three ordered records are enough: a job is consumed two generations
    // after production, so at most two results are pending at any time.
    _records.resize(3);
    for (size_t i = 0; i < _records.size(); ++i)
    {
      _records[i].localResult = new Sample[2 * _tailBlockSize];
      ::memset(_records[i].localResult, 0, (2 * _tailBlockSize) * sizeof(Sample));
    }
  }

  resetHistory();
  return true;
}


void AsyncFFTConvolver::processTailBlock(const Sample* dryInput)
{
  // 1) Consume the job whose 2*tailBlockSize delayed result window starts at
  //    the upcoming block boundary. This happens before producing a new job so
  //    that the small record ring can be reused in strict production order.
  Record& due = _records[_recordHead];
  if (due.state != JOB_EMPTY &&
      due.position + (2 * _tailBlockSize) == _nextDuePosition)
  {
    const Sample* result = 0;

    if (due.state == JOB_BUSY && due.slotJob)
    {
      const TailJobSlot::State slotState = _slot.loadState();
      if (slotState == TailJobSlot::DONE && _slot.position() == due.position)
      {
        // The worker published in time and touches the slot no longer.
        result = _slot.resultData();
        _slot.storeState(TailJobSlot::IDLE, std::memory_order_release);
      }
      else
      {
        // Deadline reached without a complete result: compute the identical
        // job on this thread. The slot snapshot is read-only, so the tardy
        // worker may keep running; its late publication is discarded by the
        // position check when the slot is reclaimed.
        ComputeTailJob(_plan,
                       _slot.snapshotData(),
                       _localAccum,
                       _localFftBuffer,
                       _localFft,
                       due.localResult);
        result = due.localResult;
      }
    }
    else
    {
      result = due.localResult;
    }

    // Overlap-add: adjacent 2*tailBlockSize windows overlap by one block.
    for (size_t i = 0; i < _tailBlockSize; ++i)
    {
      _tailOutput[i] = _overlap[i] + result[i];
    }
    ::memcpy(_overlap.data(),
             result + _tailBlockSize,
             _tailBlockSize * sizeof(Sample));
    _tailOutputPos = 0;

    due.state = JOB_EMPTY;
    due.slotJob = false;
    _recordHead = (_recordHead + 1) % _records.size();
    _nextDuePosition += _tailBlockSize;
  }

  // 2) Rotate the frequency-domain input ring and transform the new block.
  _ringCurrent = (_ringCurrent + 1) % _ring.size();
  CopyAndPad(_inputFftBuffer, dryInput, _tailBlockSize);
  _inputFft.fft(_inputFftBuffer.data(), _ring[_ringCurrent]->re(), _ring[_ringCurrent]->im());

  const uint64_t jobPosition = _nextJobPosition;
  _nextJobPosition += _tailBlockSize;

  // View ordered newest ... oldest: index i is the block i generations ago.
  for (size_t i = 0; i < _tailSegCount; ++i)
  {
    const size_t ringIndex = (_ringCurrent + _ring.size() - i) % _ring.size();
    _ringView[i] = _ring[ringIndex];
  }

  // 3) Reclaim a slot whose worker finished after a local fallback. Its late
  //    result belongs to an older generation and is discarded.
  if (_executor && _slot.loadState() == TailJobSlot::DONE)
  {
    _slot.storeState(TailJobSlot::IDLE, std::memory_order_release);
  }

  // 4) Produce the ordered job for the just completed input block.
  Record& produce = _records[_recordTail];
  produce.position = jobPosition;
  produce.slotJob = false;

  bool submitted = false;
  if (_executor && _slot.loadState() == TailJobSlot::IDLE)
  {
    _slot.arm(jobPosition, &_plan, _ringView.data());
    _slot.storeState(TailJobSlot::BUSY, std::memory_order_release);
    if (_executor->submit(&TailJobSlot::run, &_slot))
    {
      submitted = true;
    }
    else
    {
      // Rejected: the call will never run, so the slot remains reusable.
      _slot.storeState(TailJobSlot::IDLE, std::memory_order_release);
    }
  }

  if (submitted)
  {
    produce.state = JOB_BUSY;
    produce.slotJob = true;
  }
  else
  {
    // No executor, an older worker still occupies the slot or the submission
    // was rejected: compute the job locally with the private workspace.
    ComputeTailJob(_plan,
                   _ringView.data(),
                   _localAccum,
                   _localFftBuffer,
                   _localFft,
                   produce.localResult);
    produce.state = JOB_LOCAL;
  }
  _recordTail = (_recordTail + 1) % _records.size();
}


void AsyncFFTConvolver::process(const Sample* input, Sample* output, size_t len)
{
  if (len == 0)
  {
    return;
  }

  if (_ir.empty())
  {
    ::memset(output, 0, len * sizeof(Sample));
    return;
  }

  if (_tailSegCount == 0)
  {
    // Head-only convolution: FFTConvolver itself handles aliased buffers.
    _head.process(input, output, len);
    return;
  }

  const bool aliased = (input == output);
  size_t processed = 0;
  while (processed < len)
  {
    const size_t chunk = std::min(len - processed, _tailBlockSize - _tailInputFill);

    // The head writes into the output first; when input and output alias, the
    // dry samples the tail still needs would be overwritten, so preserve them.
    const Sample* dry = input + processed;
    if (aliased)
    {
      ::memcpy(_inputFftBuffer.data(), input + processed, chunk * sizeof(Sample));
      dry = _inputFftBuffer.data();
    }

    // Head contribution for this chunk.
    _head.process(dry, output + processed, chunk);

    // Mix the tail contribution assembled for the current output block.
    for (size_t i = 0; i < chunk; ++i)
    {
      output[processed + i] += _tailOutput[_tailOutputPos + i];
    }
    _tailOutputPos += chunk;
    if (_tailOutputPos == _tailBlockSize)
    {
      _tailOutputPos = 0;
    }

    // Collect dry samples until a full tail block is available.
    ::memcpy(_tailInput.data() + _tailInputFill, dry, chunk * sizeof(Sample));
    _tailInputFill += chunk;

    if (_tailInputFill == _tailBlockSize)
    {
      // Assembles the tail output block that becomes valid exactly at the next
      // sample position, before any of its samples are mixed.
      processTailBlock(_tailInput.data());
      _tailInputFill = 0;
    }

    processed += chunk;
  }
}

} // End of namespace fftconvolver
