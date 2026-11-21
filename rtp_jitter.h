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
******************************************************************************/

#ifndef RTP_JITTER_H_cc8e302e_b008_4588_a29a_79a9f555804d
#define RTP_JITTER_H_cc8e302e_b008_4588_a29a_79a9f555804d

#include <deque>
#include <map>
#include <memory>
#include <vector>
#include "stdinc.h"
#include "rtp.h"


// Configuration for the RFC 4588 retransmission repair window.  All timing
//  is driven by the int64 microsecond timestamps handed to push_at/pop_at/
//  nacks_at; the buffer never reads a system clock, sleeps or runs threads.
struct RepairConfig
{
    uint32  primary_ssrc;
    uint32  rtx_ssrc;
    uint8   primary_pt;
    uint8   rtx_pt;
    uint32  clock_rate;         // samples per second
    uint16  packet_ms;          // fixed frame duration, 1..65535
    uint32  delay_ms;           // playout delay added to the anchor arrival
    uint32  retry_ms;           // minimum gap between NACKs for one gap
    uint16  window_packets;     // accept window measured from the play head

    RepairConfig() :
        primary_ssrc(0), rtx_ssrc(0), primary_pt(0), rtx_pt(0),
        clock_rate(0), packet_ms(0), delay_ms(0), retry_ms(0),
        window_packets(0) {}
};


class RTPJitter
{
public:
    enum RESULT
    {
        SUCCESS = 0,
        BUFFERING,
        BAD_PACKET,
        BUFFER_OVERFLOW,
        BUFFER_EMPTY,
        DROPPED_PACKET
    };


    RTPJitter(const unsigned depth, const uint32 sample_rate = 8000);
    ~RTPJitter();

    void    init(const unsigned depth, const uint32 sample_rate = 8000);
    RESULT  push(rawrtp_ptr packet);
    RESULT  pop(rawrtp_ptr& packet);
    RESULT  reset();
    void    set_depth(const unsigned ms_depth, const unsigned max_depth = 0);
    int     get_depth();
    int     get_depth_ms();
    int     get_nominal_depth();
    bool    buffering()         { return _buffering; }
    void    eot_detected();

    // - statistics retrieval
    int overflow_count()        { return _stats.overflow_count; }
    int out_of_order_count()    { return _stats.ooo_count; }
    int empty_count()           { return _stats.empty_count; }
    uint32 jitter()             { return (uint32)_stats.jitter; }
    uint32 max_jitter()         { return (uint32)_stats.max_jitter; }

    // - RFC 4588 retransmission repair window.  A failed configuration
    //  leaves the entire prior state untouched; a successful one clears the
    //  old stream and switches the instance into repair mode.  While repair
    //  mode is active the legacy push/pop entry points reject every call.
    bool                    configure_repair(const RepairConfig& config);
    RESULT                  push_at(rawrtp_ptr packet, int64 now_us);
    RESULT                  pop_at(rawrtp_ptr& packet, int64 now_us);
    std::vector<uint16>     nacks_at(int64 now_us);

private:
    static const int DEFAULT_BUFFER_ELEMENTS = 18;  // 360ms given 20ms packets
    static const int DEFAULT_MS_PER_PACKET   = 20;

    // TODO: there should be no need for a recursive mutex unless there
    //  is a possibility of a single thread calling one function from
    //  another where it will attempt to get the lock twice, but I don't
    //  think such a case exists.  Certainly there are probably 2 threads
    //  accessing an instance of this class i.e. packets are pushed in
    //  from a socket thread and popped off by an application thread.
    std::recursive_mutex    _mutex;
    std::deque<rawrtp_ptr>  _buffer;
    unsigned                _nominal_depth_ms;      // requested buffer depth - may dynamically adjust
    int                     _max_buffer_depth;      // as measured in milliseconds
    unsigned                _payload_sample_rate;   // rate of audio in packet payloads
    unsigned                _depth_ms;              // actual current buffer depth
    uint16                  _first_buf_sequence;    // at head of buffer (next to be popped)
    uint16                  _last_buf_sequence;     // at tail of buffer (most recent arrival)
    uint16                  _last_pop_sequence;
    bool                    _buffering;             // while buffering, don't pop packets

    timepoint               _buffering_timestamp;   // the time we start buffering

    struct stats {
        uint32      ooo_count;          // count of out of order packets
        uint32      empty_count;        // how many times was buffer empty
        uint32      overflow_count;     //
        double      jitter;
        double      max_jitter;
        uint32      prev_arrival;
        uint32      prev_transit;
        timepoint   prev_rx_timestamp;
        int         conversion_factor_timestamp_units;
    } _stats;

    void        _calc_jitter(RTPHeader *rtp);
    void        _clean_buffer();
    uint8       _get_payload_type(RTPHeader *packet);
    uint8      *_get_payload(RTPHeader *packet);
    void        _log(std::string s);
    void        _reset_buffer_stats(const uint32 sample_rate);

    // - repair mode state -------------------------------------------------
    //  Slots are keyed by the full (wrapping) 16-bit primary sequence and
    //  identified by an integer index anchored at the first legal primary
    //  packet.  Every accepted packet -- primary or restored RTX -- lands in
    //  the same slot map the player reads from; no side queue is kept.
    struct RepairSlot
    {
        rawrtp_ptr packet;          // empty while the slot is a gap
        int64      last_nack_us;    // last time the gap was reported
        bool       nacked;          // whether last_nack_us is meaningful
        RepairSlot() : last_nack_us(0), nacked(false) {}
    };

    RepairConfig                _repair_config;
    bool                        _repair_enabled;
    bool                        _repair_anchored;     // anchor packet seen
    uint16                      _anchor_sequence;     // S
    uint32                      _anchor_timestamp;    // T
    int64                       _anchor_arrival_us;   // A
    uint16                      _play_head;           // next slot to play
    uint16                      _highest_known;       // newest revealed slot
    std::map<uint16, RepairSlot> _repair_slots;

    void        _repair_reset_stream();
    RESULT      _repair_accept(rawrtp_ptr packet, int64 now_us,
                               uint16 sequence, uint32 timestamp);
    int64       _slot_playtime(uint16 sequence) const;
    uint32      _slot_timestamp(uint16 sequence) const;
    int         _slot_distance(uint16 from, uint16 to) const;
    uint16      _sequence_forward(uint16 sequence, int distance) const;
    bool        _repair_validate(rawrtp_ptr packet, RtpHeaderInfo& info,
                                 bool& is_rtx, uint16& osn) const;
};

#endif  // RTP_JITTER_H_cc8e302e_b008_4588_a29a_79a9f555804d
