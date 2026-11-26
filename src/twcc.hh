#pragma once

/*
 * Transport-wide congestion control feedback (draft-holmer-rmcat-transport-wide-cc-extensions-01
 * fixed subset used by this task).
 *
 * A TwccState is shared by all media streams that receive RTP over one socket /
 * reception_flow. It owns the single configured RFC 8285 extension id, the
 * transport-sequence arrival ledger and the RTCP feedback packet count.
 *
 * All public methods are thread-safe: the reception thread records arrivals
 * while an application thread builds/sends feedback.
 */

#include "uvgrtp/util.hh"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace uvgrtp::twcc {

/* Maximum number of transport sequence positions retained without feedback. */
static constexpr size_t MAX_RETAINED_SEQUENCES = 8192;

/* Explicit microsecond clock entry used by both real reception and fixed-clock tests. */
int64_t now_microseconds();

/* Parse a one-byte RFC 8285 extension element with id "extension_id" and read the
 * two-byte big-endian transport sequence.
 *
 * Returns true only when exactly one well-formed target element with data length 2
 * exists. Packets without the extension, with an unknown/unsupported profile,
 * malformed/out-of-bounds extension items or several target items yield false. */
bool extract_transport_sequence(const uint8_t* packet, size_t size,
                                uint8_t extension_id, uint16_t& sequence);

class TwccState {
public:
    struct PreparedPacket {
        std::vector<uint8_t> bytes;
        int64_t old_base = 0;
        int64_t new_base = 0;
        uint8_t old_feedback_count = 0;
        uint8_t new_feedback_count = 0;
        bool valid = false;
    };

    TwccState();

    /* Per-media-stream configuration accounting.
     *
     * "wanted" is the id requested by the calling stream; 0 releases it.
     * "current" is the calling stream's currently configured id (0 if none) and is
     * updated on success. The first user sets the shared id, further users must
     * ask for the same id, conflicting ids fail with RTP_INVALID_VALUE.
     * Repeating the current configuration is idempotent. When the last user
     * releases, all shared state is cleared. */
    rtp_error_t configure_stream(uint8_t wanted, uint8_t& current);

    uint8_t extension_id() const;
    unsigned user_count();

    /* Record one transport sequence arrival at an explicit microsecond time.
     * The first valid arrival for a sequence wins (duplicates do not refresh).
     * Returns true if this call created a new ledger entry. */
    bool record_sequence(uint16_t raw_sequence, int64_t arrival_us);

    /* Parse the configured extension from a raw received datagram and record it.
     * Single entry point for real reception; no record is created on any parse issue. */
    bool ingest_datagram(const uint8_t* packet, size_t size, int64_t arrival_us);

    /* Build one RTPFB/FMT=15 packet covering the longest contiguous prefix that
     * fits max_bytes. This does not advance the reported interval or feedback
     * count; call commit() after the packet was sent successfully. */
    rtp_error_t prepare_feedback(size_t max_bytes, uint32_t sender_ssrc,
                                PreparedPacket& prepared) const;
    void commit_prepared(const PreparedPacket& prepared);

    /* Convenience used by fixed-clock wire tests: prepare and commit in one step. */
    rtp_error_t poll_feedback(size_t max_bytes, uint32_t sender_ssrc,
                              std::vector<uint8_t>& packet);

    /* Prepare a packet, hand it to "send" while holding the state lock (so no other
     * report can interleave), and commit only if send returns RTP_OK. The send
     * callback receives the complete RTCP datagram. */
    template <typename SendFn>
    rtp_error_t send_feedback(size_t max_bytes, uint32_t sender_ssrc, SendFn&& send)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        PreparedPacket prepared;
        rtp_error_t ret = prepare_locked(max_bytes, sender_ssrc, prepared);
        if (ret != RTP_OK)
            return ret;

        ret = send(prepared.bytes);
        if (ret == RTP_OK)
            commit_locked(prepared);
        return ret;
    }

    bool has_pending() const;
    uint8_t feedback_count() const;

    /* White-box accessors for the lifecycle/wire tests. */
    int64_t test_next_base() const;
    int64_t test_highest() const;
    size_t test_arrival_count() const;

private:
    rtp_error_t prepare_locked(size_t max_bytes, uint32_t sender_ssrc,
                               PreparedPacket& prepared) const;
    void commit_locked(const PreparedPacket& prepared);
    void reset_locked(uint8_t new_extension_id);
    void trim_locked();

    mutable std::mutex mutex_;

    /* Shared RFC 8285 extension id. Atomic so the reception thread can cheaply
     * check whether TWCC is disabled before taking mutex_. Configuration changes
     * themselves are serialized by mutex_. */
    std::atomic<uint8_t> extension_id_{0};
    unsigned users_ = 0;

    bool have_first_ = false;
    int64_t highest_extended_ = 0;   /* expanded sequence of the highest arrival */
    int64_t next_base_ = 0;          /* first sequence not reported yet */

    /* expanded transport sequence -> first arrival time in microseconds */
    std::map<int64_t, int64_t> arrivals_;

    uint8_t feedback_packet_count_ = 0;
};

} // namespace uvgrtp::twcc
