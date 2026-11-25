// Regression tests for the built-in BeatClock timeline and VM scheduling.
// Each case compiles real ChucK source, renders audio through the embedded
// ChucK host, and checks complete PCM and/or concrete scheduler queues.

#include "task_support/check.h"
#include "task_support/audio_host.h"

#include "src/core/chuck_beat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{

struct Pulse
{
    int frame;
    float value;
};

struct CurveCalc
{
    double sr = 48000.0;
    double anchor_sample = 0.0;
    double anchor_beat = 0.0;
    double bpm0 = 120.0;
    double target = 120.0;
    double ramp_beats = 0.0;
    bool ramp = false;

    static CurveCalc constant( double bpm, double sample = 0.0, double beat = 0.0 )
    {
        CurveCalc c;
        c.bpm0 = c.target = bpm;
        c.anchor_sample = sample;
        c.anchor_beat = beat;
        return c;
    }

    double beat_at_sample( double sample ) const
    {
        const double C = 60.0 * sr;
        const double y = sample - anchor_sample;
        if( !ramp ) return anchor_beat + y * bpm0 / C;

        const double d = target - bpm0;
        double ramp_time;
        if( d == 0.0 ) ramp_time = C * ramp_beats / bpm0;
        else ramp_time = C * ramp_beats / d * std::log1p( d / bpm0 );

        if( y < ramp_time )
        {
            if( d == 0.0 ) return anchor_beat + y * bpm0 / C;
            const double B = bpm0 * std::exp( d * y / ( ramp_beats * C ) );
            return anchor_beat + ramp_beats * ( B - bpm0 ) / d;
        }
        return anchor_beat + ramp_beats + ( y - ramp_time ) * target / C;
    }

    double frame( double absolute_beat ) const
    {
        const double C = 60.0 * sr;
        const double x = absolute_beat - anchor_beat;
        double t;
        if( !ramp || x == 0.0 )
            t = anchor_sample + C * x / bpm0;
        else if( x >= ramp_beats )
        {
            const double d = target - bpm0;
            const double ramp_time = d == 0.0
                ? C * ramp_beats / bpm0
                : C * ramp_beats / d * std::log1p( d / bpm0 );
            t = anchor_sample + ramp_time + C * ( x - ramp_beats ) / target;
        }
        else
        {
            const double d = target - bpm0;
            if( d == 0.0 ) t = anchor_sample + C * x / bpm0;
            else t = anchor_sample + C * ramp_beats / d *
                     std::log1p( d * x / ( ramp_beats * bpm0 ) );
        }
        return (int)std::floor( t + 0.5 );
    }
};

bool check_pcm( const std::vector<float> & pcm,
                int frames,
                const std::vector<Pulse> & pulses )
{
    if( (int)pcm.size() != frames )
    {
        std::cerr << "pcm size " << pcm.size() << " expected " << frames << "\n";
        return false;
    }

    std::map<int, float> expected;
    for( const Pulse & p : pulses )
    {
        if( p.frame < 0 || p.frame >= frames )
        {
            std::cerr << "pulse frame out of range: " << p.frame << "\n";
            return false;
        }
        expected[p.frame] = p.value;
    }

    for( int i = 0; i < frames; ++i )
    {
        float want = 0.0f;
        std::map<int, float>::const_iterator it = expected.find( i );
        if( it != expected.end() ) want = it->second;
        if( !std::isfinite( pcm[i] ) || std::fabs( (double)pcm[i] - want ) > 1e-6 )
        {
            std::cerr << "PCM mismatch frame=" << i << " actual=" << pcm[i]
                      << " want=" << want << "\n";
            return false;
        }
    }
    return true;
}

bool same_pcm( const std::vector<float> & a, const std::vector<float> & b )
{
    if( a.size() != b.size() ) return false;
    for( size_t i = 0; i < a.size(); ++i )
        if( std::fabs( (double)a[i] - (double)b[i] ) > 1e-6 ) return false;
    return true;
}

bool check_matrix( const std::string & code,
                   int frames,
                   const std::vector<Pulse> & pulses )
{
    std::vector<float> reference;
    for( int adaptive_i = 0; adaptive_i < 2; ++adaptive_i )
    {
        bool adaptive = adaptive_i != 0;
        const int blocks[3] = { 1, 7, 113 };
        for( int block : blocks )
        {
            std::vector<float> pcm;
            try
            {
                pcm = task_render( code, frames, block, adaptive );
            }
            catch( const std::exception & e )
            {
                std::cerr << "render failed block=" << block
                          << " adaptive=" << adaptive << ": " << e.what() << "\n";
                return false;
            }
            if( !check_pcm( pcm, frames, pulses ) )
            {
                std::cerr << "failed matrix block=" << block
                          << " adaptive=" << adaptive << "\n";
                return false;
            }
            if( reference.empty() ) reference = pcm;
            else if( !same_pcm( reference, pcm ) )
            {
                std::cerr << "PCM differs across block/adaptive matrix\n";
                return false;
            }
        }
    }
    return true;
}

// A host retained by a test so scheduler queues can be inspected mid-render.
struct ProbeHost
{
    std::unique_ptr<ChucK> ck;
    std::vector<float> out;
    int rendered = 0;
    int block = 113;
    bool adaptive = true;

    bool start( const std::string & code, int block_size = 113, bool use_adaptive = true )
    {
        block = block_size;
        adaptive = use_adaptive;
        ck = std::make_unique<ChucK>();
        ck->setParam( CHUCK_PARAM_SAMPLE_RATE, 48000 );
        ck->setParam( CHUCK_PARAM_INPUT_CHANNELS, 1 );
        ck->setParam( CHUCK_PARAM_OUTPUT_CHANNELS, 1 );
        ck->setParam( CHUCK_PARAM_VM_HALT, FALSE );
        ck->setParam( CHUCK_PARAM_CHUGIN_ENABLE, FALSE );
        ck->setParam( CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, FALSE );
        ck->setParam( CHUCK_PARAM_VM_ADAPTIVE, adaptive ? 512 : 0 );
        if( !ck->init() || !ck->start() ) return false;
        return ck->compileCode( code );
    }

    bool render_to( int target_frame )
    {
        if( !ck ) return false;
        std::vector<SAMPLE> in( block, 0 ), outbuf( block, 0 );
        while( rendered < target_frame )
        {
            int n = std::min( block, target_frame - rendered );
            std::fill( outbuf.begin(), outbuf.end(), 0 );
            ck->run( in.data(), outbuf.data(), n );
            out.insert( out.end(), outbuf.begin(), outbuf.begin() + n );
            rendered += n;
        }
        return true;
    }

    Chuck_VM * vm() { return ck ? ck->vm() : nullptr; }
};

} // namespace



static int timing_constant_tempo()
{
    CurveCalc c = CurveCalc::constant( 120.0 );
    const int f1 = c.frame( 1.0 );
    const int f2 = c.frame( 2.5 );
    TASK_CHECK( f1 == 24000 );
    TASK_CHECK( f2 == 60000 );

    const std::string code =
        "BeatClock c; Step s => dac; 0.0 => s.next;"
        "c.at(1.0) => now; 0.25 => s.next; 1::samp => now; 0.0 => s.next;"
        "c.at(2.5) => now; -0.5 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, 61000, { { f1, 0.25f }, { f2, -0.5f } } ) );
    return 0;
}

static int timing_slow_constant()
{
    CurveCalc c = CurveCalc::constant( 60.0 );
    const int f1 = c.frame( 0.5 );
    const int f2 = c.frame( 1.25 );
    TASK_CHECK( f1 == 24000 );
    TASK_CHECK( f2 == 60000 );

    const std::string code =
        "BeatClock c; c.bpm(60.0); Step s => dac; 0.0 => s.next;"
        "c.at(0.5) => now; 0.2 => s.next; 1::samp => now; 0.0 => s.next;"
        "c.at(1.25) => now; 0.4 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, 61000, { { f1, 0.2f }, { f2, 0.4f } } ) );
    return 0;
}

static int timing_late_creation_anchor()
{
    // The clock's beat zero is the VM time when the object is constructed.
    const int f = 100 + 24000;
    const std::string code =
        "100::samp => now; BeatClock c; Step s => dac; 0.0 => s.next;"
        "c.at(1.0) => now; 0.25 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, f + 10, { { f, 0.25f } } ) );
    return 0;
}

static int timing_getters()
{
    const std::string code =
        "BeatClock c; Step s => dac; 0.0 => s.next;"
        "1 => int ok;"
        "if( c.bpm() != 120.0 || c.beat() != 0.0 ) { 0 => ok; }"
        "100::samp => now;"
        "100.0/24000.0 => float expected;"
        "if( c.beat() < expected - 1e-9 || c.beat() > expected + 1e-9 ) { 0 => ok; }"
        "if( c.bpm(96.0) != 96.0 || c.bpm() != 96.0 ) { 0 => ok; }"
        "ok => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, 200, { { 100, 1.0f } } ) );
    return 0;
}

static int timing_expired_rewait()
{
    CurveCalc c = CurveCalc::constant( 120.0 );
    const int f = c.frame( 1.0 );
    TASK_CHECK( f == 24000 );

    const std::string code =
        "BeatClock c; Step s => dac; 0.0 => s.next; Event @ e; c.at(1.0) @=> e;"
        "e => now; 0.25 => s.next; 1::samp => now; 0.0 => s.next; 1::samp => now;"
        "e => now; 0.30 => s.next; 1::samp => now; 0.0 => s.next; 1::samp => now;"
        "e => now; -0.20 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, f + 10,
        { { f, 0.25f }, { f + 2, 0.30f }, { f + 4, -0.20f } } ) );
    return 0;
}

static int timing_same_frame_plain_wait()
{
    CurveCalc c = CurveCalc::constant( 120.0 );
    const int f = c.frame( 1.0 );
    TASK_CHECK( f == 24000 );

    const std::string code =
        "fun void beatPart(BeatClock c, Step s) {"
        "  c.at(1.0) => now; 0.25 => s.next; 1::samp => now; 0.0 => s.next;"
        "}"
        "BeatClock c; Step s => dac; 0.0 => s.next;"
        "spork ~ beatPart(c, s);"
        "24000::samp => now; 0.10 => s.next;"
        "50::samp => now; 0.10 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, f + 70,
        { { f, 0.25f }, { f + 50, 0.10f } } ) );
    return 0;
}



static int ramp_up_example_math()
{
    CurveCalc c;
    c.anchor_sample = 0.0; c.anchor_beat = 0.0;
    c.bpm0 = 60.0; c.target = 120.0; c.ramp_beats = 2.0; c.ramp = true;
    const int f2 = c.frame( 2.0 );
    const int f3 = c.frame( 3.0 );
    TASK_CHECK( f2 == 66542 );
    TASK_CHECK( f3 == 90542 );

    const std::string code =
        "BeatClock c; c.bpm(60.0); c.ramp(120.0, 2.0);"
        "Step s => dac; 0.0 => s.next;"
        "c.at(2.0) => now; 0.25 => s.next; 1::samp => now; 0.0 => s.next;"
        "c.at(3.0) => now; -0.5 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, 91000, { { f2, 0.25f }, { f3, -0.5f } } ) );
    return 0;
}

static int ramp_down()
{
    CurveCalc c;
    c.bpm0 = 120.0; c.target = 60.0; c.ramp_beats = 2.0; c.ramp = true;
    const int f1 = c.frame( 1.0 );
    const int f2 = c.frame( 2.0 );
    const int f3 = c.frame( 3.0 );
    TASK_CHECK( f1 > 24000 && f1 < 48000 );
    TASK_CHECK( f2 == 66542 );
    TASK_CHECK( f3 == 114542 );

    const std::string code =
        "BeatClock c; c.ramp(60.0, 2.0);"
        "Step s => dac; 0.0 => s.next;"
        "c.at(1.0) => now; 0.2 => s.next; 1::samp => now; 0.0 => s.next;"
        "c.at(2.0) => now; 0.3 => s.next; 1::samp => now; 0.0 => s.next;"
        "c.at(3.0) => now; -0.4 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, 145000,
        { { f1, 0.2f }, { f2, 0.3f }, { f3, -0.4f } } ) );
    return 0;
}

static int ramp_near_constant()
{
    CurveCalc c;
    c.bpm0 = 120.0; c.target = 120.0001; c.ramp_beats = 4.0; c.ramp = true;
    const int f1 = c.frame( 1.0 );
    const int f25 = c.frame( 2.5 );
    TASK_CHECK( std::abs( f1 - 24000 ) <= 1 );
    TASK_CHECK( std::abs( f25 - 60000 ) <= 1 );

    const std::string code =
        "BeatClock c; c.ramp(120.0001, 4.0);"
        "Step s => dac; 0.0 => s.next;"
        "c.at(1.0) => now; 0.25 => s.next; 1::samp => now; 0.0 => s.next;"
        "c.at(2.5) => now; -0.25 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, 61000, { { f1, 0.25f }, { f25, -0.25f } } ) );
    return 0;
}

static int ramp_midcourse_replan()
{
    // Old curve: 60 -> 120 over beats [0,2].  At exactly 30000 samples,
    // preserve the true beat and switch to constant 120.
    CurveCalc old_curve;
    old_curve.bpm0 = 60.0; old_curve.target = 120.0;
    old_curve.ramp_beats = 2.0; old_curve.ramp = true;
    const double current_beat = old_curve.beat_at_sample( 30000.0 );
    TASK_CHECK( current_beat > 0.70 && current_beat < 0.76 );

    CurveCalc next = CurveCalc::constant( 120.0, 30000.0, current_beat );
    const int new_frame = next.frame( 3.0 );
    const int old_frame = old_curve.frame( 3.0 );
    TASK_CHECK( new_frame > 80000 && new_frame < old_frame );

    const std::string code =
        "fun void pulse(Event e, Step s, float v) {"
        " e => now; v => s.next; 1::samp => now; 0.0 => s.next;"
        "}"
        "BeatClock c; c.bpm(60.0); c.ramp(120.0, 2.0);"
        "Step s => dac; 0.0 => s.next; Event @ e; c.at(3.0) @=> e;"
        "spork ~ pulse(e, s, 0.25);"
        "30000::samp => now; c.bpm(120.0);"
        "2::second => now;";
    TASK_CHECK( check_matrix( code, new_frame + 20, { { new_frame, 0.25f } } ) );
    return 0;
}



static int lifecycle_multiple_clocks_waiters()
{
    CurveCalc fast = CurveCalc::constant( 120.0 );
    CurveCalc slow = CurveCalc::constant( 60.0 );
    const int f_fast = fast.frame( 1.0 );
    const int f_slow = slow.frame( 1.0 );
    TASK_CHECK( f_fast == 24000 );
    TASK_CHECK( f_slow == 48000 );

    const std::string valid =
        "fun void pulse(Event e, Step s, float v, float delay) {"
        " e => now; delay::samp => now; v => s.next;"
        " 1::samp => now; 0.0 => s.next;"
        "}"
        "BeatClock a; BeatClock b; b.bpm(60.0);"
        "Step s => dac; 0.0 => s.next;"
        "Event @ ea; a.at(1.0) @=> ea;"
        "spork ~ pulse(ea, s, 0.20, 0.0);"
        "spork ~ pulse(ea, s, 0.30, 2.0);"
        "b.at(1.0) => now; 0.40 => s.next; 1::samp => now; 0.0 => s.next;"
        "2::second => now;";

    // At f_fast two waiters continue with distinct post-wait offsets.
    TASK_CHECK( check_matrix( valid, f_slow + 10,
        { { f_fast, 0.20f }, { f_fast + 2, 0.30f }, { f_slow, 0.40f } } ) );
    return 0;
}

static int lifecycle_event_owns_clock_scene()
{
    const std::string code =
        "fun Event makeBeatEvent() {"
        " BeatClock c; c.bpm(120.0); return c.at(1.0);"
        "}"
        "Event @ e; makeBeatEvent() @=> e;"
        "Step s => dac; 0.0 => s.next;"
        "e => now; 0.33 => s.next; 1::samp => now; 0.0 => s.next;"
        "1::second => now;";
    TASK_CHECK( check_matrix( code, 24010, { { 24000, 0.33f } } ) );
    return 0;
}

static int lifecycle_wait_order_counts()
{
    ProbeHost host;
    const std::string code =
        "fun void waitOnly(Event e, float d) { d::samp => now; e => now; }"
        "BeatClock c;"
        "Event @ e1; c.at(1.0) @=> e1;"
        "Event @ e2; c.at(1.0) @=> e2;"
        "Event @ e3; c.at(1.0) @=> e3;"
        "spork ~ waitOnly(e1, 30.0);"
        "spork ~ waitOnly(e2, 10.0);"
        "spork ~ waitOnly(e3, 20.0);"
        "10::second => now;";
    TASK_CHECK( host.start( code, 113, true ) );
    TASK_CHECK( host.render_to( 100 ) );
    Chuck_VM * vm = host.vm();
    TASK_CHECK( vm != nullptr );

    ChuckBeatStats stats = chuck_beat_stats( vm );
    TASK_CHECK( stats.clocks == 1 );
    TASK_CHECK( stats.live_events >= 3 );
    TASK_CHECK( stats.active_events == 3 );
    TASK_CHECK( stats.active_waiters == 3 );
    TASK_CHECK( vm->shreduler()->blocked.size() == 3 );

    unsigned long long order[8] = {};
    TASK_CHECK( chuck_beat_active_order( vm, order, 8 ) == 3 );
    TASK_CHECK( order[0] == 1 && order[1] == 2 && order[2] == 3 );

    TASK_CHECK( host.render_to( 24100 ) );
    stats = chuck_beat_stats( vm );
    TASK_CHECK( stats.active_events == 0 );
    TASK_CHECK( stats.active_waiters == 0 );
    TASK_CHECK( stats.signal_count == 3 );

    unsigned long long wake[8] = {};
    size_t wake_n = chuck_beat_wake_order( vm, wake, 8 );
    TASK_CHECK( wake_n == 3 );
    TASK_CHECK( ( wake[0] >> 32 ) == 1 );
    TASK_CHECK( ( wake[1] >> 32 ) == 2 );
    TASK_CHECK( ( wake[2] >> 32 ) == 3 );
    return 0;
}

static int lifecycle_remove_waiter()
{
    ProbeHost host;
    const std::string code =
        "fun void waitOnly(Event e) { e => now; }"
        "BeatClock c; Event @ e; c.at(10.0) @=> e;"
        "spork ~ waitOnly(e); spork ~ waitOnly(e); spork ~ waitOnly(e);"
        "10::second => now;";
    TASK_CHECK( host.start( code, 7, false ) );
    TASK_CHECK( host.render_to( 100 ) );
    Chuck_VM * vm = host.vm();
    TASK_CHECK( vm != nullptr );

    TASK_CHECK( chuck_beat_stats( vm ).active_waiters == 3 );
    TASK_CHECK( vm->shreduler()->blocked.size() == 3 );

    auto it = vm->shreduler()->blocked.begin();
    TASK_CHECK( it != vm->shreduler()->blocked.end() );
    const t_CKUINT removed_id = it->second->xid;
    Chuck_Msg * remove_msg = new Chuck_Msg;
    remove_msg->type = CK_MSG_REMOVE;
    remove_msg->param = removed_id;
    TASK_CHECK( vm->queue_msg( remove_msg ) == TRUE );
    TASK_CHECK( host.render_to( 101 ) );

    TASK_CHECK( chuck_beat_stats( vm ).active_waiters == 2 );
    TASK_CHECK( chuck_beat_stats( vm ).active_events == 1 );
    TASK_CHECK( vm->shreduler()->blocked.size() == 2 );

    // Rendering well past beat 10 (240000 frames) wakes the two survivors.
    TASK_CHECK( host.render_to( 240100 ) );
    ChuckBeatStats stats = chuck_beat_stats( vm );
    TASK_CHECK( stats.active_waiters == 0 );
    TASK_CHECK( stats.signal_count == 2 );
    return 0;
}

static int lifecycle_invalid_args_and_other_shreds()
{
    // The waiting shred must survive the root shred's InvalidArgument
    // exception, and the pre-existing 120 BPM curve/wait must be unchanged.
    const std::string bad_bpm =
        "fun void waitPulse(BeatClock c, Step s) {"
        " c.at(1.0) => now; 0.25 => s.next; 1::samp => now; 0.0 => s.next;"
        "}"
        "fun void badBpm(BeatClock c) { c.bpm(-3.0); }"
        "BeatClock c; Step s => dac; 0.0 => s.next;"
        "spork ~ waitPulse(c, s); spork ~ badBpm(c);"
        "999::second => now;";;
    TASK_CHECK( check_matrix( bad_bpm, 24010, { { 24000, 0.25f } } ) );

    const std::string bad_ramp =
        "BeatClock c; c.ramp(0.0, 2.0); 1::second => now;";
    TASK_CHECK( task_render( bad_ramp, 128, 113 ).size() == 128 );

    const std::string bad_ramp_len =
        "BeatClock c; c.ramp(180.0, 0.0); 1::second => now;";
    TASK_CHECK( task_render( bad_ramp_len, 128, 113 ).size() == 128 );

    const std::string bad_at =
        "BeatClock c; c.at(-0.5) => now; 1::second => now;";
    TASK_CHECK( task_render( bad_at, 128, 113 ).size() == 128 );

    // A separate surviving shred proves the VM continues after at() throws.
    const std::string at_survivor =
        "fun void alive(Step s) {"
        " 100::samp => now; 0.75 => s.next; 1::samp => now; 0.0 => s.next;"
        "}"
        "fun void badAt(BeatClock c) { c.at(-1.0); }"
        "Step s => dac; 0.0 => s.next; spork ~ alive(s);"
        "BeatClock c; spork ~ badAt(c);"
        "999::second => now;";;
    TASK_CHECK( check_matrix( at_survivor, 130, { { 100, 0.75f } } ) );
    return 0;
}



extern const struct task_case task_cases[] =
{
    { "timing", "constant_tempo", timing_constant_tempo },
    { "timing", "slow_constant", timing_slow_constant },
    { "timing", "late_creation_anchor", timing_late_creation_anchor },
    { "timing", "getters", timing_getters },
    { "timing", "expired_rewait", timing_expired_rewait },
    { "timing", "same_frame_plain_wait", timing_same_frame_plain_wait },

    { "ramp", "up_example_math", ramp_up_example_math },
    { "ramp", "down", ramp_down },
    { "ramp", "near_constant", ramp_near_constant },
    { "ramp", "midcourse_replan", ramp_midcourse_replan },

    { "lifecycle", "multiple_clocks_waiters", lifecycle_multiple_clocks_waiters },
    { "lifecycle", "event_owns_clock_scene", lifecycle_event_owns_clock_scene },
    { "lifecycle", "wait_order_counts", lifecycle_wait_order_counts },
    { "lifecycle", "remove_waiter", lifecycle_remove_waiter },
    { "lifecycle", "invalid_args_and_other_shreds", lifecycle_invalid_args_and_other_shreds },
};

extern const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
