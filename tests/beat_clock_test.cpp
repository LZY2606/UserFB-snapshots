#include "task_support/check.h"
#include "task_support/audio_host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr int SR = 48000;

struct Pulse
{
    int frame;
    float value;
};

bool near_float( float a, float b, float eps = 1e-6f )
{
    return std::fabs( (double) a - (double) b ) <= eps;
}

std::vector<Pulse> find_pulses( const std::vector<float> & pcm )
{
    std::vector<Pulse> out;
    for( int i = 0; i < (int) pcm.size(); ++i )
    {
        if( std::isfinite( pcm[i] ) && std::fabs( pcm[i] ) > 1e-7f )
            out.push_back( { i, pcm[i] } );
    }
    return out;
}

bool compare_pcm( const std::vector<float> & a, const std::vector<float> & b )
{
    if( a.size() != b.size() )
    {
        std::fprintf( stderr, "PCM size mismatch %zu vs %zu\n", a.size(), b.size() );
        return false;
    }
    for( size_t i = 0; i < a.size(); ++i )
    {
        if( !std::isfinite( a[i] ) || !std::isfinite( b[i] ) ||
            std::fabs( (double) a[i] - (double) b[i] ) > 1e-6 )
        {
            std::fprintf( stderr, "PCM mismatch at %zu: %.9f vs %.9f\n",
                          i, a[i], b[i] );
            return false;
        }
    }
    return true;
}

// Render all required host chunk/adaptive combinations and compare complete
// PCM.  Returns the 113-frame/adaptive reference in ref.
bool matrix_render( const std::string & code, int frames,
                    std::vector<float> & ref )
{
    static const int blocks[3] = { 113, 7, 1 };
    std::vector<std::vector<float>> all;
    all.reserve( 6 );

    for( int adaptive = 1; adaptive >= 0; --adaptive )
    {
        for( int bi = 0; bi < 3; ++bi )
        {
            std::vector<float> pcm;
            try
            {
                pcm = task_render( code, frames, blocks[bi], adaptive != 0 );
            }
            catch( const std::exception & e )
            {
                std::fprintf( stderr, "render failed block=%d adaptive=%d: %s\n",
                               blocks[bi], adaptive, e.what() );
                return false;
            }
            if( (int) pcm.size() != frames ) return false;
            if( all.empty() ) ref = pcm;
            if( !compare_pcm( ref, pcm ) )
            {
                std::fprintf( stderr, "matrix failure block=%d adaptive=%d\n",
                               blocks[bi], adaptive );
                return false;
            }
            all.push_back( std::move( pcm ) );
        }
    }
    return true;
}

bool check_pulse_frames( const std::vector<float> & pcm,
                         const std::vector<int> & expected_frames,
                         const std::vector<float> & expected_values )
{
    std::vector<Pulse> pulses = find_pulses( pcm );
    if( pulses.size() != expected_frames.size() ||
        expected_values.size() != expected_frames.size() )
    {
        std::fprintf( stderr, "pulse count %zu, expected frames %zu values %zu\n",
                      pulses.size(), expected_frames.size(),
                      expected_values.size() );
        for( const auto & p : pulses )
            std::fprintf( stderr, "  pulse frame=%d value=%.9f\n", p.frame, p.value );
        return false;
    }
    for( size_t i = 0; i < pulses.size(); ++i )
    {
        if( pulses[i].frame != expected_frames[i] )
        {
            std::fprintf( stderr, "pulse %zu frame %d, expected %d\n",
                          i, pulses[i].frame, expected_frames[i] );
            return false;
        }
        if( !near_float( pulses[i].value, expected_values[i] ) )
        {
            std::fprintf( stderr, "pulse %zu value %.9f, expected %.9f\n",
                          i, pulses[i].value, expected_values[i] );
            return false;
        }
    }
    return true;
}

long double half_up( long double x )
{
    return std::floor( x + 0.5L );
}

long double const_frame( double beat, double bpm, double start_frame = 0.0 )
{
    return half_up( start_frame + (long double) SR * 60.0L * beat / bpm );
}

long double ramp_exact( long double beat, long double v0, long double v1,
                        long double length, long double s0 = 0.0L,
                        long double b0 = 0.0L )
{
    long double x = beat - b0;
    long double s = s0;
    if( x <= 0.0L ) return s0;
    if( length <= 0.0L || v0 == v1 )
        return s0 + (long double) SR * 60.0L * x / v0;

    if( x <= length )
    {
        long double v = v0 + ( v1 - v0 ) * ( x / length );
        s += (long double) SR * 60.0L * length / ( v1 - v0 )
            * std::log( v / v0 );
    }
    else
    {
        long double end = (long double) SR * 60.0L * length / ( v1 - v0 )
            * std::log( v1 / v0 );
        s += end + (long double) SR * 60.0L * ( x - length ) / v1;
    }
    return s;
}

int ramp_frame( double beat, double v0, double v1, double length )
{
    return (int) half_up( ramp_exact( beat, v0, v1, length ) );
}

int replaced_ramp_frame( double target_beat, double call_frame,
                         double old_v0, double old_v1, double old_length,
                         double new_target, double new_length )
{
    // Invert the old beat-from-sample curve at the integer call frame.
    long double f = call_frame;
    long double lo = 0.0L, hi = old_length;
    for( int i = 0; i < 80; ++i )
    {
        long double mid = ( lo + hi ) * 0.5L;
        long double s = ramp_exact( mid, old_v0, old_v1, old_length );
        if( s < f ) lo = mid; else hi = mid;
    }
    long double current_beat = ( lo + hi ) * 0.5L;
    long double current_bpm = old_v0 +
        ( old_v1 - old_v0 ) * ( current_beat / old_length );
    if( new_length <= 0.0 )
    {
        long double exact = call_frame +
            (long double) SR * 60.0L * ( target_beat - current_beat ) / new_target;
        return (int) half_up( exact );
    }
    return (int) half_up( ramp_exact( target_beat, (double) current_bpm,
                                      new_target, new_length,
                                      call_frame, (double) current_beat ) );
}

const char * MARK_FUN = R"CK(
fun void mark(Step s, float v) {
    v => s.next;
    1::samp => now;
    0 => s.next;
}
)CK";

int timing_constant_120()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.at(0.5) => now; mark(s, 0.10);
c.at(1.0) => now; mark(s, 0.20);
c.at(1.5) => now; mark(s, 0.30);
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 40000, pcm ) );
    int f1 = (int) const_frame( 0.5, 120 );
    int f2 = (int) const_frame( 1.0, 120 );
    int f3 = (int) const_frame( 1.5, 120 );
    TASK_CHECK( check_pulse_frames( pcm, { f1, f2, f3 }, { .10f, .20f, .30f } ) );
    return 0;
}

int timing_constant_90_rounding_and_getters()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(90.0);
(0.25 * c.bpm()) => float expectedBpm;
c.at(1.0/3.0) => now;
mark(s, expectedBpm);
100::samp => now;
mark(s, c.beat());
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 36000, pcm ) );
    int f1 = (int) const_frame( 1.0 / 3.0, 90 );
    std::vector<Pulse> pulses = find_pulses( pcm );
    TASK_CHECK( pulses.size() == 2 );
    TASK_CHECK( pulses[0].frame == f1 );
    TASK_CHECK( near_float( pulses[0].value, 22.5f ) );
    TASK_CHECK( pulses[1].frame == f1 + 1 + 100 );
    float expected_beat = pulses[1].frame * 90.0f / ( 60.0f * 48000.0f );
    TASK_CHECK( near_float( pulses[1].value, expected_beat, 1e-6f ) );
    return 0;
}

int timing_example_exact_frames()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(60.0);
c.ramp(120.0, 2.0);
c.at(2.0) => now; mark(s, 0.25);
c.at(3.0) => now; mark(s, -0.50);
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 100000, pcm ) );
    TASK_CHECK( check_pulse_frames( pcm, { 66542, 90542 }, { .25f, -.50f } ) );
    return 0;
}

int timing_multiple_clocks()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock a;
BeatClock b;
Step s => dac;
0 => s.next;
a.bpm(120.0);
b.bpm(60.0);
spork ~ wait_mark(a.at(1.0), s, 0.11);
spork ~ wait_mark(b.at(0.25), s, 0.22);
40000::samp => now;

fun void wait_mark(Event e, Step s, float v) {
    e => now;
    mark(s, v);
}
)CK");
    // Function is textually after use but ChucK permits top-level functions.
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 40000, pcm ) );
    int fa = (int) const_frame( 1, 120 );
    int fb = (int) const_frame( .25, 60 );
    std::vector<Pulse> pulses = find_pulses( pcm );
    TASK_CHECK( pulses.size() == 2 );
    TASK_CHECK( pulses[0].frame == fb && near_float( pulses[0].value, .22f ) );
    TASK_CHECK( pulses[1].frame == fa && near_float( pulses[1].value, .11f ) );
    return 0;
}

int ramp_down_and_beyond_end()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(120.0);
c.ramp(60.0, 2.0);
c.at(1.0) => now; mark(s, 0.31);
c.at(2.0) => now; mark(s, 0.41);
c.at(2.5) => now; mark(s, 0.51);
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 130000, pcm ) );
    int f1 = ramp_frame( 1, 120, 60, 2 );
    int f2 = ramp_frame( 2, 120, 60, 2 );
    int f3 = ramp_frame( 2.5, 120, 60, 2 );
    TASK_CHECK( check_pulse_frames( pcm, { f1, f2, f3 },
                                    { .31f, .41f, .51f } ) );
    return 0;
}

int ramp_hold_getters_after_end()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(120.0);
c.ramp(60.0, 2.0);
80000::samp => now;
mark(s, c.beat());
100::samp => now;
mark(s, c.bpm() / 100.0);
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 82000, pcm ) );
    long double end_sample = ramp_exact( 2, 120, 60, 2 );
    float expected_beat = (float)( 2.0L + ( 80000.0L - end_sample ) / 48000.0L );
    TASK_CHECK( check_pulse_frames( pcm, { 80000, 80101 },
                                    { expected_beat, 0.60f } ) );
    return 0;
}

int ramp_near_constant()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(120.0);
c.ramp(120.001, 4.0);
c.at(1.0) => now; mark(s, 0.17);
c.at(3.0) => now; mark(s, 0.27);
c.at(5.0) => now; mark(s, 0.37);
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 120000, pcm ) );
    int f1 = ramp_frame( 1, 120, 120.001, 4 );
    int f3 = ramp_frame( 3, 120, 120.001, 4 );
    int f5 = ramp_frame( 5, 120, 120.001, 4 );
    TASK_CHECK( check_pulse_frames( pcm, { f1, f3, f5 },
                                    { .17f, .27f, .37f } ) );
    return 0;
}

int ramp_midcourse_replaces_blocked_deadlines()
{
    constexpr int CALL_FRAME = 20000;
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(60.0);
c.ramp(120.0, 2.0);
c.at(2.0) @=> Event e2;
c.at(3.0) @=> Event e3;
spork ~ wait_mark(e2, s, 0.25);
spork ~ wait_mark(e3, s, -0.50);
20000::samp => now;
c.ramp(60.0, 2.0);
120000::samp => now;

fun void wait_mark(Event e, Step s, float v) {
    e => now;
    mark(s, v);
}
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 140000, pcm ) );
    int f2 = replaced_ramp_frame( 2.0, CALL_FRAME, 60, 120, 2, 60, 2 );
    int f3 = replaced_ramp_frame( 3.0, CALL_FRAME, 60, 120, 2, 60, 2 );
    TASK_CHECK( f2 > CALL_FRAME && f3 > f2 );
    TASK_CHECK( check_pulse_frames( pcm, { f2, f3 }, { .25f, -.50f } ) );
    return 0;
}

int ramp_bpm_setter_cancels_ramp()
{
    constexpr int CALL_FRAME = 30000;
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(60.0);
c.ramp(180.0, 4.0);
c.at(2.0) @=> Event e;
spork ~ wait_mark(e, s, 0.77);
30000::samp => now;
c.bpm(120.0);
60000::samp => now;

fun void wait_mark(Event e, Step s, float v) {
    e => now;
    mark(s, v);
}
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 70000, pcm ) );
    int expected = replaced_ramp_frame( 2.0, CALL_FRAME, 60, 180, 4, 120, 0 );
    TASK_CHECK( check_pulse_frames( pcm, { expected }, { .77f } ) );
    return 0;
}

int lifecycle_late_wait_is_immediate()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.at(0.25) @=> Event e;
50000::samp => now;
e => now;
mark(s, 0.42);
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 52000, pcm ) );
    // The wait starts at frame 50000 and must continue there, not block until
    // the next (non-existent) broadcast.
    TASK_CHECK( check_pulse_frames( pcm, { 50000 }, { .42f } ) );
    return 0;
}

int lifecycle_completed_event_can_wait_again()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.at(0.5) @=> Event e;
e => now; mark(s, 0.21);
e => now; mark(s, 0.31);
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 26000, pcm ) );
    int f = (int) const_frame( .5, 120 );
    TASK_CHECK( check_pulse_frames( pcm, { f, f + 1 }, { .21f, .31f } ) );
    return 0;
}

int lifecycle_clock_variable_can_be_dropped()
{
    std::string code = MARK_FUN + std::string(R"CK(
Step s => dac;
0 => s.next;
make_event() @=> Event e;
e => now;
mark(s, 0.63);

fun Event make_event() {
    BeatClock c;
    c.bpm(60.0);
    return c.at(1.0);
}
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 50000, pcm ) );
    TASK_CHECK( check_pulse_frames( pcm, { 48000 }, { .63f } ) );
    return 0;
}

int lifecycle_same_frame_order_and_multiple_waiters()
{
    constexpr int F = 12000;
    std::string code = R"CK(
BeatClock c;
Step s => dac;
int log[0];
0 => s.next;

spork ~ ordinary(1, 12000::samp, log);
spork ~ ordinary(2, 12000::samp, log);
spork ~ ordinary(3, 12000::samp, log);

c.at(0.5) @=> Event b2;
spork ~ beat_wait(4, b2, log);
spork ~ beat_wait(5, b2, log);
c.at(0.5) @=> Event b1;
spork ~ beat_wait(6, b1, log);
spork ~ beat_wait(7, b1, log);

12001::samp => now;
for( 0 => int i; i < log.size(); i++ ) {
    (0.01 * log[i]) => s.next;
    1::samp => now;
    0 => s.next;
    4::samp => now;
}

fun void ordinary(int id, dur d, int log[]) {
    d => now;
    log << id;
}
fun void beat_wait(int id, Event e, int log[]) {
    e => now;
    log << id;
}
)CK";
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 12000 + 8 * 6 + 20, pcm ) );
    std::vector<Pulse> pulses = find_pulses( pcm );
    TASK_CHECK( pulses.size() == 7 );
    for( int i = 0; i < 7; ++i )
    {
        TASK_CHECK( near_float( pulses[i].value, 0.01f * ( i + 1 ), 1e-6f ) );
        if( i ) TASK_CHECK( pulses[i].frame > pulses[i - 1].frame );
    }
    return 0;
}

int lifecycle_remove_waiter_and_other_shreds_continue()
{
    using namespace ChuckBeat;
    unsigned long long removed_before = test_counter( COUNTER_REMOVED_WAITERS );
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
int id[1];
0 => s.next;
c.at(100.0) @=> Event e;
spork ~ forever_wait(e, id);
100::samp => now;
Machine.remove(id[0]);
1000::samp => now;
mark(s, 0.88);

fun void forever_wait(Event e, int id[]) {
    me.id() => id[0];
    e => now;
}
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 2000, pcm ) );
    TASK_CHECK( check_pulse_frames( pcm, { 1100 }, { .88f } ) );
    unsigned long long removed_after = test_counter( COUNTER_REMOVED_WAITERS );
    TASK_CHECK( removed_after >= removed_before + 1 );
    return 0;
}

int lifecycle_invalid_arguments_leave_curve_and_other_shreds()
{
    std::string code = MARK_FUN + std::string(R"CK(
BeatClock c;
Step s => dac;
0 => s.next;
c.bpm(60.0);
c.at(1.0) @=> Event e;
spork ~ bad(c, 0);
spork ~ bad(c, 1);
spork ~ bad(c, 2);
e => now;
mark(s, 0.91);
1000::samp => now;
mark(s, 0.19);

fun void bad(BeatClock c, int kind) {
    if( kind == 0 ) c.bpm(-2.0);
    if( kind == 1 ) c.ramp(120.0, 0.0);
    if( kind == 2 ) c.at(-0.25);
}
)CK");
    std::vector<float> pcm;
    TASK_CHECK( matrix_render( code, 50000, pcm ) );
    // Invalid calls must not replace the 60 BPM constant curve.
    TASK_CHECK( check_pulse_frames( pcm, { 48000, 48000 + 1 + 1000 },
                                    { .91f, .19f } ) );
    return 0;
}

int lifecycle_host_destruction_clears_registries()
{
    using namespace ChuckBeat;
    {
        std::vector<float> pcm;
        TASK_CHECK( matrix_render(
            "BeatClock c; c.at(2.0) @=> Event e; 100::samp => now;",
            256, pcm ) );
    }
    // task_render destroys its ChucK host before returning; all native objects
    // and beat side tables must be gone.
    TASK_CHECK( test_clock_count() == 0 );
    TASK_CHECK( test_event_record_count() == 0 );
    return 0;
}
} // namespace

const struct task_case task_cases[] = {
    { "timing", "constant_120_and_absolute_events", timing_constant_120 },
    { "timing", "constant_90_rounding_getters", timing_constant_90_rounding_and_getters },
    { "timing", "canonical_ramp_frames", timing_example_exact_frames },
    { "timing", "multiple_clocks", timing_multiple_clocks },

    { "ramp", "down_ramp_and_hold", ramp_down_and_beyond_end },
    { "ramp", "hold_getters_after_end", ramp_hold_getters_after_end },
    { "ramp", "near_constant_ramp", ramp_near_constant },
    { "ramp", "midcourse_reblocks_events", ramp_midcourse_replaces_blocked_deadlines },
    { "ramp", "bpm_setter_cancels_ramp", ramp_bpm_setter_cancels_ramp },

    { "lifecycle", "late_wait_immediate", lifecycle_late_wait_is_immediate },
    { "lifecycle", "completed_event_rewait", lifecycle_completed_event_can_wait_again },
    { "lifecycle", "dropped_clock_keeps_scene", lifecycle_clock_variable_can_be_dropped },
    { "lifecycle", "same_frame_order", lifecycle_same_frame_order_and_multiple_waiters },
    { "lifecycle", "remove_waiter", lifecycle_remove_waiter_and_other_shreds_continue },
    { "lifecycle", "invalid_arguments", lifecycle_invalid_arguments_leave_curve_and_other_shreds },
    { "lifecycle", "host_destruction_registry_empty", lifecycle_host_destruction_clears_registries },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
