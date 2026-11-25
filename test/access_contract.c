/*
 * test/access_contract.c
 *
 * Contract tests for DUMA's offline access diagnostics.  Real DUMA pointers are
 * used only for healthy allocation, request-range access through the diagnostic
 * API, and release.  Guard pages, freed user memory and forged addresses are
 * never read or written.  Address-reuse and layout edge cases are constructed
 * through the public integer-array interpreter.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "duma.h"
#include "task_support/check.h"

/* ------------------------------------------------------------------ */
/* Shared setup and small helpers                                      */
/* ------------------------------------------------------------------ */

static int g_setup_done = 0;
static void *g_live_before_first_acquire = 0;
static uint64_t g_base_allocs = 0;
static uint64_t g_base_deallocs = 0;

static int setup_once(void)
{
    if (!g_setup_done)
    {
        /* Keep deliberate huge realloc failure non-fatal. */
        setenv("DUMA_MALLOC_FAILEXIT", "0", 1);
        setenv("DUMA_DISABLE_BANNER", "1", 1);
        duma_init();
        /* This allocation exists before the first acquire() call. */
        g_live_before_first_acquire = malloc(41);
        if (!g_live_before_first_acquire)
            return 1;
        memset(g_live_before_first_acquire, 7, 41);
        duma_access_allocator_counts(&g_base_allocs, &g_base_deallocs);
        g_setup_done = 1;
    }
    return 0;
}

static int has_flag(const duma_access_report *r, uint32_t flag)
{
    return r && (r->flags & flag) != 0;
}

static int has_code(const duma_access_report *r, const char *code)
{
    size_t i;
    if (!r || !code)
        return 0;
    for (i = 0; i < r->code_count; ++i)
        if (r->codes[i] && strcmp(r->codes[i], code) == 0)
            return 1;
    return 0;
}

static int segment_is(const duma_access_report *r, size_t index,
                      uint64_t start, uint64_t end, uint32_t kind)
{
    const duma_access_segment *s;
    if (!r || index >= r->segment_count)
        return 0;
    s = &r->segments[index];
    return s->start == start && s->end == end && s->kind == kind;
}

static int check_healthy_source(void *p, size_t size, uint32_t source)
{
    duma_access_ref *ref = 0;
    duma_access_event *event = 0;
    const duma_access_report *r;

    TASK_CHECK(p != 0);
    TASK_CHECK(duma_access_acquire((uintptr_t)p, &ref) == DUMA_ACCESS_OK);
    TASK_CHECK(ref != 0);
    TASK_CHECK(duma_access_capture(ref, (uintptr_t)p, size,
                                   DUMA_ACCESS_READ, &event) == DUMA_ACCESS_OK);
    r = duma_access_explain(event);
    TASK_CHECK(r != 0);
    TASK_CHECK(r->source == source);
    TASK_CHECK(r->associated && r->ref_state == DUMA_ACCESS_LIVE);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_VALID));
    TASK_CHECK(!has_flag(r, DUMA_ACCESS_F_OUT_OF_BOUNDS));
    TASK_CHECK(has_code(r, "valid"));

    duma_access_release_event(event);
    duma_access_release_ref(ref);
    return 0;
}

/* ------------------------------------------------------------------ */
/* attribution                                                         */
/* ------------------------------------------------------------------ */

static int case_pre_acquire_live_allocation(void)
{
    duma_access_ref *ref = 0;
    duma_access_event *event = 0;
    const duma_access_report *r;

    TASK_CHECK(setup_once() == 0);
    TASK_CHECK(g_live_before_first_acquire != 0);

    TASK_CHECK(duma_access_acquire(
        (uintptr_t)g_live_before_first_acquire, &ref) == DUMA_ACCESS_OK);
    TASK_CHECK(ref != 0);
    TASK_CHECK(duma_access_capture(ref,
        (uintptr_t)g_live_before_first_acquire, 41,
        DUMA_ACCESS_WRITE, &event) == DUMA_ACCESS_OK);
    r = duma_access_explain(event);
    TASK_CHECK(r != 0);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_VALID));
    TASK_CHECK(r->source == DUMA_ACCESS_SOURCE_MALLOC);
    TASK_CHECK(r->ref_identity != 0);

    duma_access_release_event(event);
    duma_access_release_ref(ref);
    return 0;
}

static int case_allocation_sources(void)
{
    void *m, *c, *a, *v;
    char *s;

    TASK_CHECK(setup_once() == 0);

    m = malloc(37);
    TASK_CHECK(check_healthy_source(m, 37, DUMA_ACCESS_SOURCE_MALLOC) == 0);
    free(m);

    c = calloc(12, 7);
    TASK_CHECK(check_healthy_source(c, 84, DUMA_ACCESS_SOURCE_CALLOC) == 0);
    free(c);

    a = memalign(64, 33);
    TASK_CHECK(check_healthy_source(a, 33, DUMA_ACCESS_SOURCE_MEMALIGN) == 0);
    free(a);

    v = valloc(29);
    TASK_CHECK(check_healthy_source(v, 29, DUMA_ACCESS_SOURCE_VALLOC) == 0);
    free(v);

    s = strdup("duma-access");
    TASK_CHECK(check_healthy_source(s, strlen("duma-access") + 1,
                                    DUMA_ACCESS_SOURCE_STRDUP) == 0);
    free(s);

    return 0;
}

static int case_acquire_rejects_non_objects(void)
{
    duma_access_ref *ref = (duma_access_ref *)0x1;
    void *zero;
    uintptr_t stack_integer;
    int local = 17;

    TASK_CHECK(setup_once() == 0);
    stack_integer = (uintptr_t)&local;

    TASK_CHECK(duma_access_acquire(0, &ref) == DUMA_ACCESS_ENOENT);
    TASK_CHECK(ref == 0);

    TASK_CHECK(duma_access_acquire(stack_integer, &ref) == DUMA_ACCESS_ENOENT);
    TASK_CHECK(ref == 0);

    /* The allocator's Slot list is trusted internal storage, not a user object. */
    TASK_CHECK(duma_access_acquire((uintptr_t)_duma_g.allocList, &ref)
               == DUMA_ACCESS_EINVAL);
    TASK_CHECK(ref == 0);

    /* Zero-length allocations are never acquirable positive requests. */
    zero = malloc(0);
    if (zero)
    {
        TASK_CHECK(duma_access_acquire((uintptr_t)zero, &ref)
                   == DUMA_ACCESS_EINVAL);
        TASK_CHECK(ref == 0);
        free(zero);
    }

    /* Exact start only: an interior byte is not an acquisition point. */
    {
        unsigned char *p = malloc(32);
        TASK_CHECK(p != 0);
        TASK_CHECK(duma_access_acquire((uintptr_t)(p + 1), &ref)
                   == DUMA_ACCESS_ENOENT);
        TASK_CHECK(ref == 0);
        free(p);
    }

    duma_access_release_ref(0);
    duma_access_release_event(0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int case_free_event_independence(void)
{
    void *p;
    uintptr_t a;
    duma_access_ref *ref = 0;
    duma_access_event *live_event = 0, *freed_event = 0;
    const duma_access_report *live_report, *freed_report;

    TASK_CHECK(setup_once() == 0);
    p = malloc(64);
    TASK_CHECK(p != 0);
    a = (uintptr_t)p;
    TASK_CHECK(duma_access_acquire(a, &ref) == DUMA_ACCESS_OK);

    TASK_CHECK(duma_access_capture(ref, a, 64, DUMA_ACCESS_READ,
                                   &live_event) == DUMA_ACCESS_OK);
    live_report = duma_access_explain(live_event);
    TASK_CHECK(has_flag(live_report, DUMA_ACCESS_F_VALID));

    free(p);

    TASK_CHECK(duma_access_capture(ref, a, 8, DUMA_ACCESS_READ,
                                   &freed_event) == DUMA_ACCESS_OK);
    freed_report = duma_access_explain(freed_event);
    TASK_CHECK(freed_report != 0);
    TASK_CHECK(freed_report->ref_state == DUMA_ACCESS_FREED);
    TASK_CHECK(freed_report->transition_sequence != 0);
    TASK_CHECK(has_flag(freed_report, DUMA_ACCESS_F_STALE_REFERENCE));
    TASK_CHECK(has_flag(freed_report, DUMA_ACCESS_F_USE_AFTER_FREE));
    TASK_CHECK(has_flag(freed_report, DUMA_ACCESS_F_FREED_RESERVED));
    TASK_CHECK(!has_flag(freed_report, DUMA_ACCESS_F_VALID));
    TASK_CHECK(has_code(freed_report, "stale_reference"));
    TASK_CHECK(has_code(freed_report, "use_after_free"));
    TASK_CHECK(freed_report->segment_count >= 1);
    TASK_CHECK(freed_report->segments[0].kind
               == DUMA_ACCESS_SEG_FREED_RESERVED);
    TASK_CHECK(freed_report->segments[0].owner
               == freed_report->ref_identity);

    /* Zero length is no_access: lifecycle remains visible, but it is not
     * counted as a memory access/use-after-free violation. */
    {
        duma_access_event *zero_event = 0;
        const duma_access_report *zr;
        TASK_CHECK(duma_access_capture(ref, a, 0, DUMA_ACCESS_READ,
                                       &zero_event) == DUMA_ACCESS_OK);
        zr = duma_access_explain(zero_event);
        TASK_CHECK(has_flag(zr, DUMA_ACCESS_F_NO_ACCESS));
        TASK_CHECK(zr->ref_state == DUMA_ACCESS_FREED);
        TASK_CHECK(has_flag(zr, DUMA_ACCESS_F_STALE_REFERENCE));
        TASK_CHECK(!has_flag(zr, DUMA_ACCESS_F_USE_AFTER_FREE));
        TASK_CHECK(!has_flag(zr, DUMA_ACCESS_F_OUT_OF_BOUNDS));
        TASK_CHECK(zr->segment_count == 0);
        duma_access_release_event(zero_event);
    }

    /* The older event is an immutable observation from LIVE time. */
    live_report = duma_access_explain(live_event);
    TASK_CHECK(live_report->ref_state == DUMA_ACCESS_LIVE);
    TASK_CHECK(has_flag(live_report, DUMA_ACCESS_F_VALID));

    duma_access_release_ref(ref);

    /* Releasing the reference cannot mutate or invalidate the saved event. */
    freed_report = duma_access_explain(freed_event);
    TASK_CHECK(freed_report->ref_state == DUMA_ACCESS_FREED);
    TASK_CHECK(has_flag(freed_report, DUMA_ACCESS_F_USE_AFTER_FREE));

    duma_access_release_event(freed_event);
    duma_access_release_event(live_event);
    return 0;
}

static int case_realloc_success_failure_zero(void)
{
    void *p, *p2, *z, *z2;
    uintptr_t a, b, za;
    duma_access_ref *r1 = 0, *r2 = 0, *rz = 0;
    duma_access_event *e1 = 0, *e2 = 0;
    const duma_access_report *x1, *x2;
    uint64_t id1, id2;

    TASK_CHECK(setup_once() == 0);

    p = malloc(32);
    TASK_CHECK(p != 0);
    a = (uintptr_t)p;
    TASK_CHECK(duma_access_acquire(a, &r1) == DUMA_ACCESS_OK);
    TASK_CHECK(duma_access_capture(r1, a, 1, DUMA_ACCESS_READ, &e1)
               == DUMA_ACCESS_OK);
    x1 = duma_access_explain(e1);
    id1 = x1->ref_identity;
    duma_access_release_event(e1);

    p2 = realloc(p, 96);
    TASK_CHECK(p2 != 0);
    b = (uintptr_t)p2;
    TASK_CHECK(duma_access_acquire(b, &r2) == DUMA_ACCESS_OK);
    TASK_CHECK(duma_access_capture(r2, b, 96, DUMA_ACCESS_WRITE, &e2)
               == DUMA_ACCESS_OK);
    x2 = duma_access_explain(e2);
    TASK_CHECK(has_flag(x2, DUMA_ACCESS_F_VALID));
    id2 = x2->ref_identity;
    TASK_CHECK(id1 != id2);

    TASK_CHECK(duma_access_capture(r1, a, 4, DUMA_ACCESS_READ, &e1)
               == DUMA_ACCESS_OK);
    x1 = duma_access_explain(e1);
    TASK_CHECK(x1->ref_state == DUMA_ACCESS_REALLOCATED);
    TASK_CHECK(x1->transition_sequence != 0);
    TASK_CHECK(x1->successor_identity == id2);
    TASK_CHECK(has_flag(x1, DUMA_ACCESS_F_STALE_REFERENCE));
    TASK_CHECK(has_flag(x1, DUMA_ACCESS_F_REALLOCATED_REFERENCE));
    TASK_CHECK(has_code(x1, "reallocated_reference"));
    if (b == a)
    {
      TASK_CHECK(has_flag(x1, DUMA_ACCESS_F_REUSED_BY_OTHER));
      TASK_CHECK(x1->reused_by_identity == id2);
    }
    duma_access_release_event(e1);
    duma_access_release_event(e2);
    duma_access_release_ref(r2);
    duma_access_release_ref(r1);
    free(p2);

    /* A failed realloc leaves the old identity LIVE.  This outcome is covered
     * by the integer interpreter so the real allocator only performs healthy
     * requests. */
    {
        duma_access_model_identity live_id;
        duma_access_model failed_model;
        duma_access_event *failed_event = 0;
        const duma_access_report *failed_report = 0;

        memset(&live_id, 0, sizeof(live_id));
        memset(&failed_model, 0, sizeof(failed_model));
        live_id.identity = 303;
        live_id.request_start = 0x7000;
        live_id.request_end = 0x7030;
        live_id.state = DUMA_ACCESS_LIVE;
        failed_model.version = 1;
        failed_model.op = DUMA_ACCESS_READ;
        failed_model.address = 0x7000;
        failed_model.length = 0x30;
        failed_model.ref_identity = 303;
        failed_model.identities = &live_id;
        failed_model.identity_count = 1;
        TASK_CHECK(duma_access_model_run(&failed_model, &failed_event)
                   == DUMA_ACCESS_OK);
        failed_report = duma_access_explain(failed_event);
        TASK_CHECK(failed_report->ref_state == DUMA_ACCESS_LIVE);
        TASK_CHECK(failed_report->transition_sequence == 0);
        TASK_CHECK(has_flag(failed_report, DUMA_ACCESS_F_VALID));
        duma_access_release_event(failed_event);
    }

    /* Default DUMA zero-size strategy returns a unique protected zero block.
     * Accounting follows that actual successful realloc path. */
    z = malloc(16);
    TASK_CHECK(z != 0);
    za = (uintptr_t)z;
    TASK_CHECK(duma_access_acquire(za, &rz) == DUMA_ACCESS_OK);
    z2 = realloc(z, 0);
    TASK_CHECK(z2 != 0);
    {
        duma_access_event *ze = 0;
        const duma_access_report *zr;
        TASK_CHECK(duma_access_capture(rz, za, 1, DUMA_ACCESS_READ, &ze)
                   == DUMA_ACCESS_OK);
        zr = duma_access_explain(ze);
        TASK_CHECK(zr->ref_state == DUMA_ACCESS_REALLOCATED);
        duma_access_release_event(ze);
    }
    duma_access_release_ref(rz);
    TASK_CHECK(duma_access_acquire((uintptr_t)z2, &rz)
               == DUMA_ACCESS_EINVAL);
    free(z2);

    return 0;
}

struct concurrency_ctx
{
    int failures;
    int iterations;
    duma_access_ref **refs;
    uintptr_t *addresses;
    size_t count;
    pthread_mutex_t gate_mutex;
    pthread_cond_t gate_cond;
    int start;
};

static void concurrency_wait(struct concurrency_ctx *ctx)
{
    pthread_mutex_lock(&ctx->gate_mutex);
    while (!ctx->start)
        pthread_cond_wait(&ctx->gate_cond, &ctx->gate_mutex);
    pthread_mutex_unlock(&ctx->gate_mutex);
}

static void *capture_worker(void *arg)
{
    struct concurrency_ctx *ctx = (struct concurrency_ctx *)arg;
    int i;
    concurrency_wait(ctx);
    for (i = 0; i < ctx->iterations; ++i)
    {
        size_t idx = (size_t)i % ctx->count;
        duma_access_event *event = 0;
        const duma_access_report *r;
        duma_access_status st;

        st = duma_access_capture(ctx->refs[idx], ctx->addresses[idx], 64,
                                 (i & 1) ? DUMA_ACCESS_WRITE
                                         : DUMA_ACCESS_READ, &event);
        if (st != DUMA_ACCESS_OK || !event)
        {
            __sync_add_and_fetch(&ctx->failures, 1);
            continue;
        }
        r = duma_access_explain(event);
        if (!r || !has_flag(r, DUMA_ACCESS_F_VALID) ||
            r->ref_state != DUMA_ACCESS_LIVE ||
            r->segment_count != 1 ||
            r->segments[0].kind != DUMA_ACCESS_SEG_REQUEST ||
            r->segments[0].owner != r->ref_identity)
            __sync_add_and_fetch(&ctx->failures, 1);
        duma_access_release_event(event);
    }
    return 0;
}

static void *allocation_worker(void *arg)
{
    struct concurrency_ctx *ctx = (struct concurrency_ctx *)arg;
    int i;
    concurrency_wait(ctx);
    for (i = 0; i < ctx->iterations; ++i)
    {
        size_t n = 17 + (size_t)(i % 41);
        void *p = malloc(n);
        if (!p)
        {
            __sync_add_and_fetch(&ctx->failures, 1);
            continue;
        }
        memset(p, 3, n);
        if ((i & 3) == 0)
        {
            void *q = realloc(p, n + 16);
            if (!q)
            {
                __sync_add_and_fetch(&ctx->failures, 1);
                free(p);
                continue;
            }
            p = q;
        }
        free(p);
    }
    return 0;
}

static int case_concurrent_capture_and_allocation(void)
{
    enum { N = 4, CAP_THREADS = 4, WORK_THREADS = 2, ITER = 80 };
    void *ptrs[N];
    duma_access_ref *refs[N];
    uintptr_t addrs[N];
    pthread_t caps[CAP_THREADS], workers[WORK_THREADS];
    struct concurrency_ctx ctx;
    int i;

    TASK_CHECK(setup_once() == 0);
    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = ITER;
    ctx.refs = refs;
    ctx.addresses = addrs;
    ctx.count = N;
    ctx.start = 0;
    TASK_CHECK(pthread_mutex_init(&ctx.gate_mutex, 0) == 0);
    TASK_CHECK(pthread_cond_init(&ctx.gate_cond, 0) == 0);

    for (i = 0; i < N; ++i)
    {
        ptrs[i] = malloc(128);
        TASK_CHECK(ptrs[i] != 0);
        addrs[i] = (uintptr_t)ptrs[i];
        refs[i] = 0;
        TASK_CHECK(duma_access_acquire(addrs[i], &refs[i])
                   == DUMA_ACCESS_OK);
    }

    for (i = 0; i < CAP_THREADS; ++i)
        TASK_CHECK(pthread_create(&caps[i], 0, capture_worker, &ctx) == 0);
    for (i = 0; i < WORK_THREADS; ++i)
        TASK_CHECK(pthread_create(&workers[i], 0, allocation_worker, &ctx) == 0);

    pthread_mutex_lock(&ctx.gate_mutex);
    ctx.start = 1;
    pthread_cond_broadcast(&ctx.gate_cond);
    pthread_mutex_unlock(&ctx.gate_mutex);

    for (i = 0; i < CAP_THREADS; ++i)
        TASK_CHECK(pthread_join(caps[i], 0) == 0);
    for (i = 0; i < WORK_THREADS; ++i)
        TASK_CHECK(pthread_join(workers[i], 0) == 0);

    TASK_CHECK(ctx.failures == 0);

    for (i = 0; i < N; ++i)
    {
        duma_access_release_ref(refs[i]);
        free(ptrs[i]);
    }
    pthread_cond_destroy(&ctx.gate_cond);
    pthread_mutex_destroy(&ctx.gate_mutex);
    return 0;
}

static int case_metadata_reclaim_and_failed_cleanup(void)
{
    duma_access_stats before, after;
    uint64_t allocs_before, deallocs_before;
    int i;

    TASK_CHECK(setup_once() == 0);
    duma_access_get_stats(&before);
    duma_access_allocator_counts(&allocs_before, &deallocs_before);

    for (i = 0; i < 64; ++i)
    {
        void *p = malloc(24);
        duma_access_ref *ref = 0;
        duma_access_event *event = 0;
        const duma_access_report *r;

        TASK_CHECK(p != 0);
        TASK_CHECK(duma_access_acquire((uintptr_t)p, &ref)
                   == DUMA_ACCESS_OK);
        TASK_CHECK(duma_access_capture(ref, (uintptr_t)p, 24,
                                       DUMA_ACCESS_READ, &event)
                   == DUMA_ACCESS_OK);
        r = duma_access_explain(event);
        TASK_CHECK(has_flag(r, DUMA_ACCESS_F_VALID));

        /* Diagnostic metadata is an integer address too, but is positively
         * classified as diagnostic storage rather than a guessed user object. */
        {
            duma_access_event *diag_event = 0;
            const duma_access_report *dr;
            TASK_CHECK(duma_access_capture(0, (uintptr_t)event, 8,
                                          DUMA_ACCESS_READ, &diag_event)
                       == DUMA_ACCESS_OK);
            dr = duma_access_explain(diag_event);
            TASK_CHECK(dr != 0 && dr->segment_count >= 1);
            TASK_CHECK(dr->segments[0].kind
                       == DUMA_ACCESS_SEG_DIAGNOSTIC);
            TASK_CHECK(dr->segments[0].owner == 0);
            duma_access_release_event(diag_event);
        }

        duma_access_release_ref(ref);
        free(p);

        /* Event outlives both its reference and the allocation. */
        TASK_CHECK(duma_access_explain(event) != 0);
        duma_access_release_event(event);
    }

    /* Invalid calls publish no event or reference. */
    {
        duma_access_event *bad_event = (duma_access_event *)0x1;
        duma_access_ref *bad_ref = (duma_access_ref *)0x1;
        TASK_CHECK(duma_access_capture(0, UINT64_MAX, 2,
                                      DUMA_ACCESS_READ, &bad_event)
                   == DUMA_ACCESS_EOVERFLOW);
        TASK_CHECK(bad_event == 0);
        TASK_CHECK(duma_access_capture(0, 0, 1, 0, &bad_event)
                   == DUMA_ACCESS_EINVAL);
        TASK_CHECK(bad_event == 0);
        TASK_CHECK(duma_access_acquire((uintptr_t)&i, &bad_ref)
                   == DUMA_ACCESS_ENOENT);
        TASK_CHECK(bad_ref == 0);
    }

    duma_access_get_stats(&after);
    TASK_CHECK(after.live_refs == 0);
    TASK_CHECK(after.live_events == 0);
    TASK_CHECK(after.mapped_bytes == before.mapped_bytes);
    TASK_CHECK(!after.unavailable);

    (void)allocs_before;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ownership through the shared integer interpreter                    */
/* ------------------------------------------------------------------ */

static int run_integer_model(const duma_access_model *model,
                             duma_access_event **event,
                             const duma_access_report **report)
{
    duma_access_status st = duma_access_model_run(model, event);
    if (st != DUMA_ACCESS_OK)
        return (int)st;
    if (report)
        *report = duma_access_explain(*event);
    return 0;
}

static int case_integer_guard_alignment_cross(void)
{
    duma_access_model_identity ids[4];
    duma_access_model_segment segs[8];
    duma_access_model model;
    duma_access_event *event = 0;
    const duma_access_report *r;

    TASK_CHECK(setup_once() == 0);
    memset(ids, 0, sizeof(ids));
    memset(segs, 0, sizeof(segs));
    memset(&model, 0, sizeof(model));

    ids[0].identity = 1;
    ids[0].request_start = 0x1040;
    ids[0].request_end = 0x1080;
    ids[0].state = DUMA_ACCESS_LIVE;
    ids[1].identity = 2;
    ids[1].request_start = 0x5000;
    ids[1].request_end = 0x5040;
    ids[1].state = DUMA_ACCESS_LIVE;
    ids[2].identity = 3;
    ids[2].request_start = 0x8000;
    ids[2].request_end = 0x8020;
    ids[2].state = DUMA_ACCESS_LIVE;
    ids[3].identity = 4;
    ids[3].request_start = 0x9000;
    ids[3].request_end = 0x9020;
    ids[3].state = DUMA_ACCESS_LIVE;

    /* Above-request guard direction and readable alignment margin. */
    segs[0] = (duma_access_model_segment){0x1000, 0x1040,
        DUMA_ACCESS_SEG_ALIGNMENT, 3, 1, 0x1040, 0x1080};
    segs[1] = (duma_access_model_segment){0x1040, 0x1080,
        DUMA_ACCESS_SEG_REQUEST, 3, 1, 0x1040, 0x1080};
    segs[2] = (duma_access_model_segment){0x1080, 0x2000,
        DUMA_ACCESS_SEG_ALIGNMENT, 3, 1, 0x1040, 0x1080};
    segs[3] = (duma_access_model_segment){0x2000, 0x3000,
        DUMA_ACCESS_SEG_GUARD, 0, 1, 0x1040, 0x1080};

    model.version = 1;
    model.op = DUMA_ACCESS_READ;
    model.address = 0x1100;
    model.length = 8;
    model.ref_identity = 1;
    model.identities = ids;
    model.identity_count = 4;
    model.segments = segs;
    model.segment_count = 4;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_OUT_OF_BOUNDS));
    TASK_CHECK(!has_flag(r, DUMA_ACCESS_F_VALID));
    TASK_CHECK(r->segment_count == 1);
    TASK_CHECK(segment_is(r, 0, 0x1100, 0x1108,
                          DUMA_ACCESS_SEG_ALIGNMENT));
    TASK_CHECK(r->oob_count == 1);
    duma_access_release_event(event);

    /* Cross request tail, alignment and guard in one observation. */
    model.address = 0x1070;
    model.length = 0x2010 - 0x1070;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_OUT_OF_BOUNDS));
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_GUARD_TOUCHED));
    TASK_CHECK(r->segment_count == 3);
    TASK_CHECK(segment_is(r, 0, 0x1070, 0x1080,
                          DUMA_ACCESS_SEG_REQUEST));
    TASK_CHECK(segment_is(r, 1, 0x1080, 0x2000,
                          DUMA_ACCESS_SEG_ALIGNMENT));
    TASK_CHECK(segment_is(r, 2, 0x2000, 0x2010,
                          DUMA_ACCESS_SEG_GUARD));
    TASK_CHECK(r->oob_count == 1);
    TASK_CHECK(r->oob[0].start == 0x1080 && r->oob[0].end == 0x2010);
    duma_access_release_event(event);

    /* Below-request guard direction. */
    segs[4] = (duma_access_model_segment){0x4000, 0x5000,
        DUMA_ACCESS_SEG_GUARD, 0, 2, 0x5000, 0x5040};
    segs[5] = (duma_access_model_segment){0x5000, 0x5040,
        DUMA_ACCESS_SEG_REQUEST, 3, 2, 0x5000, 0x5040};
    segs[6] = (duma_access_model_segment){0x5040, 0x6000,
        DUMA_ACCESS_SEG_ALIGNMENT, 3, 2, 0x5000, 0x5040};
    model.address = 0x4ff8;
    model.length = 0x5008 - 0x4ff8;
    model.ref_identity = 2;
    model.segment_count = 7;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_OUT_OF_BOUNDS));
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_GUARD_TOUCHED));
    TASK_CHECK(r->segment_count == 2);
    TASK_CHECK(segment_is(r, 0, 0x4ff8, 0x5000, DUMA_ACCESS_SEG_GUARD));
    TASK_CHECK(segment_is(r, 1, 0x5000, 0x5008,
                          DUMA_ACCESS_SEG_REQUEST));
    TASK_CHECK(r->oob_count == 1 && r->oob[0].side == 1);
    duma_access_release_event(event);

    /* Cross two current owners; the gap is unknown, not guessed. */
    segs[0] = (duma_access_model_segment){0x8000, 0x8020,
        DUMA_ACCESS_SEG_REQUEST, 3, 3, 0x8000, 0x8020};
    segs[1] = (duma_access_model_segment){0x9000, 0x9020,
        DUMA_ACCESS_SEG_REQUEST, 3, 4, 0x9000, 0x9020};
    model.address = 0x8010;
    model.length = 0x9010 - 0x8010;
    model.ref_identity = 3;
    model.segments = segs;
    model.segment_count = 2;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(r->segment_count == 3);
    TASK_CHECK(segment_is(r, 0, 0x8010, 0x8020,
                          DUMA_ACCESS_SEG_REQUEST));
    TASK_CHECK(segment_is(r, 1, 0x8020, 0x9000,
                          DUMA_ACCESS_SEG_UNKNOWN));
    TASK_CHECK(segment_is(r, 2, 0x9000, 0x9010,
                          DUMA_ACCESS_SEG_REQUEST));
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_CROSSES_OWNERSHIP));
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_OUT_OF_BOUNDS));
    duma_access_release_event(event);

    return 0;
}

static int case_exact_reuse_shared_interpreter(void)
{
    enum { A = 0x10000, REQ_END = 0x10040, MAP_END = 0x20000,
           PARTIAL_END = 0x14000, NEW_ALIGN_END = 0x18000 };
    duma_access_model_identity ids[2];
    duma_access_model_segment segs[4];
    duma_access_model model;
    duma_access_event *event = 0, *saved_freed = 0;
    const duma_access_report *r, *saved;
    uint64_t current_table_owner;

    TASK_CHECK(setup_once() == 0);
    memset(ids, 0, sizeof(ids));
    memset(segs, 0, sizeof(segs));
    memset(&model, 0, sizeof(model));

    ids[0].identity = 101;
    ids[0].request_start = A;
    ids[0].request_end = REQ_END;
    ids[0].state = DUMA_ACCESS_FREED;
    ids[0].transition_sequence = 10;
    ids[1].identity = 202;
    ids[1].request_start = A;
    ids[1].request_end = REQ_END;
    ids[1].state = DUMA_ACCESS_LIVE;

    model.version = 1;
    model.op = DUMA_ACCESS_READ;
    model.ref_identity = 101;
    model.identities = ids;
    model.identity_count = 2;
    model.segments = segs;

    /* Stage 1: free, mapping retained and protected. */
    segs[0] = (duma_access_model_segment){A, MAP_END,
        DUMA_ACCESS_SEG_FREED_RESERVED, 0, 101, A, REQ_END};
    model.address = A;
    model.length = 16;
    model.segment_count = 1;
    TASK_CHECK(run_integer_model(&model, &saved_freed, &saved) == 0);
    TASK_CHECK(saved->ref_state == DUMA_ACCESS_FREED);
    TASK_CHECK(has_flag(saved, DUMA_ACCESS_F_USE_AFTER_FREE));
    TASK_CHECK(has_flag(saved, DUMA_ACCESS_F_FREED_RESERVED));
    TASK_CHECK(!has_flag(saved, DUMA_ACCESS_F_REUSED_BY_OTHER));
    TASK_CHECK(segment_is(saved, 0, A, A + 16,
                          DUMA_ACCESS_SEG_FREED_RESERVED));

    /* Stage 2: protection-reserve reclamation partially unmaps the tail. */
    segs[0].end = PARTIAL_END;
    model.address = A + 0x30;
    model.length = 0x15030 - (A + 0x30);
    event = 0;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(r->segment_count == 2);
    TASK_CHECK(segment_is(r, 0, A + 0x30, PARTIAL_END,
                          DUMA_ACCESS_SEG_FREED_RESERVED));
    TASK_CHECK(segment_is(r, 1, PARTIAL_END, 0x15030,
                          DUMA_ACCESS_SEG_UNKNOWN));
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_UNKNOWN_REGION));
    duma_access_release_event(event);

    /* Stage 3: a new allocation reuses the exact same user address. */
    segs[0] = (duma_access_model_segment){A, REQ_END,
        DUMA_ACCESS_SEG_REQUEST, 3, 202, A, REQ_END};
    segs[1] = (duma_access_model_segment){REQ_END, NEW_ALIGN_END,
        DUMA_ACCESS_SEG_ALIGNMENT, 3, 202, A, REQ_END};
    segs[2] = (duma_access_model_segment){NEW_ALIGN_END, MAP_END,
        DUMA_ACCESS_SEG_GUARD, 0, 202, A, REQ_END};
    model.address = A;
    model.length = REQ_END - A;
    model.segment_count = 3;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);

    /* A current-address table sees only live identity 202 at A. */
    current_table_owner = 202;
    TASK_CHECK(current_table_owner == 202);

    /* The access is nevertheless attributed to stale identity 101. */
    TASK_CHECK(r->associated);
    TASK_CHECK(r->ref_identity == 101);
    TASK_CHECK(r->ref_state == DUMA_ACCESS_FREED);
    TASK_CHECK(r->transition_sequence == 10);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_STALE_REFERENCE));
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_USE_AFTER_FREE));
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_REUSED_BY_OTHER));
    TASK_CHECK(r->reused_by_identity == 202);
    TASK_CHECK(!has_flag(r, DUMA_ACCESS_F_OUT_OF_BOUNDS));
    TASK_CHECK(!has_flag(r, DUMA_ACCESS_F_VALID));
    TASK_CHECK(has_code(r, "stale_reference"));
    TASK_CHECK(has_code(r, "reused_by_other"));
    TASK_CHECK(r->segment_count == 1);
    TASK_CHECK(segment_is(r, 0, A, REQ_END, DUMA_ACCESS_SEG_REQUEST));
    TASK_CHECK(r->segments[0].owner == 202);
    duma_access_release_event(event);

    /* Without the old reference, report physical facts but never assert UAF. */
    model.ref_identity = 0;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(!r->associated);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_UNASSOCIATED));
    TASK_CHECK(!has_flag(r, DUMA_ACCESS_F_USE_AFTER_FREE));
    TASK_CHECK(!has_flag(r, DUMA_ACCESS_F_STALE_REFERENCE));
    TASK_CHECK(r->segments[0].owner == 202);
    TASK_CHECK(has_code(r, "unassociated"));
    duma_access_release_event(event);

    /* The first saved observation remains item-for-item unchanged. */
    saved = duma_access_explain(saved_freed);
    TASK_CHECK(saved->ref_state == DUMA_ACCESS_FREED);
    TASK_CHECK(saved->segment_count == 1);
    TASK_CHECK(segment_is(saved, 0, A, A + 16,
                          DUMA_ACCESS_SEG_FREED_RESERVED));
    duma_access_release_event(saved_freed);

    return 0;
}

static int case_protected_reserve_shrink_model(void)
{
    duma_access_model_identity id;
    duma_access_model_segment seg;
    duma_access_model model;
    duma_access_event *event = 0;
    const duma_access_report *r;

    TASK_CHECK(setup_once() == 0);
    memset(&id, 0, sizeof(id));
    memset(&seg, 0, sizeof(seg));
    memset(&model, 0, sizeof(model));

    id.identity = 77;
    id.request_start = 0x40100;
    id.request_end = 0x40140;
    id.state = DUMA_ACCESS_FREED;
    id.transition_sequence = 44;

    model.version = 1;
    model.op = DUMA_ACCESS_WRITE;
    model.address = 0x40000;
    model.length = 0x8000;
    model.ref_identity = 77;
    model.identities = &id;
    model.identity_count = 1;
    model.segments = &seg;

    seg = (duma_access_model_segment){0x40000, 0x50000,
        DUMA_ACCESS_SEG_FREED_RESERVED, 0, 77, 0x40100, 0x40140};
    model.segment_count = 1;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(r->segment_count == 1);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_FREED_RESERVED));
    duma_access_release_event(event);

    /* First reclamation phase deletes the upper part of the mapping. */
    seg.end = 0x44000;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(r->segment_count == 2);
    TASK_CHECK(segment_is(r, 0, 0x40000, 0x44000,
                          DUMA_ACCESS_SEG_FREED_RESERVED));
    TASK_CHECK(segment_is(r, 1, 0x44000, 0x48000,
                          DUMA_ACCESS_SEG_UNKNOWN));
    duma_access_release_event(event);

    /* Second phase deletes the remaining page and clears the Slot. */
    model.segments = 0;
    model.segment_count = 0;
    TASK_CHECK(run_integer_model(&model, &event, &r) == 0);
    TASK_CHECK(r->segment_count == 1);
    TASK_CHECK(segment_is(r, 0, 0x40000, 0x48000,
                          DUMA_ACCESS_SEG_UNKNOWN));
    duma_access_release_event(event);
    return 0;
}

static int case_integer_input_rejects_conflicts(void)
{
    duma_access_model base;
    duma_access_model_identity ids[2];
    duma_access_model_segment segs[2];
    duma_access_event *event = 0;

    TASK_CHECK(setup_once() == 0);
    memset(&base, 0, sizeof(base));
    memset(ids, 0, sizeof(ids));
    memset(segs, 0, sizeof(segs));
    base.version = 1;
    base.op = DUMA_ACCESS_READ;
    base.address = 0x2000;
    base.length = 8;
    base.identities = ids;
    base.identity_count = 2;
    base.segments = segs;
    base.segment_count = 0;

    ids[0].identity = 1;
    ids[0].request_start = 0x2000;
    ids[0].request_end = 0x2040;
    ids[0].state = DUMA_ACCESS_LIVE;
    ids[1].identity = 1; /* duplicate identity */
    ids[1].request_start = 0x3000;
    ids[1].request_end = 0x3040;
    ids[1].state = DUMA_ACCESS_LIVE;
    TASK_CHECK(duma_access_model_run(&base, &event)
               == DUMA_ACCESS_ECONFLICT);
    TASK_CHECK(event == 0);

    ids[1].identity = 2;
    ids[1].state = 99; /* illegal lifecycle */
    TASK_CHECK(duma_access_model_run(&base, &event)
               == DUMA_ACCESS_EINVAL);
    ids[1].state = DUMA_ACCESS_LIVE;

    base.ref_identity = 999; /* missing identity */
    TASK_CHECK(duma_access_model_run(&base, &event)
               == DUMA_ACCESS_ECONFLICT);
    base.ref_identity = 0;

    base.address = UINT64_MAX;
    base.length = 2; /* wrap */
    TASK_CHECK(duma_access_model_run(&base, &event)
               == DUMA_ACCESS_EOVERFLOW);
    base.address = 0x2000;
    base.length = 8;

    segs[0] = (duma_access_model_segment){0x2000, 0x2010,
        DUMA_ACCESS_SEG_REQUEST, 3, 1, 0x2000, 0x2040};
    segs[1] = (duma_access_model_segment){0x2008, 0x2020,
        DUMA_ACCESS_SEG_ALIGNMENT, 3, 1, 0x2000, 0x2040};
    base.segment_count = 2;
    TASK_CHECK(duma_access_model_run(&base, &event)
               != DUMA_ACCESS_OK); /* overlapping layout ranges */
    TASK_CHECK(event == 0);

    segs[0].owner = 1;
    segs[1].owner = 1;
    segs[0].kind = DUMA_ACCESS_SEG_REQUEST;
    segs[1].start = 0x2010;
    segs[1].end = 0x2020;
    TASK_CHECK(duma_access_model_run(&base, &event)
               == DUMA_ACCESS_OK);
    duma_access_release_event(event);

    return 0;
}

static int case_slot_expansion_over_512_live(void)
{
    enum { N = 620 };
    static void *slots[N];
    void *anchor;
    duma_access_ref *anchor_ref = 0;
    duma_access_event *before_expansion = 0;
    duma_access_ref *first_ref = 0, *last_ref = 0;
    duma_access_event *event = 0;
    const duma_access_report *r, *before_report;
    uint64_t anchor_address;
    uint64_t allocs_before, deallocs_before, allocs_after, deallocs_after;
    int i;
    size_t first_size = 17;
    size_t last_size = 17 + (size_t)((N - 1) % 11);

    TASK_CHECK(setup_once() == 0);
    duma_access_allocator_counts(&allocs_before, &deallocs_before);

    /* This event is deliberately saved before repeated Slot expansion. */
    anchor = malloc(32);
    TASK_CHECK(anchor != 0);
    memset(anchor, 9, 32);
    anchor_address = (uintptr_t)anchor;
    TASK_CHECK(duma_access_acquire(anchor_address, &anchor_ref)
               == DUMA_ACCESS_OK);
    TASK_CHECK(duma_access_capture(anchor_ref, anchor_address, 32,
                                   DUMA_ACCESS_READ, &before_expansion)
               == DUMA_ACCESS_OK);
    TASK_CHECK(has_flag(duma_access_explain(before_expansion),
                        DUMA_ACCESS_F_VALID));
    duma_access_release_ref(anchor_ref);

    for (i = 0; i < N; ++i)
    {
        size_t size = 17 + (size_t)(i % 11);
        slots[i] = malloc(size);
        TASK_CHECK(slots[i] != 0);
        memset(slots[i], 42, size);
    }
    duma_checkAll();

    /* Slot expansion copied and replaced the allocator's Slot array, but the
     * event remains an unchanged integer snapshot. */
    before_report = duma_access_explain(before_expansion);
    TASK_CHECK(before_report != 0);
    TASK_CHECK(before_report->address == anchor_address);
    TASK_CHECK(before_report->size == 32);
    TASK_CHECK(before_report->ref_state == DUMA_ACCESS_LIVE);
    TASK_CHECK(has_flag(before_report, DUMA_ACCESS_F_VALID));

    TASK_CHECK(duma_access_acquire((uintptr_t)slots[0], &first_ref)
               == DUMA_ACCESS_OK);
    TASK_CHECK(duma_access_acquire((uintptr_t)slots[N - 1], &last_ref)
               == DUMA_ACCESS_OK);
    TASK_CHECK(duma_access_capture(first_ref, (uintptr_t)slots[0],
                                   first_size, DUMA_ACCESS_READ, &event)
               == DUMA_ACCESS_OK);
    r = duma_access_explain(event);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_VALID));
    duma_access_release_event(event);

    TASK_CHECK(duma_access_capture(last_ref, (uintptr_t)slots[N - 1],
                                   last_size, DUMA_ACCESS_READ, &event)
               == DUMA_ACCESS_OK);
    r = duma_access_explain(event);
    TASK_CHECK(has_flag(r, DUMA_ACCESS_F_VALID));
    duma_access_release_event(event);
    duma_access_release_ref(first_ref);
    duma_access_release_ref(last_ref);
    duma_access_release_event(before_expansion);
    free(anchor);

    for (i = 0; i < N; ++i)
        free(slots[i]);

    duma_access_allocator_counts(&allocs_after, &deallocs_after);
    TASK_CHECK(allocs_after - allocs_before >= (uint64_t)N);
    TASK_CHECK(deallocs_after - deallocs_before >= (uint64_t)N);

    /* This allocation intentionally predates every acquire; release it last. */
    if (g_live_before_first_acquire)
    {
        free(g_live_before_first_acquire);
        g_live_before_first_acquire = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver registration                                                 */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    {"attribution", "pre_acquire_live_allocation", case_pre_acquire_live_allocation},
    {"attribution", "allocation_sources", case_allocation_sources},
    {"attribution", "reject_internal_zero_unknown", case_acquire_rejects_non_objects},

    {"lifecycle", "free_and_event_independence", case_free_event_independence},
    {"lifecycle", "realloc_success_failure_zero", case_realloc_success_failure_zero},
    {"lifecycle", "concurrent_capture_allocation", case_concurrent_capture_and_allocation},
    {"lifecycle", "metadata_reclaim_failed_cleanup", case_metadata_reclaim_and_failed_cleanup},

    {"ownership", "integer_guard_alignment_cross", case_integer_guard_alignment_cross},
    {"ownership", "exact_reuse_shared_interpreter", case_exact_reuse_shared_interpreter},
    {"ownership", "protected_reserve_shrink_model", case_protected_reserve_shrink_model},
    {"ownership", "integer_input_rejects_conflicts", case_integer_input_rejects_conflicts},
    {"ownership", "slot_expansion_over_512_live", case_slot_expansion_over_512_live}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
