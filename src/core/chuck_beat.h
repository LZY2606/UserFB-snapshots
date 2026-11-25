/*----------------------------------------------------------------------------
  ChucK BeatClock: absolute beat timeline and VM-integrated beat Events.

  This header is intentionally force-included by the task_support target build
  for every translation unit.  Defining CHUCK_BEAT_CLOCK here lets the small
  VM/Event integration points be compiled out in the baseline build.
-----------------------------------------------------------------------------*/
#ifndef CK_CHUCK_BEAT_H
#define CK_CHUCK_BEAT_H

#define CHUCK_BEAT_CLOCK 1

#include "chuck_dl.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Chuck_Event;
struct Chuck_VM;
struct Chuck_VM_Shred;

/* Native ChucK class query for BeatClock. */
DLL_QUERY beatclock_query( Chuck_DL_Query * QUERY );

/* Hooks called from the core Event/VM implementation. */
int chuck_beat_pre_wait( struct Chuck_Event * event,
                         struct Chuck_VM_Shred * shred,
                         struct Chuck_VM * vm );
void chuck_beat_post_wait( struct Chuck_Event * event,
                           struct Chuck_VM_Shred * shred,
                           struct Chuck_VM * vm );
void chuck_beat_on_remove( struct Chuck_Event * event,
                           struct Chuck_VM_Shred * shred );
void chuck_beat_on_signal( struct Chuck_Event * event,
                           struct Chuck_VM_Shred * shred );
int chuck_beat_process_due( struct Chuck_VM * vm, double now_sample );
double chuck_beat_next_delay_samples( struct Chuck_VM * vm, double now_sample );
long long chuck_beat_adaptive_limit( struct Chuck_VM * vm,
                                     double now_sample,
                                     long long current_limit );
void chuck_beat_vm_shutdown( struct Chuck_VM * vm );

/* Introspection used by the regression's queue/count assertions. */
typedef struct ChuckBeatStats
{
    size_t clocks;
    size_t live_events;
    size_t active_events;
    size_t active_waiters;
    size_t signal_count;
    size_t due_batch_count;
} ChuckBeatStats;

ChuckBeatStats chuck_beat_stats( struct Chuck_VM * vm );
size_t chuck_beat_wake_order( struct Chuck_VM * vm,
                              unsigned long long * ids,
                              size_t capacity );
size_t chuck_beat_active_order( struct Chuck_VM * vm,
                                unsigned long long * ids,
                                size_t capacity );

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CK_CHUCK_BEAT_H */
