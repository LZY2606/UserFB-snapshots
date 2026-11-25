/*----------------------------------------------------------------------------
  ChucK BeatClock implementation.
-----------------------------------------------------------------------------*/
#include "chuck_dl.h"
#include "chuck_oo.h"
#include "chuck_vm.h"
#include "chuck_instr.h"

#include "chuck_beat.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace
{
    struct ClockBinding
    {
        std::shared_ptr<ChuckBeat::Clock> clock;
        Chuck_VM * vm = nullptr;
    };

    std::mutex g_mutex;
    std::unordered_map<Chuck_Object *, ClockBinding> g_clocks;
    std::unordered_map<Chuck_Event *, std::shared_ptr<ChuckBeat::EventRecord> > g_events;
    std::atomic<unsigned long long> g_order{ 1 };
    std::atomic<unsigned long long> g_counters[ChuckBeat::COUNTER__COUNT]{};

    std::shared_ptr<ChuckBeat::Clock> find_clock( Chuck_Object * self )
    {
        std::lock_guard<std::mutex> lock( g_mutex );
        auto it = g_clocks.find( self );
        return it == g_clocks.end() ? nullptr : it->second.clock;
    }

    void throw_argument( Chuck_VM_Shred * shred, const char * message )
    {
        // The DL API is not available in this helper's signature, and the
        // native exception helper is the same path used by core libraries.
        ck_throw_exception( shred, "InvalidArgument", message );
    }
}

namespace ChuckBeat
{
Clock::Clock( double sample_rate, double now_system )
{
    m_segment.sample_rate = (long double) sample_rate;
    m_segment.s0 = (long double) now_system;
    m_segment.b0 = 0.0L;
    m_segment.v0 = m_segment.v1 = 120.0L;
    m_segment.length = 0.0L;
}

double Clock::current_beat( double now_system ) const
{
    return (double) m_segment.beat_at_sample( (long double) now_system );
}

double Clock::current_bpm( double now_system ) const
{
    long double b = m_segment.beat_at_sample( (long double) now_system );
    return (double) m_segment.bpm_at_beat( b );
}

void Clock::set_bpm( double value, double now_system )
{
    long double now = (long double) now_system;
    long double beat = m_segment.beat_at_sample( now );
    Segment next;
    next.sample_rate = m_segment.sample_rate;
    next.s0 = now;
    next.b0 = beat;
    next.v0 = next.v1 = (long double) value;
    next.length = 0.0L;
    m_segment = next;
}

void Clock::ramp( double target_bpm, double beats, double now_system )
{
    long double now = (long double) now_system;
    long double beat = m_segment.beat_at_sample( now );
    long double actual_bpm = m_segment.bpm_at_beat( beat );

    Segment next;
    next.sample_rate = m_segment.sample_rate;
    next.s0 = now;
    next.b0 = beat;
    next.v0 = actual_bpm;
    next.v1 = (long double) target_bpm;
    next.length = (long double) beats;
    m_segment = next;
}

long double Clock::exact_sample_for_beat( double beat ) const
{
    return m_segment.sample_at_beat( (long double) beat );
}

double Clock::frame_for_beat( double beat ) const
{
    return round_frame_half_up( exact_sample_for_beat( beat ) );
}

unsigned long long test_counter( int which )
{
    if( which < 0 || which >= COUNTER__COUNT ) return 0;
    return g_counters[which].load();
}

void test_reset_counters()
{
    for( int i = 0; i < COUNTER__COUNT; ++i ) g_counters[i] = 0;
    g_order = 1;
}

std::size_t test_clock_count()
{
    std::lock_guard<std::mutex> lock( g_mutex );
    return g_clocks.size();
}

std::size_t test_event_record_count()
{
    std::lock_guard<std::mutex> lock( g_mutex );
    return g_events.size();
}
} // namespace ChuckBeat

//-----------------------------------------------------------------------------
// VM/core hooks
//-----------------------------------------------------------------------------
void chuck_beat_on_wait( Chuck_Event * event, Chuck_VM_Shred * shred,
                         Chuck_VM * vm )
{
    using namespace ChuckBeat;
    ++g_counters[COUNTER_WAIT_CALLS];
    if( !event || !vm ) return;

    std::shared_ptr<EventRecord> rec;
    {
        std::lock_guard<std::mutex> lock( g_mutex );
        auto it = g_events.find( event );
        if( it == g_events.end() ) return;
        rec = it->second;
        if( rec->first_wait_order == 0 )
            rec->first_wait_order = g_order.fetch_add( 1 );
        if( !rec->vm ) rec->vm = vm;
    }

    double now = vm->shreduler() ? vm->shreduler()->now_system : 0.0;
    rec->deadline = rec->clock->frame_for_beat( rec->target_beat );
    bool immediate = rec->fired || rec->deadline <= now + 0.5;
    if( immediate )
    {
        {
            std::lock_guard<std::mutex> lock( g_mutex );
            if( !rec->fired )
            {
                rec->fired = true;
                ++g_counters[COUNTER_DUE_EVENTS];
            }
            ++g_counters[COUNTER_IMMEDIATE_WAKES];
        }
        // The normal Event queue owns ordering and refcount handoff.  Calling
        // this at the end of wait() wakes the just-added waiter through the
        // ordinary signal/schedule path.
        Chuck_Event * event_ref = event;
        CK_SAFE_ADD_REF( event_ref );
        event->broadcast_local();
        CK_SAFE_RELEASE( event_ref );
    }

    chuck_beat_refresh_deadline( vm );
}

void chuck_beat_on_event_remove( Chuck_Event * event, Chuck_VM_Shred * shred )
{
    using namespace ChuckBeat;
    if( !event ) return;
    bool known = false;
    Chuck_VM * vm = nullptr;
    {
        std::lock_guard<std::mutex> lock( g_mutex );
        auto it = g_events.find( event );
        if( it != g_events.end() )
        {
            known = true;
            vm = it->second->vm;
        }
    }
    if( known )
    {
        ++g_counters[COUNTER_REMOVED_WAITERS];
        if( vm ) chuck_beat_refresh_deadline( vm );
    }
}

void chuck_beat_on_event_destroy( Chuck_Event * event )
{
    if( !event ) return;
    std::lock_guard<std::mutex> lock( g_mutex );
    g_events.erase( event );
}

void chuck_beat_on_vm_shutdown( Chuck_VM * vm )
{
    using namespace ChuckBeat;
    if( !vm ) return;
    ++g_counters[COUNTER_VM_SHUTDOWNS];
    std::lock_guard<std::mutex> lock( g_mutex );
    for( auto it = g_clocks.begin(); it != g_clocks.end(); )
    {
        if( it->second.vm == vm ) it = g_clocks.erase( it );
        else ++it;
    }
    for( auto it = g_events.begin(); it != g_events.end(); )
    {
        if( it->second && it->second->vm == vm ) it = g_events.erase( it );
        else ++it;
    }
}

double chuck_beat_next_delta( Chuck_VM * vm, double now_system )
{
    using namespace ChuckBeat;
    if( !vm ) return -1.0;

    std::vector<std::pair<Chuck_Event *, std::shared_ptr<EventRecord> > > records;
    {
        std::lock_guard<std::mutex> lock( g_mutex );
        records.reserve( g_events.size() );
        for( auto & pair : g_events )
        {
            if( pair.second && pair.second->vm == vm && !pair.second->fired )
                records.push_back( pair );
        }
    }

    double best = -1.0;
    for( auto & item : records )
    {
        Chuck_Event * event = item.first;
        auto & rec = item.second;
        if( event->waiter_count() == 0 ) continue;
        double deadline = rec->clock->frame_for_beat( rec->target_beat );
        rec->deadline = deadline;
        double delta = deadline - now_system;
        if( delta < 0.0 ) delta = 0.0;
        if( best < 0.0 || delta < best ) best = delta;
    }
    return best;
}

void chuck_beat_process_due( Chuck_VM * vm, double now_system )
{
    using namespace ChuckBeat;
    if( !vm ) return;

    struct Due
    {
        Chuck_Event * event;
        std::shared_ptr<EventRecord> rec;
    };
    std::vector<Due> due;

    {
        std::lock_guard<std::mutex> lock( g_mutex );
        for( auto & pair : g_events )
        {
            Chuck_Event * event = pair.first;
            auto & rec = pair.second;
            if( !rec || rec->vm != vm || rec->fired ) continue;
            if( event->waiter_count() == 0 ) continue;

            double deadline = rec->clock->frame_for_beat( rec->target_beat );
            rec->deadline = deadline;
            if( deadline <= now_system + 0.5 )
            {
                rec->fired = true;
                ++g_counters[COUNTER_DUE_EVENTS];
                due.push_back( { event, rec } );
            }
        }
    }

    std::stable_sort( due.begin(), due.end(),
        []( const Due & a, const Due & b ) {
            return a.rec->first_wait_order < b.rec->first_wait_order;
        } );

    for( auto & item : due )
    {
        ++g_counters[COUNTER_BROADCAST_EVENTS];
        Chuck_Event * event = item.event;
        // Keep the Event alive across its final waiter reference handoff.
        CK_SAFE_ADD_REF( event );
        event->broadcast_local();
        CK_SAFE_RELEASE( event );
    }

    if( !due.empty() ) chuck_beat_refresh_deadline( vm );
}

void chuck_beat_refresh_deadline( Chuck_VM * vm )
{
    if( !vm || !vm->shreduler() ) return;
    Chuck_VM_Shreduler * shreduler = vm->shreduler();

    double best = -1.0;
    if( shreduler->shred_list )
    {
        best = shreduler->shred_list->wake_time - shreduler->now_system;
        if( best < 0.0 ) best = 0.0;
    }

    double beat = chuck_beat_next_delta( vm, shreduler->now_system );
    if( beat >= 0.0 && ( best < 0.0 || beat < best ) ) best = beat;
    shreduler->m_samps_until_next = best < 0.0 ? -1.0 : best;
}

//-----------------------------------------------------------------------------
// Native BeatClock class
//-----------------------------------------------------------------------------
CK_DLL_CTOR( beatclock_ctor )
{
    double now = VM->shreduler() ? VM->shreduler()->now_system : 0.0;
    auto clock = std::make_shared<ChuckBeat::Clock>( VM->srate(), now );
    std::lock_guard<std::mutex> lock( g_mutex );
    g_clocks[SELF] = { clock, VM };
}

CK_DLL_DTOR( beatclock_dtor )
{
    std::lock_guard<std::mutex> lock( g_mutex );
    g_clocks.erase( SELF );
}

CK_DLL_MFUN( beatclock_beat )
{
    auto clock = find_clock( SELF );
    if( !clock )
    {
        RETURN->v_float = 0.0f;
        return;
    }
    double now = VM->shreduler() ? VM->shreduler()->now_system : 0.0;
    RETURN->v_float = (t_CKFLOAT) clock->current_beat( now );
}

CK_DLL_MFUN( beatclock_bpm_get )
{
    auto clock = find_clock( SELF );
    if( !clock )
    {
        RETURN->v_float = 120.0f;
        return;
    }
    double now = VM->shreduler() ? VM->shreduler()->now_system : 0.0;
    RETURN->v_float = (t_CKFLOAT) clock->current_bpm( now );
}

CK_DLL_MFUN( beatclock_bpm_set )
{
    t_CKFLOAT value = GET_NEXT_FLOAT( ARGS );
    if( !ChuckBeat::positive_finite( (double) value ) )
    {
        throw_argument( SHRED, "BeatClock.bpm: BPM must be positive and finite" );
        RETURN->v_float = 0.0f;
        return;
    }

    auto clock = find_clock( SELF );
    if( clock )
    {
        double now = VM->shreduler() ? VM->shreduler()->now_system : 0.0;
        clock->set_bpm( (double) value, now );
        chuck_beat_refresh_deadline( VM );
    }
    RETURN->v_float = value;
}

CK_DLL_MFUN( beatclock_ramp )
{
    t_CKFLOAT target = GET_NEXT_FLOAT( ARGS );
    t_CKFLOAT beats = GET_NEXT_FLOAT( ARGS );

    if( !ChuckBeat::positive_finite( (double) target ) ||
        !ChuckBeat::positive_finite( (double) beats ) )
    {
        throw_argument( SHRED,
            "BeatClock.ramp: target BPM and ramp length must be positive and finite" );
        return;
    }

    auto clock = find_clock( SELF );
    if( clock )
    {
        double now = VM->shreduler() ? VM->shreduler()->now_system : 0.0;
        clock->ramp( (double) target, (double) beats, now );
        chuck_beat_refresh_deadline( VM );
    }
}

CK_DLL_MFUN( beatclock_at )
{
    t_CKFLOAT absolute_beat = GET_NEXT_FLOAT( ARGS );
    if( !ChuckBeat::non_negative_finite( (double) absolute_beat ) )
    {
        throw_argument( SHRED,
            "BeatClock.at: absoluteBeat must be non-negative and finite" );
        RETURN->v_object = nullptr;
        return;
    }

    auto clock = find_clock( SELF );
    if( !clock || !VM->env() || !VM->env()->ckt_event )
    {
        RETURN->v_object = nullptr;
        return;
    }

    Chuck_Event * event = new Chuck_Event();
    initialize_object( event, VM->env()->ckt_event, SHRED, VM );

    auto rec = std::make_shared<ChuckBeat::EventRecord>();
    rec->clock = clock;
    rec->vm = VM;
    rec->target_beat = (double) absolute_beat;
    double now = VM->shreduler() ? VM->shreduler()->now_system : 0.0;
    rec->deadline = clock->frame_for_beat( rec->target_beat );
    rec->fired = rec->deadline <= now + 0.5;
    if( rec->fired ) ++g_counters[ChuckBeat::COUNTER_DUE_EVENTS];

    {
        std::lock_guard<std::mutex> lock( g_mutex );
        g_events[event] = rec;
    }

    RETURN->v_object = event;
}

DLL_QUERY beatclock_query( Chuck_DL_Query * QUERY )
{
    QUERY->setname( QUERY, "BeatClock" );
    QUERY->begin_class( QUERY, "BeatClock", "Object" );
    QUERY->doc_class( QUERY,
        "An audio-timeline beat clock with tempo ramps and beat-scheduled Events." );

    QUERY->add_ctor( QUERY, beatclock_ctor );
    QUERY->add_dtor( QUERY, beatclock_dtor );

    QUERY->add_mfun( QUERY, beatclock_beat, "float", "beat" );
    QUERY->doc_func( QUERY,
        "Current continuous beat position; creation time is beat zero." );

    QUERY->add_mfun( QUERY, beatclock_bpm_get, "float", "bpm" );
    QUERY->doc_func( QUERY, "Current tempo in beats per minute." );

    QUERY->add_mfun( QUERY, beatclock_bpm_set, "float", "bpm" );
    QUERY->add_arg( QUERY, "float", "value" );
    QUERY->doc_func( QUERY,
        "Set a constant tempo while preserving the current beat position and canceling any ramp." );

    QUERY->add_mfun( QUERY, beatclock_ramp, "void", "ramp" );
    QUERY->add_arg( QUERY, "float", "targetBpm" );
    QUERY->add_arg( QUERY, "float", "beats" );
    QUERY->doc_func( QUERY,
        "Linearly ramp BPM as a function of beat position, then hold the target." );

    QUERY->add_mfun( QUERY, beatclock_at, "Event", "at" );
    QUERY->add_arg( QUERY, "float", "absoluteBeat" );
    QUERY->doc_func( QUERY,
        "Return a one-shot Event scheduled at an absolute beat." );

    QUERY->end_class( QUERY );
    return TRUE;
}
