#pragma once

// H.264/AVC access unit framing for MPEG-TS elementary streams.
//
// The demuxer delivers PES payloads. A PES may contain half a NAL unit or
// several coded pictures, so PES boundaries must not be reported as frames.
// H264AccessUnitFramer reassembles the Annex-B byte stream across PES
// boundaries and applies the H.264 "first VCL NAL unit of a primary coded
// picture" identity rules (ISO/IEC 14496-10, 7.4.1.2.4) to split access
// units.
//
// PES time stamps are bound by byte origin instead of frame rate: every
// start code remembers which PES its first zero byte was carried in, and the
// PTS/DTS of that PES is attached to the first picture only.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mpegts {

// Byte storage of one framed access unit in normalized Annex-B form (every
// NAL is prefixed by a four byte 00 00 00 01 start code). This is a plain
// byte vector; the implicit conversion to std::string and the mixed
// comparison operators are only conveniences for text-style assertions.
class H264AnnexB : public std::vector<uint8_t> {
public:
    using std::vector<uint8_t>::vector;

    H264AnnexB() = default;
    H264AnnexB(const std::vector<uint8_t>& lOther) : std::vector<uint8_t>(lOther) {}
    H264AnnexB(std::vector<uint8_t>&& lOther) : std::vector<uint8_t>(std::move(lOther)) {}
    H264AnnexB(const std::string& rString)
        : std::vector<uint8_t>(rString.begin(), rString.end()) {}

    static constexpr size_t npos = static_cast<size_t>(-1);

    [[nodiscard]] std::string toString() const {
        return std::string(reinterpret_cast<const char*>(data()), size());
    }

    operator std::string() const { return toString(); }

    // NUL terminated view kept in a mutable cache so it remains usable from
    // a const H264AccessUnit reference in a callback.
    const char* c_str() const {
        mCString.assign(reinterpret_cast<const char*>(data()), size());
        return mCString.c_str();
    }

    [[nodiscard]] size_t find(const std::string& rNeedle, size_t lPos = 0) const {
        return toString().find(rNeedle, lPos);
    }
    [[nodiscard]] size_t find(const char* pNeedle, size_t lPos = 0) const {
        return toString().find(pNeedle, lPos);
    }
    [[nodiscard]] size_t find(char cNeedle, size_t lPos = 0) const {
        return toString().find(cNeedle, lPos);
    }
    [[nodiscard]] std::string substr(size_t lPos = 0, size_t lCount = npos) const {
        return toString().substr(lPos, lCount);
    }

    friend bool operator==(const H264AnnexB& rLhs, const std::string& rRhs) {
        return rLhs.toString() == rRhs;
    }
    friend bool operator==(const std::string& rLhs, const H264AnnexB& rRhs) {
        return rLhs == rRhs.toString();
    }
    friend bool operator==(const H264AnnexB& rLhs, const char* pRhs) {
        return rLhs.toString() == std::string(pRhs);
    }
    friend bool operator==(const char* pLhs, const H264AnnexB& rRhs) {
        return std::string(pLhs) == rRhs.toString();
    }
    friend bool operator!=(const H264AnnexB& rLhs, const std::string& rRhs) { return !(rLhs == rRhs); }
    friend bool operator!=(const std::string& rLhs, const H264AnnexB& rRhs) { return !(rLhs == rRhs); }
    friend bool operator!=(const H264AnnexB& rLhs, const char* pRhs) { return !(rLhs == pRhs); }
    friend bool operator!=(const char* pLhs, const H264AnnexB& rRhs) { return !(pLhs == rRhs); }

private:
    mutable std::string mCString;
};

// One assembled H.264 access unit. The instance passed to h264OutCallback is
// borrowed by the callback only and must not be retained after it returns.
struct H264AccessUnit final {
    uint16_t pid = 0;
    H264AnnexB annexB;
    bool hasPts = false;
    bool hasDts = false;
    uint64_t pts = 0; // Raw 33 bit PTS value
    uint64_t dts = 0; // Raw 33 bit DTS value, falls back to PTS when DTS is absent
    bool isIdr = false;
};

// PTS/DTS presence and raw 33 bit values of one source PES.
struct H264TimeInfo final {
    bool hasPts = false;
    bool hasDts = false;
    uint64_t pts = 0;
    uint64_t dts = 0;
};

class H264AccessUnitFramer final {
public:
    explicit H264AccessUnitFramer(uint16_t lPid);
    ~H264AccessUnitFramer();

    // Start a new PES. Its time info is attached to every start code whose
    // first zero byte is part of this PES. Continuation TS packets of the
    // same PES must only call feed().
    void beginPes(const H264TimeInfo& rTimeInfo);

    // Feed Annex-B bytes, any split (including inside a start code) is valid.
    void feed(const uint8_t* pData, size_t lSize);

    // Flush the last access unit. Idempotent, repeated calls do not emit twice.
    void finish();

    std::function<void(const H264AccessUnit& rAccessUnit)> accessUnitCallback = nullptr;
    std::function<void(uint16_t lPid, const std::string& rMessage)> errorCallback = nullptr;

private:
    struct TimeSource {
        H264TimeInfo mTime;
        bool mTimeConsumed = false;
    };

    struct SpsInfo {
        bool mValid = false;
        bool mSeparateColourPlaneFlag = false;
        int mLog2MaxFrameNumMinus4 = 0;
        int mPicOrderCntType = 0;
        int mLog2MaxPicOrderCntLsbMinus4 = 0;
        bool mDeltaPicOrderAlwaysZeroFlag = false;
        bool mFrameMbsOnlyFlag = true;
    };

    struct PpsInfo {
        bool mValid = false;
        int mSpsId = 0;
        bool mBottomFieldPicOrderInFramePresentFlag = false;
    };

    struct SliceIdentity {
        int mFrameNum = 0;
        int mPpsId = 0;
        int mFieldPicFlag = 0;
        int mBottomFieldFlag = 0;
        int mNalRefIdc = 0;
        bool mIsIdr = false;
        int mIdrPicId = 0;
        int mPicOrderCntType = 0;
        int mPicOrderCntLsb = 0;
        int mDeltaPicOrderCntBottom = 0;
        int mDeltaPicOrderCnt0 = 0;
        int mDeltaPicOrderCnt1 = 0;
    };

    // H.264 7.4.1.2.4 identity comparison.
    static bool startsNewPicture(const SliceIdentity& rA, const SliceIdentity& rB);

    // Process one complete NAL unit (header byte included, start code excluded,
    // emulation prevention bytes still present). Returns false if an error was
    // reported and the stream was reset to the wait-for-IDR state.
    bool handleNal(const std::vector<uint8_t>& rNal, const std::shared_ptr<TimeSource>& rpSource);

    bool parseSps(const std::vector<uint8_t>& rNal);
    bool parsePps(const std::vector<uint8_t>& rNal);
    bool parseSliceIdentity(const std::vector<uint8_t>& rNal, int lNalType,
                            SliceIdentity& rIdentity, bool& rbIsIdr);

    void placeNonVcl(const std::vector<uint8_t>& rNal, bool bMovesToNextAu);
    void handleVcl(const std::vector<uint8_t>& rNal,
                   const std::shared_ptr<TimeSource>& rpSource,
                   int lNalType, int lNalRefIdc);

    // Emit mAuNals as one H264AccessUnit and clear the AU assembly state.
    void emitAccessUnit();

    // Report a framing/parse error, drop the pending picture and tail bytes
    // and enter the wait-for-fresh-IDR recovery state. Parsed SPS/PPS are kept.
    void fail(const std::string& rMessage);

    uint16_t mPid = 0;

    // Parameter sets, owned per PID.
    std::map<int, SpsInfo> mSps;
    std::map<int, PpsInfo> mPps;

    // Start code scanner state.
    bool mNalStarted = false;
    std::vector<uint8_t> mNal;
    std::shared_ptr<TimeSource> mNalSource;
    struct HeldZero {
        std::shared_ptr<TimeSource> pSource;
    };
    std::vector<HeldZero> mHeldZeros;
    std::shared_ptr<TimeSource> mCurrentSource;

    // Access unit assembly state. Raw NAL bytes in output order.
    std::vector<std::vector<uint8_t>> mAuNals;
    // SPS/PPS/SEI/AUD seen after VCL of the current AU, belonging to the next one.
    std::vector<std::vector<uint8_t>> mNextAuPrefix;
    bool mAuHasVcl = false;
    bool mAuIsIdr = false;
    bool mHavePreviousIdentity = false;
    std::unique_ptr<SliceIdentity> mPreviousIdentity;

    // Time captured for the AU currently being assembled.
    bool mAuHasPts = false;
    bool mAuHasDts = false;
    uint64_t mAuPts = 0;
    uint64_t mAuDts = 0;

    // After an error, discard everything until a new legal IDR slice.
    bool mWaitForIdr = false;
    bool mFinished = false;
};

} // namespace mpegts
