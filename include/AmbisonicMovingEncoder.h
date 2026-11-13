/*############################################################################*/
/*#                                                                          #*/
/*#  Ambisonic C++ Library                                                   #*/
/*#  AmbisonicMovingEncoder - Moving source encoder with propagation        #*/
/*#  Copyright © 2024                                                        #*/
/*#                                                                          #*/
/*#  Filename:      AmbisonicMovingEncoder.h                                 #*/
/*#  Version:       0.1                                                      #*/
/*#  Licence:       MIT                                                      #*/
/*#                                                                          #*/
/*############################################################################*/


#ifndef SPATIALAUDIO_AMBISONIC_MOVING_ENCODER_H
#define SPATIALAUDIO_AMBISONIC_MOVING_ENCODER_H

#include "SpatialaudioAPI.h"
#include "AmbisonicSource.h"
#include "BFormat.h"
#include "Coordinates.h"

#include <vector>

namespace spaudio {

    /// Offline Ambisonic encoder for a moving point source.

    /** Unlike AmbisonicEncoderDist, which delays a mono stream and encodes it
        with the source's current direction, this encoder keeps the full
        propagation chain of every emitted sample: each mono sample is encoded
        with the azimuth and elevation measured at its own emission time, gets
        the W-panning gain of its own emission distance, and is then placed on
        the listener time line at its arrival time
        a(n) = n + distance(n) * sampleRate / 344. Listener samples falling
        between two arrival times are linear interpolations of the two signed
        encoded vectors, which yields the Doppler shift of a moving source.

        Two independent clocks are maintained, both starting at zero and
        continuous across calls: an emission clock, counting the mono samples
        handed to Process(), and a listener clock, counting the B-Format
        frames produced. The listener clock advances by one frame per emitted
        sample, so the first listener frames may be silence while sound is
        still propagating.

        This is an offline component: all in-flight sound is kept in a bounded
        history buffer allocated by Configure() and Process()/Finish() never
        allocate heap memory. */

    class SPAUDIO_API AmbisonicMovingEncoder : public AmbisonicSource
    {
    public:
        AmbisonicMovingEncoder();
        ~AmbisonicMovingEncoder();

        /** Re-create the object for the given configuration. Previous data is
         *  lost. Returns true if successful.
         * @param nOrder        Ambisonic order, from 0 to 3 (ACN/SN3D).
         * @param b3D           Flag true if the encoding is 3D.
         * @param nSampleRate   Sample rate of the signal to be encoded.
         * @param nMaxBlockSize Maximum number of samples passed per Process()
         *                      call. Sets the bounded history capacity.
         * @param fRoomRadius   Room radius in metres, used for the interior
         *                      effect (W-Panning). Must be strictly positive.
         * @return              Returns true if successfully configured.
         */
        bool Configure(unsigned nOrder, bool b3D, unsigned nSampleRate,
            unsigned nMaxBlockSize, float fRoomRadius);

        /** Clears both clocks, all in-flight sound and the finished state.
         *  The configuration is retained. */
        virtual void Reset() override;

        /** Recalculates coefficients of the underlying Ambisonic source. */
        virtual void Refresh() override;

        /** Encode a mono block with one emission position per sample.
         *
         *  If any sample or position is non-finite, an elevation is outside
         *  [-pi/2, pi/2], a distance is outside [0, 150] metres, or the
         *  difference between two consecutive arrival times (including the
         *  pair crossing a Process() boundary) is not in [0.25, 4], the whole
         *  block is rejected: no audio and no position is consumed and the
         *  object state is left unchanged.
         * @param pfSrc         Pointer to nSamples mono input samples.
         * @param nSamples      Number of emission samples in this block, not
         *                      more than the configured maximum block size.
         * @param pPositions    One polar position per emission sample. Angles
         *                      in radians, distances in metres.
         * @param pBFDst        Output B-Format signal, receives nSamples
         *                      listener frames starting at its first sample.
         * @return              Returns true if the block was accepted.
         */
        bool Process(const float* pfSrc, unsigned nSamples,
            const PolarPosition<float>* pPositions, BFormat* pBFDst);

        /** Fix the last position, append a silent emission vector after the
         *  last real sample and drain the propagating tail.
         *
         *  May be called repeatedly with an output buffer of any capacity;
         *  the returned number of frames were written. It returns zero once
         *  the tail is fully drained, and zero forever afterwards. With N
         *  real input samples the total number of listener frames produced is
         *  ceil(N + lastDistance * sampleRate / 344); finishing without any
         *  real input produces zero frames. After Finish(), any non-empty
         *  Process() call is rejected.
         * @param pBFDst    Output B-Format signal.
         * @param nSamples  Capacity, in listener frames, of pBFDst.
         * @return          Number of listener frames written.
         */
        unsigned Finish(BFormat* pBFDst, unsigned nSamples);

        /** Set the room radius in metres used for W-Panning.
         * @param fRoomRadius   Room radius in metres.
         */
        void SetRoomRadius(float fRoomRadius);

        /** Returns the room radius in metres.
         * @return  Room radius in metres.
         */
        float GetRoomRadius() const;

        /** Returns the maximum accepted Process() block size.
         * @return  Maximum block size in samples.
         */
        unsigned GetMaxBlockSize() const;

        /** Returns true once Finish() has been called.
         * @return  True after the stream has been finished.
         */
        bool IsFinished() const;

    protected:
        using AmbisonicSource::Configure;

        /** Arrival time, in listener samples, of emission sample nIndex
         *  emitted at the given distance. */
        double GetArrivalTime(unsigned long long nIndex, float fDistance) const;

        /** Arrival time stored for an emission index already in the history.
         *  Index -1 is the virtual silent frame preceding the first real
         *  sample. */
        double GetStoredArrival(long long nIndex) const;

        /** Pointer to the encoded vector stored for emission index nIndex. */
        float* GetHistoryVector(unsigned long long nIndex);

        /** Encode one mono sample at its emission position and store it. */
        void EncodeSample(unsigned long long nIndex, float fSample,
            const PolarPosition<float>& polPosition);

        /** Write one listener frame, locating the bracketing arrival times
         *  and interpolating the signed encoded vectors. */
        void WriteListenerFrame(unsigned long long nListenerFrame,
            BFormat* pBFDst, unsigned nOffset);

    private:
        unsigned m_nSampleRate;
        unsigned m_nMaxBlockSize;
        float m_fRoomRadius;

        // Bounded history: one slot per emission index, used as a ring.
        unsigned m_nHistoryLength;
        std::vector<double> m_pfArrivalTimes;
        std::vector<float> m_pfEncodedHistory;

        // Emission and listener clocks.
        unsigned long long m_nEmissionCount;
        unsigned long long m_nListenerCount;

        // Arrival time of the last accepted real sample, for the arrival
        // difference check across Process() boundaries.
        double m_fLastArrival;
        // Distance of the last accepted real sample.
        float m_fLastDistance;
        // Arrival time of the virtual silent emission frame -1.
        double m_fVirtualArrival;
        // Left index of the arrival interval bracketing the listener clock.
        long long m_niSearch;

        bool m_bHasInput;
        bool m_bFinished;
        // Listener frame at which the drained tail ends.
        unsigned long long m_nEndFrame;
    };

} // namespace spaudio

#endif // SPATIALAUDIO_AMBISONIC_MOVING_ENCODER_H
