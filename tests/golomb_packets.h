// golomb_packets.h
//
// Independent construction of legal ALAC SCE/CPE packets for the packets
// group.  All header/audio bits are assembled by our own MSB-first writer;
// production BitBufferWrite is deliberately not used for material.  The
// only production entry these bytes are handed to is ALACDecoder::Decode
// (via BitBufferInit, which just names the array/size).

#ifndef GOLOMB_PACKETS_H
#define GOLOMB_PACKETS_H

#include "golomb_model.h"
#include <stdint.h>
#include <vector>
#include <string.h>

namespace gp {

// Build a raw 24-byte ALACSpecificConfig magic cookie.  Multi-byte integer
// fields are big-endian in the cookie; pb/mb/kb/bitDepth are single bytes.
inline std::vector<uint8_t> makeCookie(int bitDepth, int numChannels,
                                       uint32_t frameLength,
                                       uint8_t pb = 0, uint8_t mb = 10,
                                       uint8_t kb = 14,
                                       uint32_t sampleRate = 44100) {
    std::vector<uint8_t> c(24, 0);
    c[0] = (uint8_t)(frameLength >> 24);
    c[1] = (uint8_t)(frameLength >> 16);
    c[2] = (uint8_t)(frameLength >> 8);
    c[3] = (uint8_t)(frameLength);
    c[4] = 0;                              // compatibleVersion
    c[5] = (uint8_t)bitDepth;
    c[6] = pb;
    c[7] = mb;
    c[8] = kb;
    c[9] = (uint8_t)numChannels;
    c[10] = 0; c[11] = 255;                // maxRun = 255 (BE)
    // maxFrameBytes / avgBitRate left zero
    c[20] = (uint8_t)(sampleRate >> 24);
    c[21] = (uint8_t)(sampleRate >> 16);
    c[22] = (uint8_t)(sampleRate >> 8);
    c[23] = (uint8_t)(sampleRate);
    return c;
}

struct Writer {
    gm::BitList bits;

    void u(uint32_t value, int n) { bits.bits(value, (uint32_t)n); }
    void tag(uint32_t elementId, uint32_t instance) { u(elementId, 3); u(instance, 4); }
    void zeros(int n) { u(0, n); }

    // Append an independently modelled AG residual block.  Returns the model
    // so callers can read its consumed bit count / records.
    gm::Block ag(const std::vector<int32_t>& residuals, uint32_t maxBits,
                 uint32_t mb0 = 10, uint32_t pb = 0, uint32_t kb = 14) {
        gm::Block m = gm::encodePlan(residuals, mb0, pb, kb, maxBits);
        for (int x : m.encoded.b) bits.one(x);
        return m;
    }

    void byteAlign() { while (bits.size() & 7u) bits.one(0); }

    size_t bitSize() const { return bits.size(); }

    // Byte-aligned packet image.
    std::vector<uint8_t> packed() {
        byteAlign();
        size_t nbytes = (bits.size() + 7) / 8;
        return gm::packAt(bits.b, 0, nbytes, 0x00);
    }
};

// Standard compressed-element audio parameter bytes:
// modeU = 0, denShift = 9 (DENSHIFT_DEFAULT); pbFactor = 4, numActive = 0
// (prediction order zero => unpc_block just copies residuals).
inline void predictorOrderZeroHeader(Writer& w) {
    w.u(0x09, 8);
    w.u((4u << 5) | 0u, 8);
}

} // namespace gp

#endif // GOLOMB_PACKETS_H
