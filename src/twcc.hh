#pragma once

#include "uvgrtp/util.hh"

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <set>

namespace uvgrtp {

    /* Receiver-side transport-wide congestion control (TWCC) state.
     *
     * One twcc object is shared by all media streams that receive from the same
     * socket (it is owned by the reception_flow of that socket). It tracks the
     * transport-wide sequence numbers carried in the RFC 8285 one-byte RTP header
     * extension and generates RTCP transport-wide feedback packets
     * (RTPFB/FMT=15, PT=205) on demand.
     *
     * The transport sequence space belongs to the whole transport, not to a
     * single media SSRC, so all packets arriving on the shared socket are
     * recorded into the same ledger regardless of their SSRC. The media's own
     * RTP sequence numbers, timestamps and framing are not affected.
     *
     * The wire format is a fixed subset of
     * draft-holmer-rmcat-transport-wide-cc-extensions-01 (see
     * task_support/twcc-wire.md). Only plaintext RTP/RTCP is supported.
     *
     * All public methods are thread-safe: the packet processing thread records
     * arrivals while the application thread generates feedback concurrently. */
    class twcc {
    public:
        twcc();

        /* Maximum number of transport sequence numbers kept in the arrival
         * ledger. When the receive span exceeds this, the oldest sequences are
         * dropped and will never be reported. */
        static constexpr size_t MAX_TRACKED = 8192;

        /* Configure the header extension ID (1-14) carrying the two-byte
         * transport-wide sequence number. "user" is an opaque token identifying
         * one media stream.
         *
         * The shared receive stream can only have one extension ID: configuring
         * with the ID that is already in use is a no-op (idempotent), a
         * conflicting ID is an error. ID 0 releases the configuration of this
         * stream; when the last user releases, all shared state (extension ID,
         * arrival ledger and feedback packet count) is cleared.
         *
         * Return RTP_OK on success, RTP_INVALID_VALUE on a conflicting or
         * invalid extension ID */
        rtp_error_t configure(uint8_t extension_id, uintptr_t user);

        /* Whether at least one stream is currently configured */
        bool has_users() const;

        /* Record the arrival of one RTP packet. If the packet carries exactly
         * one valid transport-wide sequence number with the configured extension
         * ID in an RFC 8285 one-byte header extension (profile 0xBEDE), the
         * arrival is recorded. Packets without the extension, with unknown IDs
         * only, or with corrupt extension items do not create reception records.
         *
         * "arrival_us" is the arrival time in microseconds and must be sampled
         * at the packet receive point, not at a later processing point. This is
         * the single recording entry used both by the live receive path and by
         * tests with a fixed clock. */
        void record_packet(const uint8_t* packet, size_t size, uint64_t arrival_us);

        /* Build one RTCP TWCC feedback packet covering the oldest unreported
         * continuous prefix of the sequence space that fits into "max_bytes"
         * (the whole RTCP packet including padding) and hand it to "send".
         * The reported range and the feedback packet count are advanced only if
         * "send" returns RTP_OK. If the remaining range does not fit, the next
         * call continues where this one left off.
         *
         * Return RTP_OK on success
         * Return RTP_NOT_FOUND if TWCC is not configured or there is nothing to report
         * Return RTP_INVALID_VALUE if max_bytes cannot hold even the first status chunk
         * Otherwise the error returned by "send" is propagated and no state changes */
        rtp_error_t send_feedback(uint32_t sender_ssrc, size_t max_bytes,
            const std::function<rtp_error_t(uint8_t* packet, size_t size)>& send);

    private:
        /* Reset the arrival ledger and feedback packet count */
        void reset_ledger();

        /* Record one valid transport sequence number, caller holds mutex_ */
        void record_sequence(uint16_t tseq, uint64_t arrival_us);

        mutable std::mutex mutex_;

        /* Configured one-byte header extension ID, 0 = disabled */
        uint8_t ext_id_;

        /* Streams that have configured this shared state */
        std::set<uintptr_t> users_;

        /* Arrival ledger: arrivals_[i] is the arrival time (us) of the
         * unwrapped sequence window_start_ + i, -1 means not received.
         * The first valid transport sequence establishes the origin; later
         * sequences are unwrapped with the shortest signed 16-bit distance
         * from the highest sequence seen so far. */
        bool have_seq_;
        int64_t window_start_;
        int64_t max_seq_;
        std::deque<int64_t> arrivals_;

        /* Next unreported unwrapped sequence. Ranges below this are never
         * reported again, so late arrivals for them are only recorded. */
        int64_t reported_end_;

        /* Feedback packet count of the shared receive stream, wraps at 8 bits */
        uint8_t feedback_count_;
    };
}
