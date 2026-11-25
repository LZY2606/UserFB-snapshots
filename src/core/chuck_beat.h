/*----------------------------------------------------------------------------
  ChucK BeatClock: absolute beat mapping and VM event scheduling hooks.

  The timing model is intentionally independent of rounded sample durations:
  every control change creates a new absolute anchor, and conversions for
  target beats are evaluated directly from that anchor.
-----------------------------------------------------------------------------*/
#ifndef CHUCK_BEAT_H
#define CHUCK_BEAT_H

// The task core makefile force-includes this header in target mode.  Defining
// the feature here lets unmodified build rules activate the VM hooks.
#ifndef CHUCK_BEAT_FEATURE
#define CHUCK_BEAT_FEATURE 1
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

struct Chuck_VM;
struct Chuck_Event;
struct Chuck_VM_Shred;

namespace ChuckBeat
{
    // Round a non-negative exact frame coordinate to the nearest integer
    // frame.  A exact half-frame rounds upwards.
    inline double round_frame_half_up( long double frame )
    {
        if( !std::isfinite( (double) frame ) )
            return frame < 0 ? 0.0 : std::numeric_limits<double>::infinity();
        return (double) std::floor( frame + 0.5L );
    }

    inline bool positive_finite( double x )
    {
        return std::isfinite( x ) && x > 0.0;
    }

    inline bool non_negative_finite( double x )
    {
        return std::isfinite( x ) && x >= 0.0;
    }

    // A tempo segment anchored at one absolute VM sample coordinate and one
    // absolute beat.  For a ramp, BPM is linear in beat position.
    struct Segment
    {
        long double s0 = 0.0L; // absolute VM time, in samples
        long double b0 = 0.0L; // absolute beat at s0
        long double v0 = 120.0L; // BPM at b0
        long double v1 = 120.0L; // BPM at b0 + length
        long double length = 0.0L; // ramp length in beats; 0 => constant

        long double end_beat() const { return b0 + length; }

        long double end_sample() const
        {
            if( length <= 0.0L || v1 == v0 )
                return s0 + seconds_per_beat_samples( v0 ) * length;
            // integral from b0 to b0+length of 60*sr/v(b) db
            return s0 + 60.0L * sample_rate * length / ( v1 - v0 )
                * std::log( v1 / v0 );
        }

        long double sample_rate = 48000.0L;

        long double seconds_per_beat_samples( long double bpm ) const
        { return 60.0L * sample_rate / bpm; }

        long double sample_at_beat( long double beat ) const
        {
            long double x = beat - b0;
            if( length <= 0.0L || x <= 0.0L || v1 == v0 )
                return s0 + seconds_per_beat_samples( v0 ) * x;

            if( beat <= end_beat() )
            {
                long double v = v0 + ( v1 - v0 ) * ( x / length );
                return s0 + 60.0L * sample_rate * length / ( v1 - v0 )
                    * std::log( v / v0 );
            }

            long double se = end_sample();
            return se + seconds_per_beat_samples( v1 ) * ( beat - end_beat() );
        }

        long double beat_at_sample( long double sample ) const
        {
            long double x = sample - s0;
            if( length <= 0.0L || v1 == v0 )
                return b0 + v0 * x / ( 60.0L * sample_rate );

            long double end_s = end_sample();
            if( sample >= end_s )
            {
                long double end_b = end_beat();
                return end_b + v1 * ( sample - end_s )
                    / ( 60.0L * sample_rate );
            }

            long double k = ( v1 - v0 ) / length; // BPM per beat
            long double exponent = x * k / ( 60.0L * sample_rate );
            // expm1 keeps near-constant ramps numerically stable.
            long double ratio_minus_one = std::expm1( exponent );
            long double v = v0 * ( 1.0L + ratio_minus_one );
            if( v <= 0.0L )
                return b0;
            long double q = ( v - v0 ) / ( v1 - v0 );
            return b0 + length * q;
        }

        long double bpm_at_beat( long double beat ) const
        {
            if( length <= 0.0L ) return v0;
            if( beat <= b0 ) return v0;
            if( beat >= end_beat() ) return v1;
            return v0 + ( v1 - v0 ) * ( ( beat - b0 ) / length );
        }
    };

    class Clock
    {
    public:
        Clock( double sample_rate, double now_system );

        double sample_rate() const { return (double) m_segment.sample_rate; }
        double origin_sample() const { return (double) m_segment.s0; }

        double current_beat( double now_system ) const;
        double current_bpm( double now_system ) const;
        double bpm() const { return (double) m_segment.v0; }

        void set_bpm( double value, double now_system );
        void ramp( double target_bpm, double beats, double now_system );

        long double exact_sample_for_beat( double beat ) const;
        double frame_for_beat( double beat ) const;

    private:
        Segment m_segment;
    };

    struct EventRecord
    {
        std::shared_ptr<Clock> clock;
        Chuck_VM * vm = nullptr;
        double target_beat = 0.0;
        double deadline = 0.0;
        unsigned long long first_wait_order = 0;
        bool fired = false;
    };

    // Test/introspection counters.  These are process-global and monotonic.
    enum Counter
    {
        COUNTER_WAIT_CALLS = 0,
        COUNTER_IMMEDIATE_WAKES,
        COUNTER_DUE_EVENTS,
        COUNTER_BROADCAST_EVENTS,
        COUNTER_REMOVED_WAITERS,
        COUNTER_VM_SHUTDOWNS,
        COUNTER__COUNT
    };

    unsigned long long test_counter( int which );
    void test_reset_counters();
    std::size_t test_clock_count();
    std::size_t test_event_record_count();
} // namespace ChuckBeat

// Core/VM hooks.  These are no-ops for non-BeatClock ChucK events.
void chuck_beat_on_wait( Chuck_Event * event, Chuck_VM_Shred * shred,
                         Chuck_VM * vm );
void chuck_beat_on_event_remove( Chuck_Event * event, Chuck_VM_Shred * shred );
void chuck_beat_on_event_destroy( Chuck_Event * event );
void chuck_beat_on_vm_shutdown( Chuck_VM * vm );

// Return the smallest non-negative sample delta to a blocked beat event, or
// -1 if there is no such waiter.
double chuck_beat_next_delta( Chuck_VM * vm, double now_system );
void chuck_beat_process_due( Chuck_VM * vm, double now_system );
void chuck_beat_refresh_deadline( Chuck_VM * vm );

#endif // CHUCK_BEAT_H

// This second section is deliberately outside the include guard: a translation
// unit may force-include this header before chuck_dl.h, then include it again
// after Chuck_DL_Query is complete.
#if defined(__CHUCK_DL_H__) && !defined(CHUCK_BEAT_QUERY_DECLARED)
#define CHUCK_BEAT_QUERY_DECLARED
CK_DLL_LINKAGE t_CKBOOL CK_DLL_CALL beatclock_query( Chuck_DL_Query * QUERY );
#endif
