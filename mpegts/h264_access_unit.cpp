#include "h264_access_unit.h"

#include <algorithm>
#include <cstring>

namespace mpegts {

namespace {

// Bounds safe MSB first bit reader with unsigned/signed Exp-Golomb support.
class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& rData) : mData(rData) {}

    bool ok() const { return mOk; }

    uint32_t readBit() {
        if (mBitPos / 8 >= mData.size()) {
            mOk = false;
            return 0;
        }
        uint8_t lByte = mData[mBitPos / 8];
        uint32_t lValue = (lByte >> (7 - (mBitPos % 8))) & 0x01;
        ++mBitPos;
        return lValue;
    }

    uint32_t readBits(int lCount) {
        uint32_t lValue = 0;
        for (int i = 0; i < lCount; ++i) {
            lValue = (lValue << 1) | readBit();
        }
        return lValue;
    }

    uint32_t readUe() {
        int lLeadingZeros = 0;
        while (readBit() == 0 && mOk) {
            ++lLeadingZeros;
            if (lLeadingZeros > 32) {
                mOk = false;
                return 0;
            }
        }
        if (!mOk) {
            return 0;
        }
        if (lLeadingZeros == 0) {
            return 0;
        }
        uint32_t lSuffix = readBits(lLeadingZeros);
        uint64_t lCode = (uint64_t(1) << lLeadingZeros) - 1 + lSuffix;
        return static_cast<uint32_t>(lCode);
    }

    int32_t readSe() {
        uint32_t lCodeNum = readUe();
        if (!mOk) {
            return 0;
        }
        if (lCodeNum & 1u) {
            return static_cast<int32_t>((lCodeNum + 1) / 2);
        }
        return -static_cast<int32_t>(lCodeNum / 2);
    }

private:
    const std::vector<uint8_t>& mData;
    size_t mBitPos = 0;
    bool mOk = true;
};

// Remove emulation prevention bytes (00 00 03 -> 00 00) as required before
// Exp-Golomb parsing.
std::vector<uint8_t> unescapeRbsp(const std::vector<uint8_t>& rNal) {
    std::vector<uint8_t> lRbsp;
    lRbsp.reserve(rNal.size());
    for (size_t i = 0; i < rNal.size(); ++i) {
        if (i + 2 < rNal.size() && rNal[i] == 0x00 && rNal[i + 1] == 0x00 && rNal[i + 2] == 0x03) {
            lRbsp.push_back(0x00);
            lRbsp.push_back(0x00);
            i += 2; // Consume the two zeros as well, loop skips the 0x03
        } else {
            lRbsp.push_back(rNal[i]);
        }
    }
    return lRbsp;
}

} // namespace

// H.264 7.4.1.2.4 boundary identity comparison. Returns true when the two VCL
// NAL units belong to different primary coded pictures.
bool H264AccessUnitFramer::startsNewPicture(const SliceIdentity& rA,
                                            const SliceIdentity& rB) {
    if (rA.mFrameNum != rB.mFrameNum) {
        return true;
    }
    if (rA.mPpsId != rB.mPpsId) {
        return true;
    }
    if (rA.mFieldPicFlag != rB.mFieldPicFlag) {
        return true;
    }
    if (rA.mBottomFieldFlag != rB.mBottomFieldFlag) {
        return true;
    }
    if (rA.mNalRefIdc != rB.mNalRefIdc && (rA.mNalRefIdc == 0 || rB.mNalRefIdc == 0)) {
        return true;
    }
    if (rA.mIsIdr != rB.mIsIdr) {
        return true;
    }
    if (rA.mIsIdr && rB.mIsIdr && rA.mIdrPicId != rB.mIdrPicId) {
        return true;
    }
    if (rA.mPicOrderCntType == 0 && rB.mPicOrderCntType == 0) {
        if (rA.mPicOrderCntLsb != rB.mPicOrderCntLsb) {
            return true;
        }
        if (rA.mDeltaPicOrderCntBottom != rB.mDeltaPicOrderCntBottom) {
            return true;
        }
    }
    if (rA.mPicOrderCntType == 1 && rB.mPicOrderCntType == 1) {
        if (rA.mDeltaPicOrderCnt0 != rB.mDeltaPicOrderCnt0) {
            return true;
        }
        if (rA.mDeltaPicOrderCnt1 != rB.mDeltaPicOrderCnt1) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Framer
// ---------------------------------------------------------------------------

H264AccessUnitFramer::H264AccessUnitFramer(uint16_t lPid) : mPid(lPid) {
    mCurrentSource = std::make_shared<TimeSource>();
}

H264AccessUnitFramer::~H264AccessUnitFramer() = default;

void H264AccessUnitFramer::beginPes(const H264TimeInfo& rTimeInfo) {
    mCurrentSource = std::make_shared<TimeSource>();
    mCurrentSource->mTime = rTimeInfo;
}

void H264AccessUnitFramer::fail(const std::string& rMessage) {
    if (errorCallback) {
        errorCallback(mPid, rMessage);
    }

    // Drop the picture being assembled, but keep successfully parsed parameter
    // sets. Recovery starts at a new legal IDR.
    mAuNals.clear();
    mNextAuPrefix.clear();
    mAuHasVcl = false;
    mAuIsIdr = false;
    mAuHasPts = false;
    mAuHasDts = false;
    mAuPts = 0;
    mAuDts = 0;
    mHavePreviousIdentity = false;
    mPreviousIdentity.reset();
    mWaitForIdr = true;
}

bool H264AccessUnitFramer::parseSps(const std::vector<uint8_t>& rNal) {
    std::vector<uint8_t> lEbsp = unescapeRbsp(rNal);
    if (lEbsp.size() < 4) {
        return false;
    }
    // Skip the one byte NAL header, the SPS RBSP starts at profile_idc.
    std::vector<uint8_t> lRbsp(lEbsp.begin() + 1, lEbsp.end());

    BitReader lBr(lRbsp);
    int lProfileIdc = static_cast<int>(lBr.readBits(8));
    lBr.readBits(8); // constraint flags + reserved
    /* int lLevelIdc = */ lBr.readBits(8);
    int lSpsId = static_cast<int>(lBr.readUe());
    if (!lBr.ok() || lSpsId < 0 || lSpsId > 31) {
        return false;
    }

    SpsInfo lSps;
    lSps.mValid = true;

    static const int kHighProfiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    bool lHighProfile = false;
    for (int lP : kHighProfiles) {
        if (lProfileIdc == lP) {
            lHighProfile = true;
            break;
        }
    }

    if (lHighProfile) {
        int lChromaFormatIdc = static_cast<int>(lBr.readUe());
        if (lChromaFormatIdc == 3) {
            lSps.mSeparateColourPlaneFlag = lBr.readBit() != 0;
        }
        lBr.readUe(); // bit_depth_luma_minus8
        lBr.readUe(); // bit_depth_chroma_minus8
        lBr.readBit(); // qpprime_y_zero_transform_bypass_flag
        bool lSeqScalingMatrixPresent = lBr.readBit() != 0;
        if (lSeqScalingMatrixPresent) {
            int lListCount = (lChromaFormatIdc != 3) ? 8 : 12;
            for (int lI = 0; lI < lListCount; ++lI) {
                if (lBr.readBit() == 0) {
                    continue;
                }
                int lSize = (lI < 6) ? 16 : 64;
                int lLastScale = 8;
                int lNextScale = 8;
                for (int lJ = 0; lJ < lSize; ++lJ) {
                    if (lNextScale != 0) {
                        int lDeltaScale = lBr.readSe();
                        lNextScale = (lLastScale + lDeltaScale + 256) % 256;
                    }
                    if (lNextScale != 0) {
                        lLastScale = lNextScale;
                    }
                }
            }
        }
    }

    lSps.mLog2MaxFrameNumMinus4 = static_cast<int>(lBr.readUe());
    lSps.mPicOrderCntType = static_cast<int>(lBr.readUe());
    if (lSps.mPicOrderCntType == 0) {
        lSps.mLog2MaxPicOrderCntLsbMinus4 = static_cast<int>(lBr.readUe());
    } else if (lSps.mPicOrderCntType == 1) {
        lSps.mDeltaPicOrderAlwaysZeroFlag = lBr.readBit() != 0;
        lBr.readSe(); // offset_for_non_ref_pic
        lBr.readSe(); // offset_for_top_to_bottom_field
        int lNumRefFramesInPocCycle = static_cast<int>(lBr.readUe());
        for (int lI = 0; lI < lNumRefFramesInPocCycle; ++lI) {
            lBr.readSe();
        }
    }

    lBr.readUe(); // max_num_ref_frames
    lBr.readBit(); // gaps_in_frame_num_value_allowed_flag
    lBr.readUe(); // pic_width_in_mbs_minus1
    lBr.readUe(); // pic_height_in_map_units_minus1
    lSps.mFrameMbsOnlyFlag = lBr.readBit() != 0;
    if (!lSps.mFrameMbsOnlyFlag) {
        lBr.readBit(); // mb_adaptive_frame_field_flag
    }

    if (!lBr.ok()) {
        return false;
    }

    mSps[lSpsId] = lSps;
    return true;
}

bool H264AccessUnitFramer::parsePps(const std::vector<uint8_t>& rNal) {
    std::vector<uint8_t> lEbsp = unescapeRbsp(rNal);
    if (lEbsp.size() < 2) {
        return false;
    }
    // Skip the one byte NAL header.
    std::vector<uint8_t> lRbsp(lEbsp.begin() + 1, lEbsp.end());

    BitReader lBr(lRbsp);
    int lPpsId = static_cast<int>(lBr.readUe());
    int lSpsId = static_cast<int>(lBr.readUe());
    lBr.readBit(); // entropy_coding_mode_flag
    bool lBottomFieldPoc = lBr.readBit() != 0; // bottom_field_pic_order_in_frame_present_flag

    if (!lBr.ok() || lPpsId < 0 || lPpsId > 255 || lSpsId < 0 || lSpsId > 31) {
        return false;
    }

    PpsInfo lPps;
    lPps.mValid = true;
    lPps.mSpsId = lSpsId;
    lPps.mBottomFieldPicOrderInFramePresentFlag = lBottomFieldPoc;
    mPps[lPpsId] = lPps;
    return true;
}

bool H264AccessUnitFramer::parseSliceIdentity(const std::vector<uint8_t>& rNal, int lNalType,
                                              SliceIdentity& rIdentity, bool& rbIsIdr) {
    std::vector<uint8_t> lRbsp = unescapeRbsp(rNal);
    // The NAL header byte is not part of the slice_header RBSP bit stream.
    std::vector<uint8_t> lSliceRbsp(lRbsp.begin() + 1, lRbsp.end());
    BitReader lBr(lSliceRbsp);

    lBr.readUe(); // first_mb_in_slice: deliberately not a frame boundary criterion
    lBr.readUe(); // slice_type
    int lPpsId = static_cast<int>(lBr.readUe());
    if (!lBr.ok()) {
        return false;
    }

    auto lPpsIt = mPps.find(lPpsId);
    if (lPpsIt == mPps.end() || !lPpsIt->second.mValid) {
        // Unknown PPS.
        rIdentity.mPpsId = lPpsId;
        return false;
    }
    const PpsInfo& rPps = lPpsIt->second;

    auto lSpsIt = mSps.find(rPps.mSpsId);
    if (lSpsIt == mSps.end() || !lSpsIt->second.mValid) {
        // PPS references an unknown SPS.
        rIdentity.mPpsId = lPpsId;
        return false;
    }
    const SpsInfo& rSps = lSpsIt->second;

    if (rSps.mSeparateColourPlaneFlag) {
        lBr.readBits(2); // colour_plane_id
    }

    int lFrameNumBits = rSps.mLog2MaxFrameNumMinus4 + 4;
    rIdentity.mPpsId = lPpsId;
    rIdentity.mFrameNum = static_cast<int>(lBr.readBits(lFrameNumBits));

    rIdentity.mFieldPicFlag = 0;
    rIdentity.mBottomFieldFlag = 0;
    if (!rSps.mFrameMbsOnlyFlag) {
        rIdentity.mFieldPicFlag = static_cast<int>(lBr.readBit());
        if (rIdentity.mFieldPicFlag) {
            rIdentity.mBottomFieldFlag = static_cast<int>(lBr.readBit());
        }
    }

    rIdentity.mIsIdr = (lNalType == 5);
    rbIsIdr = rIdentity.mIsIdr;
    if (rIdentity.mIsIdr) {
        rIdentity.mIdrPicId = static_cast<int>(lBr.readUe());
    }

    rIdentity.mPicOrderCntType = rSps.mPicOrderCntType;
    if (rSps.mPicOrderCntType == 0) {
        int lPocLsbBits = rSps.mLog2MaxPicOrderCntLsbMinus4 + 4;
        rIdentity.mPicOrderCntLsb = static_cast<int>(lBr.readBits(lPocLsbBits));
        if (rPps.mBottomFieldPicOrderInFramePresentFlag && !rIdentity.mFieldPicFlag) {
            rIdentity.mDeltaPicOrderCntBottom = lBr.readSe();
        }
    } else if (rSps.mPicOrderCntType == 1 && !rSps.mDeltaPicOrderAlwaysZeroFlag) {
        rIdentity.mDeltaPicOrderCnt0 = lBr.readSe();
        if (rPps.mBottomFieldPicOrderInFramePresentFlag && !rIdentity.mFieldPicFlag) {
            rIdentity.mDeltaPicOrderCnt1 = lBr.readSe();
        }
    }

    return lBr.ok();
}

void H264AccessUnitFramer::placeNonVcl(const std::vector<uint8_t>& rNal, bool bMovesToNextAu) {
    if (bMovesToNextAu && mAuHasVcl) {
        // Parameter sets / SEI / AUD appearing after VCL NALs belong to the
        // next access unit.
        mNextAuPrefix.push_back(rNal);
    } else {
        mAuNals.push_back(rNal);
    }
}

void H264AccessUnitFramer::emitAccessUnit() {
    if (!mAuHasVcl) {
        return;
    }

    H264AccessUnit lAu;
    lAu.pid = mPid;
    lAu.isIdr = mAuIsIdr;
    lAu.hasPts = mAuHasPts;
    lAu.hasDts = mAuHasDts;
    lAu.pts = mAuPts;
    lAu.dts = mAuDts;

    size_t lTotalSize = mAuNals.size() * 4;
    for (const auto& rNal : mAuNals) {
        lTotalSize += rNal.size();
    }
    lAu.annexB.reserve(lTotalSize);

    static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    for (const auto& rNal : mAuNals) {
        lAu.annexB.insert(lAu.annexB.end(), kStartCode, kStartCode + 4);
        lAu.annexB.insert(lAu.annexB.end(), rNal.begin(), rNal.end());
    }

    if (accessUnitCallback) {
        accessUnitCallback(lAu);
    }
}

void H264AccessUnitFramer::handleVcl(const std::vector<uint8_t>& rNal,
                                     const std::shared_ptr<TimeSource>& rpSource,
                                     int lNalType, int lNalRefIdc) {
    SliceIdentity lIdentity;
    lIdentity.mNalRefIdc = lNalRefIdc;
    bool bIsIdr = false;
    bool bParsed = parseSliceIdentity(rNal, lNalType, lIdentity, bIsIdr);

    if (mWaitForIdr) {
        // In recovery state non-IDR VCL NALs are ignored without raising more
        // errors. An IDR has to be legal (known parameter sets and a readable
        // slice header) before framing resumes.
        if (lNalType != 5) {
            return;
        }
        if (!bParsed) {
            fail("H.264 recovery IDR uses an unknown parameter set or an incomplete slice header");
            return;
        }
        mWaitForIdr = false;
    } else if (!bParsed) {
        if (mPps.find(lIdentity.mPpsId) == mPps.end()) {
            fail("H.264 slice references unknown PPS id " + std::to_string(lIdentity.mPpsId));
        } else {
            fail("H.264 slice references unknown SPS or has an incomplete slice header");
        }
        return;
    }

    bool bNewPicture = !mHavePreviousIdentity ||
                       startsNewPicture(*mPreviousIdentity, lIdentity);

    if (bNewPicture) {
        // Emit the picture assembled so far before opening the new one.
        if (mAuHasVcl) {
            emitAccessUnit();
            // SPS/PPS/SEI/AUD seen after its VCLs lead the next AU.
            mAuNals.clear();
            mAuNals.insert(mAuNals.end(), mNextAuPrefix.begin(), mNextAuPrefix.end());
            mNextAuPrefix.clear();
        }
        // When no VCL was seen yet, mAuNals already holds this AU's leading
        // parameter sets / SEI / AUD and must be preserved.

        mAuHasVcl = true;
        mAuIsIdr = bIsIdr;

        // Bind the source PES time to the first VCL start code of this picture.
        mAuHasPts = false;
        mAuHasDts = false;
        mAuPts = 0;
        mAuDts = 0;
        if (rpSource && (rpSource->mTime.hasPts || rpSource->mTime.hasDts) &&
            !rpSource->mTimeConsumed) {
            rpSource->mTimeConsumed = true;
            const H264TimeInfo& rTime = rpSource->mTime;
            mAuHasPts = rTime.hasPts;
            mAuPts = rTime.pts;
            mAuHasDts = rTime.hasDts;
            // When DTS is absent it may alias PTS, but hasDts stays false.
            mAuDts = rTime.hasDts ? rTime.dts : rTime.pts;
        }

        mPreviousIdentity = std::make_unique<SliceIdentity>(lIdentity);
        mHavePreviousIdentity = true;
    }

    mAuNals.push_back(rNal);
}

bool H264AccessUnitFramer::handleNal(const std::vector<uint8_t>& rNal,
                                     const std::shared_ptr<TimeSource>& rpSource) {
    if (rNal.empty()) {
        fail("Incomplete H.264 NAL header (empty NAL unit)");
        return false;
    }

    uint8_t lNalHeader = rNal[0];
    int lNalType = lNalHeader & 0x1F;
    int lNalRefIdc = (lNalHeader >> 5) & 0x03;

    // Unsupported data partitioning NALs (DPA/DPB/DPC).
    if (lNalType >= 2 && lNalType <= 4) {
        fail("Unsupported H.264 partitioned slice NAL unit type " + std::to_string(lNalType));
        return false;
    }
    // Unsupported coded slice extension NAL unit (SVC/MVC style extension).
    if (lNalType == 20) {
        fail("Unsupported H.264 extension NAL unit type 20");
        return false;
    }

    switch (lNalType) {
        case 7: // SPS
            if (!parseSps(rNal)) {
                fail("Failed to parse H.264 SPS");
                return false;
            }
            placeNonVcl(rNal, true);
            return true;
        case 8: // PPS
            if (!parsePps(rNal)) {
                fail("Failed to parse H.264 PPS");
                return false;
            }
            placeNonVcl(rNal, true);
            return true;
        case 6:  // SEI
        case 9:  // AUD
            placeNonVcl(rNal, true);
            return true;
        case 10: // End of sequence
        case 11: // End of stream
        case 12: // Filler data: stay in the current AU
            placeNonVcl(rNal, false);
            return true;
        case 1: // Non-IDR slice
        case 5: // IDR slice
            handleVcl(rNal, rpSource, lNalType, lNalRefIdc);
            return true;
        default:
            // Other non-VCL NAL units keep their position relative to VCL NALs.
            placeNonVcl(rNal, true);
            return true;
    }
}

void H264AccessUnitFramer::feed(const uint8_t* pData, size_t lSize) {
    if (lSize == 0) {
        return;
    }

    for (size_t i = 0; i < lSize; ++i) {
        uint8_t lByte = pData[i];

        if (lByte == 0x00) {
            mHeldZeros.push_back({mCurrentSource});
            continue;
        }

        if (lByte == 0x01 && mHeldZeros.size() >= 2) {
            // Start code found. With three or more held zeros use the four byte
            // form; its first zero is held zero index k-3, otherwise index k-2.
            size_t lK = mHeldZeros.size();
            size_t lPrefixFirst = (lK >= 3) ? (lK - 3) : (lK - 2);

            // Extra zeros before the start code prefix complete the previous NAL.
            for (size_t j = 0; j < lPrefixFirst; ++j) {
                mNal.push_back(0x00);
            }
            std::shared_ptr<TimeSource> lPrefixSource = mHeldZeros[lPrefixFirst].pSource;

            bool lHadNal = mNalStarted;
            std::vector<uint8_t> lCompletedNal;
            lCompletedNal.swap(mNal);
            std::shared_ptr<TimeSource> lCompletedSource = mNalSource;

            mHeldZeros.clear();
            mNal.clear();
            mNalStarted = true;
            mNalSource = lPrefixSource;

            if (lHadNal) {
                handleNal(lCompletedNal, lCompletedSource);
            }
            continue;
        }

        // Not a start code: the held zeros are NAL payload bytes.
        for (size_t j = 0; j < mHeldZeros.size(); ++j) {
            mNal.push_back(0x00);
        }
        mHeldZeros.clear();
        mNal.push_back(lByte);
    }
}

void H264AccessUnitFramer::finish() {
    if (mFinished) {
        return;
    }
    mFinished = true;

    for (size_t j = 0; j < mHeldZeros.size(); ++j) {
        mNal.push_back(0x00);
    }
    mHeldZeros.clear();

    if (mNalStarted) {
        std::vector<uint8_t> lLastNal;
        lLastNal.swap(mNal);
        handleNal(lLastNal, mNalSource);
    }

    if (mAuHasVcl) {
        emitAccessUnit();
        mAuHasVcl = false;
    }
}

} // namespace mpegts
