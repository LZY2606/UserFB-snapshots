//	Copyright (c) 2010 Martin Eastwood
//  This code is distributed under the terms of the GNU General Public License

//  MVerb is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  at your option) any later version.
//
//  MVerb is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this MVerb.  If not, see <http://www.gnu.org/licenses/>.

#ifndef EMVERB_H
#define EMVERB_H

#include <cmath>
#include <cstring>

//forward declaration
template<typename T, int maxLength> class Allpass;
template<typename T, int maxLength> class StaticAllpassFourTap;
template<typename T, int maxLength> class StaticDelayLine;
template<typename T, int maxLength> class StaticDelayLineFourTap;
template<typename T, int maxLength> class StaticDelayLineEightTap;
template<typename T, int maxLength> class MorphAllpassFourTap;
template<typename T, int maxLength> class MorphDelayLineFourTap;
template<typename T, int OverSampleCount> class StateVariable;

// ---------------------------------------------------------------------------
// Continuous-size mode primitives.
//
// Each line keeps a history ordered by actual write time. Sample at age a
// (a == 0 is the most recently written sample) is read with linear
// interpolation; ages that fall before the first stored sample read as zero.
// Lengths and tap offsets are supplied every frame as floating point values
// derived directly from the per-second constants, the sample rate and the
// current room size, without any prior rounding.
// ---------------------------------------------------------------------------
template<typename T, int maxLength>
class MorphLine
{
protected:
    T buffer[maxLength] = {};
    // Absolute write time of the slot that will be written next.
    long long writeTime = 0;
    // Number of samples currently stored (grows to maxLength, never shrinks
    // while the room changes size: old samples are neither moved nor cleared).
    int stored = 0;

    // Read the sample written at absolute time t; negative or not-yet-written
    // times read as zero.
    T readAt(long long t) const
    {
        if(t < 0) return T(0);
        long long age = writeTime - 1 - t;
        if(age < 0 || age >= stored) return T(0);
        long long pos = t % maxLength;
        if(pos < 0) pos += maxLength;
        return buffer[static_cast<int>(pos)];
    }

    // Fractional read at an age measured from the most recent write.
    T readAge(T age) const
    {
        if(stored == 0) return T(0);
        T baseT = static_cast<T>(writeTime - 1) - age;
        long long t0 = static_cast<long long>(std::floor(baseT));
        T frac = baseT - static_cast<T>(t0);
        T v0 = readAt(t0);
        T v1 = readAt(t0 + 1);
        return v0 + frac * (v1 - v0);
    }

public:
    void Clear()
    {
        std::memset(buffer, 0, sizeof(buffer));
        writeTime = 0;
        stored = 0;
    }

    // One write per frame. The main output at fractional delay L is taken
    // before the write, matching the integer line whose pre-write read
    // returns x[n - L]: relative to the newest stored sample x[n - 1] that is
    // age L - 1.
    T process(T input, T L)
    {
        T output = readAge(L - T(1));
        long long pos = writeTime % maxLength;
        if(pos < 0) pos += maxLength;
        buffer[static_cast<int>(pos)] = input;
        ++writeTime;
        if(stored < maxLength) ++stored;
        return output;
    }

    // Observed after the write/advance of a frame, exactly like GetIndex on
    // the integer lines. Reads history at age L - q - 1 of the newest write.
    T tap(T L, T q) const
    {
        return readAge(L - q - T(1));
    }
};

template<typename T, int maxLength>
class MorphAllpassFourTap : public MorphLine<T, maxLength>
{
private:
    T Feedback = T(0.5);
public:
    void SetFeedback(T feedback) { Feedback = feedback; }

    // Allpass kept as y = d - g*x, writing x + g*y (the original structure).
    T process(T input, T L)
    {
        T d = this->readAge(L - T(1));
        T y = d - Feedback * input;
        T writeValue = input + Feedback * y;
        long long pos = this->writeTime % maxLength;
        if(pos < 0) pos += maxLength;
        this->buffer[static_cast<int>(pos)] = writeValue;
        ++this->writeTime;
        if(this->stored < maxLength) ++this->stored;
        return y;
    }

    using MorphLine<T, maxLength>::tap;
};

template<typename T, int maxLength>
class MorphDelayLineFourTap : public MorphLine<T, maxLength>
{
};

template<typename T>
class MVerb
{
private:
    Allpass<T, 96000> allpass[4] = {};
    StaticAllpassFourTap<T, 96000> allpassFourTap[4] = {};
    StateVariable<T,4> bandwidthFilter[2] = {};
    StateVariable<T,4> damping[2] = {};
    StaticDelayLine<T, 96000> predelay = {};
    StaticDelayLineFourTap<T, 96000> staticDelayLine[4] = {};
    StaticDelayLineEightTap<T, 96000> earlyReflectionsDelayLine[2] = {};

    // Continuous-size equivalents of the four tank allpasses and four static
    // delays. Only used when size morphing is enabled; they never allocate.
    MorphAllpassFourTap<T, 96000> morphAllpass[4] = {};
    MorphDelayLineFourTap<T, 96000> morphDelay[4] = {};

    T SampleRate = {};
    T DampingFreq = {};
    T Density1 = {};
    T Density2 = {};
    T BandwidthFreq = {};
    T PreDelayTime = {};
    T Decay = {};
    T Gain = {};
    T Mix = {};
    T EarlyMix = {};
    T Size = {};

    T MixSmooth = {};
    T EarlyLateSmooth = {};
    T BandwidthSmooth = {};
    T DampingSmooth = {};
    T PredelaySmooth = {};
    T SizeSmooth = {};
    T DensitySmooth = {};
    T DecaySmooth = {};

    T PreviousLeftTank = {};
    T PreviousRightTank = {};

    int ControlRate = 0;
    int ControlRateCounter = 0;

    // --- Continuous size mode ---------------------------------------------
    bool morphEnabled = false;
    // Set after reset; cleared by the first non-empty process call. The mode
    // can only be toggled while this is still true.
    bool pristineAfterReset = true;
    // Normalized size in [0,1]; actual room size is 0.05 + 0.95 * this.
    T morphNormalized = T(1);
    // Size used for the next output frame.
    T morphCurrent = T(1);
    T morphTarget = T(1);
    T morphStep = T(0);
    unsigned morphFramesRemaining = 0;

    // Per-second base lengths and tap constants from reset(), kept so the
    // per-frame floating point L and q are derived without rounding first.
    static constexpr T kAllpassSec[4] = {T(0.020), T(0.060), T(0.030), T(0.089)};
    static constexpr T kDelaySec[4] = {T(0.15), T(0.12), T(0.14), T(0.11)};
    // Tap offset seconds per line, index 0..2 (SetIndex slot 0 is always 0).
    static constexpr T kAllpassTapSec[4][3] = {
        {T(0), T(0), T(0)},
        {T(0.006), T(0.041), T(0)},
        {T(0), T(0), T(0)},
        {T(0.031), T(0.011), T(0)}
    };
    static constexpr T kDelayTapSec[4][3] = {
        {T(0.067), T(0.011), T(0.121)},
        {T(0.036), T(0.089), T(0)},
        {T(0.0089), T(0.099), T(0)},
        {T(0.067), T(0.0041), T(0)}
    };

public:
    enum
		{
			DAMPINGFREQ=0,
			DENSITY,
			BANDWIDTHFREQ,
            DECAY,
            PREDELAY,
            SIZE,
            GAIN,
            MIX,
            EARLYMIX,
            NUM_PARAMS
		};

    MVerb(){
        DampingFreq = 0.9;
        BandwidthFreq = 0.9;
        SampleRate = 44100.;
        Decay = 0.5;
        Gain = 1.;
        Mix = 1.;
        Size = 1.;
        EarlyMix = 1.;
        PreviousLeftTank = 0.;
        PreviousRightTank = 0.;
        PreDelayTime = 100 * (SampleRate / 1000);
        MixSmooth = EarlyLateSmooth = BandwidthSmooth = DampingSmooth = PredelaySmooth = SizeSmooth = DecaySmooth = DensitySmooth = 0.;
        ControlRate = static_cast<int>(SampleRate / 1000);
        ControlRateCounter = 0;
        reset();
    }

    ~MVerb(){
        //nowt to do here
    }

    void process(T **inputs, T **outputs, int sampleFrames){
        if(sampleFrames == 0)
            return;
        if(pristineAfterReset){
            pristineAfterReset = false;
            if(morphEnabled){
                // Pin every legacy block smoother to its current target so
                // old intra-block ramps cannot perturb the size comparison.
                MixSmooth = Mix;
                EarlyLateSmooth = EarlyMix;
                BandwidthSmooth = static_cast<T>(BandwidthFreq * 18400. + 100.);
                DampingSmooth = static_cast<T>(DampingFreq * 18400. + 100.);
                PredelaySmooth = static_cast<T>(PreDelayTime * 200 * (SampleRate / 1000));
                SizeSmooth = Size;
                DecaySmooth = static_cast<T>(0.7995 * Decay + 0.005);
                DensitySmooth = static_cast<T>(0.7995 * Density1 + 0.005);
            }
        }
        T OneOverSampleFrames = static_cast<T>(1. / sampleFrames);
        T MixDelta	= (Mix - MixSmooth) * OneOverSampleFrames;
        T EarlyLateDelta = (EarlyMix - EarlyLateSmooth) * OneOverSampleFrames;
        T BandwidthDelta = static_cast<T>((((BandwidthFreq * 18400.) + 100.) - BandwidthSmooth) * OneOverSampleFrames);
        T DampingDelta = static_cast<T>((((DampingFreq * 18400.) + 100.) - DampingSmooth) * OneOverSampleFrames);
        T PredelayDelta = static_cast<T>(((PreDelayTime * 200 * (SampleRate / 1000)) - PredelaySmooth) * OneOverSampleFrames);
        T SizeDelta	= static_cast<T>((Size - SizeSmooth) * OneOverSampleFrames);
        T DecayDelta = static_cast<T>((((0.7995f * Decay) + 0.005) - DecaySmooth) * OneOverSampleFrames);
        T DensityDelta = static_cast<T>((((0.7995f * Density1) + 0.005) - DensitySmooth) * OneOverSampleFrames);
        for(int i=0;i<sampleFrames;++i){
            T left = inputs[0][i];
            T right = inputs[1][i];
            MixSmooth += MixDelta;
            EarlyLateSmooth += EarlyLateDelta;
            BandwidthSmooth += BandwidthDelta;
            DampingSmooth += DampingDelta;
            PredelaySmooth += PredelayDelta;
            if(!morphEnabled)
                SizeSmooth += SizeDelta;
            DecaySmooth += DecayDelta;
            DensitySmooth += DensityDelta;
            if (ControlRateCounter >= ControlRate){
                ControlRateCounter = 0;
                bandwidthFilter[0].Frequency(BandwidthSmooth);
                bandwidthFilter[1].Frequency(BandwidthSmooth);
                damping[0].Frequency(DampingSmooth);
                damping[1].Frequency(DampingSmooth);
            }
            ++ControlRateCounter;
            predelay.SetLength(static_cast<int>(PredelaySmooth));
            Density2 = static_cast<T>(DecaySmooth + 0.15);
            if (Density2 > 0.5)
                Density2 = 0.5;
            if (Density2 < 0.25)
                Density2 = 0.25;
            if(morphEnabled){
                morphAllpass[1].SetFeedback(Density2);
                morphAllpass[3].SetFeedback(Density2);
                morphAllpass[0].SetFeedback(Density1);
                morphAllpass[2].SetFeedback(Density1);
            } else {
                allpassFourTap[1].SetFeedback(Density2);
                allpassFourTap[3].SetFeedback(Density2);
                allpassFourTap[0].SetFeedback(Density1);
                allpassFourTap[2].SetFeedback(Density1);
            }
            T bandwidthLeft = bandwidthFilter[0](left) ;
            T bandwidthRight = bandwidthFilter[1](right) ;
            T earlyReflectionsL = static_cast<T>(earlyReflectionsDelayLine[0] ( bandwidthLeft * 0.5 + bandwidthRight * 0.3 )
                                + earlyReflectionsDelayLine[0].GetIndex(2) * 0.6
                                + earlyReflectionsDelayLine[0].GetIndex(3) * 0.4
                                + earlyReflectionsDelayLine[0].GetIndex(4) * 0.3
                                + earlyReflectionsDelayLine[0].GetIndex(5) * 0.3
                                + earlyReflectionsDelayLine[0].GetIndex(6) * 0.1
                                + earlyReflectionsDelayLine[0].GetIndex(7) * 0.1
                                + ( bandwidthLeft * 0.4 + bandwidthRight * 0.2 ) * 0.5);
            T earlyReflectionsR = static_cast<T>(earlyReflectionsDelayLine[1] ( bandwidthLeft * 0.3 + bandwidthRight * 0.5 )
                                + earlyReflectionsDelayLine[1].GetIndex(2) * 0.6
                                + earlyReflectionsDelayLine[1].GetIndex(3) * 0.4
                                + earlyReflectionsDelayLine[1].GetIndex(4) * 0.3
                                + earlyReflectionsDelayLine[1].GetIndex(5) * 0.3
                                + earlyReflectionsDelayLine[1].GetIndex(6) * 0.1
                                + earlyReflectionsDelayLine[1].GetIndex(7) * 0.1
                                + ( bandwidthLeft * 0.2 + bandwidthRight * 0.4 ) * 0.5);
            T predelayMonoInput = predelay(( bandwidthRight + bandwidthLeft ) * 0.5f);
            T smearedInput = predelayMonoInput;
            for(int j=0;j<4;j++)
                smearedInput = allpass[j] ( smearedInput );
            T accumulatorL;
            T accumulatorR;
            if(morphEnabled){
                processMorphTank(smearedInput, accumulatorL, accumulatorR);
            } else {
                T leftTank = allpassFourTap[0] ( smearedInput + PreviousRightTank ) ;
                leftTank = staticDelayLine[0] (leftTank);
                leftTank = damping[0](leftTank);
                leftTank = allpassFourTap[1](leftTank);
                leftTank = staticDelayLine[1](leftTank);
                T rightTank = allpassFourTap[2] (smearedInput + PreviousLeftTank) ;
                rightTank = staticDelayLine[2](rightTank);
                rightTank = damping[1] (rightTank);
                rightTank = allpassFourTap[3](rightTank);
                rightTank = staticDelayLine[3](rightTank);
                PreviousLeftTank = leftTank * DecaySmooth;
                PreviousRightTank = rightTank * DecaySmooth;
                accumulatorL = static_cast<T>((0.6*staticDelayLine[2].GetIndex(1))
                                +(0.6*staticDelayLine[2].GetIndex(2))
                                -(0.6*allpassFourTap[3].GetIndex(1))
                                +(0.6*staticDelayLine[3].GetIndex(1))
                                -(0.6*staticDelayLine[0].GetIndex(1))
                                -(0.6*allpassFourTap[1].GetIndex(1))
                                -(0.6*staticDelayLine[1].GetIndex(1)));
                accumulatorR = static_cast<T>((0.6*staticDelayLine[0].GetIndex(2))
                                +(0.6*staticDelayLine[0].GetIndex(3))
                                -(0.6*allpassFourTap[1].GetIndex(2))
                                +(0.6*staticDelayLine[1].GetIndex(2))
                                -(0.6*staticDelayLine[2].GetIndex(3))
                                -(0.6*allpassFourTap[3].GetIndex(2))
                                -(0.6*staticDelayLine[3].GetIndex(2)));
            }
            accumulatorL = ((accumulatorL * EarlyMix) + ((1 - EarlyMix) * earlyReflectionsL));
            accumulatorR = ((accumulatorR * EarlyMix) + ((1 - EarlyMix) * earlyReflectionsR));
            left = ( left + MixSmooth * ( accumulatorL - left ) ) * Gain;
            right = ( right + MixSmooth * ( accumulatorR - right ) ) * Gain;
            outputs[0][i] = left;
            outputs[1][i] = right;
        }
    }

private:
    // Configure the integer tank lines' lengths and tap positions for the
    // current Size (no clearing); used by reset, the legacy SIZE setter and
    // when leaving continuous mode before any audio has run.
    void sizeLegacyTanks(){
        allpassFourTap[0].SetLength(static_cast<int>(0.020 * SampleRate * Size));
        allpassFourTap[1].SetLength(static_cast<int>(0.060 * SampleRate * Size));
        allpassFourTap[2].SetLength(static_cast<int>(0.030 * SampleRate * Size));
        allpassFourTap[3].SetLength(static_cast<int>(0.089 * SampleRate * Size));
        allpassFourTap[1].SetIndex(0, static_cast<int>(0.006 * SampleRate * Size), static_cast<int>(0.041 * SampleRate * Size), 0);
        allpassFourTap[3].SetIndex(0, static_cast<int>(0.031 * SampleRate * Size), static_cast<int>(0.011 * SampleRate * Size), 0);
        staticDelayLine[0].SetLength(static_cast<int>(0.15 * SampleRate * Size));
        staticDelayLine[1].SetLength(static_cast<int>(0.12 * SampleRate * Size));
        staticDelayLine[2].SetLength(static_cast<int>(0.14 * SampleRate * Size));
        staticDelayLine[3].SetLength(static_cast<int>(0.11 * SampleRate * Size));
        staticDelayLine[0].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size), static_cast<int>(0.011 * SampleRate * Size), static_cast<int>(0.121 * SampleRate * Size));
        staticDelayLine[1].SetIndex(0, static_cast<int>(0.036 * SampleRate * Size), static_cast<int>(0.089 * SampleRate * Size), 0);
        staticDelayLine[2].SetIndex(0, static_cast<int>(0.0089 * SampleRate * Size), static_cast<int>(0.099 * SampleRate * Size), 0);
        staticDelayLine[3].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size), static_cast<int>(0.0041 * SampleRate * Size), 0);
    }

    // Advance the dual-tank network on the fractional-length lines using the
    // size scheduled for this frame, preserving all in-flight sound. The
    // trajectory position is advanced afterwards so that morphCurrent always
    // holds the size the next output frame is going to use.
    void processMorphTank(T smearedInput, T &accumulatorL, T &accumulatorR){
        const T sz = morphCurrent;
        T La[4], Ld[4], qa[4][3], qd[4][3];
        for(int j=0;j<4;j++){
            La[j] = kAllpassSec[j] * SampleRate * sz;
            Ld[j] = kDelaySec[j] * SampleRate * sz;
            for(int k=0;k<3;k++){
                qa[j][k] = kAllpassTapSec[j][k] * SampleRate * sz;
                qd[j][k] = kDelayTapSec[j][k] * SampleRate * sz;
            }
        }
        T leftTank = morphAllpass[0].process(smearedInput + PreviousRightTank, La[0]);
        leftTank = morphDelay[0].process(leftTank, Ld[0]);
        leftTank = damping[0](leftTank);
        leftTank = morphAllpass[1].process(leftTank, La[1]);
        leftTank = morphDelay[1].process(leftTank, Ld[1]);
        T rightTank = morphAllpass[2].process(smearedInput + PreviousLeftTank, La[2]);
        rightTank = morphDelay[2].process(rightTank, Ld[2]);
        rightTank = damping[1](rightTank);
        rightTank = morphAllpass[3].process(rightTank, La[3]);
        rightTank = morphDelay[3].process(rightTank, Ld[3]);
        PreviousLeftTank = leftTank * DecaySmooth;
        PreviousRightTank = rightTank * DecaySmooth;
        accumulatorL = static_cast<T>(
                        (0.6*morphDelay[2].tap(Ld[2], qd[2][0]))
                       +(0.6*morphDelay[2].tap(Ld[2], qd[2][1]))
                       -(0.6*morphAllpass[3].tap(La[3], qa[3][0]))
                       +(0.6*morphDelay[3].tap(Ld[3], qd[3][0]))
                       -(0.6*morphDelay[0].tap(Ld[0], qd[0][0]))
                       -(0.6*morphAllpass[1].tap(La[1], qa[1][0]))
                       -(0.6*morphDelay[1].tap(Ld[1], qd[1][0])));
        accumulatorR = static_cast<T>(
                        (0.6*morphDelay[0].tap(Ld[0], qd[0][1]))
                       +(0.6*morphDelay[0].tap(Ld[0], qd[0][2]))
                       -(0.6*morphAllpass[1].tap(La[1], qa[1][1]))
                       +(0.6*morphDelay[1].tap(Ld[1], qd[1][1]))
                       -(0.6*morphDelay[2].tap(Ld[2], qd[2][2]))
                       -(0.6*morphAllpass[3].tap(La[3], qa[3][1]))
                       -(0.6*morphDelay[3].tap(Ld[3], qd[3][1])));

        // Schedule the size for the following frame; morphCurrent always
        // mirrors the next frame's size, which is also the base a redirect
        // command restarts from.
        if(morphFramesRemaining > 0){
            --morphFramesRemaining;
            if(morphFramesRemaining == 0){
                morphCurrent = morphTarget;
                morphStep = T(0);
            } else {
                morphCurrent += morphStep;
            }
        }
    }

public:
    // Enable or disable continuous size morphing. Only allowed after reset()
    // and before the first non-empty process call; returns false otherwise.
    // When enabled the mode starts at the size currently held by SIZE.
    bool enableSizeMorph(bool enabled){
        if(!pristineAfterReset)
            return false;
        morphEnabled = enabled;
        if(enabled){
            morphNormalized = (Size - T(0.05)) / T(0.95);
            morphCurrent = Size;
            morphTarget = Size;
            morphStep = T(0);
            morphFramesRemaining = 0;
        } else {
            // Keep the integer lines consistent with the current Size, which
            // a zero-frame morph command may have moved while enabled.
            sizeLegacyTanks();
        }
        return true;
    }

    // Schedule a size change to normalized in [0,1], spread over frames output
    // samples starting with the next frame (frames == 0: immediate). A new
    // command while a trajectory is still running restarts from the size the
    // next frame would have used. Invalid input returns false and leaves both
    // the audio and the modulation state untouched.
    bool morphSize(T normalized, unsigned frames){
        if(!morphEnabled)
            return false;
        if(!std::isfinite(normalized) || normalized < T(0) || normalized > T(1))
            return false;
        T target = T(0.05) + T(0.95) * normalized;
        morphNormalized = normalized;
        Size = target;
        morphTarget = target;
        if(frames == 0){
            morphCurrent = target;
            morphStep = T(0);
            morphFramesRemaining = 0;
        } else {
            // morphCurrent is the next frame's scheduled size and therefore
            // the anchor of the new ramp; over 'frames' output samples the
            // sizes are anchor+step ... target, one equal step per frame.
            morphStep = (target - morphCurrent) / static_cast<T>(frames);
            morphFramesRemaining = frames;
            morphCurrent += morphStep;
        }
        return true;
    }

    void reset(){
        ControlRateCounter = 0;
        bandwidthFilter[0].SetSampleRate(SampleRate);
        bandwidthFilter[1].SetSampleRate(SampleRate);
        bandwidthFilter[0].Reset();
        bandwidthFilter[1].Reset();
        damping[0].SetSampleRate(SampleRate);
        damping[1].SetSampleRate(SampleRate);
        damping[0].Reset();
        damping[1].Reset();
        predelay.Clear();
        predelay.SetLength(static_cast<int>(PreDelayTime));
        allpass[0].Clear();
        allpass[1].Clear();
        allpass[2].Clear();
        allpass[3].Clear();
        allpass[0].SetLength(static_cast<int>(0.0048 * SampleRate));
        allpass[1].SetLength(static_cast<int>(0.0036 * SampleRate));
        allpass[2].SetLength(static_cast<int>(0.0127 * SampleRate));
        allpass[3].SetLength(static_cast<int>(0.0093 * SampleRate));
        allpass[0].SetFeedback(0.75);
        allpass[1].SetFeedback(0.75);
        allpass[2].SetFeedback(0.625);
        allpass[3].SetFeedback(0.625);
        allpassFourTap[0].Clear();
        allpassFourTap[1].Clear();
        allpassFourTap[2].Clear();
        allpassFourTap[3].Clear();
        allpassFourTap[0].SetLength(static_cast<int>(0.020 * SampleRate * Size));
        allpassFourTap[1].SetLength(static_cast<int>(0.060 * SampleRate * Size));
        allpassFourTap[2].SetLength(static_cast<int>(0.030 * SampleRate * Size));
        allpassFourTap[3].SetLength(static_cast<int>(0.089 * SampleRate * Size));
        allpassFourTap[0].SetFeedback(Density1);
        allpassFourTap[1].SetFeedback(Density2);
        allpassFourTap[2].SetFeedback(Density1);
        allpassFourTap[3].SetFeedback(Density2);
        staticDelayLine[0].Clear();
        staticDelayLine[1].Clear();
        staticDelayLine[2].Clear();
        staticDelayLine[3].Clear();
        sizeLegacyTanks();
        earlyReflectionsDelayLine[0].Clear();
        earlyReflectionsDelayLine[1].Clear();
        earlyReflectionsDelayLine[0].SetLength(static_cast<int>(0.089 * SampleRate));
        earlyReflectionsDelayLine[0].SetIndex(0, static_cast<int>(0.0199 * SampleRate), static_cast<int>(0.0219 * SampleRate), static_cast<int>(0.0354 * SampleRate), static_cast<int>(0.0389 * SampleRate), static_cast<int>(0.0414 * SampleRate), static_cast<int>(0.0692 * SampleRate), 0);
        earlyReflectionsDelayLine[1].SetLength(static_cast<int>(0.069 * SampleRate));
        earlyReflectionsDelayLine[1].SetIndex(0, static_cast<int>(0.0099 * SampleRate), static_cast<int>(0.011 * SampleRate), static_cast<int>(0.0182 * SampleRate), static_cast<int>(0.0189 * SampleRate), static_cast<int>(0.0213 * SampleRate), static_cast<int>(0.0431 * SampleRate), 0);

        // Continuous-size state: histories and any unfinished trajectory are
        // dropped, the enabled flag is retained, and the new trajectory starts
        // from the most recent target size.
        for(int j=0;j<4;j++){
            morphAllpass[j].Clear();
            morphDelay[j].Clear();
            morphAllpass[j].SetFeedback(j == 1 || j == 3 ? Density2 : Density1);
        }
        pristineAfterReset = true;
        morphNormalized = (Size - T(0.05)) / T(0.95);
        morphCurrent = Size;
        morphTarget = Size;
        morphStep = T(0);
        morphFramesRemaining = 0;
    }

    void setParameter(int index, T value){
        switch(index){
            case DAMPINGFREQ:
                    DampingFreq = static_cast<T>(1. - value);
                    break;
            case DENSITY:
                    Density1 = value;
                    break;
            case BANDWIDTHFREQ:
                    BandwidthFreq = value;
                    break;
            case PREDELAY:
                    PreDelayTime = value;
                    break;
            case SIZE:
                    if(morphEnabled){
                        // Continuous mode: the legacy setter becomes a
                        // zero-frame morph command, never clearing the tanks.
                        morphSize(value, 0);
                        break;
                    }
                    Size = static_cast<T>((0.95 * value) + 0.05);
					allpassFourTap[0].Clear();
					allpassFourTap[1].Clear();
					allpassFourTap[2].Clear();
					allpassFourTap[3].Clear();
					staticDelayLine[0].Clear();
					staticDelayLine[1].Clear();
					staticDelayLine[2].Clear();
					staticDelayLine[3].Clear();
                    sizeLegacyTanks();
                    break;
            case DECAY:
                    Decay = value;
                    break;
            case GAIN:
                    Gain = value;
                    break;
            case MIX:
                    Mix = value;
                    break;
            case EARLYMIX:
                    EarlyMix = value;
                    break;
        }
    }

    float getParameter(int index){
        switch(index){
            case DAMPINGFREQ:
                    return DampingFreq * 100.;
                    break;
            case DENSITY:
                    return Density1 * 100.f;
                    break;
            case BANDWIDTHFREQ:
                    return BandwidthFreq * 100.;
                    break;
            case PREDELAY:
                    return PreDelayTime * 100.;
                    break;
            case SIZE:
                    return (((0.95 * Size) + 0.05)*100.);
                    break;
            case DECAY:
                    return Decay * 100.f;
                    break;
            case GAIN:
                    return Gain * 100.f;
                    break;
            case MIX:
                    return Mix * 100.f;
                    break;
            case EARLYMIX:
                    return EarlyMix * 100.f;
                    break;
            default: return 0.f;
                break;

        }
    }

    void setSampleRate(T sr){
        SampleRate = sr;
        ControlRate = static_cast<int>(SampleRate / 1000);
        reset();
    }
};



template<typename T, int maxLength>
class Allpass
{
private:
    T buffer[maxLength] = {};
    int index = 0;
    int Length = 0;
    T Feedback = {};

public:
    Allpass()
    {
		SetLength ( maxLength - 1 );
		Clear();
		Feedback = 0.5;
    }

	T operator()(T input)
    {
		T output;
		T bufout;
		bufout = buffer[index];
		T temp = input * -Feedback;
		output = bufout + temp;
		buffer[index] = input + ((bufout+temp)*Feedback);
		if(++index>=Length) index = 0;
		return output;

    }

	void SetLength (int Length)
    {
       if( Length >= maxLength )
			Length = maxLength;
	   if( Length < 0 )
			Length = 0;

        this->Length = Length;
    }

	void SetFeedback(T feedback)
    {
        Feedback = feedback;
    }

    void Clear()
    {
        memset(buffer, 0, sizeof(buffer));
		index = 0;
    }

    int GetLength() const
    {
        return Length;
    }
};

template<typename T, int maxLength>
class StaticAllpassFourTap
{
private:
    T buffer[maxLength] = {};
    int index1 = 0;
    int index2 = 0;
    int index3 = 0;
    int index4 = 0;
    int Length = 0;
    T Feedback = {};

public:
    StaticAllpassFourTap()
    {
		SetLength ( maxLength - 1 );
		Clear();
		Feedback = 0.5;
    }

	T operator()(T input)
    {
		T output;
		T bufout;

		bufout = buffer[index1];
		T temp = input * -Feedback;
		output = bufout + temp;
		buffer[index1] = input + ((bufout+temp)*Feedback);

		if(++index1>=Length)
			index1 = 0;
		if(++index2 >= Length)
			index2 = 0;
		if(++index3 >= Length)
			index3 = 0;
		if(++index4 >= Length)
			index4 = 0;

		return output;

    }

	void SetIndex (int Index1, int Index2, int Index3, int Index4)
	{
		index1 = Index1;
		index2 = Index2;
		index3 = Index3;
		index4 = Index4;
	}

	T GetIndex (int Index)
	{
		switch (Index)
		{
			case 0:
				return buffer[index1];
				break;
			case 1:
				return buffer[index2];
				break;
			case 2:
				return buffer[index3];
				break;
			case 3:
				return buffer[index4];
				break;
			default:
				return buffer[index1];
				break;
		}
	}

	void SetLength (int Length)
    {
       if( Length >= maxLength )
			Length = maxLength;
	   if( Length < 0 )
			Length = 0;

        this->Length = Length;
    }


    void Clear()
    {
        memset(buffer, 0, sizeof(buffer));
		index1 = index2  = index3 = index4 = 0;
    }

	void SetFeedback(T feedback)
    {
        Feedback = feedback;
    }


    int GetLength() const
    {
        return Length;
    }
};

template<typename T, int maxLength>
class StaticDelayLine
{
private:
    T buffer[maxLength] = {};
    int index = 0;
    int Length = 0;
    T Feedback = {};

public:
    StaticDelayLine()
    {
		SetLength ( maxLength - 1 );
		Clear();
    }

	T operator()(T input)
    {
		T output = buffer[index];
		buffer[index++] = input;
		if(index >= Length)
			index = 0;
		return output;

    }

	void SetLength (int Length)
    {
       if( Length >= maxLength )
			Length = maxLength;
	   if( Length < 0 )
			Length = 0;

        this->Length = Length;
    }

    void Clear()
    {
        memset(buffer, 0, sizeof(buffer));
		index = 0;
    }

    int GetLength() const
    {
        return Length;
    }
};

template<typename T, int maxLength>
class StaticDelayLineFourTap
{
private:
    T buffer[maxLength] = {};
    int index1 = 0;
    int index2 = 0;
    int index3 = 0;
    int index4 = 0;
    int Length = 0;
    T Feedback = {};

public:
    StaticDelayLineFourTap()
    {
		SetLength ( maxLength - 1 );
		Clear();
    }

	//get ouput and iterate
	T operator()(T input)
    {
		T output = buffer[index1];
		buffer[index1++] = input;
		if(index1 >= Length)
			index1 = 0;
		if(++index2 >= Length)
			index2 = 0;
		if(++index3 >= Length)
			index3 = 0;
		if(++index4 >= Length)
			index4 = 0;
		return output;

    }

	void SetIndex (int Index1, int Index2, int Index3, int Index4)
	{
		index1 = Index1;
		index2 = Index2;
		index3 = Index3;
		index4 = Index4;
	}


	T GetIndex (int Index)
	{
		switch (Index)
		{
			case 0:
				return buffer[index1];
				break;
			case 1:
				return buffer[index2];
				break;
			case 2:
				return buffer[index3];
				break;
			case 3:
				return buffer[index4];
				break;
			default:
				return buffer[index1];
				break;
		}
	}


	void SetLength (int Length)
    {
       if( Length >= maxLength )
			Length = maxLength;
	   if( Length < 0 )
			Length = 0;

        this->Length = Length;
    }


    void Clear()
    {
        memset(buffer, 0, sizeof(buffer));
		index1 = index2  = index3 = index4 = 0;
    }


    int GetLength() const
    {
        return Length;
    }
};

template<typename T, int maxLength>
class StaticDelayLineEightTap
{
private:
    T buffer[maxLength] = {};
    int index1 = 0;
    int index2 = 0;
    int index3 = 0;
    int index4 = 0;
    int index5 = 0;
    int index6 = 0;
    int index7 = 0;
    int index8 = 0;
    int Length = 0;
    T Feedback = {};

public:
    StaticDelayLineEightTap()
    {
		SetLength ( maxLength - 1 );
		Clear();
    }

	//get ouput and iterate
	T operator()(T input)
    {
		T output = buffer[index1];
		buffer[index1++] = input;
		if(index1 >= Length)
			index1 = 0;
		if(++index2 >= Length)
			index2 = 0;
		if(++index3 >= Length)
			index3 = 0;
		if(++index4 >= Length)
			index4 = 0;
		if(++index5 >= Length)
			index5 = 0;
		if(++index6 >= Length)
			index6 = 0;
		if(++index7 >= Length)
			index7 = 0;
		if(++index8 >= Length)
			index8 = 0;
		return output;

    }

	void SetIndex (int Index1, int Index2, int Index3, int Index4, int Index5, int Index6, int Index7, int Index8)
	{
		index1 = Index1;
		index2 = Index2;
		index3 = Index3;
		index4 = Index4;
		index5 = Index5;
		index6 = Index6;
		index7 = Index7;
		index8 = Index8;
	}


	T GetIndex (int Index)
	{
		switch (Index)
		{
			case 0:
				return buffer[index1];
				break;
			case 1:
				return buffer[index2];
				break;
			case 2:
				return buffer[index3];
				break;
			case 3:
				return buffer[index4];
				break;
            case 4:
				return buffer[index5];
				break;
			case 5:
				return buffer[index6];
				break;
			case 6:
				return buffer[index7];
				break;
			case 7:
				return buffer[index8];
				break;
			default:
				return buffer[index1];
				break;
		}
	}


	void SetLength (int Length)
    {
       if( Length >= maxLength )
			Length = maxLength;
	   if( Length < 0 )
			Length = 0;

        this->Length = Length;
    }


    void Clear()
    {
        memset(buffer, 0, sizeof(buffer));
		index1 = index2  = index3 = index4 = index5 = index6 = index7 = index8 = 0;
    }


    int GetLength() const
    {
        return Length;
    }
};

template<typename T, int OverSampleCount>
    class StateVariable
    {
    public:

        enum FilterType
        {
            LOWPASS,
            HIGHPASS,
            BANDPASS,
            NOTCH,
            FilterTypeCount
        };

    private:

        T sampleRate = {};
        T frequency = {};
        T q = {};
        T f = {};

        T low = {};
        T high = {};
        T band = {};
        T notch = {};

        T *out = nullptr;

    public:
        StateVariable()
        {
            SetSampleRate(44100.);
            Frequency(1000.);
            Resonance(0);
            Type(LOWPASS);
            Reset();
        }

        T operator()(T input)
        {
            for(unsigned int i = 0; i < OverSampleCount; i++)
            {
                low += static_cast<T>(f * band + 1e-25);
                high = input - low - q * band;
                band += f * high;
                notch = low + high;
            }
			return *out;
        }

        void Reset()
        {
            low = high = band = notch = 0;
        }

        void SetSampleRate(T sampleRate)
        {
            this->sampleRate = sampleRate * OverSampleCount;
            UpdateCoefficient();
        }

        void Frequency(T frequency)
        {
            this->frequency = frequency;
            UpdateCoefficient();
        }

        void Resonance(T resonance)
        {
            this->q = 2 - 2 * resonance;
        }

        void Type(int type)
        {
            switch(type)
            {
            case LOWPASS:
                out = &low;
                break;

            case HIGHPASS:
                out = &high;
                break;

            case BANDPASS:
                out = &band;
                break;

            case NOTCH:
                out = &notch;
                break;

            default:
                out = &low;
                break;
            }
        }

    private:
        void UpdateCoefficient()
        {
            f = static_cast<T>(2. * sinf(3.141592654 * frequency / sampleRate));
        }
	};
#endif
