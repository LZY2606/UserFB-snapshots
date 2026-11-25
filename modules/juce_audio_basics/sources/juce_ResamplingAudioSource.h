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

//==============================================================================
/**
    A type of AudioSource that takes an input source and changes its sample rate.

    @see AudioSource, LagrangeInterpolator, CatmullRomInterpolator

    @tags{Audio}
*/
class JUCE_API  ResamplingAudioSource  : public AudioSource
{
public:
    //==============================================================================
    /** Creates a ResamplingAudioSource for a given input source.

        @param inputSource              the input source to read from
        @param deleteInputWhenDeleted   if true, the input source will be deleted when
                                        this object is deleted
        @param numChannels              the number of channels to process
    */
    ResamplingAudioSource (AudioSource* inputSource,
                           bool deleteInputWhenDeleted,
                           int numChannels = 2);

    /** Destructor. */
    ~ResamplingAudioSource() override;

    /** Changes the resampling ratio.

        (This value can be changed at any time, even while the source is running).

        @param samplesInPerOutputSample     if set to 1.0, the input is passed through; higher
                                            values will speed it up; lower values will slow it
                                            down. The ratio must be greater than 0
    */
    void setResamplingRatio (double samplesInPerOutputSample);

    /** Returns the current resampling ratio.

        This is the value that was set by setResamplingRatio().
    */
    double getResamplingRatio() const noexcept                  { return ratio; }

    /** Clears any buffers and filters that the resampler is using. */
    void flushBuffers();

    //==============================================================================
    /** Notifies the resampler that a region of its input source has been edited
        in place, in absolute input-source sample frames.

        This discards any not-yet-interpolated input samples buffered at or after
        @p changedStartFrame so that they are re-read from the source. Unlike
        flushBuffers(), the fractional interpolation position and the filter state
        are deliberately retained: the edit is guaranteed (by the caller) to start
        far enough after the currently delivered position that the retained state
        only describes unchanged samples.

        @returns the absolute input frame the owning PositionableAudioSource should
                 be repositioned to before the next block, or -1 if none of the
                 buffered input samples are affected and no reposition is needed.

        @see AudioTransportSource::notifySourceChanged
    */
    int64 discardBufferedInputFrom (int64 changedStartFrame);

    /** Convenience notification that also rewinds a positionable input source.

        This is only effective when the input source is itself a
        PositionableAudioSource. When the input is wrapped (for example by a
        BufferingAudioSource), prefer driving the wrapper together with
        discardBufferedInputFrom() so that its cache invalidation stays in sync.
    */
    void notifySourceChanged (Range<int64> sourceFrames);

    //==============================================================================
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const AudioSourceChannelInfo&) override;

private:
    //==============================================================================
    OptionalScopedPointer<AudioSource> input;
    double ratio = 1.0, lastRatio = 1.0;
    AudioBuffer<float> buffer;
    int bufferPos = 0, sampsInBuffer = 0;
    double subSampleOffset = 0.0;
    int64 bufferPosFrame = 0;
    int64 filteredUntilFrame = 0;
    int filterRingPos = 0;
    double coefficients[6];
    SpinLock ratioLock;
    CriticalSection callbackLock;
    const int numChannels;
    HeapBlock<float*> destBuffers;
    HeapBlock<const float*> srcBuffers;

    void setFilterCoefficients (double c1, double c2, double c3, double c4, double c5, double c6);
    void createLowPass (double proportionalRate);

    struct FilterState
    {
        double x1, x2, y1, y2;
    };

    HeapBlock<FilterState> filterStates;
    void resetFilters();

    void applyFilter (float* samples, int num, FilterState& fs);
    void ensureDownsamplingFilteredUpTo (int64 throughFrame, int channelsToProcess);
    int64 getInputAnchorPosition() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResamplingAudioSource)
};

} // namespace juce
