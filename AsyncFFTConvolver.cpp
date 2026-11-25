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
#include <new>


namespace fftconvolver
{

namespace
{
  bool IsPowerOfTwoImpl(size_t val)
  {
    return val >= 2 && (val & (val - 1)) == 0;
  }
}


AsyncFFTConvolver::AsyncFFTConvolver() :
  _headBlockSize(0),
  _tailBlockSize(0),
  _fftSize(0),
  _fftComplexSize(0),
  _tailPartitionCount(0),
  _executor(nullptr),
  _irCopy(),
  _headConvolver(),
  _tailIR(),
  _tailFFT(),
  _tailFFTBuffer(),
  _inputRing(),
  _ringPos(0),
  _inputBlockCount(0),
  _inputBlockRing(),
  _backgroundJob(),
  _slotOccupied(false),
  _localJob(),
  _outputBlocks(),
  _nextOutputGeneration(0),
  _streamPosition(0),
  _tailInputBuffer(),
  _tailInputFill(0),
  _dryCopy(),
  _historyPtrs()
{
}


AsyncFFTConvolver::~AsyncFFTConvolver()
{
  reset();
}


bool AsyncFFTConvolver::IsPowerOfTwo(size_t val)
{
  return IsPowerOfTwoImpl(val);
}


bool AsyncFFTConvolver::IsFiniteIR(const Sample* ir, size_t irLen)
{
  for (size_t i = 0; i < irLen; ++i)
  {
    if (!std::isfinite(ir[i]))
    {
      return false;
    }
  }
  return true;
}


void AsyncFFTConvolver::reclaimBackground()
{
  // Control thread only: the host guarantees that process() is not running
  // concurrently. quiesce() blocks until every accepted worker has fully
  // returned, so afterwards no thread touches the background job anymore.
  if (_executor != nullptr && _slotOccupied)
  {
    _executor->quiesce();
  }
  _slotOccupied = false;
}


void AsyncFFTConvolver::teardown()
{
  _headConvolver.reset();

  for (size_t i = 0; i < _tailIR.size(); ++i)
  {
    delete _tailIR[i];
  }
  _tailIR.clear();

  for (size_t i = 0; i < _inputRing.size(); ++i)
  {
    delete _inputRing[i];
  }
  _inputRing.clear();

  _inputBlockRing.clear();
  _outputBlocks.clear();
  _backgroundJob.reset();
  _localJob.reset();
  _tailFFTBuffer.clear();
  _tailFFT.reset();
  _tailInputBuffer.clear();
  _dryCopy.clear();
  _historyPtrs.clear();

  _headBlockSize = 0;
  _tailBlockSize = 0;
  _fftSize = 0;
  _fftComplexSize = 0;
  _tailPartitionCount = 0;
  _executor = nullptr;
  _irCopy.clear();
  _ringPos = 0;
  _inputBlockCount = 0;
  _slotOccupied = false;
  _nextOutputGeneration = 0;
  _streamPosition = 0;
  _tailInputFill = 0;
}


void AsyncFFTConvolver::reset()
{
  reclaimBackground();
  teardown();
}


bool AsyncFFTConvolver::init(size_t headBlockSize,
                             size_t tailBlockSize,
                             const Sample* ir,
                             size_t irLen,
                             TailExecutor* executor)
{
  // ----- Parameter validation (before touching any existing state) -----
  if (!IsPowerOfTwoImpl(headBlockSize) || !IsPowerOfTwoImpl(tailBlockSize))
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
  if (irLen > 0 && ir == nullptr)
  {
    return false;
  }
  if (!IsFiniteIR(ir, irLen))
  {
    return false;
  }

  const size_t B = headBlockSize;
  const size_t T = tailBlockSize;
  const size_t fftSize = 2 * T;
  const size_t fftComplexSize = audiofft::AudioFFT::ComplexSize(fftSize);
  const size_t headIrLen = std::min(irLen, fftSize);
  const size_t tailIrLen = (irLen > fftSize) ? (irLen - fftSize) : 0;
  const size_t partitionCount = (tailIrLen + T - 1) / T;

  // ----- Build the whole new configuration before replacing the old one -----
  std::vector<Sample> newIrCopy;
  newIrCopy.reserve(irLen);
  newIrCopy.assign(ir, ir + irLen);

  std::unique_ptr<FFTConvolver> newHead(new FFTConvolver());
  if (!newHead->init(B, ir, headIrLen))
  {
    return false;
  }

  std::unique_ptr<audiofft::AudioFFT> newTailFFT;
  SampleBuffer newTailFFTBuffer;
  std::vector<SplitComplex*> newTailIR;
  std::vector<const SplitComplex*> newTailIRPtr;
  std::vector<SplitComplex*> newInputRing;
  std::vector<std::unique_ptr<SampleBuffer>> newInputBlockRing;
  std::vector<std::unique_ptr<SampleBuffer>> newOutputBlocks;
  std::unique_ptr<TailJob> newBackgroundJob;
  std::unique_ptr<TailJob> newLocalJob;
  std::vector<const SplitComplex*> newHistoryPtrs;
  SampleBuffer newDryCopy;
  SampleBuffer newTailInputBuffer;

  try
  {
    newDryCopy.resize(B);
    if (partitionCount > 0)
    {
      newTailFFT.reset(new audiofft::AudioFFT());
      newTailFFT->init(fftSize);
      newTailFFTBuffer.resize(fftSize);

      // Tail IR spectra: partition p holds IR coefficients
      // [2T + p*T, 2T + (p+1)*T).
      for (size_t p = 0; p < partitionCount; ++p)
      {
        SplitComplex* segment = new SplitComplex(fftComplexSize);
        const size_t irOffset = fftSize + p * T;
        const size_t remaining = irLen - irOffset;
        const size_t sizeCopy = std::min(remaining, T);
        CopyAndPad(newTailFFTBuffer, ir + irOffset, sizeCopy);
        newTailFFT->fft(newTailFFTBuffer.data(), segment->re(), segment->im());
        newTailIR.push_back(segment);
      }
      // Each job copies this pointer array into private storage in init().
      newTailIRPtr.resize(partitionCount);
      for (size_t p = 0; p < partitionCount; ++p)
      {
        newTailIRPtr[p] = newTailIR[p];
      }

      // The ring must keep the P-1 spectra a local fallback may read even
      // while the newest spectrum has just been stored.
      const size_t ringSize = partitionCount + 2;

      for (size_t i = 0; i < ringSize; ++i)
      {
        newInputRing.push_back(new SplitComplex(fftComplexSize));
      }
      newInputBlockRing.resize(ringSize);
      for (size_t i = 0; i < ringSize; ++i)
      {
        newInputBlockRing[i].reset(new SampleBuffer(T));
      }
      newOutputBlocks.resize(ringSize);
      for (size_t i = 0; i < ringSize; ++i)
      {
        newOutputBlocks[i].reset(new SampleBuffer(fftSize));
      }

      newBackgroundJob.reset(new TailJob());
      newLocalJob.reset(new TailJob());
      if (!newBackgroundJob->init(T, partitionCount, newTailIRPtr.data()) ||
          !newLocalJob->init(T, partitionCount, newTailIRPtr.data()))
      {
        for (size_t i = 0; i < newTailIR.size(); ++i) delete newTailIR[i];
        for (size_t i = 0; i < newInputRing.size(); ++i) delete newInputRing[i];
        return false;
      }

      newHistoryPtrs.resize(partitionCount - 1);
      newTailInputBuffer.resize(T);
    }
  }
  catch (const std::bad_alloc&)
  {
    for (size_t i = 0; i < newTailIR.size(); ++i) delete newTailIR[i];
    for (size_t i = 0; i < newInputRing.size(); ++i) delete newInputRing[i];
    return false;
  }

  // ----- New configuration is complete: safely reclaim the old one -----
  reclaimBackground();
  teardown();

  _headBlockSize = B;
  _tailBlockSize = T;
  _fftSize = fftSize;
  _fftComplexSize = fftComplexSize;
  _tailPartitionCount = partitionCount;
  _executor = executor;
  _irCopy.swap(newIrCopy);

  _headConvolver = std::move(newHead);
  _tailIR.swap(newTailIR);
  _tailFFT = std::move(newTailFFT);
  SampleBuffer::Swap(_tailFFTBuffer, newTailFFTBuffer);
  _inputRing.swap(newInputRing);
  _inputBlockRing.swap(newInputBlockRing);
  _outputBlocks.swap(newOutputBlocks);
  _backgroundJob = std::move(newBackgroundJob);
  _localJob = std::move(newLocalJob);
  _historyPtrs.swap(newHistoryPtrs);

  _ringPos = 0;
  _inputBlockCount = 0;
  _slotOccupied = false;
  _nextOutputGeneration = 0;
  _streamPosition = 0;
  _tailInputFill = 0;

  // All allocation already happened above, so these swaps cannot throw and
  // the object can never be left half-initialized after reclaiming old work.
  SampleBuffer::Swap(_dryCopy, newDryCopy);
  SampleBuffer::Swap(_tailInputBuffer, newTailInputBuffer);

  return true;
}


void AsyncFFTConvolver::computeLocal(uint64_t generation,
                                     const Sample* inputBlock,
                                     Sample* destBlock)
{
  // Independent workspace: the local job never touches the background slot
  // and keeps private copies of every spectrum it reads.
  for (size_t i = 0; i < _tailPartitionCount - 1; ++i)
  {
    if (static_cast<uint64_t>(i) + 1 <= generation)
    {
      const uint64_t historyGeneration = generation - 1 - static_cast<uint64_t>(i);
      const size_t ringIndex =
        static_cast<size_t>(historyGeneration % _inputRing.size());
      _historyPtrs[i] = _inputRing[ringIndex];
    }
    else
    {
      _historyPtrs[i] = nullptr; // zero input before the stream start
    }
  }

  _localJob->prepare(generation, inputBlock,
                     _historyPtrs.data(), _historyPtrs.size());
  _localJob->run();
  ::memcpy(destBlock, _localJob->result(), _fftSize * sizeof(Sample));
}


void AsyncFFTConvolver::advanceTailQueue()
{
  // Block g is due once 2*T samples after its input block have been
  // collected, i.e. while (g + 2) <= number of collected blocks.
  while (_nextOutputGeneration + 2 <= _inputBlockCount)
  {
    const uint64_t g = _nextOutputGeneration;
    const size_t slot = static_cast<size_t>(g % _outputBlocks.size());

    // A worker that is still running after its block fell due keeps the slot
    // occupied until it publishes or quiesce() reclaims it. A result published
    // for an older block is discarded (that block was already computed
    // locally): overlap must never be advanced a second time.
    bool handled = false;
    if (_slotOccupied && _backgroundJob->isCompleted())
    {
      if (_backgroundJob->generation() == g)
      {
        // Published in time: the worker no longer touches any job buffer.
        ::memcpy(_outputBlocks[slot]->data(), _backgroundJob->result(),
                 _fftSize * sizeof(Sample));
        _slotOccupied = false;
        handled = true;
      }
      else if (_backgroundJob->generation() < g)
      {
        // Published too late: discard the result, release the slot.
        _slotOccupied = false;
      }
      // generation > g cannot occur: jobs are emitted in strict order and
      // the slot is released before any newer job can be submitted.
    }

    if (!handled)
    {
      // Not published in time (or no background job): compute the very same
      // job on the audio thread with the independent local workspace.
      const size_t inputSlot =
        static_cast<size_t>(g % _inputBlockRing.size());
      computeLocal(g, _inputBlockRing[inputSlot]->data(),
                   _outputBlocks[slot]->data());
    }

    ++_nextOutputGeneration;
  }
}


void AsyncFFTConvolver::emitTailJob(const Sample* block)
{
  const uint64_t generation = _inputBlockCount;

  // Frequency-domain ring entry for the new input block
  CopyAndPad(_tailFFTBuffer, block, _tailBlockSize);
  SplitComplex* ringSegment = _inputRing[_ringPos];
  _tailFFT->fft(_tailFFTBuffer.data(), ringSegment->re(), ringSegment->im());
  _ringPos = (_ringPos + 1) % _inputRing.size();

  // Time-domain copy used for local fallback and as the job input source
  SampleBuffer& blockSlot =
    *_inputBlockRing[static_cast<size_t>(generation % _inputBlockRing.size())];
  ::memcpy(blockSlot.data(), block, _tailBlockSize * sizeof(Sample));

  ++_inputBlockCount;

  if (_executor == nullptr)
  {
    return;
  }

  // At most one accepted background call may still reference job buffers:
  // while the slot is occupied the newer job is handled locally when due.
  if (_slotOccupied)
  {
    return;
  }

  // Read-only history pointers: history i needs the spectrum of block g-1-i.
  for (size_t i = 0; i < _tailPartitionCount - 1; ++i)
  {
    if (static_cast<uint64_t>(i) + 1 <= generation)
    {
      const uint64_t historyGeneration =
        generation - 1 - static_cast<uint64_t>(i);
      const size_t ringIndex =
        static_cast<size_t>(historyGeneration % _inputRing.size());
      _historyPtrs[i] = _inputRing[ringIndex];
    }
    else
    {
      _historyPtrs[i] = nullptr;
    }
  }

  _backgroundJob->arm();
  _backgroundJob->prepare(generation, block,
                          _historyPtrs.data(), _historyPtrs.size());

  const bool accepted = _executor->submit(&TailJob::runEntry,
                                          _backgroundJob.get());
  if (accepted)
  {
    // The slot stays occupied until the worker has published completion and
    // therefore no longer accesses any buffer of the job.
    _slotOccupied = true;
  }
  // Rejected: computed locally when the block falls due.
}


void AsyncFFTConvolver::process(const Sample* input, Sample* output, size_t len)
{
  if (len == 0)
  {
    return;
  }

  if (_headBlockSize == 0)
  {
    ::memset(output, 0, len * sizeof(Sample));
    return;
  }

  const size_t B = _headBlockSize;
  const size_t T = _tailBlockSize;
  size_t processed = 0;

  while (processed < len)
  {
    size_t processing = std::min(len - processed, B);
    if (_tailPartitionCount > 0)
    {
      // Never overrun the tail accumulator: arbitrary chunk sizes can leave
      // _tailInputFill unaligned to B, so cap at the remaining tail space.
      processing = std::min(processing, T - _tailInputFill);
    }

    // Support input == output: preserve the dry input before any mixing
    // overwrites it.
    ::memcpy(_dryCopy.data(), input + processed, processing * sizeof(Sample));

    // Short head convolution (first 2*T IR coefficients), B-sized partitions
    _headConvolver->process(_dryCopy.data(), output + processed, processing);

    if (_tailPartitionCount > 0)
    {
      // Feed the tail input accumulator
      ::memcpy(_tailInputBuffer.data() + _tailInputFill,
               _dryCopy.data(), processing * sizeof(Sample));
      _tailInputFill += processing;

      if (_tailInputFill == T)
      {
        // A full tail block was collected. Emit its ordered job first (this
        // publishes the block's spectrum and increments the block count),
        // then make every due result available. The just emitted job can
        // never be due itself (a job is due only two blocks later), so the
        // background slot handling stays unambiguous.
        emitTailJob(_tailInputBuffer.data());
        advanceTailQueue();
        _tailInputFill = 0;
      }

      // Mix the due tail contribution into this chunk. The job for input
      // block g computes the 2*T point result for output block q = g+2 in
      // the standard overlap-save layout:
      //   - first T: main part of output block q (times [q*T, (q+1)*T))
      //   - second T: overlap added to output block q+1
      // So sample n (block q = n/T, offset m = n%T) receives the first half
      // of job (q-2) and the second half of job (q-3). Both jobs are due by
      // the time n is rendered.
      for (size_t j = 0; j < processing; ++j)
      {
        const uint64_t n = _streamPosition + static_cast<uint64_t>(j);
        if (n >= 2 * static_cast<uint64_t>(T))
        {
          const uint64_t q = n / static_cast<uint64_t>(T);
          const size_t m = static_cast<size_t>(n % static_cast<uint64_t>(T));

          const uint64_t mainGen = q - 2;
          const size_t mainSlot =
            static_cast<size_t>(mainGen % _outputBlocks.size());
          output[processed + j] += (*_outputBlocks[mainSlot])[m];

          if (q >= 3)
          {
            const uint64_t overlapGen = q - 3;
            const size_t overlapSlot =
              static_cast<size_t>(overlapGen % _outputBlocks.size());
            output[processed + j] +=
              (*_outputBlocks[overlapSlot])[_tailBlockSize + m];
          }
        }
      }
    }

    processed += processing;
    _streamPosition += static_cast<uint64_t>(processing);
  }
}

} // End of namespace fftconvolver
