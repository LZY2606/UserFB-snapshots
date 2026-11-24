#pragma once

// Prefixes used
// m class member
// p pointer (*)
// r reference (&)
// l local scope

#include "common.h"
#include "ts_packet.h"
#include "simple_buffer.h"
#include "h264_access_unit.h"

#include <cstdint>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mpegts {

class MpegTsDemuxer final {
public:

    MpegTsDemuxer() = default;

    ~MpegTsDemuxer() = default;

    uint8_t decode(SimpleBuffer &rIn);

    // Flush the complete zero length PES and the last H.264 access units.
    // Calling finish() more than once does not repeat any callbacks.
    void finish();

    std::function<void(const EsFrame& pEs)> esOutCallback = nullptr;

    // Optional H.264 access unit output. When set, PMT PIDs with stream_type
    // 0x1b are framed into H264AccessUnits; all other streams keep using
    // esOutCallback. esOutCallback itself keeps its original behavior.
    std::function<void(const H264AccessUnit& rAccessUnit)> h264OutCallback = nullptr;
    std::function<void(uint16_t lPid, const std::string& rMessage)> h264ErrorCallback = nullptr;

    std::function<void(uint64_t lPcr)> pcrOutCallback = nullptr;
    std::function<void(LogLevel level, const std::string&)> streamInfoCallback = nullptr;
    std::function<void(uint16_t pid, uint8_t expectedCC, uint8_t actualCC)> ccErrorCallback = nullptr;

    // stream, pid
    const int kPatId = 0x0;
    std::map<uint8_t, int> mStreamPidMap;
    int mPmtId = 0;

    // PAT
    PATHeader mPatHeader;
    bool mPatIsValid = false;

    // PMT
    PMTHeader mPmtHeader;
    bool mPmtIsValid = false;

private:
    // Check that the CC is incremented according to spec. ccErrorCallback is called on error.
    void checkContinuityCounter(const TsHeader &rTsHeader, uint8_t discontinuityIndicator);

    // Check that the current position in buffer is pointing to a sync byte and if the buffer is larger than a TS packet,
    // the function checks that the first byte after the TS packet is also a sync byte. Returns true if the buffer is in
    // sync with the TS packet boundaries, otherwise false.
    static bool checkSync(SimpleBuffer& rIn);

    // Parse a TS packet, the input buffer should be aligned to the start of a TS packet and the buffer should be at least
    // 188 bytes long.
    void parseTsPacket(SimpleBuffer& rSb);

    // Parse the adaptation field, will look for PCR values and pass to the pcrOutCallback. Returns true if parsing was
    // successful, otherwise false. The randomAccessIndicator is set to 1 if the packet contains a random access point.
    bool parseAdaptationField(const TsHeader& rTsHeader, SimpleBuffer& rSb, size_t bytesLeftInPacket, uint8_t& randomAccessIndicator);

    // Parse the PAT table
    bool parsePat(const TsHeader& rTsHeader, SimpleBuffer& rSb);

    // Parse the PMT table
    bool parsePmt(const TsHeader& rTsHeader, SimpleBuffer& rSb);

    // Parse the PES data and create EsFrames. The EsFrames are passed to the esOutCallback.
    bool parsePes(const TsHeader& rTsHeader, uint8_t randomAccessIndicator, SimpleBuffer& rSb, size_t payloadSize);

    // Parse the PCR value from the adaptation field and pass to the pcrOutCallback.
    void parsePcr(const AdaptationFieldHeader& rAdaptionField, SimpleBuffer& rSb) const;

    // Check if the PCR value should be parsed based on the PID and the pcrOutCallback.
    bool shouldParsePcr(uint16_t pid) const;

    // Elementary stream data frames
    std::map<int, EsFrame> mEsFrames;
    int mPcrId = 0;
    SimpleBuffer mRestData;

    // H.264 access unit framers, one per stream_type 0x1b PID.
    H264AccessUnitFramer& getH264Framer(uint16_t lPid);
    std::map<uint16_t, std::unique_ptr<H264AccessUnitFramer>> mH264Framers;
    bool mFinished = false;

    // Continuity counters. Map from PID to current CC value.
    std::unordered_map<uint16_t, uint8_t> mCCs;

    size_t mBytesDroppedToRecoverSync = 0; // Keep track of how many bytes were dropped to recover sync
};

}
