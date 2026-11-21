#ifndef LAYER_DEPENDENCY_H
#define LAYER_DEPENDENCY_H

#include <cstdint>
#include <vector>
#include <deque>
#include "RtpQueue.h"

extern "C" {

    /*
    * Tracks layer dependencies between a base stream and one or more
    *  enhancement (child) streams for ScreamV2Tx.
    *
    * An enhancement frame at the head of a child stream's RTP queue may only
    *  become a transmission candidate once the base frame with the same RTP
    *  timestamp is confirmed, i.e :
    *  + The marker packet of the base frame was actually transmitted
    *    (registered through ScreamV2Tx::addTransmitted)
    *  + Every base packet of that RTP timestamp that was actually transmitted
    *    has been positively ACKed through the normal feedback path
    *
    * Each enhancement frame has an absolute playout deadline in NTP (Q16) :
    *   anchorPlayoutNtp + floor(uint32_t(timeStamp - anchorRtp) * 65536 / clockRate)
    * Unsent enhancement packets whose deadline is not later than the current
    *  time are released (packet_free) and base frame proofs are only kept
    *  until the last of the associated child deadlines has passed.
    */
    class LayerDependencyManager {
    public:
        LayerDependencyManager() {}

        /*
        * Register a dependency child->base.
        * Fails (returns false, state unchanged) if
        *  + child == base or clockRate == 0 or queues are NULL
        *  + child is already configured as a child
        *  + base is already configured as a child (no chains)
        *  + child is already configured as a base (no chains/cycles)
        * One base may have several children.
        */
        bool addDependency(uint32_t childSsrc,
            uint32_t baseSsrc,
            RtpQueueIface* childQueue,
            RtpQueueLayerIface* childLayerQueue,
            uint32_t anchorRtp,
            uint32_t anchorPlayoutNtp,
            uint32_t clockRate);

        /*
        * True if ssrc is configured as a child (enhancement) stream
        */
        bool hasDependency(uint32_t childSsrc) const;

        /*
        * True if ssrc is a child stream whose head frame is not yet
        *  confirmed by the base layer (and thus may not be transmitted)
        */
        bool isChildHeadBlocked(uint32_t childSsrc);

        /*
        * Called from ScreamV2Tx::addTransmitted, registers transmitted
        *  base layer packets
        */
        void notifyTransmitted(uint32_t ssrc, uint32_t timeStamp, bool isMark);

        /*
        * Called when a transmitted packet is positively ACKed
        */
        void notifyAcked(uint32_t ssrc, uint32_t timeStamp);

        /*
        * Release unsent enhancement packets whose playout deadline has
        *  passed and garbage collect expired base frame proofs
        */
        void purgeExpired(uint32_t time_ntp);

        /*
        * Absolute playout deadline in NTP (Q16) domain for a frame
        */
        static uint32_t playoutDeadlineNtp(uint32_t anchorPlayoutNtp,
            uint32_t timeStamp,
            uint32_t anchorRtp,
            uint32_t clockRate);

    private:
        struct Dependency {
            uint32_t childSsrc;
            uint32_t baseSsrc;
            RtpQueueIface* childQueue;
            RtpQueueLayerIface* childLayerQueue;
            uint32_t anchorRtp;
            uint32_t anchorPlayoutNtp;
            uint32_t clockRate;
        };
        struct BaseFrameProof {
            uint32_t timeStamp;
            int nTransmitted;
            int nAcked;
            bool markerTransmitted;
            uint32_t deadline_ntp;
        };
        struct BaseState {
            uint32_t baseSsrc;
            std::deque<BaseFrameProof> proofs;
        };

        BaseState* findBase(uint32_t baseSsrc);
        const Dependency* findChildDep(uint32_t childSsrc) const;
        bool isBaseFrameConfirmed(uint32_t baseSsrc, uint32_t timeStamp);
        uint32_t baseFrameDeadlineNtp(uint32_t baseSsrc, uint32_t timeStamp);

        std::vector<Dependency> dependencies;
        std::vector<BaseState> bases;
    };
}
#endif
