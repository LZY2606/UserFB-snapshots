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
*   16-bit sequence number helpers.  The active sequence span is guaranteed
*   to stay below 32768, so a forward distance in that range means "newer"
*   while anything in the upper half means the number is older.
******************************************************************************/
int RTPJitter::_slot_distance(uint16 from, uint16 to) const
{
    int distance = static_cast<int>(to) - static_cast<int>(from);
    if (distance < 0) {
        distance += 65536;
    }
    return distance;
}


uint16 RTPJitter::_sequence_forward(uint16 sequence, int distance) const
{
    return static_cast<uint16>((static_cast<unsigned>(sequence)
                              + static_cast<unsigned>(distance)) & 0xFFFFu);
}


// Media RTP timestamp expected on slot "sequence", wrapping in 32 bits just
//  like the wire field.
uint32 RTPJitter::_slot_timestamp(uint16 sequence) const
{
    uint32 step = static_cast<uint32>(_repair_config.clock_rate)
                * static_cast<uint32>(_repair_config.packet_ms) / 1000u;
    uint32 k = static_cast<uint32>(_slot_distance(_anchor_sequence, sequence));
    return static_cast<uint32>(_anchor_timestamp + k * step);
}


// Playout instant (microseconds) of slot "sequence": anchor arrival plus the
//  configured delay plus one frame duration per slot.  All integer math.
int64 RTPJitter::_slot_playtime(uint16 sequence) const
{
    int64 k = _slot_distance(_anchor_sequence, sequence);
    return _anchor_arrival_us
         + static_cast<int64>(_repair_config.delay_ms) * 1000
         + k * static_cast<int64>(_repair_config.packet_ms) * 1000;
}


/******************************************************************************
*   Just call _init to get initialization "things" done.
*
*   Returns n/a
******************************************************************************/
RTPJitter::RTPJitter(const unsigned depth, const uint32 sample_rate /* = 8000 */)
{
    _repair_enabled = false;
    _repair_anchored = false;
    _anchor_sequence = 0;
    _anchor_timestamp = 0;
    _anchor_arrival_us = 0;
    _play_head = 0;
    _highest_known = 0;
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
    // unconditionally start from an empty buffer so the depth accounting can
    //  never read a stale or uninitialized value on a fresh instance
    _clean_buffer();

    _first_buf_sequence = 0;
    _last_buf_sequence = 0;
    _last_pop_sequence = 0;
    set_depth(depth);
    _payload_sample_rate = sample_rate;

    _buffering = true;
    _buffering_timestamp = timepoint::min();
    _reset_buffer_stats(sample_rate);
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

    // repair mode owns its own timestamp-driven entry point
    if (_repair_enabled) {
        return BAD_PACKET;
    }

    if ((p != nullptr)
     && ((rtp = reinterpret_cast<PRTPHeader>(p->pData))))
    {
        rscoped_lock lock(_mutex);

        rtp_sequence = ntohs(rtp->sequence);

        if (_depth_ms > _max_buffer_depth) {
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

    // repair mode owns its own timestamp-driven entry point
    if (_repair_enabled) {
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
    init(_nominal_depth_ms, _payload_sample_rate);

    // repair configuration survives; only the stream and its timing anchor go
    if (_repair_enabled) {
        _repair_reset_stream();
    }

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
    if (_repair_enabled) {
        int count = 0;
        for (map<uint16, RepairSlot>::const_iterator i = _repair_slots.begin();
             i != _repair_slots.end(); ++i) {
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
    if (_repair_enabled) {
        return get_depth() * static_cast<int>(_repair_config.packet_ms);
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
*   Repair mode --------------------------------------------------------------
******************************************************************************/



/******************************************************************************
*   Drops every repair slot and the timing anchor while keeping the repair
*   configuration itself, so the instance can be reused after reset().
******************************************************************************/
void RTPJitter::_repair_reset_stream()
{
    _repair_slots.clear();
    _repair_anchored = false;
    _anchor_sequence = 0;
    _anchor_timestamp = 0;
    _anchor_arrival_us = 0;
    _play_head = 0;
    _highest_known = 0;
}



/******************************************************************************
*   Validates a candidate packet against the repair configuration.  A packet
*   is either a primary (matching primary SSRC/PT) or an RTX packet (matching
*   the RTX SSRC/PT and carrying at least a two-byte OSN).  Anything else,
*   including malformed RTP, is rejected with no side effects.
******************************************************************************/
bool RTPJitter::_repair_validate(rawrtp_ptr packet, RtpHeaderInfo& info,
                                 bool& is_rtx, uint16& osn) const
{
    if (packet == NULL || packet->pData == NULL || packet->nLen == 0) {
        return false;
    }
    if (!RTPPacket::parse_header(packet->pData, packet->nLen, info)) {
        return false;
    }

    if (info.ssrc == _repair_config.rtx_ssrc
     && info.payload_type == _repair_config.rtx_pt) {
        is_rtx = true;
    } else if (info.ssrc == _repair_config.primary_ssrc
            && info.payload_type == _repair_config.primary_pt) {
        is_rtx = false;
    } else {
        return false;
    }

    osn = 0;
    if (is_rtx) {
        // RFC 4588: the payload starts with the two-byte big-endian OSN
        if (info.payload_len < 2) {
            return false;
        }
        const uint8 *osn_ptr = packet->pData + info.header_len;
        osn = static_cast<uint16>((osn_ptr[0] << 8) | osn_ptr[1]);
    }
    return true;
}



/******************************************************************************
*   Stores an accepted primary packet (original or restored from RTX) in its
*   sequence slot.  The first accepted primary packet anchors the stream;
*   later packets must match the derived timestamp, stay unexpired and inside
*   the receive window.  The first content accepted for a slot always wins.
******************************************************************************/
RTPJitter::RESULT RTPJitter::_repair_accept(rawrtp_ptr packet, int64 now_us,
                                            uint16 seq, uint32 timestamp)
{
    if (!_repair_anchored) {
        _repair_anchored = true;
        _anchor_sequence = seq;
        _anchor_timestamp = timestamp;
        _anchor_arrival_us = now_us;
        _play_head = seq;
        _highest_known = seq;
        _repair_slots.clear();
        _repair_slots[seq].packet = packet;
        return SUCCESS;
    }

    // the media timestamp is fully determined by the anchor and slot index
    if (timestamp != _slot_timestamp(seq)) {
        return BAD_PACKET;
    }

    int distance = _slot_distance(_play_head, seq);
    if (distance >= 32768) {
        // the slot already lies behind the play head: too old to accept
        return BAD_PACKET;
    }
    if (distance >= _repair_config.window_packets) {
        // ahead of the play head but outside the receive window
        return BUFFER_OVERFLOW;
    }

    std::map<uint16, RepairSlot>::iterator it = _repair_slots.find(seq);
    if (it != _repair_slots.end() && it->second.packet) {
        // duplicate primary or RTX: keep the first content, clocks unchanged
        return SUCCESS;
    }

    // a still-open slot can be filled up to and including its playout
    //  instant; one microsecond later it is expired and may not be received
    if (now_us > _slot_playtime(seq)) {
        return BAD_PACKET;
    }

    RepairSlot& slot = _repair_slots[seq];
    slot.packet = packet;

    // anything inside the window that is not older than the current high
    //  water mark extends the newest known slot
    if (_slot_distance(_highest_known, seq) > 0
     && _slot_distance(_highest_known, seq) < 32768) {
        _highest_known = seq;
    }
    return SUCCESS;
}



/******************************************************************************
*   Validates RepairConfig fields before touching any live state.  On success
*   the old stream is discarded and the instance enters repair mode; on
*   failure the caller's previous configuration and buffered stream survive.
******************************************************************************/
bool RTPJitter::configure_repair(const RepairConfig& config)
{
    if (config.primary_ssrc == config.rtx_ssrc) {
        return false;
    }
    if (config.primary_pt == config.rtx_pt
     || config.primary_pt > 127 || config.rtx_pt > 127) {
        return false;
    }
    if (config.clock_rate == 0 || config.retry_ms == 0) {
        return false;
    }
    if (config.packet_ms < 1 || config.packet_ms > 65535) {
        return false;
    }

    uint64 timestamp_step = static_cast<uint64>(config.clock_rate)
                          * static_cast<uint64>(config.packet_ms);
    if (timestamp_step % 1000u != 0u || timestamp_step / 1000u == 0u) {
        return false;
    }
    if (config.window_packets < 2 || config.window_packets > 512) {
        return false;
    }

    rscoped_lock lock(_mutex);
    _repair_config = config;
    _repair_enabled = true;
    _clean_buffer();
    _repair_reset_stream();
    return true;
}



/******************************************************************************
*   Repair-mode ingress.  Primary packets are accepted directly; RTX packets
*   are validated, restored to their primary form (the caller's object is
*   never modified) and then travel the exact same slot path, keyed by the
*   original media sequence number.
******************************************************************************/
RTPJitter::RESULT RTPJitter::push_at(rawrtp_ptr packet, int64 now_us)
{
    if (now_us < 0) {
        return BAD_PACKET;
    }
    rscoped_lock lock(_mutex);
    if (!_repair_enabled) {
        return BAD_PACKET;
    }

    RtpHeaderInfo info;
    bool is_rtx = false;
    uint16 osn = 0;
    if (!_repair_validate(packet, info, is_rtx, osn)) {
        return BAD_PACKET;
    }

    rawrtp_ptr candidate = packet;
    uint16 seq = info.sequence;
    uint32 timestamp = info.timestamp;

    if (is_rtx) {
        // an RTX packet arriving before the first primary cannot anchor state
        if (!_repair_anchored) {
            return BAD_PACKET;
        }
        candidate = RTPPacket::restore_rtx(packet->pData, packet->nLen, info,
                                           osn, _repair_config.primary_ssrc,
                                           _repair_config.primary_pt,
                                           _repair_config.packet_ms);
        if (candidate == NULL) {
            return BAD_PACKET;
        }
        seq = osn;
    }

    return _repair_accept(candidate, now_us, seq, timestamp);
}



/******************************************************************************
*   Repair-mode playout.  One known slot is handled per call: before its
*   playout instant the call buffers; at or after it the slot is delivered or
*   reported dropped and the play head advances one slot.  Asking past the
*   newest known slot returns BUFFER_EMPTY without advancing.  The output
*   parameter is only rewritten on SUCCESS.
******************************************************************************/
RTPJitter::RESULT RTPJitter::pop_at(rawrtp_ptr& packet, int64 now_us)
{
    if (now_us < 0) {
        return BAD_PACKET;
    }
    rscoped_lock lock(_mutex);
    if (!_repair_enabled) {
        return BAD_PACKET;
    }
    if (!_repair_anchored) {
        return BUFFER_EMPTY;
    }

    // the play head must not pass the newest slot revealed so far.  A forward
    //  distance that wraps into the upper half means the head has advanced
    //  past every known slot.
    if (_slot_distance(_play_head, _highest_known) >= 32768) {
        return BUFFER_EMPTY;
    }

    int64 deadline = _slot_playtime(_play_head);
    if (now_us < deadline) {
        return BUFFERING;
    }

    RESULT rc = DROPPED_PACKET;
    std::map<uint16, RepairSlot>::iterator it = _repair_slots.find(_play_head);
    if (it != _repair_slots.end() && it->second.packet) {
        packet = it->second.packet;
        rc = SUCCESS;
    }

    _repair_slots.erase(_play_head);
    _play_head = _sequence_forward(_play_head, 1);
    return rc;
}



/******************************************************************************
*   Reports the original media sequence numbers of gaps that have already
*   been revealed (between the play head and the newest known slot), are not
*   filled yet and have not reached their playout instant.  Each gap is
*   reported immediately on the first query and again no sooner than
*   retry_ms later; repeated queries at the same instant never double-report.
*   Playout is neither advanced nor re-anchored by a query.
******************************************************************************/
std::vector<uint16> RTPJitter::nacks_at(int64 now_us)
{
    std::vector<uint16> requested;
    if (now_us < 0) {
        return requested;
    }
    rscoped_lock lock(_mutex);
    if (!_repair_enabled || !_repair_anchored) {
        return requested;
    }
    // once every known slot has been played there are no real gaps left to
    //  request; without this the sequence wrap would enumerate phantom slots
    if (_slot_distance(_play_head, _highest_known) >= 32768) {
        return requested;
    }

    const int64 retry_us = static_cast<int64>(_repair_config.retry_ms) * 1000;
    uint16 seq = _play_head;
    while (seq != _highest_known) {
        std::map<uint16, RepairSlot>::iterator it = _repair_slots.find(seq);
        bool filled = (it != _repair_slots.end() && it->second.packet);
        if (!filled && now_us <= _slot_playtime(seq)) {
            RepairSlot& slot = _repair_slots[seq];
            if (!slot.nacked || now_us - slot.last_nack_us >= retry_us) {
                requested.push_back(seq);
                slot.nacked = true;
                slot.last_nack_us = now_us;
            }
        }
        seq = _sequence_forward(seq, 1);
    }
    return requested;
}
