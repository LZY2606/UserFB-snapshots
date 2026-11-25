/*----------------------------------------------------------------------------
  ChucK BeatClock implementation

  The tempo curve is a function from absolute beats to absolute VM samples.
  Changes anchor a new segment at the true current (time, beat, BPM), so
  pending absolute-beat Events always recompute from a single absolute anchor.
-----------------------------------------------------------------------------*/
#include "chuck_beat.h"

#include "chuck.h"
#include "chuck_instr.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
static const long double CK_BEAT_SECONDS_PER_MINUTE = 60.0L;

struct BeatCurve
{
    long double anchor_sample = 0.0L;
    long double anchor_beat = 0.0L;
    long double bpm0 = 120.0L;
    long double target_bpm = 120.0L;
    long double ramp_beats = 0.0L;
    bool ramp = false;

    static BeatCurve constant( long double sample, long double beat, long double bpm )
    {
        BeatCurve c;
        c.anchor_sample = sample;
        c.anchor_beat = beat;
        c.bpm0 = c.target_bpm = bpm;
        c.ramp_beats = 0.0L;
        c.ramp = false;
        return c;
    }

    long double conversion_constant( long double srate ) const
    {
        return CK_BEAT_SECONDS_PER_MINUTE * srate;
    }

    long double ramp_slope() const
    {
        return ramp ? ( target_bpm - bpm0 ) / ramp_beats : 0.0L;
    }

    long double ramp_sample_duration( long double srate ) const
    {
        if( !ramp || ramp_beats <= 0.0L ) return 0.0L;
        const long double C = conversion_constant( srate );
        const long double d = target_bpm - bpm0;
        if( d == 0.0L ) return C * ramp_beats / bpm0;
        // C * integral_0^L db / (bpm0 + d*b/L)
        return C * ramp_beats / d * log1pl( d / bpm0 );
    }

    long double beat_at_sample( long double sample, long double srate ) const
    {
        const long double C = conversion_constant( srate );
        const long double y = sample - anchor_sample;
        if( !ramp || y <= 0.0L )
        {
            // Before the anchor only the local tangent is meaningful; callers
            // use this to detect that an absolute target is already past.
            return anchor_beat + y * bpm0 / C;
        }

        const long double ramp_time = ramp_sample_duration( srate );
        if( y < ramp_time )
        {
            const long double d = target_bpm - bpm0;
            if( d == 0.0L )
                return anchor_beat + y * bpm0 / C;

            // B(y) = bpm0 * exp(k*y/C), k = d/L
            const long double B = bpm0 * expl( d * y / ( ramp_beats * C ) );
            return anchor_beat + ramp_beats * ( B - bpm0 ) / d;
        }

        const long double end_beat = anchor_beat + ramp_beats;
        return end_beat + ( y - ramp_time ) * target_bpm / C;
    }

    long double bpm_at_sample( long double sample, long double srate ) const
    {
        const long double y = sample - anchor_sample;
        if( !ramp || y <= 0.0L ) return bpm0;

        const long double ramp_time = ramp_sample_duration( srate );
        if( y >= ramp_time ) return target_bpm;

        const long double C = conversion_constant( srate );
        const long double d = target_bpm - bpm0;
        if( d == 0.0L ) return bpm0;
        return bpm0 * expl( d * y / ( ramp_beats * C ) );
    }

    long double sample_for_beat( long double beat, long double srate ) const
    {
        const long double C = conversion_constant( srate );
        const long double x = beat - anchor_beat;

        // A target before the current anchor is necessarily due.  Return a
        // safely-past absolute sample rather than extrapolating through a
        // possibly zero/negative tempo singularity.
        if( x < 0.0L )
            return anchor_sample - 1.0L;

        if( !ramp || x == 0.0L )
            return anchor_sample + C * x / bpm0;

        if( x >= ramp_beats )
        {
            const long double ramp_time = ramp_sample_duration( srate );
            return anchor_sample + ramp_time + C * ( x - ramp_beats ) / target_bpm;
        }

        const long double d = target_bpm - bpm0;
        if( d == 0.0L )
            return anchor_sample + C * x / bpm0;

        const long double z = d * x / ( ramp_beats * bpm0 );
        return anchor_sample + C * ramp_beats / d * log1pl( z );
    }

    long long frame_for_beat( long double beat, long double srate ) const
    {
        const long double t = sample_for_beat( beat, srate );
        if( !std::isfinite( t ) )
            return std::numeric_limits<long long>::min();
        if( t <= (long double)std::numeric_limits<long long>::min() + 1 )
            return std::numeric_limits<long long>::min();
        if( t >= (long double)std::numeric_limits<long long>::max() )
            return std::numeric_limits<long long>::max();

        // Nearest frame; non-negative exact halves round up.
        const long double rounded = floorl( t + 0.5L );
        return (long long)rounded;
    }
};

struct ClockState
{
    Chuck_VM * vm = nullptr;
    long double srate = 48000.0L;
    BeatCurve curve;
    std::set<struct ChuckBeatEvent *> events;
};

struct ChuckBeatEvent : Chuck_Event
{
    std::shared_ptr<ClockState> clock;
    double target_beat = 0.0;
    unsigned long long wait_order = 0;
    std::set<Chuck_VM_Shred *> beat_waiters;
    bool completed = false;

    ChuckBeatEvent( std::shared_ptr<ClockState> c, double target )
        : clock( std::move(c) ), target_beat( target )
    {
    }

    virtual ~ChuckBeatEvent() override;

    long long deadline_frame() const
    {
        return clock ? clock->curve.frame_for_beat( target_beat, clock->srate )
                     : std::numeric_limits<long long>::min();
    }
};

struct Scheduler
{
    Chuck_VM * vm = nullptr;
    std::map<unsigned long long, ChuckBeatEvent *> active;
    unsigned long long next_order = 0;
    size_t signal_count = 0;
    size_t due_batch_count = 0;
    std::vector<unsigned long long> wake_order;
};

static std::map<Chuck_VM *, Scheduler *> g_schedulers;
static std::map<Chuck_Object *, std::shared_ptr<ClockState>> g_clocks;
static size_t g_live_events = 0;

static Scheduler * find_scheduler( Chuck_VM * vm, bool create = false )
{
    if( !vm ) return nullptr;
    std::map<Chuck_VM *, Scheduler *>::iterator it = g_schedulers.find( vm );
    if( it != g_schedulers.end() ) return it->second;
    if( !create ) return nullptr;
    Scheduler * s = new Scheduler;
    s->vm = vm;
    g_schedulers[vm] = s;
    return s;
}

static void forget_event( ChuckBeatEvent * event )
{
    if( !event ) return;

    Chuck_VM * vm = event->clock ? event->clock->vm : nullptr;
    if( event->clock ) event->clock->events.erase( event );

    Scheduler * s = find_scheduler( vm );
    if( s && event->wait_order != 0 )
        s->active.erase( event->wait_order );
}

ChuckBeatEvent::~ChuckBeatEvent()
{
    forget_event( this );
    if( g_live_events > 0 ) --g_live_events;
}

static long long now_frame( Chuck_VM * vm )
{
    if( !vm ) return 0;
    return (long long)std::llround( (long double)vm->now() );
}

static void throw_invalid( CK_DL_API api, Chuck_VM_Shred * shred, const char * message )
{
    if( api && api->vm && api->vm->throw_exception )
        api->vm->throw_exception( "InvalidArgument", message, shred );
    else if( shred )
        ck_throw_exception( shred, "InvalidArgument", message );
}

} // anonymous namespace



//-----------------------------------------------------------------------------
// native BeatClock methods
//-----------------------------------------------------------------------------
CK_DLL_CTOR( beatclock_ctor )
{
    std::shared_ptr<ClockState> state = std::make_shared<ClockState>();
    state->vm = VM;
    state->srate = VM ? (long double)VM->srate() : 48000.0L;
    const long double now = VM ? (long double)VM->now() : 0.0L;
    state->curve = BeatCurve::constant( now, 0.0L, 120.0L );
    g_clocks[SELF] = state;
}

CK_DLL_DTOR( beatclock_dtor )
{
    g_clocks.erase( SELF );
}

CK_DLL_MFUN( beatclock_beat )
{
    std::map<Chuck_Object *, std::shared_ptr<ClockState>>::iterator it = g_clocks.find( SELF );
    if( it == g_clocks.end() || !it->second )
    {
        throw_invalid( API, SHRED, "BeatClock internal state is missing" );
        RETURN->v_float = 0.0;
        return;
    }
    const long double now = VM ? (long double)VM->now() : it->second->curve.anchor_sample;
    RETURN->v_float = (t_CKFLOAT)it->second->curve.beat_at_sample( now, it->second->srate );
}

CK_DLL_MFUN( beatclock_bpm_get )
{
    std::map<Chuck_Object *, std::shared_ptr<ClockState>>::iterator it = g_clocks.find( SELF );
    if( it == g_clocks.end() || !it->second )
    {
        throw_invalid( API, SHRED, "BeatClock internal state is missing" );
        RETURN->v_float = 120.0;
        return;
    }
    const long double now = VM ? (long double)VM->now() : it->second->curve.anchor_sample;
    RETURN->v_float = (t_CKFLOAT)it->second->curve.bpm_at_sample( now, it->second->srate );
}

CK_DLL_MFUN( beatclock_bpm_set )
{
    const t_CKFLOAT value = GET_NEXT_FLOAT( ARGS );
    if( !std::isfinite( (double)value ) || value <= 0.0 )
    {
        throw_invalid( API, SHRED, "BeatClock.bpm value must be a positive finite number" );
        RETURN->v_float = 0.0;
        return;
    }

    std::map<Chuck_Object *, std::shared_ptr<ClockState>>::iterator it = g_clocks.find( SELF );
    if( it == g_clocks.end() || !it->second )
    {
        throw_invalid( API, SHRED, "BeatClock internal state is missing" );
        RETURN->v_float = 0.0;
        return;
    }

    ClockState & state = *it->second;
    const long double now = VM ? (long double)VM->now() : state.curve.anchor_sample;
    const long double beat = state.curve.beat_at_sample( now, state.srate );
    // Re-anchor at the true current beat and sample; this cancels any ramp.
    state.curve = BeatCurve::constant( now, beat, (long double)value );
    RETURN->v_float = value;
}

CK_DLL_MFUN( beatclock_ramp )
{
    const t_CKFLOAT target = GET_NEXT_FLOAT( ARGS );
    const t_CKFLOAT beats = GET_NEXT_FLOAT( ARGS );

    if( !std::isfinite( (double)target ) || target <= 0.0 ||
        !std::isfinite( (double)beats ) || beats <= 0.0 )
    {
        throw_invalid( API, SHRED,
                       "BeatClock.ramp target BPM and beat length must be positive finite numbers" );
        return;
    }

    std::map<Chuck_Object *, std::shared_ptr<ClockState>>::iterator it = g_clocks.find( SELF );
    if( it == g_clocks.end() || !it->second )
    {
        throw_invalid( API, SHRED, "BeatClock internal state is missing" );
        return;
    }

    ClockState & state = *it->second;
    const long double now = VM ? (long double)VM->now() : state.curve.anchor_sample;
    const long double beat = state.curve.beat_at_sample( now, state.srate );
    const long double current_bpm = state.curve.bpm_at_sample( now, state.srate );

    BeatCurve next;
    next.anchor_sample = now;
    next.anchor_beat = beat;
    next.bpm0 = current_bpm;
    next.target_bpm = (long double)target;
    next.ramp_beats = (long double)beats;
    next.ramp = true;
    state.curve = next;
}

CK_DLL_MFUN( beatclock_at )
{
    const t_CKFLOAT target = GET_NEXT_FLOAT( ARGS );
    if( !std::isfinite( (double)target ) || target < 0.0 )
    {
        throw_invalid( API, SHRED, "BeatClock.at target beat must be non-negative and finite" );
        RETURN->v_object = nullptr;
        return;
    }

    std::map<Chuck_Object *, std::shared_ptr<ClockState>>::iterator it = g_clocks.find( SELF );
    if( it == g_clocks.end() || !it->second || !VM || !VM->env() || !VM->env()->ckt_event )
    {
        throw_invalid( API, SHRED, "BeatClock cannot create beat Event" );
        RETURN->v_object = nullptr;
        return;
    }

    ChuckBeatEvent * event = new ChuckBeatEvent( it->second, (double)target );
    ++g_live_events;
    it->second->events.insert( event );
    initialize_object( event, VM->env()->ckt_event, SHRED, VM );
    RETURN->v_object = event;
}



//-----------------------------------------------------------------------------
// query/registration
//-----------------------------------------------------------------------------
DLL_QUERY beatclock_query( Chuck_DL_Query * QUERY )
{
    QUERY->setname( QUERY, "BeatClock" );
    QUERY->begin_class( QUERY, "BeatClock", "Object" );
    QUERY->doc_class( QUERY,
        "A sample-accurate beat timeline. Tempo changes and ramps are "
        "integrated against absolute beats; at() returns a one-shot Event." );

    QUERY->add_ctor( QUERY, beatclock_ctor );
    QUERY->add_dtor( QUERY, beatclock_dtor );

    QUERY->add_mfun( QUERY, beatclock_beat, "float", "beat" );
    QUERY->doc_func( QUERY, "Current absolute beat on this clock." );

    QUERY->add_mfun( QUERY, beatclock_bpm_get, "float", "bpm" );
    QUERY->doc_func( QUERY, "Current instantaneous beats per minute." );

    QUERY->add_mfun( QUERY, beatclock_bpm_set, "float", "bpm" );
    QUERY->add_arg( QUERY, "float", "bpm" );
    QUERY->doc_func( QUERY,
        "Set constant BPM while preserving the current beat; cancels a ramp." );

    QUERY->add_mfun( QUERY, beatclock_ramp, "void", "ramp" );
    QUERY->add_arg( QUERY, "float", "targetBpm" );
    QUERY->add_arg( QUERY, "float", "beats" );
    QUERY->doc_func( QUERY,
        "Linearly ramp BPM as a function of beats, then hold targetBpm." );

    QUERY->add_mfun( QUERY, beatclock_at, "Event", "at" );
    QUERY->add_arg( QUERY, "float", "absoluteBeat" );
    QUERY->doc_func( QUERY,
        "Return a one-shot Event at an absolute beat. Past beats continue immediately." );

    QUERY->end_class( QUERY );
    return TRUE;
}



//-----------------------------------------------------------------------------
// VM/Event integration hooks
//-----------------------------------------------------------------------------
extern "C" int chuck_beat_pre_wait( Chuck_Event * base_event,
                                    Chuck_VM_Shred * shred,
                                    Chuck_VM * vm )
{
    ChuckBeatEvent * event = dynamic_cast<ChuckBeatEvent *>( base_event );
    if( !event || !shred || !vm ) return 0;

    const long long f = event->deadline_frame();
    const long long now = now_frame( vm );
    if( event->completed || f <= now )
    {
        // Latch one-shot completion: waiting again must not block.
        event->completed = true;
        if( Scheduler * s = find_scheduler( vm ) )
            if( event->wait_order != 0 ) s->active.erase( event->wait_order );
        return 1;
    }
    return 0;
}

extern "C" void chuck_beat_post_wait( Chuck_Event * base_event,
                                      Chuck_VM_Shred * shred,
                                      Chuck_VM * vm )
{
    ChuckBeatEvent * event = dynamic_cast<ChuckBeatEvent *>( base_event );
    if( !event || !shred || !vm ) return;

    Scheduler * s = find_scheduler( vm, true );
    if( !s ) return;

    event->beat_waiters.insert( shred );
    if( event->wait_order == 0 )
        event->wait_order = ++s->next_order;
    event->completed = false;
    s->active[event->wait_order] = event;
}

extern "C" void chuck_beat_on_remove( Chuck_Event * base_event,
                                      Chuck_VM_Shred * shred )
{
    ChuckBeatEvent * event = dynamic_cast<ChuckBeatEvent *>( base_event );
    if( !event || !shred ) return;

    event->beat_waiters.erase( shred );
    if( event->beat_waiters.empty() )
    {
        Scheduler * s = find_scheduler( shred->vm_ref );
        if( s && event->wait_order != 0 )
            s->active.erase( event->wait_order );
    }
}

extern "C" void chuck_beat_on_signal( Chuck_Event * base_event,
                                      Chuck_VM_Shred * shred )
{
    ChuckBeatEvent * event = dynamic_cast<ChuckBeatEvent *>( base_event );
    if( !event || !shred || !shred->vm_ref ) return;
    Scheduler * s = find_scheduler( shred->vm_ref, true );
    if( !s ) return;

    ++s->signal_count;
    const unsigned long long sid = shred->xid & 0xffffffffULL;
    s->wake_order.push_back( ( event->wait_order << 32 ) | sid );
    event->beat_waiters.erase( shred );
}

extern "C" int chuck_beat_process_due( Chuck_VM * vm, double now_sample )
{
    Scheduler * s = find_scheduler( vm );
    if( !s || s->active.empty() ) return 0;

    const long long now = (long long)std::llround( (long double)now_sample );
    std::vector<ChuckBeatEvent *> due;

    // std::map is ordered by first-actual-wait sequence.
    for( std::map<unsigned long long, ChuckBeatEvent *>::iterator it = s->active.begin();
         it != s->active.end(); ++it )
    {
        ChuckBeatEvent * event = it->second;
        if( event && event->deadline_frame() <= now )
            due.push_back( event );
    }

    if( due.empty() ) return 0;
    ++s->due_batch_count;

    for( ChuckBeatEvent * event : due )
    {
        // Remove before broadcasting so removals/signals do not mutate the
        // active map out from under this dispatch.
        if( event->wait_order != 0 ) s->active.erase( event->wait_order );
        event->completed = true;
        event->beat_waiters.clear();

        // Keep the C++ object alive across broadcast.  ChucK's wait reference
        // normally does this, but this makes the invariant explicit.
        event->add_ref();
        event->broadcast_local();
        event->release();
    }

    return (int)due.size();
}

extern "C" double chuck_beat_next_delay_samples( Chuck_VM * vm, double now_sample )
{
    Scheduler * s = find_scheduler( vm );
    if( !s || s->active.empty() ) return -1.0;

    const long long now = (long long)std::llround( (long double)now_sample );
    long long nearest = std::numeric_limits<long long>::max();
    for( const auto & entry : s->active )
    {
        ChuckBeatEvent * event = entry.second;
        if( !event ) continue;
        const long long f = event->deadline_frame();
        if( f <= now ) return 0.0;
        nearest = std::min( nearest, f );
    }
    return nearest == std::numeric_limits<long long>::max()
               ? -1.0 : (double)( nearest - now );
}

extern "C" long long chuck_beat_adaptive_limit( Chuck_VM * vm,
                                                double now_sample,
                                                long long current_limit )
{
    double delay = chuck_beat_next_delay_samples( vm, now_sample );
    if( delay < 0.0 ) return current_limit;
    long long frames = (long long)std::llround( (long double)delay );
    if( frames < 1 ) frames = 1;
    return std::min( current_limit, frames );
}

extern "C" void chuck_beat_vm_shutdown( Chuck_VM * vm )
{
    // Remove clock bindings first.  Some ChucK objects may be reclaimed later
    // during type-system/global teardown; detach their raw VM identity so a
    // later VM at the same address can never be touched by a stale object.
    for( std::map<Chuck_Object *, std::shared_ptr<ClockState>>::iterator it = g_clocks.begin();
         it != g_clocks.end(); )
    {
        if( it->second && it->second->vm == vm )
        {
            it->second->vm = nullptr;
            it = g_clocks.erase( it );
        }
        else ++it;
    }

    std::map<Chuck_VM *, Scheduler *>::iterator sit = g_schedulers.find( vm );
    if( sit == g_schedulers.end() ) return;
    delete sit->second;
    g_schedulers.erase( sit );
}

extern "C" ChuckBeatStats chuck_beat_stats( Chuck_VM * vm )
{
    ChuckBeatStats stats;
    stats.clocks = g_clocks.size();
    stats.live_events = g_live_events;
    stats.active_events = 0;
    stats.active_waiters = 0;
    stats.signal_count = 0;
    stats.due_batch_count = 0;

    Scheduler * s = find_scheduler( vm );
    if( s )
    {
        stats.active_events = s->active.size();
        stats.signal_count = s->signal_count;
        stats.due_batch_count = s->due_batch_count;
        for( const auto & entry : s->active )
            if( entry.second ) stats.active_waiters += entry.second->beat_waiters.size();
    }
    return stats;
}

extern "C" size_t chuck_beat_wake_order( Chuck_VM * vm,
                                         unsigned long long * ids,
                                         size_t capacity )
{
    Scheduler * s = find_scheduler( vm );
    if( !s ) return 0;
    size_t n = std::min( capacity, s->wake_order.size() );
    for( size_t i = 0; i < n; ++i ) ids[i] = s->wake_order[i];
    return s->wake_order.size();
}

extern "C" size_t chuck_beat_active_order( Chuck_VM * vm,
                                           unsigned long long * ids,
                                           size_t capacity )
{
    Scheduler * s = find_scheduler( vm );
    if( !s ) return 0;
    size_t i = 0;
    for( const auto & entry : s->active )
    {
        if( i < capacity ) ids[i] = entry.first;
        ++i;
    }
    return i;
}
