#include "task_support/check.h"
#include "theft.h"
#include "alloc_fault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/stat.h>

/*
 * Bounded allocation-fault contract tests for libtheft.
 *
 * For every fixed (seed, config, property) we first run a fault-free
 * baseline to obtain the real allocation trajectory, then replay from
 * a clean recorder once per positive-length allocation attempt k,
 * failing only that attempt. Every replay must actually reach k. After
 * each run both ledgers (library blocks and test-owned argument
 * objects) are reconciled: everything the library was responsible for
 * must have been returned exactly once, with no double/invalid frees,
 * no lost realloc blocks, and no half-built arguments handed to the
 * property.
 */

#define QA_TRIAL_LIMIT       QA_MAX_TRIALS
#define QA_RUN_DEADLINE_SEC  5
#define QA_PLAN_DIR          ".task-build/alloc-fault-plans"

/* ---- Test objects: serial + read-only checksum ------------------ */

#define TOBJ_MAGIC 0x746f6231u  /* "tob1" */

struct tobj {
    uint32_t magic;
    uint32_t serial;
    uint64_t checksum;
    uint32_t value;
};

static uint64_t tobj_checksum(const struct tobj *o) {
    /* FNV-1a over the stable fields, excluding checksum itself. */
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *bytes;
    bytes = (const uint8_t *)&o->magic;
    for (size_t i = 0; i < sizeof(o->magic); i++) {
        h ^= bytes[i]; h *= 1099511628211ULL;
    }
    bytes = (const uint8_t *)&o->serial;
    for (size_t i = 0; i < sizeof(o->serial); i++) {
        h ^= bytes[i]; h *= 1099511628211ULL;
    }
    bytes = (const uint8_t *)&o->value;
    for (size_t i = 0; i < sizeof(o->value); i++) {
        h ^= bytes[i]; h *= 1099511628211ULL;
    }
    return h;
}

static bool tobj_verify(const void *p) {
    if (p == NULL) { return false; }
    const struct tobj *o = (const struct tobj *)p;
    return o->magic == TOBJ_MAGIC && o->checksum == tobj_checksum(o);
}

/* ---- Per-test context -------------------------------------------- */

enum tctx_mode {
    TCTX_PASS,          /* property always holds */
    TCTX_FAIL_BIG,      /* property fails for large values */
    TCTX_SKIP,          /* property always skips */
    TCTX_ERROR,         /* property always errors */
};

struct tctx {
    const char *name;
    enum tctx_mode mode;
    uint32_t fail_threshold;   /* value >= threshold -> FAIL */

    unsigned alloc_calls;
    unsigned free_calls;
    unsigned prop_calls;
    unsigned shrink_calls;
    unsigned gen_pre_calls;
    unsigned bad_objects_seen;

    bool alloc_returns_skip;   /* next user alloc returns SKIP */
    bool alloc_returns_error;  /* next user alloc returns ERROR */
    bool shrink_returns_error; /* manual shrink cb returns ERROR */
    bool halt_in_gen_pre;      /* gen_args_pre requests HALT */

    uint32_t next_serial;
    size_t pool_size;          /* autoshrink pool override (0 = default) */
    unsigned random_draws;     /* random bits requested per autoshrink alloc */
    size_t max_failed_shrinks; /* 0 -> library default */

    /* Stage reachability, latched by the hooks. */
    bool saw_generate;
    bool saw_trial;
    bool saw_shrink;

    /* Counterexample observed for a failing run (arg 0's value). */
    bool saw_counterexample;
    uint32_t counterexample_value;
};

static void tctx_init(struct tctx *c, const char *name) {
    memset(c, 0, sizeof(*c));
    c->name = name;
    c->mode = TCTX_PASS;
    c->fail_threshold = 8;
    c->random_draws = 6;
}

/* ---- Manual-shrink type callbacks -------------------------------- */

static enum theft_trial_res
prop_single(struct theft *t, void *arg) {
    (void)t;
    struct tctx *c = (struct tctx *)theft_hook_get_env(t);
    const struct tobj *o = (const struct tobj *)arg;
    if (!tobj_verify(o)) { c->bad_objects_seen++; return THEFT_TRIAL_ERROR; }
    c->prop_calls++;
    switch (c->mode) {
    case TCTX_SKIP:  return THEFT_TRIAL_SKIP;
    case TCTX_ERROR: return THEFT_TRIAL_ERROR;
    case TCTX_FAIL_BIG:
        return o->value >= c->fail_threshold ? THEFT_TRIAL_FAIL
                                             : THEFT_TRIAL_PASS;
    case TCTX_PASS:
    default:         return THEFT_TRIAL_PASS;
    }
}

static enum theft_trial_res
prop_pair(struct theft *t, void *a, void *b) {
    (void)t;
    struct tctx *c = (struct tctx *)theft_hook_get_env(t);
    const struct tobj *o0 = (const struct tobj *)a;
    const struct tobj *o1 = (const struct tobj *)b;
    if (!tobj_verify(o0) || !tobj_verify(o1)) {
        c->bad_objects_seen++;
        return THEFT_TRIAL_ERROR;
    }
    c->prop_calls++;
    if (c->mode == TCTX_SKIP) { return THEFT_TRIAL_SKIP; }
    if (c->mode == TCTX_ERROR) { return THEFT_TRIAL_ERROR; }
    if (c->mode == TCTX_FAIL_BIG) {
        return (o0->value + o1->value) >= c->fail_threshold
            ? THEFT_TRIAL_FAIL : THEFT_TRIAL_PASS;
    }
    return THEFT_TRIAL_PASS;
}

/* Allocator for the manual (non-autoshrink) type. */
static enum theft_alloc_res
manual_alloc(struct theft *t, void *env, void **output) {
    struct tctx *c = (struct tctx *)env;
    if (c->alloc_returns_skip) { return THEFT_ALLOC_SKIP; }
    if (c->alloc_returns_error) { return THEFT_ALLOC_ERROR; }

    struct tobj *o = (struct tobj *)qa_user_malloc(sizeof(*o), "manual", 0);
    if (o == NULL) { return THEFT_ALLOC_ERROR; }
    o->magic = TOBJ_MAGIC;
    o->serial = ++c->next_serial;
    o->value = (uint32_t)theft_random_bits(t, 16);
    o->checksum = tobj_checksum(o);
    c->alloc_calls++;
    *output = o;
    return THEFT_ALLOC_OK;
}

static void tobj_free_cb(void *instance, void *env) {
    struct tctx *c = (struct tctx *)env;
    if (instance == NULL) { return; }
    if (!tobj_verify(instance)) { c->bad_objects_seen++; }
    c->free_calls++;
    qa_user_free(instance, "manual");
}

static theft_hash tobj_hash_cb(const void *instance, void *env) {
    (void)env;
    const struct tobj *o = (const struct tobj *)instance;
    return theft_hash_onepass((const uint8_t *)&o->value, sizeof(o->value));
}

/* Hand-written shrink: tactic 0 halves, tactic 1 decrements. */
static enum theft_shrink_res
manual_shrink(struct theft *t, const void *instance, uint32_t tactic,
        void *env, void **output) {
    (void)t;
    struct tctx *c = (struct tctx *)env;
    const struct tobj *o = (const struct tobj *)instance;
    if (c->shrink_returns_error) { return THEFT_SHRINK_ERROR; }
    if (tactic > 1 || o->value == 0) {
        return THEFT_SHRINK_NO_MORE_TACTICS;
    }
    struct tobj *q = (struct tobj *)qa_user_malloc(sizeof(*q), "candidate", 0);
    if (q == NULL) { return THEFT_SHRINK_ERROR; }
    q->magic = TOBJ_MAGIC;
    q->serial = ++c->next_serial;
    q->value = (tactic == 0 ? o->value / 2 : o->value - 1);
    q->checksum = tobj_checksum(q);
    c->shrink_calls++;
    *output = q;
    return THEFT_SHRINK_OK;
}

/* ---- Autoshrink type callbacks ----------------------------------- */

static enum theft_alloc_res
auto_alloc(struct theft *t, void *env, void **output) {
    struct tctx *c = (struct tctx *)env;
    if (c->alloc_returns_skip) { return THEFT_ALLOC_SKIP; }
    if (c->alloc_returns_error) { return THEFT_ALLOC_ERROR; }

    struct tobj *o = (struct tobj *)qa_user_malloc(sizeof(*o), "autoshrink", 0);
    if (o == NULL) { return THEFT_ALLOC_ERROR; }
    o->magic = TOBJ_MAGIC;
    o->serial = ++c->next_serial;

    /* Several requests so a small pool must grow and the request
     * index must expand past its initial ceiling. */
    uint32_t acc = 0;
    for (unsigned i = 0; i < c->random_draws; i++) {
        acc = (acc << 4) | (uint32_t)theft_random_bits(t, 4);
    }
    o->value = acc;
    o->checksum = tobj_checksum(o);
    c->alloc_calls++;
    *output = o;
    return THEFT_ALLOC_OK;
}

static void auto_free_cb(void *instance, void *env) {
    struct tctx *c = (struct tctx *)env;
    if (instance == NULL) { return; }
    if (!tobj_verify(instance)) { c->bad_objects_seen++; }
    c->free_calls++;
    qa_user_free(instance, "autoshrink");
}

/* ---- Silent hooks used to label stages and avoid stdout noise ---- */

static enum theft_hook_run_pre_res
hook_run_pre(const struct theft_hook_run_pre_info *info, void *env) {
    (void)info; (void)env;
    qa_set_stage("run");
    return THEFT_HOOK_RUN_PRE_CONTINUE;
}

static enum theft_hook_run_post_res
hook_run_post(const struct theft_hook_run_post_info *info, void *env) {
    (void)info; (void)env;
    qa_set_stage("run-post");
    return THEFT_HOOK_RUN_POST_CONTINUE;
}

static enum theft_hook_gen_args_pre_res
hook_gen_args_pre(const struct theft_hook_gen_args_pre_info *info, void *env) {
    struct tctx *c = (struct tctx *)env;
    (void)info;
    c->gen_pre_calls++;
    c->saw_generate = true;
    qa_set_stage("generate");
    if (c->halt_in_gen_pre) { return THEFT_HOOK_GEN_ARGS_PRE_HALT; }
    return THEFT_HOOK_GEN_ARGS_PRE_CONTINUE;
}

static enum theft_hook_trial_pre_res
hook_trial_pre(const struct theft_hook_trial_pre_info *info, void *env) {
    struct tctx *c = (struct tctx *)env;
    (void)info;
    c->saw_trial = true;
    qa_set_stage("trial");
    return THEFT_HOOK_TRIAL_PRE_CONTINUE;
}

static enum theft_hook_trial_post_res
hook_trial_post_silent(const struct theft_hook_trial_post_info *info,
        void *env) {
    (void)info; (void)env;
    return THEFT_HOOK_TRIAL_POST_CONTINUE;
}

static enum theft_hook_counterexample_res
hook_counterexample_silent(const struct theft_hook_counterexample_info *info,
        void *env) {
    struct tctx *c = (struct tctx *)env;
    if (info->arity >= 1 && info->args[0] != NULL) {
        const struct tobj *o = (const struct tobj *)info->args[0];
        if (tobj_verify(o)) {
            c->saw_counterexample = true;
            c->counterexample_value = o->value;
        } else {
            c->bad_objects_seen++;
        }
    }
    return THEFT_HOOK_COUNTEREXAMPLE_CONTINUE;
}

static enum theft_hook_shrink_pre_res
hook_shrink_pre(const struct theft_hook_shrink_pre_info *info, void *env) {
    struct tctx *c = (struct tctx *)env;
    (void)info;
    c->saw_shrink = true;
    qa_set_stage("shrink");
    return THEFT_HOOK_SHRINK_PRE_CONTINUE;
}

/* ---- Type info builders ------------------------------------------ */

static struct theft_type_info make_manual_type(struct tctx *c) {
    struct theft_type_info ti = {
        .alloc = manual_alloc,
        .free = tobj_free_cb,
        .hash = tobj_hash_cb,
        .shrink = manual_shrink,
        .env = c,
    };
    return ti;
}

static struct theft_type_info make_auto_type(struct tctx *c, bool with_hash) {
    struct theft_type_info ti = {
        .alloc = auto_alloc,
        .free = auto_free_cb,
        .env = c,
        .autoshrink_config = {
            .enable = true,
            .pool_size = c->pool_size,
            .max_failed_shrinks = c->max_failed_shrinks,
        },
    };
    if (with_hash) { ti.hash = tobj_hash_cb; }
    return ti;
}

/* ---- Run wrapper and ledger reconciliation ----------------------- */

static void prepare_plan_dir(void);
static void save_evidence(const char *tag);

struct baseline {
    int res;
    uint32_t attempts;           /* positive-length allocation attempts */
    size_t events;
    size_t user_allocs;
    size_t user_frees;
    unsigned prop_calls;
    bool reached_shrink;
    bool reached_generate;
    bool had_counterexample;
    uint32_t counterexample_value;
};

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Locate the single failed positive-length event in the current log. */
static const struct qa_event *find_failed_event(void) {
    for (size_t i = 0; i < qa_event_count(); i++) {
        const struct qa_event *e = qa_event_at(i);
        if (!e->ok && e->attempt != 0) { return e; }
    }
    return NULL;
}

/* Allocation size signatures of the fault-built library on this
 * (LP64) platform. Fixed sizes are pinned from the struct layouts;
 * the two variable-size families are recognized structurally. Only
 * bloom allocations may degrade; every other failure must error.
 *
 *   struct theft                 560
 *   struct theft_rng            2504
 *   struct autoshrink_env        104
 *   struct theft_bloom table    4104  (degradable: dedup disabled)
 *   struct bloom_filter block   16 + 2^k, k >= 6  (degradable)
 *   autoshrink request index    request_count * 8, shrink stage only
 */
#define QA_SIG_THEFT            560
#define QA_SIG_RNG             2504
#define QA_SIG_AUTOSHRINK_ENV   104
#define QA_SIG_BLOOM_TABLE     4104

static bool is_power2(size_t x) { return x != 0 && (x & (x - 1)) == 0; }

/* A lazily grown bloom filter block: sizeof(header)=16 plus a power
 * of two number of bitmap bytes (64, 128, 256, ...). */
static bool is_bloom_block_size(size_t size) {
    return size >= 80 && is_power2(size - 16);
}

/* The autoshrink request index is a shrink-stage malloc of
 * request_count size_t entries. A grown bloom block can share its
 * size, so this is only used for the "known allocation" guard; the
 * degradable decision additionally requires an observed normal
 * completion (an index failure always returns THEFT_RUN_ERROR). */
static bool is_autoshrink_index(const struct qa_event *e) {
    return e->op == QA_OP_MALLOC
        && strcmp(e->stage, "shrink") == 0
        && e->size >= 8 && (e->size % 8) == 0;
}

static bool is_known_malloc_signature_event(const struct qa_event *e) {
    if (e->op != QA_OP_MALLOC) { return true; }
    if (e->size == QA_SIG_THEFT || e->size == QA_SIG_RNG
        || e->size == QA_SIG_AUTOSHRINK_ENV
        || e->size == QA_SIG_BLOOM_TABLE
        || is_bloom_block_size(e->size)
        || is_autoshrink_index(e)) {
        return true;
    }
    return false;
}

/* Bloom filter table/block allocation failures are the library's
 * single supported degradation path. A same-sized autoshrink index is
 * never ignored, so callers must additionally confirm the run still
 * completed normally before treating the fault as degraded. */
static bool is_degradable_fault(const struct qa_event *e) {
    if (e == NULL || e->op != QA_OP_MALLOC) { return false; }
    return e->size == QA_SIG_BLOOM_TABLE || is_bloom_block_size(e->size);
}

static bool is_normal_run_result(int res) {
    return res == THEFT_RUN_PASS || res == THEFT_RUN_FAIL
        || res == THEFT_RUN_SKIP;
}

/* Fail loudly if the library ever allocates a malloc that matches none
 * of the pinned/structural signatures, so a future library change
 * cannot be silently misclassified as degradable. */
static int check_malloc_signatures(void) {
    for (size_t i = 0; i < qa_event_count(); i++) {
        const struct qa_event *e = qa_event_at(i);
        if (e->op == QA_OP_MALLOC && e->ok) {
            TASK_CHECK(is_known_malloc_signature_event(e));
        }
    }
    return 0;
}

static bool valid_run_result(int res) {
    return res == THEFT_RUN_PASS || res == THEFT_RUN_FAIL
        || res == THEFT_RUN_SKIP || res == THEFT_RUN_ERROR
        || res == THEFT_RUN_ERROR_MEMORY
        || res == THEFT_RUN_ERROR_BAD_ARGS;
}

/* The core post-run ownership check. allow_normal_result is true for
 * the supported bloom degradation points. */
static int check_ledger_clean(int run_res, bool allow_normal_result,
        const char *label) {
    (void)label;
    TASK_CHECK(valid_run_result(run_res));
    TASK_CHECK(qa_violation_count() == 0);
    TASK_CHECK(qa_live_count() == 0);
    TASK_CHECK(qa_user_live_count() == 0);
    TASK_CHECK(!qa_budget_exceeded());

    if (!allow_normal_result) {
        /* A real resource shortage must not be reported as ordinary
         * completion; hook halt/ERROR and property FAIL are exercised
         * separately without faults. */
        TASK_CHECK(run_res == THEFT_RUN_ERROR
            || run_res == THEFT_RUN_ERROR_MEMORY);
    }
    return 0;
}

static void write_plan_file(const char *cfg_name, uint64_t seed,
        const uint32_t *faults, size_t fault_count, int run_res,
        const char *tag) {
    struct qa_plan plan = {
        .config = cfg_name,
        .seed = seed,
        .fault_count = fault_count,
    };
    for (size_t i = 0; i < fault_count; i++) { plan.fault_seq[i] = faults[i]; }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.txt", QA_PLAN_DIR, tag);
    FILE *f = fopen(path, "a");
    if (f == NULL) { return; }
    struct qa_outcome oc = {
        .run_result = run_res,
        .fault_hit = qa_fault_hit(),
        .fault_hits = qa_fault_hits(),
        .live_blocks = qa_live_count(),
        .live_user_objects = qa_user_live_count(),
        .violations = qa_violation_count(),
        .last_stage = qa_stage(),
        .incomplete = qa_budget_exceeded(),
    };
    qa_plan_write(f, &plan, &oc);
    fclose(f);
}

struct run_cfg {
    uint8_t arity;
    struct theft_type_info *ti[3];
    theft_propfun1 *prop1;
    theft_propfun2 *prop2;
    bool default_trial_post;   /* use the print-result hook (needs env) */
    size_t trials;
    theft_seed seed;
    const char *name;
};

static int execute_run(const struct run_cfg *rc, struct tctx *c,
        int *run_res_out) {
    double start = now_seconds();

    /* Enforce the per-plan trial bound. */
    TASK_CHECK(rc->trials > 0 && rc->trials <= QA_TRIAL_LIMIT);

    struct theft_run_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = rc->name;
    cfg.trials = rc->trials;
    cfg.seed = rc->seed;
    for (uint8_t i = 0; i < rc->arity; i++) { cfg.type_info[i] = rc->ti[i]; }
    if (rc->arity == 1) { cfg.prop1 = rc->prop1; }
    else { cfg.prop2 = rc->prop2; }

    cfg.hooks.env = c;
    cfg.hooks.run_pre = hook_run_pre;
    cfg.hooks.run_post = hook_run_post;
    cfg.hooks.gen_args_pre = hook_gen_args_pre;
    cfg.hooks.trial_pre = hook_trial_pre;
    cfg.hooks.counterexample = hook_counterexample_silent;
    cfg.hooks.shrink_pre = hook_shrink_pre;
    if (!rc->default_trial_post) {
        cfg.hooks.trial_post = hook_trial_post_silent;
    }

    qa_set_stage("init");
    int res = theft_run(&cfg);
    double elapsed = now_seconds() - start;
    if (run_res_out) { *run_res_out = res; }

    TASK_CHECK(elapsed < QA_RUN_DEADLINE_SEC);
    return 0;
}

/* Run a fault-free baseline and capture the real trajectory. */
static int run_baseline(const struct run_cfg *rc, struct tctx *c,
        struct baseline *b) {
    c->saw_generate = false;
    c->saw_trial = false;
    c->saw_shrink = false;
    c->saw_counterexample = false;
    c->counterexample_value = 0;
    prepare_plan_dir();
    qa_reset();
    qa_fault_reset();
    unsigned prop_before = c->prop_calls;
    int res;
    TASK_CHECK(execute_run(rc, c, &res) == 0);

    memset(b, 0, sizeof(*b));
    b->res = res;
    b->attempts = qa_attempt_count();
    b->events = qa_event_count();
    b->user_allocs = qa_user_total_allocs();
    b->user_frees = qa_user_total_frees();
    b->prop_calls = c->prop_calls - prop_before;
    b->reached_shrink = c->saw_shrink;
    b->reached_generate = c->saw_generate;
    b->had_counterexample = c->saw_counterexample;
    b->counterexample_value = c->counterexample_value;

    if (getenv("QA_DEBUG")) {
        fprintf(stderr,
            "DBG baseline %s attempts=%u events=%zu users=%zu/%zu "
            "shrink=%u gen=%u\n",
            rc->name, b->attempts, b->events, b->user_allocs,
            b->user_frees, b->reached_shrink, b->reached_generate);
    }

    /* A clean baseline must leave nothing behind. */
    save_evidence(rc->name);
    TASK_CHECK(check_malloc_signatures() == 0);
    TASK_CHECK(qa_live_count() == 0);
    TASK_CHECK(qa_user_live_count() == 0);
    TASK_CHECK(qa_violation_count() == 0);
    TASK_CHECK(b->attempts > 0);
    TASK_CHECK(b->attempts <= QA_MAX_EVENTS);
    TASK_CHECK(b->events <= QA_MAX_EVENTS);
    TASK_CHECK(b->user_allocs == b->user_frees);
    return 0;
}

/* Replay, failing only the k'th positive-length allocation attempt. */
static int run_single_fault(const struct run_cfg *rc, struct tctx *c,
        uint32_t k, int *run_res_out) {
    qa_reset();
    qa_fault_single(k);
    TASK_CHECK(execute_run(rc, c, run_res_out) == 0);
    /* The replay must actually reach attempt k. */
    TASK_CHECK(qa_fault_hit());
    TASK_CHECK(qa_fault_hits() == 1);
    TASK_CHECK(qa_attempt_count() >= k);
    return 0;
}

/* Persist a violation/leak report before any assertion fails, so the
 * evidence survives. The library is never cleaned up on our behalf
 * first: the recorder still holds the live identities. */
static void save_evidence(const char *tag) {
    if (qa_live_count() == 0 && qa_violation_count() == 0
        && qa_user_live_count() == 0) {
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.violations.txt",
        QA_PLAN_DIR, tag);
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        qa_report(stderr);
        return;
    }
    fputs("VIOLATION REPORT (live blocks / bad frees / user leaks)\n", f);
    qa_report(f);
    fclose(f);
    qa_report(stderr);
}

/* Full sweep for one (seed, config): baseline + every single fault +
 * a clean re-run after the faults are withdrawn. */
static int sweep_config(const struct run_cfg *rc, struct tctx *c,
        bool expect_shrink, const char *tag, uint64_t seed) {
    struct baseline b;
    TASK_CHECK(run_baseline(rc, c, &b) == 0);
    if (expect_shrink) { TASK_CHECK(b.reached_shrink); }
    TASK_CHECK(b.reached_generate);

    for (uint32_t k = 1; k <= b.attempts; k++) {
        int res;
        TASK_CHECK(run_single_fault(rc, c, k, &res) == 0);
        const struct qa_event *fe = find_failed_event();
        TASK_CHECK(fe != NULL);
        if (getenv("QA_DEBUG") && qa_event_count() > b.events) {
            fprintf(stderr,
                "DBG replay %s k=%u events=%zu > baseline %zu users=%zu\n",
                tag, k, qa_event_count(), b.events,
                qa_user_total_allocs());
        }
        /* A bloom-shaped fault is only an accepted degradation when the
         * library actually keeps running to a normal completion. */
        bool degraded = is_degradable_fault(fe) && is_normal_run_result(res);
        char label[128];
        snprintf(label, sizeof(label), "%s k=%u", tag, k);
        /* Save the replayable plan and any evidence BEFORE asserting,
         * so a leak is recorded rather than reported as zero. */
        write_plan_file(rc->name, seed, &k, 1, res, tag);
        save_evidence(tag);
        TASK_CHECK(check_ledger_clean(res, degraded, label) == 0);
    }

    /* Withdraw all faults and re-run in the same process: the result,
     * property call count and object free count must match the clean
     * baseline, proving no residual state pollutes later fuzzing. */
    qa_reset();
    qa_fault_reset();
    unsigned prop_before = c->prop_calls;
    size_t free_before = qa_user_total_frees();
    c->saw_counterexample = false;
    c->counterexample_value = 0;
    int res2;
    TASK_CHECK(execute_run(rc, c, &res2) == 0);
    TASK_CHECK(res2 == b.res);
    TASK_CHECK(c->prop_calls - prop_before == b.prop_calls);
    TASK_CHECK(qa_user_total_frees() - free_before == b.user_frees);
    /* A failing run must reproduce the same counterexample value. */
    TASK_CHECK(c->saw_counterexample == b.had_counterexample);
    if (b.had_counterexample) {
        TASK_CHECK(c->counterexample_value == b.counterexample_value);
    }
    save_evidence(tag);
    TASK_CHECK(qa_live_count() == 0);
    TASK_CHECK(qa_user_live_count() == 0);
    TASK_CHECK(qa_violation_count() == 0);
    return 0;
}

/* ---- Two-fault combinations -------------------------------------- *
 * The second position is chosen from the trajectory observed after
 * the first fault actually fired, never copied from the fault-free
 * baseline's numbering. */

enum f2_sel {
    F2_GENERATE,    /* first later attempt in the generate stage */
    F2_REALLOC,     /* first later realloc (pool / index growth) */
    F2_SHRINK,      /* first later attempt in the shrink stage */
};

static bool event_matches_sel(const struct qa_event *e, enum f2_sel sel) {
    switch (sel) {
    case F2_GENERATE: return strcmp(e->stage, "generate") == 0;
    case F2_REALLOC:  return e->op == QA_OP_REALLOC && e->attempt != 0;
    case F2_SHRINK:   return strcmp(e->stage, "shrink") == 0;
    }
    return false;
}

/* f1_pos: a degradable first fault so the run keeps going. The f2
 * position is read out of the f1-only run's real trajectory. */
static int sweep_pair(const struct run_cfg *rc, struct tctx *c,
        uint32_t f1_pos, enum f2_sel sel, const char *tag,
        uint64_t seed) {
    /* First, reproduce the f1-only run and read its real trajectory. */
    qa_reset();
    qa_fault_single(f1_pos);
    int r1;
    TASK_CHECK(execute_run(rc, c, &r1) == 0);
    TASK_CHECK(qa_fault_hits() == 1);

    uint32_t f2_pos = 0;
    for (size_t i = 0; i < qa_event_count(); i++) {
        const struct qa_event *e = qa_event_at(i);
        if (e->attempt > f1_pos && event_matches_sel(e, sel)) {
            f2_pos = e->attempt;
            break;
        }
    }
    /* The configured property must reach the requested later stage;
     * a missing f2 is a real coverage failure, not a silent skip. */
    TASK_CHECK(f2_pos != 0);

    /* Now fail both positions in one clean run. */
    uint32_t faults[2] = { f1_pos, f2_pos };
    qa_reset();
    qa_fault_pair(f1_pos, f2_pos);
    int r2;
    TASK_CHECK(execute_run(rc, c, &r2) == 0);
    TASK_CHECK(qa_fault_hits() == 2);

    /* Classify both observed failures. Degradation is only accepted
     * when both faults are bloom-shaped and the run still completes
     * normally. */
    unsigned degradable = 0;
    for (size_t i = 0; i < qa_event_count(); i++) {
        const struct qa_event *e = qa_event_at(i);
        if (!e->ok && e->attempt != 0 && is_degradable_fault(e)) {
            degradable++;
        }
    }
    bool allow_normal = (degradable == 2) && is_normal_run_result(r2);
    write_plan_file(rc->name, seed, faults, 2, r2, tag);
    save_evidence(tag);
    TASK_CHECK(check_ledger_clean(r2, allow_normal, tag) == 0);
    return 0;
}

/* Find the top-level bloom table fault position (init stage) from a
 * baseline log. */
static uint32_t find_bloom_init_position(void) {
    for (size_t i = 0; i < qa_event_count(); i++) {
        const struct qa_event *e = qa_event_at(i);
        if (e->op == QA_OP_MALLOC && e->size == QA_SIG_BLOOM_TABLE
            && strcmp(e->stage, "init") == 0) {
            return e->attempt;
        }
    }
    return 0;
}

static void prepare_plan_dir(void) {
    /* Best-effort; plan writes are skipped if the directory is absent. */
    mkdir(QA_PLAN_DIR, 0755);
}

/* ============================== init ============================== */

static int case_init_default_post_hash(void) {
    prepare_plan_dir();
    struct tctx c; tctx_init(&c, "init_default_post_hash");
    c.mode = TCTX_PASS;
    struct theft_type_info ti = make_manual_type(&c);   /* has hash */
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = true, .trials = 4,
        .seed = 0x1111000011110000ULL, .name = "init_post_hash",
    };
    return sweep_config(&rc, &c, false, "init_default_post_hash", rc.seed);
}

static int case_init_default_post_no_hash(void) {
    struct tctx c; tctx_init(&c, "init_default_post_no_hash");
    c.mode = TCTX_PASS;
    struct theft_type_info ti = make_manual_type(&c);
    ti.hash = NULL;                                     /* no bloom */
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = true, .trials = 4,
        .seed = 0x2222000022220000ULL, .name = "init_post_nohash",
    };
    return sweep_config(&rc, &c, false,
        "init_default_post_no_hash", rc.seed);
}

static int case_init_custom_post(void) {
    struct tctx c; tctx_init(&c, "init_custom_post");
    c.mode = TCTX_PASS;
    struct theft_type_info ti = make_manual_type(&c);
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 4,
        .seed = 0x3333000033330000ULL, .name = "init_custom_post",
    };
    return sweep_config(&rc, &c, false, "init_custom_post", rc.seed);
}

/* ============================= trials ============================= */

static int case_trials_multiarg_partial_gen(void) {
    prepare_plan_dir();
    struct tctx c; tctx_init(&c, "trials_multiarg_partial_gen");
    c.mode = TCTX_PASS;
    c.pool_size = 64;                 /* force pool growth */
    c.random_draws = 12;

    struct tctx c2; tctx_init(&c2, "trials_multiarg_partial_gen_2");
    c2.mode = TCTX_PASS;
    c2.pool_size = 64;
    c2.random_draws = 8;

    struct theft_type_info t0 = make_auto_type(&c, false);
    struct theft_type_info t1 = make_auto_type(&c2, false);
    struct run_cfg rc = {
        .arity = 2, .ti = { &t0, &t1 }, .prop2 = prop_pair,
        .default_trial_post = false, .trials = 6,
        .seed = 0x4444000044440000ULL, .name = "trials_partial_gen",
    };
    int rv = sweep_config(&rc, &c, false,
        "trials_multiarg_partial_gen", rc.seed);
    if (rv != 0) { return rv; }

    /* Multi-arg generation that fails partway must release every
     * argument object built before the failure, exactly once. */
    TASK_CHECK(c.bad_objects_seen == 0);
    TASK_CHECK(c2.bad_objects_seen == 0);
    return 0;
}

static int case_trials_normal_skip(void) {
    struct tctx c; tctx_init(&c, "trials_normal_skip");
    c.mode = TCTX_SKIP;
    c.pool_size = 128;
    struct theft_type_info ti = make_auto_type(&c, true);
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 8,
        .seed = 0x5555000055550000ULL, .name = "trials_skip",
    };
    int rv = sweep_config(&rc, &c, false, "trials_normal_skip", rc.seed);
    if (rv != 0) { return rv; }
    TASK_CHECK(c.bad_objects_seen == 0);
    return 0;
}

static int case_trials_user_alloc_error(void) {
    prepare_plan_dir();
    struct tctx c; tctx_init(&c, "trials_user_alloc_error");
    c.mode = TCTX_PASS;
    struct theft_type_info ti = make_manual_type(&c);

    /* Clean run, but the user's own allocator refuses once: this is a
     * user allocation error, distinct from a library OOM. */
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 6,
        .seed = 0x6666000066660000ULL, .name = "trials_user_alloc_err",
    };
    qa_reset();
    qa_fault_reset();
    qa_user_fail_next(1);
    int res;
    TASK_CHECK(execute_run(&rc, &c, &res) == 0);
    TASK_CHECK(res == THEFT_RUN_ERROR);      /* not ERROR_MEMORY, not PASS */
    TASK_CHECK(qa_live_count() == 0);
    TASK_CHECK(qa_user_live_count() == 0);
    TASK_CHECK(qa_violation_count() == 0);

    /* And a library fault alongside the user failure must still
     * reconcile both ledgers. First capture the real (aborting)
     * trajectory of the user-failing run: only positions it actually
     * reaches are valid replay targets. */
    qa_reset();
    qa_fault_reset();
    qa_user_fail_next(1);
    int ur;
    TASK_CHECK(execute_run(&rc, &c, &ur) == 0);
    TASK_CHECK(ur == THEFT_RUN_ERROR);
    uint32_t user_fail_attempts = qa_attempt_count();
    TASK_CHECK(user_fail_attempts > 0);
    TASK_CHECK(qa_live_count() == 0);
    TASK_CHECK(qa_user_live_count() == 0);

    for (uint32_t k = 1; k <= user_fail_attempts; k++) {
        qa_reset();
        qa_fault_single(k);
        qa_user_fail_next(1);
        int r2;
        TASK_CHECK(execute_run(&rc, &c, &r2) == 0);
        /* The deterministic user-fail trajectory reaches every k up
         * to its own attempt count, so the fault must hit. */
        TASK_CHECK(qa_fault_hit());
        TASK_CHECK(check_ledger_clean(r2, false, "user+lib fault") == 0);
    }
    TASK_CHECK(c.bad_objects_seen == 0);
    return 0;
}

static int case_trials_stop_callback(void) {
    struct tctx c; tctx_init(&c, "trials_stop_callback");
    c.mode = TCTX_PASS;
    c.halt_in_gen_pre = true;               /* callback asks to stop */
    struct theft_type_info ti = make_manual_type(&c);
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 8,
        .seed = 0x7777000077770000ULL, .name = "trials_stop_cb",
    };
    qa_reset();
    qa_fault_reset();
    int res;
    TASK_CHECK(execute_run(&rc, &c, &res) == 0);
    /* A requested stop is ordinary control flow, never an error. */
    TASK_CHECK(res == THEFT_RUN_SKIP);
    TASK_CHECK(qa_live_count() == 0);
    TASK_CHECK(qa_user_live_count() == 0);
    TASK_CHECK(qa_violation_count() == 0);
    TASK_CHECK(c.gen_pre_calls >= 1);
    return 0;
}

/* =========================== shrinking =========================== */

static int case_shrink_manual(void) {
    prepare_plan_dir();
    struct tctx c; tctx_init(&c, "shrink_manual");
    c.mode = TCTX_FAIL_BIG;
    c.fail_threshold = 8;
    struct theft_type_info ti = make_manual_type(&c);
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 10,
        .seed = 0x8888000088880000ULL, .name = "shrink_manual",
    };
    int rv = sweep_config(&rc, &c, true, "shrink_manual", rc.seed);
    if (rv != 0) { return rv; }
    TASK_CHECK(c.bad_objects_seen == 0);
    TASK_CHECK(c.shrink_calls > 0);
    return 0;
}

/* A hand-written shrink callback that reports SHRINK_ERROR: the run
 * must abort with THEFT_RUN_ERROR and the current (never handed off)
 * argument must be released exactly once, with a clean ledger. */
static int case_shrink_manual_error(void) {
    struct tctx c; tctx_init(&c, "shrink_manual_error");
    c.mode = TCTX_FAIL_BIG;
    c.fail_threshold = 0;               /* every trial fails, deterministically */
    c.shrink_returns_error = true;
    struct theft_type_info ti = make_manual_type(&c);
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 6,
        .seed = 0x8889000088890000ULL, .name = "shrink_manual_err",
    };

    /* No-fault run: the shrink error itself must clean everything up. */
    qa_reset();
    qa_fault_reset();
    int res;
    TASK_CHECK(execute_run(&rc, &c, &res) == 0);
    TASK_CHECK(res == THEFT_RUN_ERROR);
    TASK_CHECK(qa_live_count() == 0);
    TASK_CHECK(qa_user_live_count() == 0);
    TASK_CHECK(qa_violation_count() == 0);

    /* And under injected library faults the same ownership holds. */
    struct baseline b;
    TASK_CHECK(run_baseline(&rc, &c, &b) == 0);
    for (uint32_t k = 1; k <= b.attempts; k++) {
        qa_reset();
        qa_fault_single(k);
        int r2;
        TASK_CHECK(execute_run(&rc, &c, &r2) == 0);
        TASK_CHECK(qa_fault_hit());
        const struct qa_event *fe = find_failed_event();
        TASK_CHECK(fe != NULL);
        bool degraded = is_degradable_fault(fe) && is_normal_run_result(r2);
        TASK_CHECK(check_ledger_clean(r2, degraded, "shrink_err") == 0);
    }
    TASK_CHECK(c.bad_objects_seen == 0);
    return 0;
}

static int case_shrink_autoshrink_int(void) {
    prepare_plan_dir();
    struct tctx c; tctx_init(&c, "shrink_autoshrink_int");
    c.mode = TCTX_FAIL_BIG;
    c.fail_threshold = 1 << 12;
    c.pool_size = 64;                      /* forces bit-pool realloc */
    c.random_draws = 10;
    c.max_failed_shrinks = 8;              /* bound the shrinking work */
    struct theft_type_info ti = make_auto_type(&c, true);
    const theft_seed seeds[] = {
        0x9999000099990000ULL, 0x9999000099990001ULL,
    };
    for (size_t s = 0; s < sizeof(seeds)/sizeof(seeds[0]); s++) {
        struct run_cfg rc = {
            .arity = 1, .ti = { &ti }, .prop1 = prop_single,
            .default_trial_post = false, .trials = 3,
            .seed = seeds[s], .name = "shrink_auto_int",
        };
        int rv = sweep_config(&rc, &c, true,
            "shrink_autoshrink_int", rc.seed);
        if (rv != 0) { return rv; }
    }
    TASK_CHECK(c.bad_objects_seen == 0);
    return 0;
}

static int case_shrink_autoshrink_bytes(void) {
    struct tctx c; tctx_init(&c, "shrink_autoshrink_bytes");
    c.mode = TCTX_FAIL_BIG;
    c.fail_threshold = 1 << 14;
    c.pool_size = 32;
    c.random_draws = 18;                   /* >16 reqs: index growth; >32 bits: pool growth */
    c.max_failed_shrinks = 4;
    struct theft_type_info ti = make_auto_type(&c, false);
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 2,
        .seed = 0xaaaa0000aaaa0000ULL, .name = "shrink_auto_bytes",
    };
    int rv = sweep_config(&rc, &c, true,
        "shrink_autoshrink_bytes", rc.seed);
    if (rv != 0) { return rv; }
    TASK_CHECK(c.bad_objects_seen == 0);
    return 0;
}

static int case_shrink_pair_faults(void) {
    prepare_plan_dir();
    struct tctx c; tctx_init(&c, "shrink_pair_faults");
    c.mode = TCTX_FAIL_BIG;
    c.fail_threshold = 1 << 12;
    c.pool_size = 32;
    /* 20 four-bit requests = 80 bits, beyond the 32-bit initial pool
     * and the 16-entry initial request index, so both must realloc. */
    c.random_draws = 20;
    c.max_failed_shrinks = 4;
    /* Hashable => bloom exists, so its init table is a degradable
     * first fault that lets the run reach the later stages. */
    struct theft_type_info ti = make_auto_type(&c, true);
    struct run_cfg rc = {
        .arity = 1, .ti = { &ti }, .prop1 = prop_single,
        .default_trial_post = false, .trials = 2,
        .seed = 0xbbbb0000bbbb0000ULL, .name = "shrink_pair",
    };

    struct baseline b;
    TASK_CHECK(run_baseline(&rc, &c, &b) == 0);
    TASK_CHECK(b.reached_shrink);
    uint32_t f1 = find_bloom_init_position();
    TASK_CHECK(f1 != 0);

    /* Confirm a realloc (pool/index growth) is actually reached in the
     * baseline, so the later-stage requirement is met, not dodged. */
    bool saw_realloc = false;
    for (size_t i = 0; i < qa_event_count(); i++) {
        if (qa_event_at(i)->op == QA_OP_REALLOC) { saw_realloc = true; }
    }
    TASK_CHECK(saw_realloc);

    TASK_CHECK(sweep_pair(&rc, &c, f1, F2_GENERATE,
        "pair_generate", rc.seed) == 0);
    TASK_CHECK(sweep_pair(&rc, &c, f1, F2_REALLOC,
        "pair_pool_growth", rc.seed) == 0);
    TASK_CHECK(sweep_pair(&rc, &c, f1, F2_SHRINK,
        "pair_shrink", rc.seed) == 0);
    TASK_CHECK(c.bad_objects_seen == 0);
    return 0;
}

/* ========================= registration ========================== */

const struct task_case task_cases[] = {
    { "init", "default_post_hash",      case_init_default_post_hash },
    { "init", "default_post_no_hash",   case_init_default_post_no_hash },
    { "init", "custom_trial_post",      case_init_custom_post },

    { "trials", "multiarg_partial_gen", case_trials_multiarg_partial_gen },
    { "trials", "normal_skip",          case_trials_normal_skip },
    { "trials", "user_alloc_error",     case_trials_user_alloc_error },
    { "trials", "stop_callback",        case_trials_stop_callback },

    { "shrinking", "manual_shrink",     case_shrink_manual },
    { "shrinking", "manual_shrink_error", case_shrink_manual_error },
    { "shrinking", "autoshrink_int",    case_shrink_autoshrink_int },
    { "shrinking", "autoshrink_bytes",  case_shrink_autoshrink_bytes },
    { "shrinking", "pair_faults",       case_shrink_pair_faults },
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
