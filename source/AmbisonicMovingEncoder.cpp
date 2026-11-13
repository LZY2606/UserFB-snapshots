/*############################################################################*/
/*#                                                                          #*/
/*#  Ambisonic C++ Library                                                   #*/
/*#  AmbisonicMovingEncoder - Moving source encoder with propagation        #*/
/*#  Copyright © 2024                                                        #*/
/*#                                                                          #*/
/*#  Filename:      AmbisonicMovingEncoder.cpp                               #*/
/*#  Version:       0.1                                                      #*/
/*#  Licence:       MIT                                                      #*/
/*#                                                                          #*/
/*############################################################################*/


#include "AmbisonicMovingEncoder.h"
#include "AmbisonicEncoderDist.h"

#include <cassert>
#include <cmath>
#include <algorithm>

namespace spaudio {

    namespace {
        // Arrival time difference bounds, in listener samples.
        const double kMinArrivalDifference = 0.25;
        const double kMaxArrivalDifference = 4.0;
    }

    AmbisonicMovingEncoder::AmbisonicMovingEncoder()
    {
        m_nSampleRate = 0;
        m_nMaxBlockSize = 0;
        m_fRoomRadius = 0.f;
        m_nHistoryLength = 0;
        m_nEmissionCount = 0;
        m_nListenerCount = 0;
        m_fLastArrival = 0.0;
        m_fLastDistance = 0.f;
        m_fVirtualArrival = 0.0;
        m_niSearch = -1;
        m_bHasInput = false;
        m_bFinished = false;
        m_nEndFrame = 0;

        Configure(DEFAULT_ORDER, DEFAULT_HEIGHT, DEFAULT_SAMPLERATE,
            DEFAULT_BLOCKSIZE, 5.f);
    }

    AmbisonicMovingEncoder::~AmbisonicMovingEncoder()
    {
    }

    bool AmbisonicMovingEncoder::Configure(unsigned nOrder, bool b3D,
        unsigned nSampleRate, unsigned nMaxBlockSize, float fRoomRadius)
    {
        if (nSampleRate == 0 || nMaxBlockSize == 0
            || !std::isfinite(fRoomRadius) || fRoomRadius <= 0.f)
            return false;

        bool success = AmbisonicSource::Configure(nOrder, b3D, nSampleRate);
        if (!success)
            return false;

        m_nSampleRate = nSampleRate;
        m_nMaxBlockSize = nMaxBlockSize;
        m_fRoomRadius = fRoomRadius;

        // Bounded ring history. A slot for emission i is read for every
        // listener frame r with a(i) <= r < a(i+1), so its newest reader is
        // below i + maxDelay + 4 (the arrival difference is at most 4). At
        // that point at most nMaxBlockSize - 1 newer slots have been written
        // ahead of the current block, hence maxDelay + nMaxBlockSize + 4
        // slots always suffice. The extra margin keeps the arithmetic exact
        // and the ring never grows with the duration of the recording.
        double fMaxDelay = (double)knMaxDistance / (double)knSpeedOfSound
            * (double)m_nSampleRate;
        m_nHistoryLength = (unsigned)std::ceil(fMaxDelay) + m_nMaxBlockSize + 8;

        m_pfArrivalTimes.assign(m_nHistoryLength, 0.0);
        m_pfEncodedHistory.assign(
            (size_t)m_nHistoryLength * m_nChannelCount, 0.f);

        Reset();

        return true;
    }

    void AmbisonicMovingEncoder::Reset()
    {
        if (m_nHistoryLength > 0)
        {
            std::fill(m_pfArrivalTimes.begin(), m_pfArrivalTimes.end(), 0.0);
            std::fill(m_pfEncodedHistory.begin(), m_pfEncodedHistory.end(), 0.f);
        }
        m_nEmissionCount = 0;
        m_nListenerCount = 0;
        m_fLastArrival = 0.0;
        m_fLastDistance = 0.f;
        m_fVirtualArrival = 0.0;
        m_niSearch = -1;
        m_bHasInput = false;
        m_bFinished = false;
        m_nEndFrame = 0;
    }

    void AmbisonicMovingEncoder::Refresh()
    {
        AmbisonicSource::Refresh();
    }

    double AmbisonicMovingEncoder::GetArrivalTime(
        unsigned long long nIndex, float fDistance) const
    {
        return (double)nIndex
            + (double)fDistance * (double)m_nSampleRate / (double)knSpeedOfSound;
    }

    double AmbisonicMovingEncoder::GetStoredArrival(long long nIndex) const
    {
        if (nIndex < 0)
            return m_fVirtualArrival;
        return m_pfArrivalTimes[(unsigned long long)nIndex % m_nHistoryLength];
    }

    float* AmbisonicMovingEncoder::GetHistoryVector(unsigned long long nIndex)
    {
        return &m_pfEncodedHistory[
            (size_t)(nIndex % m_nHistoryLength) * m_nChannelCount];
    }

    void AmbisonicMovingEncoder::EncodeSample(unsigned long long nIndex,
        float fSample, const PolarPosition<float>& polPosition)
    {
        AmbisonicSource::SetPosition(polPosition);
        AmbisonicSource::Refresh();

        float fWGain = 0.f;
        float fOtherGain = 0.f;
        if (polPosition.distance < m_fRoomRadius)
        {
            // Interior effect (W-Panning).
            fWGain = 1.f - polPosition.distance / (2.f * m_fRoomRadius);
            fOtherGain = polPosition.distance / (2.f * m_fRoomRadius);
        }
        else
        {
            fWGain = m_fRoomRadius / (2.f * polPosition.distance);
            fOtherGain = fWGain;
        }

        float* pfVector = GetHistoryVector(nIndex);
        for (unsigned niChannel = 0; niChannel < m_nChannelCount; niChannel++)
        {
            float fGain = niChannel == 0 ? fWGain : fOtherGain;
            pfVector[niChannel] = fSample * GetCoefficient(niChannel) * fGain;
        }
    }

    void AmbisonicMovingEncoder::WriteListenerFrame(
        unsigned long long nListenerFrame, BFormat* pBFDst, unsigned nOffset)
    {
        long long i = m_niSearch;

        // Locate the interval a(i) <= r <= a(i+1) bracketing this frame.
        double fArrivalNext = GetStoredArrival(i + 1);
        while ((double)nListenerFrame > fArrivalNext)
        {
            i++;
            fArrivalNext = GetStoredArrival(i + 1);
        }
        m_niSearch = i;

        double fArrivalLo = GetStoredArrival(i);
        double fFraction = ((double)nListenerFrame - fArrivalLo)
            / (fArrivalNext - fArrivalLo);
        if (fFraction < 0.0)
            fFraction = 0.0;
        else if (fFraction > 1.0)
            fFraction = 1.0;

        // Interpolate the signed encoded vectors, never angles first. The
        // virtual frame -1 is a zero vector.
        const float* pfLo = i < 0 ? nullptr : GetHistoryVector((unsigned long long)i);
        const float* pfHi = GetHistoryVector((unsigned long long)(i + 1));
        float fOneMinusFraction = (float)(1.0 - fFraction);
        float fFloatFraction = (float)fFraction;
        for (unsigned niChannel = 0; niChannel < m_nChannelCount; niChannel++)
        {
            float fLo = pfLo ? pfLo[niChannel] : 0.f;
            pBFDst->GetChannelPointer(niChannel)[nOffset]
                = fLo * fOneMinusFraction + pfHi[niChannel] * fFloatFraction;
        }
    }

    bool AmbisonicMovingEncoder::Process(const float* pfSrc, unsigned nSamples,
        const PolarPosition<float>* pPositions, BFormat* pBFDst)
    {
        if (nSamples == 0)
            return true;

        // Once finished, no further real input is accepted.
        if (m_bFinished || nSamples > m_nMaxBlockSize)
            return false;
        assert(pBFDst->GetChannelCount() == m_nChannelCount);
        assert(pBFDst->GetSampleCount() >= nSamples);

        // Validate the whole block first so a rejection consumes nothing.
        for (unsigned niSample = 0; niSample < nSamples; niSample++)
        {
            if (!std::isfinite(pfSrc[niSample]))
                return false;
            const PolarPosition<float>& position = pPositions[niSample];
            if (!std::isfinite(position.azimuth)
                || !std::isfinite(position.elevation)
                || !std::isfinite(position.distance))
                return false;
            if (position.elevation < -(float)M_PI / 2.f
                || position.elevation > (float)M_PI / 2.f)
                return false;
            if (position.distance < 0.f
                || position.distance > (float)knMaxDistance)
                return false;
        }

        // Arrival time differences, including the pair crossing the boundary
        // with the previous Process() call.
        double fPreviousArrival = m_fLastArrival;
        for (unsigned niSample = 0; niSample < nSamples; niSample++)
        {
            double fArrival = GetArrivalTime(
                m_nEmissionCount + niSample, pPositions[niSample].distance);
            if (m_bHasInput || niSample > 0)
            {
                double fDifference = fArrival - fPreviousArrival;
                if (fDifference < kMinArrivalDifference
                    || fDifference > kMaxArrivalDifference)
                    return false;
            }
            fPreviousArrival = fArrival;
        }

        // Block accepted: encode every sample at its own emission position.
        for (unsigned niSample = 0; niSample < nSamples; niSample++)
        {
            unsigned long long nIndex = m_nEmissionCount + niSample;
            double fArrival = GetArrivalTime(nIndex,
                pPositions[niSample].distance);

            if (!m_bHasInput && niSample == 0)
            {
                // Virtual silent emission frame -1, using the first distance.
                m_fVirtualArrival = fArrival - 1.0;
                m_niSearch = -1;
            }

            m_pfArrivalTimes[nIndex % m_nHistoryLength] = fArrival;
            EncodeSample(nIndex, pfSrc[niSample], pPositions[niSample]);

            m_fLastArrival = fArrival;
            m_fLastDistance = pPositions[niSample].distance;
        }

        m_bHasInput = true;
        m_nEmissionCount += nSamples;

        // One listener frame per accepted emission sample.
        for (unsigned niSample = 0; niSample < nSamples; niSample++)
        {
            WriteListenerFrame(m_nListenerCount, pBFDst, niSample);
            m_nListenerCount++;
        }

        return true;
    }

    unsigned AmbisonicMovingEncoder::Finish(BFormat* pBFDst, unsigned nSamples)
    {
        if (!m_bFinished)
        {
            m_bFinished = true;

            if (!m_bHasInput)
            {
                // Empty stream: no tail and no output at all.
                m_nEndFrame = 0;
                return 0;
            }

            // Fix the last position and append a silent emission at index N,
            // using the last real distance, to drain the propagation tail.
            unsigned long long nTailIndex = m_nEmissionCount;
            double fTailArrival = GetArrivalTime(nTailIndex, m_fLastDistance);
            m_pfArrivalTimes[nTailIndex % m_nHistoryLength] = fTailArrival;
            float* pfTail = GetHistoryVector(nTailIndex);
            for (unsigned niChannel = 0; niChannel < m_nChannelCount; niChannel++)
                pfTail[niChannel] = 0.f;

            // Total output is ceil(N + lastDistance * sampleRate / 344). The
            // epsilon keeps an exact integer arrival from rounding up.
            double fEpsilon = 1e-12 * std::max(1.0, fTailArrival);
            m_nEndFrame = (unsigned long long)std::ceil(fTailArrival - fEpsilon);
        }

        if (m_nListenerCount >= m_nEndFrame)
            return 0;

        unsigned long long nRemaining = m_nEndFrame - m_nListenerCount;
        unsigned nToWrite = (unsigned)std::min((unsigned long long)nSamples,
            nRemaining);
        assert(pBFDst->GetChannelCount() == m_nChannelCount);
        assert(pBFDst->GetSampleCount() >= nToWrite);

        for (unsigned niSample = 0; niSample < nToWrite; niSample++)
        {
            WriteListenerFrame(m_nListenerCount, pBFDst, niSample);
            m_nListenerCount++;
        }

        return nToWrite;
    }

    void AmbisonicMovingEncoder::SetRoomRadius(float fRoomRadius)
    {
        m_fRoomRadius = fRoomRadius;
    }

    float AmbisonicMovingEncoder::GetRoomRadius() const
    {
        return m_fRoomRadius;
    }

    unsigned AmbisonicMovingEncoder::GetMaxBlockSize() const
    {
        return m_nMaxBlockSize;
    }

    bool AmbisonicMovingEncoder::IsFinished() const
    {
        return m_bFinished;
    }

} // namespace spaudio
