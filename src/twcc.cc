#include "twcc.hh"

#include "debug.hh"

#include <cstring>
#include <vector>

/* RTCP TWCC feedback packet (RTPFB/FMT=15) fixed header size:
 * RTCP header (4) + sender SSRC (4) + media SSRC (4) + base sequence (2) +
 * status count (2) + reference time (3) + feedback count (1) */
constexpr size_t TWCC_FB_HEADER_SIZE = 20;

/* Statuses are encoded as 2-bit symbol vector chunks, each holding 7 symbols */
constexpr size_t TWCC_SYMBOLS_PER_CHUNK = 7;

uvgrtp::twcc::twcc() :
    ext_id_(0),
    users_({}),
    have_seq_(false),
    window_start_(0),
    max_seq_(0),
    arrivals_({}),
    reported_end_(0),
    feedback_count_(0)
{}

rtp_error_t uvgrtp::twcc::configure(uint8_t extension_id, uintptr_t user)
{
    std::lock_guard<std::mutex> lg(mutex_);

    if (extension_id == 0) {
        users_.erase(user);

        /* Last user released the shared state: clear everything so a fresh
         * configuration starts from a clean slate */
        if (users_.empty()) {
            ext_id_ = 0;
            reset_ledger();
        }
        return RTP_OK;
    }

    /* one-byte header extension IDs 1-14 are supported */
    if (extension_id > 14) {
        UVG_LOG_ERROR("Invalid TWCC extension ID %u", extension_id);
        return RTP_INVALID_VALUE;
    }

    /* The shared receive stream can only have one extension ID */
    if (ext_id_ != 0 && ext_id_ != extension_id) {
        UVG_LOG_ERROR("TWCC already configured with extension ID %u", ext_id_);
        return RTP_INVALID_VALUE;
    }

    ext_id_ = extension_id;
    users_.insert(user);
    return RTP_OK;
}

bool uvgrtp::twcc::has_users() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return !users_.empty();
}

void uvgrtp::twcc::reset_ledger()
{
    have_seq_ = false;
    window_start_ = 0;
    max_seq_ = 0;
    arrivals_.clear();
    reported_end_ = 0;
    feedback_count_ = 0;
}

void uvgrtp::twcc::record_sequence(uint16_t tseq, uint64_t arrival_us)
{
    if (!have_seq_) {
        /* The first valid transport sequence establishes the origin */
        have_seq_ = true;
        window_start_ = tseq;
        max_seq_ = tseq;
        reported_end_ = tseq;
        arrivals_.push_back((int64_t)arrival_us);
        return;
    }

    /* Unwrap with the shortest signed 16-bit distance from the highest
     * sequence seen so far. The working range stays below half of the
     * sequence space because at most MAX_TRACKED sequences are retained. */
    int16_t delta = (int16_t)((uint16_t)tseq - (uint16_t)(max_seq_ & 0xffff));
    int64_t useq = max_seq_ + delta;

    if (useq > max_seq_) {
        max_seq_ = useq;
    }

    if (useq < window_start_) {
        /* Older than the retained window (or before the origin): cannot be
         * recorded and will never be reported */
        return;
    }

    size_t index = (size_t)(useq - window_start_);
    if (index >= arrivals_.size()) {
        arrivals_.resize(index + 1, -1);
    }

    /* Only the first valid arrival of a packet takes effect */
    if (arrivals_[index] != -1) {
        return;
    }

    arrivals_[index] = (int64_t)arrival_us;

    /* Keep memory bounded when feedback is not generated for a long time:
     * retain only the newest MAX_TRACKED sequences. Dropped ranges are never
     * reported. */
    while ((int64_t)arrivals_.size() > (int64_t)MAX_TRACKED) {
        arrivals_.pop_front();
        ++window_start_;
    }

    if (reported_end_ < window_start_) {
        reported_end_ = window_start_;
    }
}

void uvgrtp::twcc::record_packet(const uint8_t* packet, size_t size, uint64_t arrival_us)
{
    std::lock_guard<std::mutex> lg(mutex_);

    if (ext_id_ == 0 || packet == nullptr) {
        return;
    }

    if (size < 12 || (packet[0] >> 6) != 0x2) {
        return;
    }

    size_t header_len = 12 + 4 * (size_t)(packet[0] & 0x0f);

    /* No header extension -> no reception record */
    if (!(packet[0] & 0x10) || size < header_len + 4) {
        return;
    }

    uint16_t profile = (uint16_t)((packet[header_len] << 8) | packet[header_len + 1]);
    if (profile != 0xbede) {
        return;
    }

    size_t ext_len = 4 * (size_t)((packet[header_len + 2] << 8) | packet[header_len + 3]);
    if (size < header_len + 4 + ext_len) {
        return;
    }

    /* Scan the one-byte header extension elements: ID 0 is single-byte
     * padding, ID 15 stops the scan, unknown IDs are skipped. The target ID
     * must appear exactly once with a two-byte network-order value. */
    size_t pos = header_len + 4;
    const size_t end = pos + ext_len;
    unsigned found = 0;
    uint16_t tseq = 0;

    while (pos < end) {
        uint8_t id = packet[pos] >> 4;

        if (id == 15) {
            break;
        }
        if (id == 0) {
            pos += 1;
            continue;
        }

        size_t len = (size_t)(packet[pos] & 0x0f) + 1;

        /* An element running out of the extension bounds corrupts this packet */
        if (pos + 1 + len > end) {
            return;
        }

        if (id == ext_id_) {
            if (len != 2) {
                return;
            }
            ++found;
            tseq = (uint16_t)((packet[pos + 1] << 8) | packet[pos + 2]);
        }

        pos += 1 + len;
    }

    /* Zero or multiple target items: no valid record for this packet */
    if (found != 1) {
        return;
    }

    record_sequence(tseq, arrival_us);
}

rtp_error_t uvgrtp::twcc::send_feedback(uint32_t sender_ssrc, size_t max_bytes,
    const std::function<rtp_error_t(uint8_t* packet, size_t size)>& send)
{
    std::lock_guard<std::mutex> lg(mutex_);

    /* Nothing to report: not configured, no records, or everything reported */
    if (ext_id_ == 0 || !have_seq_ || reported_end_ > max_seq_) {
        return RTP_NOT_FOUND;
    }

    const int64_t report_begin = reported_end_;
    const int64_t report_end = max_seq_; /* inclusive */

    /* The reference time is based on the first received sequence of the
     * report. max_seq_ is always a received sequence, so one exists. */
    int64_t first_received = report_end;
    for (int64_t s = report_begin; s <= report_end; ++s) {
        if (arrivals_[(size_t)(s - window_start_)] >= 0) {
            first_received = s;
            break;
        }
    }

    const uint64_t first_arrival = (uint64_t)arrivals_[(size_t)(first_received - window_start_)];
    const uint64_t ref_time_us = (first_arrival / 64000) * 64000;

    /* Compute the status symbol and receive delta for every sequence of the
     * range. Deltas are in 250 us ticks: the first received packet is relative
     * to the reference time, the rest to the previously encoded received
     * packet. A delta that does not fit into a signed 16-bit value ends the
     * report before that packet; the next report re-establishes a reference
     * time for the remaining range. */
    std::vector<uint8_t> symbols;
    std::vector<int32_t> deltas; /* one entry per received sequence, in order */
    symbols.reserve((size_t)(report_end - report_begin + 1));

    int64_t prev_ticks = -1;
    int64_t cut = report_begin - 1; /* last sequence included in this report */

    for (int64_t s = report_begin; s <= report_end; ++s) {
        int64_t arrival = arrivals_[(size_t)(s - window_start_)];

        if (arrival < 0) {
            symbols.push_back(0); /* lost */
            cut = s;
            continue;
        }

        int64_t ticks = arrival / 250;
        int64_t delta = (prev_ticks < 0) ? ticks - (int64_t)(ref_time_us / 250) : ticks - prev_ticks;

        if (delta >= 0 && delta <= 255) {
            symbols.push_back(1); /* small delta, one unsigned byte */
        }
        else if (delta >= -32768 && delta <= 32767) {
            symbols.push_back(2); /* large delta, signed 16 bits */
        }
        else {
            break; /* delta out of range: end the report here */
        }

        deltas.push_back((int32_t)delta);
        prev_ticks = ticks;
        cut = s;
    }

    /* Pack the statuses into 2-bit symbol vector chunks of up to 7 symbols and
     * include the longest continuous prefix that fits into max_bytes. A
     * sequence is included only if its whole status chunk and the
     * corresponding receive deltas fit. */
    size_t included = 0;
    size_t chunk_bytes = 0;
    size_t delta_bytes = 0;
    bool received_included = false;

    for (size_t cstart = 0; cstart < symbols.size(); cstart += TWCC_SYMBOLS_PER_CHUNK) {
        size_t cend = cstart + TWCC_SYMBOLS_PER_CHUNK;
        if (cend > symbols.size()) {
            cend = symbols.size();
        }

        size_t chunk_delta_bytes = 0;
        bool chunk_has_received = false;
        for (size_t i = cstart; i < cend; ++i) {
            if (symbols[i] != 0) {
                chunk_delta_bytes += (symbols[i] == 1) ? 1 : 2;
                chunk_has_received = true;
            }
        }

        size_t candidate = TWCC_FB_HEADER_SIZE + chunk_bytes + 2 + delta_bytes + chunk_delta_bytes;
        size_t padded = (candidate + 3) & ~(size_t)3;
        if (padded > max_bytes) {
            break;
        }

        chunk_bytes += 2;
        delta_bytes += chunk_delta_bytes;
        included = cend;
        received_included = received_included || chunk_has_received;
    }

    /* A report without any received packet has no reference time, and a report
     * that cannot hold even the first status chunk is over budget */
    if (included == 0 || !received_included) {
        return RTP_INVALID_VALUE;
    }

    const size_t total = TWCC_FB_HEADER_SIZE + chunk_bytes + delta_bytes;
    const size_t padded = (total + 3) & ~(size_t)3;

    std::vector<uint8_t> pkt(padded, 0);

    pkt[0] = (2 << 6) | 15; /* V=2, P=0, FMT=15 */
    pkt[1] = 205;           /* PT=RTPFB */
    uint16_t length = (uint16_t)(padded / 4 - 1);
    pkt[2] = (uint8_t)(length >> 8);
    pkt[3] = (uint8_t)(length & 0xff);
    pkt[4] = (uint8_t)(sender_ssrc >> 24);
    pkt[5] = (uint8_t)(sender_ssrc >> 16);
    pkt[6] = (uint8_t)(sender_ssrc >> 8);
    pkt[7] = (uint8_t)(sender_ssrc & 0xff);
    /* media SSRC is zero (bytes 8-11) */
    uint16_t base_seq = (uint16_t)(report_begin & 0xffff);
    pkt[12] = (uint8_t)(base_seq >> 8);
    pkt[13] = (uint8_t)(base_seq & 0xff);
    pkt[14] = (uint8_t)(included >> 8);
    pkt[15] = (uint8_t)(included & 0xff);
    uint32_t ref_time = (uint32_t)((ref_time_us / 64000) & 0xffffff);
    pkt[16] = (uint8_t)(ref_time >> 16);
    pkt[17] = (uint8_t)(ref_time >> 8);
    pkt[18] = (uint8_t)(ref_time & 0xff);
    pkt[19] = feedback_count_;

    size_t pos = TWCC_FB_HEADER_SIZE;

    for (size_t cstart = 0; cstart < included; cstart += TWCC_SYMBOLS_PER_CHUNK) {
        size_t cend = cstart + TWCC_SYMBOLS_PER_CHUNK;
        if (cend > included) {
            cend = included;
        }

        /* Vector chunk with 2-bit symbols; unused trailing symbols are zero */
        uint16_t chunk = 0xc000;
        for (size_t i = cstart; i < cend; ++i) {
            chunk |= (uint16_t)(symbols[i] << (12 - 2 * (i - cstart)));
        }
        pkt[pos++] = (uint8_t)(chunk >> 8);
        pkt[pos++] = (uint8_t)(chunk & 0xff);
    }

    size_t delta_index = 0;
    for (size_t i = 0; i < included; ++i) {
        if (symbols[i] == 0) {
            continue; /* lost packets have no receive delta */
        }
        int32_t delta = deltas[delta_index++];
        if (symbols[i] == 1) {
            pkt[pos++] = (uint8_t)delta;
        }
        else {
            pkt[pos++] = (uint8_t)(((uint16_t)(int16_t)delta) >> 8);
            pkt[pos++] = (uint8_t)(delta & 0xff);
        }
    }

    rtp_error_t ret = send(pkt.data(), pkt.size());

    /* Only a successful send advances the reported range and feedback count */
    if (ret != RTP_OK) {
        return ret;
    }

    reported_end_ = report_begin + (int64_t)included;
    feedback_count_ = (uint8_t)((feedback_count_ + 1) & 0xff);
    return RTP_OK;
}
