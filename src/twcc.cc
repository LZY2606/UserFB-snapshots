#include "twcc.hh"

#include <arpa/inet.h>

#include <chrono>
#include <cstring>

namespace uvgrtp::twcc {

int64_t now_microseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static int64_t floor_div(int64_t value, int64_t divisor)
{
    int64_t q = value / divisor;
    int64_t r = value % divisor;
    if (r != 0 && ((r < 0) != (divisor < 0)))
        --q;
    return q;
}

bool extract_transport_sequence(const uint8_t* packet, size_t size,
                               uint8_t extension_id, uint16_t& sequence)
{
    if (extension_id < 1 || extension_id > 14 || packet == nullptr)
        return false;

    /* RTP fixed header is 12 bytes; version 2 and extension bit must be set. */
    if (size < 16 || (packet[0] & 0xC0) != 0x80 || (packet[0] & 0x10) == 0)
        return false;

    /* With RCE_RTCP_MUX the same socket also sees RTCP datagrams. Their payload
     * type is in the RTCP range; never account those as RTP arrivals. */
    if (packet[1] >= 200 && packet[1] <= 211)
        return false;

    unsigned csrc_count = packet[0] & 0x0F;
    size_t ext_offset = 12 + static_cast<size_t>(4) * csrc_count;
    if (ext_offset + 4 > size)
        return false;

    uint16_t profile = static_cast<uint16_t>((packet[ext_offset] << 8) | packet[ext_offset + 1]);
    if (profile != 0xBEDE) /* RFC 8285 one-byte header form only */
        return false;

    uint16_t ext_words = static_cast<uint16_t>((packet[ext_offset + 2] << 8) | packet[ext_offset + 3]);
    size_t data_start = ext_offset + 4;
    size_t data_end = data_start + static_cast<size_t>(4) * ext_words;
    if (data_end > size)
        return false;

    size_t pos = data_start;
    int target_count = 0;
    uint16_t target_value = 0;

    while (pos < data_end) {
        uint8_t header = packet[pos];
        uint8_t id = header >> 4;
        size_t data_len = static_cast<size_t>(header & 0x0F) + 1;

        if (id == 0) {
            /* RFC 8285 padding element: a single byte. */
            ++pos;
            continue;
        }
        if (id == 15) {
            /* Termination marker. */
            break;
        }

        /* The whole element (header byte + data) must lie inside the extension.
         * An out-of-bounds item invalidates accounting for this packet. */
        if (pos + 1 + data_len > data_end)
            return false;

        if (id == extension_id) {
            /* The fixed TWCC subset only uses a two-byte sequence. */
            if (data_len != 2)
                return false;
            /* Several target items in one extension -> no valid record. */
            if (target_count > 0)
                return false;
            target_value = static_cast<uint16_t>((packet[pos + 1] << 8) | packet[pos + 2]);
            ++target_count;
        }

        pos += 1 + data_len;
    }

    if (target_count != 1)
        return false;

    sequence = target_value;
    return true;
}

TwccState::TwccState() = default;

rtp_error_t TwccState::configure_stream(uint8_t wanted, uint8_t& current)
{
    if (wanted != 0 && (wanted < 1 || wanted > 14))
        return RTP_INVALID_VALUE;

    std::lock_guard<std::mutex> guard(mutex_);

    /* Idempotent repetition of the current configuration (including "already released"). */
    if (wanted == current)
        return RTP_OK;

    if (wanted == 0) {
        if (current != 0 && users_ > 0)
            --users_;
        current = 0;
        if (users_ == 0)
            reset_locked(0);
        return RTP_OK;
    }

    /* A configured stream cannot switch to another id without releasing first. */
    if (current != 0)
        return RTP_INVALID_VALUE;

    /* Another stream/writer owns this socket with a different extension id. */
    if (users_ > 0 && extension_id_.load() != wanted)
        return RTP_INVALID_VALUE;

    if (users_ == 0)
        reset_locked(wanted);

    extension_id_.store(wanted);
    ++users_;
    current = wanted;
    return RTP_OK;
}

uint8_t TwccState::extension_id() const
{
    return extension_id_.load();
}

unsigned TwccState::user_count()
{
    std::lock_guard<std::mutex> guard(mutex_);
    return users_;
}

void TwccState::reset_locked(uint8_t new_extension_id)
{
    extension_id_.store(new_extension_id);
    have_first_ = false;
    highest_extended_ = 0;
    next_base_ = 0;
    arrivals_.clear();
    feedback_packet_count_ = 0;
}

void TwccState::trim_locked()
{
    /* Keep at most MAX_RETAINED_SEQUENCES sequence positions ending at the highest. */
    int64_t oldest_kept = highest_extended_ - static_cast<int64_t>(MAX_RETAINED_SEQUENCES - 1);

    if (next_base_ < oldest_kept)
        next_base_ = oldest_kept;

    while (!arrivals_.empty() && arrivals_.begin()->first < next_base_)
        arrivals_.erase(arrivals_.begin());
}

bool TwccState::record_sequence(uint16_t raw_sequence, int64_t arrival_us)
{
    std::lock_guard<std::mutex> guard(mutex_);

    int64_t extended;
    if (!have_first_) {
        have_first_ = true;
        extended = raw_sequence;
        highest_extended_ = extended;
        next_base_ = extended;
    } else {
        /* Expand the 16-bit value using the shortest signed distance from the
         * highest expanded sequence. The working interval is below half a circle. */
        uint16_t highest_low = static_cast<uint16_t>(highest_extended_ & 0xFFFFu);
        int distance = static_cast<int>(raw_sequence) - static_cast<int>(highest_low);
        if (distance > 32767)
            distance -= 65536;
        else if (distance < -32768)
            distance += 65536;

        extended = highest_extended_ + distance;
        if (extended > highest_extended_)
            highest_extended_ = extended;
    }

    trim_locked();

    /* Anything at or before the report frontier belongs to an already reported or
     * discarded range: late/duplicate arrivals there must not reopen a report. */
    if (extended < next_base_)
        return false;

    /* First valid arrival for a sequence wins; duplicates do not refresh time. */
    auto result = arrivals_.emplace(extended, arrival_us);
    return result.second;
}

bool TwccState::ingest_datagram(const uint8_t* packet, size_t size, int64_t arrival_us)
{
    uint8_t id = extension_id_.load();
    if (id == 0)
        return false;

    uint16_t sequence;
    if (!extract_transport_sequence(packet, size, id, sequence))
        return false;

    return record_sequence(sequence, arrival_us);
}

bool TwccState::has_pending() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return have_first_ && next_base_ <= highest_extended_;
}

uint8_t TwccState::feedback_count() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return feedback_packet_count_;
}

int64_t TwccState::test_next_base() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return next_base_;
}

int64_t TwccState::test_highest() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return highest_extended_;
}

size_t TwccState::test_arrival_count() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return arrivals_.size();
}

namespace {

/* Delta classification shared by the sizing pass and the encoding pass. */
struct DeltaState {
    bool have_received = false;
    int64_t previous_tick = 0;
    int64_t reference_units = 0;
    uint32_t reference_time_24 = 0;

    /* Returns status symbol 1/2 for a received packet, 0 when its delta cannot be
     * encoded. Fills delta bytes (1 or 2). Does not mutate state. */
    int classify(int64_t arrival_us, uint8_t delta[2], size_t& delta_len)
    {
        int64_t tick = floor_div(arrival_us, 250);

        if (!have_received) {
            reference_units = floor_div(arrival_us, 64000);
            reference_time_24 = static_cast<uint32_t>(reference_units & 0xFFFFFF);
            int64_t relative = tick - reference_units * 256; /* 64000/250 = 256 */

            if (relative >= 0 && relative <= 255) {
                delta[0] = static_cast<uint8_t>(relative);
                delta_len = 1;
                return 1;
            }
            if (relative >= -32768 && relative <= 32767) {
                uint16_t v = htons(static_cast<uint16_t>(static_cast<int16_t>(relative)));
                std::memcpy(delta, &v, 2);
                delta_len = 2;
                return 2;
            }
            delta_len = 0;
            return 0;
        }

        int64_t d = tick - previous_tick;
        if (d >= 0 && d <= 255) {
            delta[0] = static_cast<uint8_t>(d);
            delta_len = 1;
            return 1;
        }
        if (d >= -32768 && d <= 32767) {
            uint16_t v = htons(static_cast<uint16_t>(static_cast<int16_t>(d)));
            std::memcpy(delta, &v, 2);
            delta_len = 2;
            return 2;
        }
        delta_len = 0;
        return 0;
    }

    void accept(int64_t arrival_us)
    {
        previous_tick = floor_div(arrival_us, 250);
        have_received = true;
    }
};

} // namespace

rtp_error_t TwccState::prepare_locked(size_t max_bytes, uint32_t sender_ssrc,
                                      PreparedPacket& prepared) const
{
    prepared = PreparedPacket{};

    if (!have_first_ || next_base_ > highest_extended_)
        return RTP_NOT_FOUND;

    const int64_t base = next_base_;

    /* ---- sizing pass: longest contiguous prefix that fits ---- */
    DeltaState sizer;
    size_t delta_bytes = 0;
    size_t best_count = 0;
    unsigned received_total = 0;
    unsigned received_in_best = 0;

    for (int64_t seq = base; seq <= highest_extended_; ++seq) {
        auto it = arrivals_.find(seq);
        int symbol = 0;
        size_t one_delta_len = 0;
        uint8_t tmp_delta[2] = {0, 0};

        if (it != arrivals_.end()) {
            symbol = sizer.classify(it->second, tmp_delta, one_delta_len);
            if (symbol == 0)
                break; /* delta out of representable range: end before this seq */
            ++received_total;
        }

        size_t status_count = static_cast<size_t>(seq - base + 1);
        size_t chunk_bytes = 2 * ((status_count + 6) / 7); /* 2-bit vector chunks */
        size_t raw_size = 20 + chunk_bytes + delta_bytes + one_delta_len;
        size_t padded_size = (raw_size + 3) & ~static_cast<size_t>(3);

        if (padded_size > max_bytes)
            break; /* later prefixes only grow */

        if (it != arrivals_.end()) {
            sizer.accept(it->second);
            delta_bytes += one_delta_len;
        }
        best_count = status_count;
        received_in_best = received_total;
    }

    /* A report needs a reference time, which comes from a received packet, and at
     * least one whole 16-bit status chunk plus its delta bytes. */
    if (best_count == 0 || received_in_best == 0)
        return RTP_INVALID_VALUE;

    /* ---- encoding pass over the chosen prefix ---- */
    std::vector<uint16_t> chunks;
    std::vector<uint8_t> deltas;
    deltas.reserve(delta_bytes);

    DeltaState encoder;
    uint32_t reference_time = 0;

    for (size_t i = 0; i < best_count; ++i) {
        int64_t seq = base + static_cast<int64_t>(i);
        uint8_t symbol = 0;

        auto it = arrivals_.find(seq);
        if (it != arrivals_.end()) {
            uint8_t delta[2] = {0, 0};
            size_t delta_len = 0;
            int s = encoder.classify(it->second, delta, delta_len);
            /* The sizing pass accepted this sequence, so encoding must succeed. */
            if (s == 0)
                return RTP_GENERIC_ERROR;
            symbol = static_cast<uint8_t>(s);
            if (delta_len == 1)
                deltas.push_back(delta[0]);
            else
                deltas.insert(deltas.end(), delta, delta + 2);
            encoder.accept(it->second);
        }

        size_t group = i / 7;
        if (i % 7 == 0) {
            /* Vector chunk, 2-bit symbols: bit15=1 vector, bit14=1 two-bit. */
            chunks.push_back(0xC000);
        }
        chunks[group] |= static_cast<uint16_t>(symbol) << (12 - 2 * (i % 7));
    }

    /* Once more make sure the first received packet supplied the reference time. */
    reference_time = encoder.reference_time_24;

    size_t total_raw = 20 + chunks.size() * 2 + deltas.size();
    size_t padding = (4 - (total_raw % 4)) % 4;
    size_t total_size = total_raw + padding;

    std::vector<uint8_t> pkt(total_size, 0);
    pkt[0] = 0x8F; /* V=2, P=0, FMT=15 */
    pkt[1] = 205;  /* PT = transport layer feedback (RTPFB/PSFB 205) */
    uint16_t length_field = static_cast<uint16_t>(total_size / 4 - 1);
    pkt[2] = static_cast<uint8_t>(length_field >> 8);
    pkt[3] = static_cast<uint8_t>(length_field & 0xFF);

    uint32_t ssrc_n = htonl(sender_ssrc);
    std::memcpy(&pkt[4], &ssrc_n, 4);
    /* bytes 8..11 media SSRC = 0 */

    uint16_t base_raw = htons(static_cast<uint16_t>(base & 0xFFFFu));
    std::memcpy(&pkt[12], &base_raw, 2);
    uint16_t count_raw = htons(static_cast<uint16_t>(best_count));
    std::memcpy(&pkt[14], &count_raw, 2);

    pkt[16] = static_cast<uint8_t>((reference_time >> 16) & 0xFF);
    pkt[17] = static_cast<uint8_t>((reference_time >> 8) & 0xFF);
    pkt[18] = static_cast<uint8_t>(reference_time & 0xFF);
    pkt[19] = feedback_packet_count_;

    size_t pos = 20;
    for (uint16_t chunk : chunks) {
        uint16_t cn = htons(chunk);
        std::memcpy(&pkt[pos], &cn, 2);
        pos += 2;
    }
    if (!deltas.empty()) {
        std::memcpy(&pkt[pos], deltas.data(), deltas.size());
        pos += deltas.size();
    }
    /* Remaining bytes are zero padding. */

    prepared.bytes = std::move(pkt);
    prepared.old_base = next_base_;
    prepared.new_base = base + static_cast<int64_t>(best_count);
    prepared.old_feedback_count = feedback_packet_count_;
    prepared.new_feedback_count = static_cast<uint8_t>((feedback_packet_count_ + 1) & 0xFF);
    prepared.valid = true;
    return RTP_OK;
}

void TwccState::commit_locked(const PreparedPacket& prepared)
{
    if (!prepared.valid)
        return;
    /* Only advance if nobody else moved the frontier underneath us. send_feedback
     * holds mutex_ across send/commit, this also guards standalone commits. */
    if (next_base_ != prepared.old_base)
        return;

    next_base_ = prepared.new_base;
    feedback_packet_count_ = prepared.new_feedback_count;

    while (!arrivals_.empty() && arrivals_.begin()->first < next_base_)
        arrivals_.erase(arrivals_.begin());
}

void TwccState::commit_prepared(const PreparedPacket& prepared)
{
    std::lock_guard<std::mutex> guard(mutex_);
    commit_locked(prepared);
}

rtp_error_t TwccState::prepare_feedback(size_t max_bytes, uint32_t sender_ssrc,
                                        PreparedPacket& prepared) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return prepare_locked(max_bytes, sender_ssrc, prepared);
}

rtp_error_t TwccState::poll_feedback(size_t max_bytes, uint32_t sender_ssrc,
                                     std::vector<uint8_t>& packet)
{
    std::lock_guard<std::mutex> guard(mutex_);
    PreparedPacket prepared;
    rtp_error_t ret = prepare_locked(max_bytes, sender_ssrc, prepared);
    if (ret != RTP_OK)
        return ret;
    packet = prepared.bytes;
    commit_locked(prepared);
    return RTP_OK;
}

} // namespace uvgrtp::twcc
