// golomb_image.h
//
// Guarded byte images.  Every hand-off to a production entry lives in a
// buffer laid out as:
//
//   [ front sentinel 16B ][ logical stream (streamBytes) ][ read padding 64B ]
//   [ hard back sentinel 32B ]
//
// The production AG reader uses wide-word loads (read32bit plus an optional
// fifth byte) that can over-read a few bytes past the current bit position.
// kReadPadding supplies those readable bytes while BitBuffer.byteSize/end
// still name the *logical* length, so the physical-EOF / end-pointer contract
// is unchanged.  The hard front/back sentinels must never change; bit-level
// sentinels immediately around the valid bit window are preserved by the
// production read-modify-write jams for our <= 24-bit fields.

#ifndef GOLOMB_IMAGE_H
#define GOLOMB_IMAGE_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace gi {

static const size_t kFrontGuard = 16;
static const size_t kReadPadding = 64;
static const size_t kBackGuard = 32;
static const uint8_t kFrontByte = 0x5a;
static const uint8_t kBackByte = 0xdb;
static const uint8_t kStreamFill = 0xa5;

struct Image {
    size_t streamBytes;
    std::vector<uint8_t> raw;
    uint8_t* p;  // logical stream pointer (raw.data()+kFrontGuard)

    explicit Image(size_t logicalBytes, uint8_t fill = kStreamFill)
        : streamBytes(logicalBytes) {
        raw.assign(kFrontGuard + streamBytes + kReadPadding + kBackGuard, fill);
        for (size_t i = 0; i < kFrontGuard; ++i) raw[i] = kFrontByte;
        size_t backStart = kFrontGuard + streamBytes + kReadPadding;
        for (size_t i = backStart; i < raw.size(); ++i) raw[i] = kBackByte;
        p = raw.data() + kFrontGuard;
    }

    bool hardGuardsIntact() const {
        for (size_t i = 0; i < kFrontGuard; ++i)
            if (raw[i] != kFrontByte) return false;
        size_t backStart = kFrontGuard + streamBytes + kReadPadding;
        for (size_t i = backStart; i < raw.size(); ++i)
            if (raw[i] != kBackByte) return false;
        return true;
    }

    // Readable length for byte-level readers (logical + padding).
    size_t readableBytes() const { return streamBytes + kReadPadding; }
};

} // namespace gi

#endif // GOLOMB_IMAGE_H
