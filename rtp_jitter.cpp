/******************************************************************************
*   Copyright (c) 2013-2015 thundernet development group, inc.
*   http://thundernet.com
*
*   Permission is hereby granted, free of charge, to any person obtaining a
*   copy of this software and associated documentation files (the "Software"),
*   to deal in the Software without restriction, including without limitation
*   the rights to use, copy, modify, merge, publish, distribute, sublicense,
*   and/or sell copies of the Software, and to permit persons to whom the
*   Software is furnished to do so, subject to the following conditions:
*
*   1. The above copyright notice and this permission notice shall be included
*      in all copies or substantial portions of the Software.
*
*   2. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
*      OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
*      MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
*      IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
*      CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
*      TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
*      SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
*   -----
*
*   jitter buffer for RTP packets based on C++11 and STL.
*
*   See rtp_jitter.h for requirements and design discussion.
*
******************************************************************************/

#include "rtp_jitter.h"
#include <iostream>
#include <cmath>
#include <cstdint>
#include <arpa/inet.h>

using namespace std;


/******************************************************************************
*   Just call _init to get initialization "things" done.
*
*   Returns n/a
******************************************************************************/
RTPJitter::RTPJitter(const unsigned depth, const uint32 sample_rate /* = 8000 */)
{
    init(depth, sample_rate);
}



/******************************************************************************
*   Need to clean out the memory we own in the buffer (unless we're expecting
*   the caller to be responsible for that).
*
*   Returns n/a
******************************************************************************/
RTPJitter::~RTPJitter()
{
    rscoped_lock lock(_mutex);

    _buffer.clear();
}



/******************************************************************************
*   Ensure a new empty buffer and associated parameters.  Reset buffer stats.
*
*   Returns none
******************************************************************************/
void RTPJitter::init(const unsigned depth, const uint32 sample_rate /* = 8000 */)
{
    if (!_buffer.empty()) {
        _clean_buffer();
    }

    _first_buf_sequence = 0;
    _last_buf_sequence = 0;
    _last_pop_sequence = 0;
    set_depth(depth);
    _payload_sample_rate = sample_rate;

    _buffering = true;
    _buffering_timestamp = timepoint::min();
    _reset_buffer_stats(sample_rate);

    // reset() keeps the repair configuration; only the stream state is dropped
    _repair_clear_stream();
}



/******************************************************************************
*   Adds the given packet to the end of the buffer, or inserts it earlier in
*   the buffer if it is out of order.
*
*   Returns rtp jitter result code
******************************************************************************/
RTPJitter::RESULT RTPJitter::push(rawrtp_ptr p)
{
    RTPHeader  *rtp;
    uint16      rtp_sequence = 0;
    RESULT      rc = SUCCESS;

    rscoped_lock lock(_mutex);

    // the legacy path is not available while a repair configuration is active
    if (_repair.configured) {
        return BAD_PACKET;
    }

    if ((p != nullptr)
     && ((rtp = reinterpret_cast<PRTPHeader>(p->pData))))
    {
        rtp_sequence = ntohs(rtp->sequence);

        if (_depth_ms > (unsigned)_max_buffer_depth) {
            LOGD("RTPJitter::push(): buffer overflow: buffer depth: %d  packet #%d", _depth_ms, rtp_sequence);
            rc = BUFFER_OVERFLOW;
            _stats.overflow_count++;

            // we are overflowing ... drop the front packet
            rawrtp_ptr old_packet = _buffer.front();
            _buffer.pop_front();
            _depth_ms -= old_packet->payload_ms;
            old_packet.reset();
        }

        // if this is our first packet since init, start the buffering clock ...
        if (_buffering && (_buffering_timestamp == timepoint::min())) {
            _buffering_timestamp = stdclock::now();
        }

        // for every packet, update jitter stats
        _calc_jitter(rtp);

        // TODO: sequence numbers are only 16 bits and will wrap around
        //  fairly often.  We need to be sure we're not naive here.

        // TODO: there's a problem here: when sequence numbers wrap around,
        //  if either the max_int one, or the zero one, go missing, then
        //  we will not recover (subsequent packets will be dropped as "bad")
        //  until the sequence numbers wrap around AGAIN and one of those
        //  packets are NOT dropped a second time.

        if ((rtp_sequence >= _last_buf_sequence)
         || ((rtp_sequence == 0) && (_last_buf_sequence == UINT16_MAX))
         || (_last_pop_sequence == _first_buf_sequence))
        {
            // if this packet has a sequence number greater than
            //  any other I've seen so far, then we can be certain
            //  that this one belongs at the end.  As a caveat, I
            //  suppose packets could arrive with the same sequnce
            //  number.  If so, this goes right after the one we
            //  already have -- in this case, it still goes on the
            //  back end ... and we don't consider it to be out of
            //  order.
            _buffer.push_back(p);
            _last_buf_sequence = rtp_sequence;
            _depth_ms += p->payload_ms;

            // if this is the only packet we have, it obviously
            //  serves as both the first and last element.  Also,
            //  we will set the _last_pop_sequence as well so when
            //  we're popping, we don't think there was a dropped
            //  packet i.e. _first_buf == _last_pop means all good.
            if (_buffer.size() == 1) {
                _first_buf_sequence = _last_pop_sequence = rtp_sequence;
            }
        } else {
            LOGD("jitter.push(): ooo packet #%d", rtp_sequence);
            ++_stats.ooo_count;
            // This is an out-of-order packet.  One of two scenarios:
            //
            // 1. preceeds front packet by more than 1
            //      - packet is too old to use, ignore it
            // 2. immediately preceeds the front packet
            //      - packet is just in time, stick on front
            // 3. belogns in the middle of the buffer
            //      - find the home and insert
            // 4. sequence numbers have wrapped around to 0 again
            //      -
            if (rtp_sequence < (_first_buf_sequence - 1)) {
                rc = BAD_PACKET;
            } else if (rtp_sequence == (_first_buf_sequence - 1)) {
                _buffer.push_front(p);
                _first_buf_sequence = rtp_sequence;
            } else {
                // this packet has a sequence number that is strictly
                //  less than the last packet in our buffer, and greater
                //  than or equal to the first packet.  Either way, this
                //  one can go is anywhere from index 1 to n-2
                for (deque<rawrtp_ptr>::iterator i = _buffer.begin(); i != _buffer.end(); ++i) {
                    RTPHeader *item = reinterpret_cast<PRTPHeader>((*i)->pData);
                    if (rtp_sequence < item->sequence) {
                        _buffer.insert(i, p);
                        break;
                    }
                }
            }
        }
    } else {
        // we were given a null pointer ... is that bad enough?
        rc = BAD_PACKET;
    }
    return rc;
}



/******************************************************************************
*   Retrieves the RTP packet from the front of the buffer, or nothing if the
*   expected packet is missing.
*
*   NOTE: be very careful in this routine -- I broke the "one entry, one exit"
*   rule.
*
*   Returns rtp jitter result code
******************************************************************************/
RTPJitter::RESULT RTPJitter::pop(rawrtp_ptr& packet)
{
    rawrtp_ptr bp;      // buffer packet tmp pointer

    rscoped_lock lock(_mutex);

    if (_repair.configured) {
        return BAD_PACKET;
    }

    // first things first -- do we need to enter or exit the buffering state?
    if (_buffer.empty()) {
        // the buffer is empty ... do we need to go back to buffering?  If the
        //  _buffering flag is not yet set, then yes.
        if (!_buffering) {
            _buffering = true;
        }
        _stats.empty_count++;
    } else {
        if (_buffering) {
            // check the time... come out of buffering once the buffering
            //  timer reaches the nominal jitter depth, or if we get a burst
            //  of packets that deepens the buffer to the nominal depth.
            //
            // It's possible that packets came bursting in i.e. we've reached
            //  our depth before the buffering delay expires.  In this case,
            //  we also come out of the buffering state.
            int buffer_time = clocks::duration_cast<clocks::milliseconds>(stdclock::now() - _buffering_timestamp).count();
            if ((buffer_time >= _nominal_depth_ms)
             || (_depth_ms >= _nominal_depth_ms))
            {
                _buffering = false;
                _buffering_timestamp = timepoint::min();
            }
        }
    }

    if (_buffering) {
        return RTPJitter::BUFFERING;
    }

    // take a look at the packet at the front of the buffer
    bp = _buffer.front();

    // let's see if we should take what's on the front of the buffer, or
    //  if we need to return nothing and indicate a dropped packet.
    //  There's a lot of logic here, so tread lightly.
    //
    //  good sequences:
    //      _last_pop and _first_buf are equal
    //      _last_pop is one less than _first_buf
    //      _last_pop is UINT16_MAX and _first_buf is 0
    //      dynamic payloads and _last_pop is 2 less than _first_buf
    if ((_last_pop_sequence == _first_buf_sequence)
     || (_last_pop_sequence == (_first_buf_sequence - 1))
     || ((_last_pop_sequence == UINT16_MAX) && (_first_buf_sequence == 0))
     || ((bp->payload_type == RTP_PAYLOAD_DYNAMIC) && (_last_pop_sequence == (_first_buf_sequence - 2))))
    {
        packet = bp;
        if ((bp->payload_type == RTP_PAYLOAD_DYNAMIC) && (_last_pop_sequence == (_first_buf_sequence - 2))) {
            // "special" case where we hang onto the front packet in the buffer
            //  but mark it becasue we expect to be able to reuse it.
            packet->use_redundant_payload = true;
        } else {
            // "normal" case where we can remove the front packet
            packet->use_redundant_payload = false;
            _buffer.pop_front();
            _depth_ms -= packet->payload_ms;
        }

        RTPHeader *p = reinterpret_cast<PRTPHeader>(packet->pData);
        _last_pop_sequence = ntohs(p->sequence);

        // did we just empty the buffer?  If so, reset the sequence counters
        if (_buffer.empty()) {
            _first_buf_sequence = _last_pop_sequence;
        } else {
            // now peek at the next packet to get it's sequence #
            bp = _buffer.front();
            p = reinterpret_cast<PRTPHeader>(bp->pData);
            _first_buf_sequence = ntohs(p->sequence);
        }
        return RTPJitter::SUCCESS;

    } else {
        ++_last_pop_sequence;
        return RTPJitter::DROPPED_PACKET;
    }
}



/******************************************************************************
*   Empties the current buffer and reinitializes.  May block while waiting for
*   any other thread accessing the buffer.
*
*   Returns rtp jitter result code
******************************************************************************/
RTPJitter::RESULT RTPJitter::reset()
{
    rscoped_lock lock(_mutex);
    _clean_buffer();
    _repair_clear_stream();
    init(_nominal_depth_ms, _payload_sample_rate);

    return SUCCESS;
}



/******************************************************************************
*   Sets the nominal and maximum depths, in milliseconds, of the buffer.  If
*   max_depth is not given, or less than ms_depth, it will be calculated to
*   2 times ms_depth.
*
*   Returns nothing
******************************************************************************/
void RTPJitter::set_depth(const unsigned ms_depth, const unsigned max_depth /* = 0 */)
{
    rscoped_lock lock(_mutex);

    _nominal_depth_ms = ms_depth;
    if (max_depth >= ms_depth) {
        _max_buffer_depth = max_depth;
    } else {
        _max_buffer_depth = (_nominal_depth_ms * 2);
    }
}



/******************************************************************************
*   Rertieves the current number of packets in the buffer.
******************************************************************************/
int RTPJitter::get_depth()
{
    rscoped_lock lock(_mutex);

    if (_repair.configured) {
        int count = 0;
        for (std::map<int64, RepairSlot>::iterator i = _repair.slots.begin();
             i != _repair.slots.end(); ++i) {
            if (i->second.packet) {
                ++count;
            }
        }
        return count;
    }

    return (_buffer.size());
}



/******************************************************************************
*   Retrieves the current 'depth' of the buffer in miliseconds.
******************************************************************************/
int RTPJitter::get_depth_ms()
{
    rscoped_lock lock(_mutex);

    if (_repair.configured) {
        return get_depth() * (int)_repair.config.packet_ms;
    }

    return _depth_ms;
}



/******************************************************************************
*   Retrieves the current "requested/nominal" depth of the buffer in miliseconds.
******************************************************************************/
int RTPJitter::get_nominal_depth()
{
    rscoped_lock lock(_mutex);
    return _nominal_depth_ms;
}



/******************************************************************************
*   Some external agent is saying an end of transmission has been detected and
*   we might want to reset our sequence numbers since there's no guarantee
*   future numbers won't overlap current ones in an odd way.  Just sayin'
******************************************************************************/
void RTPJitter::eot_detected()
{
    _first_buf_sequence = 0;
    _last_buf_sequence = 0;
    _last_pop_sequence = 0;
}



/******************************************************************************
*   Calculates/updates jitter stats based on the given packet.  We adhere to
*   the formula estimating interarrival jitter as proscribed in RFC3550 section
*   6.4.1 and on the sample code in appendix A.8.
*
*   Returns none -- there's no return value, but internal stats are updated.
******************************************************************************/
void RTPJitter::_calc_jitter(RTPHeader *rtp)
{
    uint32      arrival;
    int         d;
    uint32      transit;

    timepoint   current_time = stdclock::now();
    int         interarrival_time_ms = clocks::duration_cast<clocks::milliseconds>(current_time - _stats.prev_rx_timestamp).count();

    // get the 'arrival time' of this packet as measured in 'timestamp units' and offset
    //  to match the timestamp range in this stream
    arrival = interarrival_time_ms * _stats.conversion_factor_timestamp_units;
    if (_stats.prev_arrival == 0) {
        arrival = ntohl(rtp->timestamp);
    } else {
        arrival += _stats.prev_arrival;
    }

    // TODO: there's a problem here in the calculation of arrival and
    //  prev_arrival times.  Need to work this out in some testing.
//    _stats.prev_arrival = arrival;
    _stats.prev_arrival = ntohl(rtp->timestamp);

    transit = arrival - ntohl(rtp->timestamp);
    d = transit - _stats.prev_transit;
    _stats.prev_transit = transit;
    if (d < 0) d = -d;

    _stats.jitter += (1.0/16.0) * ((double)d - _stats.jitter);

    _stats.prev_rx_timestamp = current_time;
    // is this a new high water mark for jitter?
    if (_stats.max_jitter < _stats.jitter) _stats.max_jitter = _stats.jitter;

//    LOGD("RTPJitter::_calc_jitter(): interarrival_time_ms[%03d]  arrival[%u]  rtp->timestamp[%u]  transit[%d]  jitter[%d]  max_jitter[%.4f] ", interarrival_time_ms, arrival, ntohl(rtp->timestamp), transit, (uint32)_stats.jitter, _stats.max_jitter);
}



/******************************************************************************
*   Clean items out of our buffer and delete/release memory resources.
*
*   Returns none
******************************************************************************/
void RTPJitter::_clean_buffer()
{
    _buffer.clear();
    _depth_ms = 0;
}



/******************************************************************************
*   Finds the index of the start of payload data in the given RTP packet by
*   accounting for the standard header and any possible extensions, etc.
*
*   Returns pointer to first byte of payload data, nullptr on error
******************************************************************************/
uint8 *RTPJitter::_get_payload(RTPHeader *packet)
{
    uint8  *payload = (uint8 *)packet;

    if (payload != nullptr) {

        payload += sizeof(RTPHeader);

        // TODO: account for CSRC identifiers

        // if the extension header bit is set, skip the dynamically
        //  sized header extension.
        uint16 flags = ntohs(packet->flags);
        if (flags & RTP_FLAGS_EXTENSION) {
            RTPHeaderExt *ext = (RTPHeaderExt *)payload;
            payload += sizeof(ext->profile_specific)
                    +  sizeof(ext->ext_length)
                    + (sizeof(ext->ext_data[0]) * ntohs(ext->ext_length));
        }

        // special handling for the Dynamic payload type
        if (_get_payload_type(packet) == RTP_PAYLOAD_DYNAMIC) {
            // skip over the redundant payload, etc.
            payload += 3;   // skip to the redundant block length
            payload += *payload + 1;    // should skip over the redundant size and the redundant payload
            payload += 1;   // ... and finally, skip over the primary Payload Type (which SHOULD be 18 G.729)
        }
    }
    return payload;
}



/******************************************************************************
*   Utility/convenience function to extract Payload Type from an RTP Header.
******************************************************************************/
uint8 RTPJitter::_get_payload_type(RTPHeader *packet)
{
    uint8 payload_type;

    uint16 flags = ntohs(packet->flags);
    payload_type = flags & RTP_FLAGS_PAYLOAD_TYPE;

    return payload_type;
}



/******************************************************************************
*   Sends given string to stdout.
*
*   Returns none
******************************************************************************/
void RTPJitter::_log(string s)
{
   // cout << s << endl;
}



/******************************************************************************
*   Just like the name says ... resets the jitter buffer statistics
*
*   Returns none
******************************************************************************/
void RTPJitter::_reset_buffer_stats(uint32 sample_rate)
{
    _stats.ooo_count = 0;
    _stats.empty_count = 0;
    _stats.overflow_count = 0;
    _stats.jitter = 0.0;
    _stats.max_jitter = 0.0;
    _stats.prev_arrival = 0;
    _stats.prev_transit = 0;
    _stats.prev_rx_timestamp = timepoint::min();
    _stats.conversion_factor_timestamp_units = sample_rate / 1000;
}


/******************************************************************************
*   ---------------- fixed-rate repair path (RFC 4588 style) ----------------
*
*   The repair path lives next to the jitter buffer but never in parallel with
*   it: while a repair configuration is active push()/pop() report BAD_PACKET
*   and the complete stream state (playout head, known slots, NACK history)
*   is kept inside this class.  All times are provided in microseconds by the
*   caller; no wall clock, threads or sleeps are involved.
******************************************************************************/

namespace
{
    uint16 repair_read_u16(const uint8 *p)
    {
        uint16 v;
        memcpy(&v, p, sizeof(v));
        return v;
    }

    uint32 repair_read_u32(const uint8 *p)
    {
        uint32 v;
        memcpy(&v, p, sizeof(v));
        return v;
    }

    // Absolute slot of a sequence number relative to the anchor, resolving
    // 16 bit wrap around.  The active span is assumed smaller than 32768.
    int64 repair_relative_slot(uint16 sequence, uint16 anchor)
    {
        int d = (int)(uint16)(sequence - anchor);
        if (d >= 32768) {
            d -= 65536;
        }
        return (int64)d;
    }

    uint16 repair_original_sequence(uint16 anchor, int64 slot)
    {
        return (uint16)(anchor + (uint16)slot);
    }

    // Timestamp for a slot, as modular 32 bit arithmetic.  Only the low 32
    // bits of the per-packet advance matter, so the step is reduced first and
    // no signed overflow can occur.
    uint32 repair_expected_timestamp(uint32 anchor_ts, int64 slot, int64 step)
    {
        uint64 advance = (uint64)(uint32)(uint64)step * (uint64)slot;
        return (uint32)((uint64)anchor_ts + advance);
    }
}



/******************************************************************************
*   Validates a repair configuration without touching any state.
*
*   Returns true when the configuration is usable
******************************************************************************/
bool RTPJitter::_repair_parse(const RepairConfig& config)
{
    int64 product;

    if (config.primary_ssrc == config.rtx_ssrc) {
        return false;
    }
    if ((config.primary_pt < 0) || (config.primary_pt > 127)) {
        return false;
    }
    if ((config.rtx_pt < 0) || (config.rtx_pt > 127)) {
        return false;
    }
    if (config.primary_pt == config.rtx_pt) {
        return false;
    }
    if (config.clock_rate <= 0) {
        return false;
    }
    if (config.retry_ms <= 0) {
        return false;
    }
    if ((config.packet_ms < 1) || (config.packet_ms > 65535)) {
        return false;
    }
    if (config.delay_ms < 0) {
        return false;
    }
    if ((config.window_packets < 2) || (config.window_packets > 512)) {
        return false;
    }

    // the per packet timestamp advance must be a positive whole number
    if (config.clock_rate > (INT64_MAX / config.packet_ms)) {
        return false;
    }
    product = config.clock_rate * config.packet_ms;
    if ((product < 1000) || ((product % 1000) != 0)) {
        return false;
    }

    return true;
}



/******************************************************************************
*   Applies a repair configuration.  On failure nothing changes.  On success
*   the previous stream and timing state is discarded and the repair path is
*   active; reset() will keep this configuration.
*
*   Returns true when the configuration was accepted
******************************************************************************/
bool RTPJitter::configure_repair(const RepairConfig& config)
{
    rscoped_lock lock(_mutex);

    if (!_repair_parse(config)) {
        return false;
    }

    _repair_clear_stream();
    _buffer.clear();
    _depth_ms = 0;

    _repair.config       = config;
    _repair.configured   = true;
    _repair.slot_us      = config.packet_ms * 1000;
    _repair.ts_step      = (config.clock_rate * config.packet_ms) / 1000;

    return true;
}



/******************************************************************************
*   Drops the stream state but leaves the configuration in place.
******************************************************************************/
void RTPJitter::_repair_clear_stream()
{
    _repair.anchored  = false;
    _repair.anchor_us = 0;
    _repair.anchor_seq = 0;
    _repair.anchor_ts = 0;
    _repair.cursor    = 0;
    _repair.newest    = 0;
    _repair.has_newest = false;
    _repair.slots.clear();
}



/******************************************************************************
*   Drops slots that can no longer be used (already played or expired while
*   still missing) together with their feedback entries.  Delivered content
*   that is merely past its deadline is kept for a late (but legal) pop.
******************************************************************************/
void RTPJitter::_repair_prune(int64 now_us)
{
    std::map<int64, RepairSlot>::iterator i = _repair.slots.begin();

    while (i != _repair.slots.end()) {
        RepairSlot& slot = i->second;
        bool stale = (i->first < _repair.cursor)
                  || (slot.missing && (now_us > slot.deadline_us));

        if (stale) {
            _repair.slots.erase(i++);
        } else {
            ++i;
        }
    }
}



/******************************************************************************
*   Records a missing, revealed slot and creates its feedback entry.  Called
*   when an accepted packet advances the newest known slot, or when another
*   packet exposes an earlier hole.
******************************************************************************/
void RTPJitter::_repair_reveal(int64 slot)
{
    if (_repair.slots.find(slot) != _repair.slots.end()) {
        return;
    }

    RepairSlot missing;
    missing.deadline_us = _repair.anchor_us
                        + _repair.config.delay_ms * 1000
                        + slot * _repair.slot_us;
    missing.missing     = true;
    missing.nack_started = false;
    missing.last_nack_us = 0;
    _repair.slots[slot] = missing;
}



/******************************************************************************
*   Stores an accepted packet in its slot.  An already accepted slot keeps its
*   first content; a previously revealed hole is filled.  A new slot advances
*   the known window and reveals every intervening hole.
*
*   Returns SUCCESS
******************************************************************************/
RTPJitter::RESULT RTPJitter::_repair_accept(int64 slot, rawrtp_ptr packet)
{
    std::map<int64, RepairSlot>::iterator it = _repair.slots.find(slot);

    if (it != _repair.slots.end()) {
        if (it->second.packet) {
            // duplicate: first accepted content wins, nothing changes
            return SUCCESS;
        }
        // a repair packet filled a hole; the known edge does not move
        it->second.packet  = packet;
        it->second.missing = false;
        return SUCCESS;
    }

    RepairSlot accepted;
    accepted.packet      = packet;
    accepted.deadline_us = _repair.anchor_us
                         + _repair.config.delay_ms * 1000
                         + slot * _repair.slot_us;
    accepted.missing     = false;
    accepted.nack_started = false;
    accepted.last_nack_us = 0;
    _repair.slots[slot]  = accepted;

    if (!_repair.has_newest || (slot > _repair.newest)) {
        int64 first = _repair.has_newest ? (_repair.newest + 1) : slot;

        _repair.newest     = slot;
        _repair.has_newest = true;

        for (int64 s = first; s < slot; ++s) {
            _repair_reveal(s);
        }
    }

    return SUCCESS;
}



/******************************************************************************
*   Restores an RTX packet to a primary packet, checking the RFC 4588 two byte
*   OSN prefix.  Returns a null pointer when the packet is malformed.
******************************************************************************/
rawrtp_ptr RTPJitter::_repair_restore_rtx(const RTPPacket& packet)
{
    RTPPacket::Layout layout;
    uint16            osn;

    if (!packet.parse_layout(layout) || (layout.media_len < 2)) {
        return rawrtp_ptr();
    }

    osn = repair_read_u16(packet.pData + layout.payload_offset);
    osn = ntohs(osn);

    return RTPPacket::restore_rtx(packet, osn,
                                  _repair.config.primary_ssrc,
                                  (uint8)_repair.config.primary_pt,
                                  (uint16)_repair.config.packet_ms);
}



/******************************************************************************
*   Accepts a primary or repair packet at the given stream time.
*
*   Returns rtp jitter result code
******************************************************************************/
RTPJitter::RESULT RTPJitter::_repair_push(rawrtp_ptr packet, int64 now_us)
{
    uint8   *data;
    uint16   sequence;
    uint32   timestamp;
    uint32   ssrc;
    uint8    payload_type;
    bool     is_primary;
    bool     is_rtx;
    int64    slot;
    int64    deadline;
    rawrtp_ptr accepted = packet;
    RTPPacket::Layout layout;

    if (!packet || !packet->pData || (packet->nLen < RTP_HEADER_LENGTH)) {
        return BAD_PACKET;
    }

    data = packet->pData;
    if ((data[0] >> 6) != RTP_VERSION) {
        return BAD_PACKET;
    }

    sequence     = ntohs(repair_read_u16(data + 2));
    timestamp    = ntohl(repair_read_u32(data + 4));
    ssrc         = ntohl(repair_read_u32(data + 8));
    payload_type = (uint8)(data[1] & RTP_FLAGS_PAYLOAD_TYPE);

    is_primary = (ssrc == _repair.config.primary_ssrc)
              && (payload_type == (uint8)_repair.config.primary_pt);
    is_rtx     = (ssrc == _repair.config.rtx_ssrc)
              && (payload_type == (uint8)_repair.config.rtx_pt);

    if (!is_primary && !is_rtx) {
        return BAD_PACKET;
    }

    // the two envelope shapes are validated fully up front so failures have
    // no side effects
    if (!packet->parse_layout(layout)) {
        return BAD_PACKET;
    }

    if (is_rtx) {
        if (layout.media_len < 2) {
            return BAD_PACKET;
        }
        accepted = _repair_restore_rtx(*packet);
        if (!accepted) {
            return BAD_PACKET;
        }
        sequence  = ntohs(repair_read_u16(accepted->pData + 2));
        timestamp = ntohl(repair_read_u32(accepted->pData + 4));
    }

    // an RTX packet before the first media packet does not establish anything
    if (!_repair.anchored) {
        if (is_rtx) {
            return BAD_PACKET;
        }

        _repair.anchored   = true;
        _repair.anchor_us  = now_us;
        _repair.anchor_seq = sequence;
        _repair.anchor_ts  = timestamp;
        _repair.cursor     = 0;
        _repair.newest     = 0;
        _repair.has_newest = true;

        return _repair_accept(0, accepted);
    }

    slot = repair_relative_slot(sequence, _repair.anchor_seq);

    // deadline for this slot, relative to the anchor arrival
    deadline = _repair.anchor_us
             + _repair.config.delay_ms * 1000
             + slot * _repair.slot_us;

    std::map<int64, RepairSlot>::iterator known = _repair.slots.find(slot);
    if (known != _repair.slots.end()) {
        if (known->second.packet) {
            return SUCCESS;                 // duplicate, content preserved
        }
        if (now_us > deadline) {
            return BAD_PACKET;              // the playout moment has passed
        }
        if (timestamp != repair_expected_timestamp(_repair.anchor_ts, slot, _repair.ts_step)) {
            return BAD_PACKET;
        }
        return _repair_accept(slot, accepted);
    }

    if (slot < _repair.cursor) {
        return BAD_PACKET;                  // already played / too old
    }
    if (slot >= (_repair.cursor + _repair.config.window_packets)) {
        return BUFFER_OVERFLOW;
    }
    if (now_us > deadline) {
        return BAD_PACKET;
    }
    if (timestamp != repair_expected_timestamp(_repair.anchor_ts, slot, _repair.ts_step)) {
        return BAD_PACKET;
    }

    return _repair_accept(slot, accepted);
}



/******************************************************************************
*   Packet arrival entry point for the repair path.
*
*   Returns rtp jitter result code
******************************************************************************/
RTPJitter::RESULT RTPJitter::push_at(rawrtp_ptr packet, int64 now_us)
{
    rscoped_lock lock(_mutex);

    if (!_repair.configured || (now_us < 0)) {
        return BAD_PACKET;
    }

    _repair_prune(now_us);
    return _repair_push(packet, now_us);
}



/******************************************************************************
*   Retrieves the packet for the current playout slot.
*
*   Returns rtp jitter result code
******************************************************************************/
RTPJitter::RESULT RTPJitter::pop_at(rawrtp_ptr& packet, int64 now_us)
{
    rscoped_lock lock(_mutex);

    if (!_repair.configured || (now_us < 0)) {
        return BAD_PACKET;
    }
    _repair_prune(now_us);
    if (!_repair.anchored) {
        // the stream has not started yet; there is no playout to advance
        return BUFFERING;
    }

    if (_repair.cursor > _repair.newest) {
        return BUFFER_EMPTY;
    }

    int64 deadline = _repair.anchor_us
                   + _repair.config.delay_ms * 1000
                   + _repair.cursor * _repair.slot_us;

    if (now_us < deadline) {
        return BUFFERING;
    }

    std::map<int64, RepairSlot>::iterator it = _repair.slots.find(_repair.cursor);

    if ((it != _repair.slots.end()) && it->second.packet) {
        packet = it->second.packet;
        _repair.slots.erase(it);
        ++_repair.cursor;
        _repair_prune(now_us);
        return SUCCESS;
    }

    if (it != _repair.slots.end()) {
        _repair.slots.erase(it);
    }
    ++_repair.cursor;
    _repair_prune(now_us);
    return DROPPED_PACKET;
}



/******************************************************************************
*   Returns the original sequence numbers of holes that are revealed, still
*   unfilled and whose playout deadline has not passed, in playback order.
*   A hole is reported on the first query and again only after retry_ms; a
*   late query reports once and does not chase missed rounds.  Querying never
*   advances playout or re-anchors the stream.
******************************************************************************/
std::vector<uint16> RTPJitter::nacks_at(int64 now_us)
{
    std::vector<uint16> result;

    rscoped_lock lock(_mutex);

    if (!_repair.configured || !_repair.anchored || (now_us < 0)) {
        return result;
    }

    // expire holes without disturbing the anchor
    _repair_prune(now_us);

    std::map<int64, RepairSlot>::iterator i;
    for (i = _repair.slots.lower_bound(_repair.cursor); i != _repair.slots.end(); ++i) {
        RepairSlot& slot = i->second;

        if (!slot.missing || (now_us > slot.deadline_us)) {
            continue;
        }

        bool report;
        if (!slot.nack_started) {
            report = true;
        } else {
            report = (now_us - slot.last_nack_us) >= (_repair.config.retry_ms * 1000);
        }

        if (report) {
            slot.nack_started = true;
            slot.last_nack_us = now_us;
            result.push_back(repair_original_sequence(_repair.anchor_seq, i->first));
        }
    }

    return result;
}

