#ifndef ALLOC_FAULT_H
#define ALLOC_FAULT_H

/*
 * Bounded allocation fault injector and ownership ledger.
 *
 * The fault build of libtheft is compiled with -Dmalloc=qa_malloc etc.,
 * so every allocation made inside the library is routed through the
 * functions below. Test code is compiled normally and calls these
 * entry points explicitly for the argument/candidate objects it owns.
 *
 * All recorder metadata lives in fixed-capacity static storage: the
 * recorder never allocates memory itself, so a fault cannot recursively
 * corrupt its own bookkeeping.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Per-plan bounds mandated by the task. */
#define QA_MAX_EVENTS       4096  /* allocation attempts / ops per run */
#define QA_MAX_RECORDS      4096  /* live and retired block identities */
#define QA_MAX_VIOLATIONS   64
#define QA_MAX_USER_OBJECTS 1024
#define QA_MAX_PLAN_FAULTS  2
#define QA_MAX_TRIALS       32

typedef enum {
    QA_OP_MALLOC = 1,
    QA_OP_CALLOC,
    QA_OP_REALLOC,
    QA_OP_FREE,
} qa_op_t;

/* One recorded allocator operation. Sequence numbers are monotonic
 * across every op (including free) within a run. */
struct qa_event {
    uint32_t seq;
    qa_op_t  op;
    size_t   size;              /* requested size (realloc: new size) */
    bool     ok;                /* success or forced/real failure */
    uint32_t id;                /* identity acted on (0 on failed new alloc) */
    uint32_t new_id;            /* successful realloc: new identity */
    void    *addr;              /* returned / freed address */
    void    *old_addr;          /* realloc: previous address */
    uint32_t attempt;           /* positive-length attempt counter */
    const char *stage;          /* library stage reported by the test */
};

struct qa_violation {
    uint32_t seq;
    qa_op_t  op;
    void    *addr;
    const char *kind;           /* static string, never freed */
};

/* Test-owned argument / candidate object. Allocated through the
 * explicit qa_user_* entry points so user leaks never blend into the
 * library ledger. */
struct qa_user_obj {
    uint32_t id;
    void    *addr;
    size_t   size;
    const char *tag;
    bool     live;
    uint32_t alloc_seq;
    uint32_t free_seq;
    uint64_t checksum;          /* read-only checksum captured at birth */
};

/* A replayable fault plan. Fault positions are 1-based positive-length
 * attempt numbers within the run, never runtime pointers. */
struct qa_plan {
    const char *config;
    uint64_t    seed;
    uint32_t    fault_seq[QA_MAX_PLAN_FAULTS];
    size_t      fault_count;
};

const char *qa_op_str(qa_op_t op);

/* Reset all recorder state for a fresh run. */
void qa_reset(void);

/* Fault configuration. Positions refer to the positive-length attempt
 * counter of the upcoming run. */
void qa_fault_reset(void);
void qa_fault_single(uint32_t attempt_pos);
void qa_fault_pair(uint32_t first, uint32_t second);
bool     qa_fault_hit(void);           /* at least one fault fired */
unsigned qa_fault_hits(void);
uint32_t qa_attempt_count(void);

/* Mark the library stage currently executing (set by test hooks). */
void qa_set_stage(const char *stage);
const char *qa_stage(void);

/* Allocator entry points used by the fault-built library. */
void *qa_malloc(size_t size);
void *qa_calloc(size_t nmemb, size_t size);
void *qa_realloc(void *ptr, size_t size);
void  qa_free(void *ptr);

/* Explicit entry points for test-owned argument objects. These record
 * into a separate ledger. qa_user_fail_next() makes the next user
 * allocation (only) return NULL, to exercise user callback errors. */
void *qa_user_malloc(size_t size, const char *tag, uint64_t checksum);
void *qa_user_calloc(size_t nmemb, size_t size, const char *tag);
void  qa_user_free(void *ptr, const char *tag);
void  qa_user_fail_next(unsigned count);

/* Introspection. */
size_t qa_event_count(void);
const struct qa_event *qa_event_at(size_t i);
size_t qa_live_count(void);           /* library blocks still live */
size_t qa_violation_count(void);
const struct qa_violation *qa_violation_at(size_t i);
bool   qa_budget_exceeded(void);      /* event table filled up */

size_t qa_user_live_count(void);
const struct qa_user_obj *qa_user_live_at(size_t i);
size_t qa_user_total_allocs(void);
size_t qa_user_total_frees(void);
const struct qa_user_obj *qa_user_find(void *addr);

/* Write a replayable plan plus its observed outcome as text. */
struct qa_outcome {
    int           run_result;       /* enum theft_run_res value */
    bool          fault_hit;
    unsigned      fault_hits;
    size_t        live_blocks;
    size_t        live_user_objects;
    size_t        violations;
    const char   *last_stage;
    bool          incomplete;       /* a bounded budget was hit */
};
void qa_plan_write(FILE *f, const struct qa_plan *plan,
    const struct qa_outcome *outcome);

/* Human-readable violation / leak report. Returns number of violations
 * and live blocks reported. */
size_t qa_report(FILE *f);

#endif
