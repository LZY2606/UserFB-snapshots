#include "LayerDependency.h"

/*
* Absolute playout deadline in NTP (Q16) domain :
*   anchorPlayoutNtp + floor(uint32_t(timeStamp - anchorRtp) * 65536 / clockRate)
* The media span is computed with 64bit integers, it is assumed that the
*  forward media span relative to anchorRtp is < 2^31 ticks and that the
*  result does not overflow uint32_t.
*/
uint32_t LayerDependencyManager::playoutDeadlineNtp(uint32_t anchorPlayoutNtp,
    uint32_t timeStamp,
    uint32_t anchorRtp,
    uint32_t clockRate) {
    uint64_t delta = (uint64_t)(uint32_t)(timeStamp - anchorRtp);
    uint64_t offset = (delta * 65536u) / clockRate;
    return anchorPlayoutNtp + (uint32_t)offset;
}

bool LayerDependencyManager::addDependency(uint32_t childSsrc,
    uint32_t baseSsrc,
    RtpQueueIface* childQueue,
    RtpQueueLayerIface* childLayerQueue,
    uint32_t anchorRtp,
    uint32_t anchorPlayoutNtp,
    uint32_t clockRate) {
    if (childSsrc == baseSsrc || clockRate == 0 || childQueue == NULL || childLayerQueue == NULL)
        return false;
    for (size_t n = 0; n < dependencies.size(); n++) {
        const Dependency& dep = dependencies[n];
        if (dep.childSsrc == childSsrc)
            /*
            * Each child is configured only once
            */
            return false;
        if (dep.childSsrc == baseSsrc)
            /*
            * base is already a child, chains are not allowed
            */
            return false;
        if (dep.baseSsrc == childSsrc)
            /*
            * child is already a base that others depend on,
            *  it cannot become a child (no chains/cycles)
            */
            return false;
    }
    Dependency dep;
    dep.childSsrc = childSsrc;
    dep.baseSsrc = baseSsrc;
    dep.childQueue = childQueue;
    dep.childLayerQueue = childLayerQueue;
    dep.anchorRtp = anchorRtp;
    dep.anchorPlayoutNtp = anchorPlayoutNtp;
    dep.clockRate = clockRate;
    dependencies.push_back(dep);
    if (findBase(baseSsrc) == NULL) {
        BaseState baseState;
        baseState.baseSsrc = baseSsrc;
        bases.push_back(baseState);
    }
    return true;
}

bool LayerDependencyManager::hasDependency(uint32_t childSsrc) const {
    return findChildDep(childSsrc) != NULL;
}

const LayerDependencyManager::Dependency* LayerDependencyManager::findChildDep(uint32_t childSsrc) const {
    for (size_t n = 0; n < dependencies.size(); n++) {
        if (dependencies[n].childSsrc == childSsrc)
            return &dependencies[n];
    }
    return NULL;
}

LayerDependencyManager::BaseState* LayerDependencyManager::findBase(uint32_t baseSsrc) {
    for (size_t n = 0; n < bases.size(); n++) {
        if (bases[n].baseSsrc == baseSsrc)
            return &bases[n];
    }
    return NULL;
}

uint32_t LayerDependencyManager::baseFrameDeadlineNtp(uint32_t baseSsrc, uint32_t timeStamp) {
    /*
    * A base frame proof is kept at most until the playout deadlines of
    *  all the associated children have passed
    */
    uint32_t deadline = 0;
    for (size_t n = 0; n < dependencies.size(); n++) {
        const Dependency& dep = dependencies[n];
        if (dep.baseSsrc == baseSsrc) {
            uint32_t d = playoutDeadlineNtp(dep.anchorPlayoutNtp, timeStamp, dep.anchorRtp, dep.clockRate);
            if (d > deadline)
                deadline = d;
        }
    }
    return deadline;
}

bool LayerDependencyManager::isBaseFrameConfirmed(uint32_t baseSsrc, uint32_t timeStamp) {
    BaseState* base = findBase(baseSsrc);
    if (base == NULL)
        return false;
    for (size_t n = 0; n < base->proofs.size(); n++) {
        const BaseFrameProof& proof = base->proofs[n];
        if (proof.timeStamp == timeStamp) {
            return proof.markerTransmitted &&
                proof.nTransmitted > 0 &&
                proof.nAcked == proof.nTransmitted;
        }
    }
    return false;
}

bool LayerDependencyManager::isChildHeadBlocked(uint32_t childSsrc) {
    const Dependency* dep = findChildDep(childSsrc);
    if (dep == NULL)
        return false;
    if (dep->childQueue->sizeOfQueue() == 0)
        return false;
    uint32_t timeStamp = dep->childLayerQueue->timeStampOfNextRtp();
    return !isBaseFrameConfirmed(dep->baseSsrc, timeStamp);
}

void LayerDependencyManager::notifyTransmitted(uint32_t ssrc, uint32_t timeStamp, bool isMark) {
    BaseState* base = findBase(ssrc);
    if (base == NULL)
        return;
    /*
    * Frames are transmitted in encoding order and all packets of a frame
    *  are contiguous, so a matching proof is found at or near the back
    */
    for (std::deque<BaseFrameProof>::reverse_iterator it = base->proofs.rbegin(); it != base->proofs.rend(); ++it) {
        if (it->timeStamp == timeStamp) {
            it->nTransmitted++;
            it->markerTransmitted |= isMark;
            return;
        }
    }
    BaseFrameProof proof;
    proof.timeStamp = timeStamp;
    proof.nTransmitted = 1;
    proof.nAcked = 0;
    proof.markerTransmitted = isMark;
    proof.deadline_ntp = baseFrameDeadlineNtp(ssrc, timeStamp);
    base->proofs.push_back(proof);
}

void LayerDependencyManager::notifyAcked(uint32_t ssrc, uint32_t timeStamp) {
    BaseState* base = findBase(ssrc);
    if (base == NULL)
        return;
    for (size_t n = 0; n < base->proofs.size(); n++) {
        if (base->proofs[n].timeStamp == timeStamp) {
            if (base->proofs[n].nAcked < base->proofs[n].nTransmitted)
                base->proofs[n].nAcked++;
            return;
        }
    }
    /*
    * No matching proof, either the frame was never transmitted or the
    *  proof has already expired. Late feedback must not re-enable old
    *  frames so nothing is done here.
    */
}

void LayerDependencyManager::purgeExpired(uint32_t time_ntp) {
    /*
    * Release unsent enhancement packets whose playout deadline is not
    *  later than the current time, one frame at a time
    */
    for (size_t n = 0; n < dependencies.size(); n++) {
        const Dependency& dep = dependencies[n];
        while (dep.childQueue->sizeOfQueue() > 0) {
            uint32_t timeStamp = dep.childLayerQueue->timeStampOfNextRtp();
            uint32_t deadline = playoutDeadlineNtp(dep.anchorPlayoutNtp, timeStamp, dep.anchorRtp, dep.clockRate);
            if (deadline <= time_ntp)
                dep.childLayerQueue->dropHeadFrame();
            else
                break;
        }
    }
    /*
    * Garbage collect base frame proofs whose last child playout deadline
    *  has passed. Proofs are stored in non-decreasing deadline order as
    *  frames arrive in encoding order.
    */
    for (size_t n = 0; n < bases.size(); n++) {
        BaseState& base = bases[n];
        while (!base.proofs.empty() && base.proofs.front().deadline_ntp <= time_ntp)
            base.proofs.pop_front();
    }
}
