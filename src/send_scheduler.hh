#pragma once

#include "uvgrtp/util.hh"

#include <cstdint>
#include <functional>

namespace uvgrtp {

    /* Access-unit level send scheduling for the media sender.
     *
     * One push of an H26x access unit occupies exactly one frame slot when frame
     * rate limiting (RCE_FRAME_RATE) is enabled, and fragment pacing
     * (RCE_PACE_FRAGMENT_SENDING) spreads the packets of the whole access unit
     * instead of pacing each NAL unit separately.
     *
     * The clock and the waiting mechanism can be replaced with hooks so that the
     * scheduling can be verified deterministically without wall-clock sleeps. */
    class send_scheduler {
        public:
            send_scheduler();

            /* Integer frame rate configuration, same semantics as before */
            void set_fps(ssize_t numerator, ssize_t denominator);

            /* Pace ratio used with RCE_PACE_FRAGMENT_SENDING, defaults to 8/10 */
            void set_pace(ssize_t numerator, ssize_t denominator);

            /* Replace the clock (monotonic nanoseconds) and the wait mechanism.
             * Passing two empty functions restores the defaults
             * (steady_clock + real waiting). Must be called before sending. */
            void set_timing_hooks(std::function<int64_t()> now_ns,
                std::function<void(int64_t)> wait_until_ns);

            /* Whether a valid frame interval has been configured */
            bool fps_valid() const;

            /* Begin a new access unit that is about to be sent.
             *
             * With "use_frame_rate" the access unit occupies exactly one frame
             * slot: the first access unit establishes the synchronization point
             * from the current monotonic time, later slots advance by the
             * configured frame interval and an access unit that is late
             * resynchronizes the schedule once to the current time. Waits until
             * the slot and returns the slot time (the access unit start) in
             * nanoseconds.
             *
             * Without frame rate limiting the access unit starts at the current
             * time and no slot is consumed. */
            int64_t begin_access_unit(bool use_frame_rate);

            /* Sending deadline of packet "index" (0-based) of an access unit with
             * "packet_count" packets whose access unit start is "au_start_ns":
             *
             *   au_start + floor(index * frame_interval * pace_numerator /
             *                    (packet_count * pace_denominator)) */
            int64_t packet_deadline(int64_t au_start_ns, uint64_t index, uint64_t packet_count) const;

            /* Wait until the given monotonic time point (nanoseconds) */
            void wait_until(int64_t deadline_ns) const;

            /* Current monotonic time in nanoseconds */
            int64_t now_ns() const;

        private:
            bool fps_valid_;
            int64_t frame_interval_ns_;

            int64_t pace_numerator_;
            int64_t pace_denominator_;

            bool sync_valid_;
            int64_t sync_point_ns_;
            uint64_t slots_since_sync_;

            std::function<int64_t()> now_hook_;
            std::function<void(int64_t)> wait_hook_;
    };
}

namespace uvg_rtp = uvgrtp;
