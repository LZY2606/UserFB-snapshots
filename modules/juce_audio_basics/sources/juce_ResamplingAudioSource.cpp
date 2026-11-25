/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

ResamplingAudioSource::ResamplingAudioSource (AudioSource* const inputSource,
                                              const bool deleteInputWhenDeleted,
                                              const int channels)
    : input (inputSource, deleteInputWhenDeleted),
      numChannels (channels)
{
    jassert (input != nullptr);
    zeromem (coefficients, sizeof (coefficients));
}

ResamplingAudioSource::~ResamplingAudioSource() {}

void ResamplingAudioSource::setResamplingRatio (const double samplesInPerOutputSample)
{
    jassert (samplesInPerOutputSample > 0);

    const SpinLock::ScopedLockType sl (ratioLock);
    ratio = jmax (0.0, samplesInPerOutputSample);
}

void ResamplingAudioSource::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    const SpinLock::ScopedLockType sl (ratioLock);

    auto scaledBlockSize = roundToInt (samplesPerBlockExpected * ratio);
    input->prepareToPlay (scaledBlockSize, sampleRate * ratio);

    buffer.setSize (numChannels, scaledBlockSize + 32);

    filterStates.calloc (numChannels);
    srcBuffers.calloc (numChannels);
    destBuffers.calloc (numChannels);
    createLowPass (ratio);

    flushBuffers();
}

int64 ResamplingAudioSource::getInputAnchorPosition() const
{
    if (auto* positionable = dynamic_cast<PositionableAudioSource*> (input.get()))
        return positionable->getNextReadPosition();

    return 0;
}

void ResamplingAudioSource::flushBuffers()
{
    const ScopedLock sl (callbackLock);

    buffer.clear();
    bufferPos = 0;
    sampsInBuffer = 0;
    subSampleOffset = 0.0;
    bufferPosFrame = getInputAnchorPosition();
    filteredUntilFrame = bufferPosFrame;
    filterRingPos = 0;
    resetFilters();
}

int64 ResamplingAudioSource::discardBufferedInputFrom (int64 changedStartFrame)
{
    const ScopedLock sl (callbackLock);

    const auto baseFrame = bufferPosFrame;
    const auto endFrame = baseFrame + sampsInBuffer;

    // Nothing that was read ahead reaches the edited region: future reads will
    // pick up the new content directly, so the fractional phase and filters can
    // be left completely untouched.
    if (changedStartFrame >= endFrame)
        return -1;

    // The caller guarantees that the edit starts at least a couple of source
    // frames beyond the position already delivered, which keeps the current
    // interpolation pair and the filter history on unchanged samples. Refuse to
    // rewind the interpolation base if that guarantee was violated.
    if (changedStartFrame < baseFrame + 2)
        return -1;

    // The down-sampling pre-filter is applied just in time for interpolation, so
    // its state must never have crossed the cut. If it had, preserving the exact
    // filter history would be impossible; the documented range rule prevents it.
    jassert (filteredUntilFrame <= changedStartFrame);

    sampsInBuffer = (int) (changedStartFrame - baseFrame);
    return changedStartFrame;
}

void ResamplingAudioSource::notifySourceChanged (Range<int64> sourceFrames)
{
    if (sourceFrames.isEmpty())
        return;

    const auto repositionFrame = discardBufferedInputFrom (sourceFrames.getStart());

    if (repositionFrame >= 0)
        if (auto* positionable = dynamic_cast<PositionableAudioSource*> (input.get()))
            positionable->setNextReadPosition (repositionFrame);
}

void ResamplingAudioSource::ensureDownsamplingFilteredUpTo (int64 throughFrame, int channelsToProcess)
{
    // callbackLock is held by getNextAudioBlock()

    if (filterStates == nullptr || buffer.getNumSamples() <= 0)
        return;

    const auto bufferSize = buffer.getNumSamples();

    while (filteredUntilFrame <= throughFrame)
    {
        jassert (filterRingPos >= 0 && filterRingPos < bufferSize);

        for (int channel = 0; channel < channelsToProcess; ++channel)
            applyFilter (buffer.getWritePointer (channel, filterRingPos), 1, filterStates[channel]);

        if (++filterRingPos >= bufferSize)
            filterRingPos = 0;

        ++filteredUntilFrame;
    }
}

void ResamplingAudioSource::releaseResources()
{
    input->releaseResources();
    buffer.setSize (numChannels, 0);
}

void ResamplingAudioSource::getNextAudioBlock (const AudioSourceChannelInfo& info)
{
    const ScopedLock sl (callbackLock);

    double localRatio;

    {
        const SpinLock::ScopedLockType ratioSl (ratioLock);
        localRatio = ratio;
    }

    if (! approximatelyEqual (lastRatio, localRatio))
    {
        createLowPass (localRatio);
        lastRatio = localRatio;
    }

    const int sampsNeeded = roundToInt (info.numSamples * localRatio) + 3;

    int bufferSize = buffer.getNumSamples();

    if (bufferSize < sampsNeeded + 8)
    {
        bufferPos %= bufferSize;
        bufferSize = sampsNeeded + 32;
        buffer.setSize (buffer.getNumChannels(), bufferSize, true, true);
    }

    bufferPos %= bufferSize;

    int endOfBufferPos = bufferPos + sampsInBuffer;
    const int channelsToProcess = jmin (numChannels, info.buffer->getNumChannels());

    while (sampsNeeded > sampsInBuffer)
    {
        endOfBufferPos %= bufferSize;

        int numToDo = jmin (sampsNeeded - sampsInBuffer,
                            bufferSize - endOfBufferPos);

        AudioSourceChannelInfo readInfo (&buffer, endOfBufferPos, numToDo);
        input->getNextAudioBlock (readInfo);

        // For down-sampling the anti-aliasing filter is applied just before each
        // sample is interpolated (see ensureDownsamplingFilteredUpTo), rather than
        // eagerly to the whole chunk, so that a source-change notification cannot
        // leave filter history that has already consumed edited samples. The per
        // sample filter operations are identical and in the same order as before.

        sampsInBuffer += numToDo;
        endOfBufferPos += numToDo;
    }

    for (int channel = 0; channel < channelsToProcess; ++channel)
    {
        destBuffers[channel] = info.buffer->getWritePointer (channel, info.startSample);
        srcBuffers[channel] = buffer.getReadPointer (channel);
    }

    int nextPos = (bufferPos + 1) % bufferSize;

    for (int m = info.numSamples; --m >= 0;)
    {
        if (localRatio > 1.0001)
            ensureDownsamplingFilteredUpTo (bufferPosFrame + 1, channelsToProcess);

        jassert (sampsInBuffer > 0 && nextPos != endOfBufferPos);

        const float alpha = (float) subSampleOffset;

        for (int channel = 0; channel < channelsToProcess; ++channel)
            *destBuffers[channel]++ = srcBuffers[channel][bufferPos]
                                        + alpha * (srcBuffers[channel][nextPos] - srcBuffers[channel][bufferPos]);

        subSampleOffset += localRatio;

        while (subSampleOffset >= 1.0)
        {
            if (++bufferPos >= bufferSize)
                bufferPos = 0;

            ++bufferPosFrame;
            --sampsInBuffer;

            nextPos = (bufferPos + 1) % bufferSize;
            subSampleOffset -= 1.0;
        }
    }

    if (localRatio < 0.9999)
    {
        // for up-sampling, apply the filter after transposing..
        for (int i = channelsToProcess; --i >= 0;)
            applyFilter (info.buffer->getWritePointer (i, info.startSample), info.numSamples, filterStates[i]);
    }
    else if (localRatio <= 1.0001 && info.numSamples > 0)
    {
        // if the filter's not currently being applied, keep it stoked with the last couple of samples to avoid discontinuities
        for (int i = channelsToProcess; --i >= 0;)
        {
            const float* const endOfBuffer = info.buffer->getReadPointer (i, info.startSample + info.numSamples - 1);
            FilterState& fs = filterStates[i];

            if (info.numSamples > 1)
            {
                fs.y2 = fs.x2 = *(endOfBuffer - 1);
            }
            else
            {
                fs.y2 = fs.y1;
                fs.x2 = fs.x1;
            }

            fs.y1 = fs.x1 = *endOfBuffer;
        }
    }

    jassert (sampsInBuffer >= 0);
}

void ResamplingAudioSource::createLowPass (const double frequencyRatio)
{
    const double proportionalRate = (frequencyRatio > 1.0) ? 0.5 / frequencyRatio
                                                           : 0.5 * frequencyRatio;

    const double n = 1.0 / std::tan (MathConstants<double>::pi * jmax (0.001, proportionalRate));
    const double nSquared = n * n;
    const double c1 = 1.0 / (1.0 + MathConstants<double>::sqrt2 * n + nSquared);

    setFilterCoefficients (c1,
                           c1 * 2.0f,
                           c1,
                           1.0,
                           c1 * 2.0 * (1.0 - nSquared),
                           c1 * (1.0 - MathConstants<double>::sqrt2 * n + nSquared));
}

void ResamplingAudioSource::setFilterCoefficients (double c1, double c2, double c3, double c4, double c5, double c6)
{
    const double a = 1.0 / c4;

    c1 *= a;
    c2 *= a;
    c3 *= a;
    c5 *= a;
    c6 *= a;

    coefficients[0] = c1;
    coefficients[1] = c2;
    coefficients[2] = c3;
    coefficients[3] = c4;
    coefficients[4] = c5;
    coefficients[5] = c6;
}

void ResamplingAudioSource::resetFilters()
{
    if (filterStates != nullptr)
        filterStates.clear ((size_t) numChannels);
}

void ResamplingAudioSource::applyFilter (float* samples, int num, FilterState& fs)
{
    while (--num >= 0)
    {
        const double in = *samples;

        double out = coefficients[0] * in
                     + coefficients[1] * fs.x1
                     + coefficients[2] * fs.x2
                     - coefficients[4] * fs.y1
                     - coefficients[5] * fs.y2;

       #if JUCE_INTEL
        if (! (out < -1.0e-8 || out > 1.0e-8))
            out = 0;
       #endif

        fs.x2 = fs.x1;
        fs.x1 = in;
        fs.y2 = fs.y1;
        fs.y1 = out;

        *samples++ = (float) out;
    }
}

} // namespace juce
