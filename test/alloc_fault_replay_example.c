#include "theft.h"
#include "alloc_fault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/*
 * Minimal replay example.
 *
 * A saved fault plan is pure data -- a config name, a run seed, and
 * 1-based positive-length allocation attempt numbers -- never runtime
 * pointers. This program reads one plan file, rebuilds the matching
 * property configuration, injects the plan's first fault position, and
 * re-runs that exact scenario in a fresh process, then prints the
 * observed result and ledger state.
 *
 * Build (against the fault-injected library):
 *   cc -std=c99 -I. -Iinc -Isrc -I.task-build \
 *     test/alloc_fault_replay_example.c test/alloc_fault.c \
 *     .task-build/fault/libtheft.a -lm -o replay_example
 *
 * Run:
 *   ./replay_example .task-build/alloc-fault-plans/init_default_post_hash.txt
 */

struct ex_obj { uint32_t value; };

static enum theft_trial_res
ex_prop(struct theft *t, void *arg) {
    (void)t;
    struct ex_obj *o = (struct ex_obj *)arg;
    return o->value < (1u << 20) ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* The argument object is a test-owned object: allocated through the
 * explicit qa_user_* entry point so it lands in the user ledger, not
 * in the recorder's own overhead. */
static enum theft_alloc_res
ex_alloc(struct theft *t, void *env, void **output) {
    (void)env;
    struct ex_obj *o = qa_user_malloc(sizeof(*o), "example", 0);
    if (o == NULL) { return THEFT_ALLOC_ERROR; }
    o->value = (uint32_t)theft_random_bits(t, 16);
    *output = o;
    return THEFT_ALLOC_OK;
}

static void ex_free(void *instance, void *env) {
    (void)env;
    qa_user_free(instance, "example");
}

static theft_hash ex_hash(const void *instance, void *env) {
    (void)env;
    const struct ex_obj *o = (const struct ex_obj *)instance;
    return theft_hash_onepass((const uint8_t *)&o->value, sizeof(o->value));
}

/* Parse the first "PLAN config=... seed=0x... faults=a,b" line. */
static int parse_plan(const char *path, char *cfg, size_t cfg_len,
        uint64_t *seed, uint32_t *first_fault) {
    FILE *f = fopen(path, "r");
    if (f == NULL) { return -1; }
    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "PLAN ", 5) != 0) { continue; }
        char faults[64] = {0};
        if (sscanf(line, "PLAN config=%255s seed=%" SCNx64 " faults=%63s",
                cfg, seed, faults) >= 3) {
            cfg[cfg_len - 1] = '\0';
            *first_fault = (uint32_t)strtoul(faults, NULL, 10);
            found = 0;
        }
        break;
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1
        ? argv[1]
        : ".task-build/alloc-fault-plans/init_default_post_hash.txt");

    char cfg[256] = {0};
    uint64_t seed = 0;
    uint32_t fault = 0;
    if (parse_plan(path, cfg, sizeof(cfg), &seed, &fault) != 0) {
        fprintf(stderr, "could not parse a PLAN line from %s\n", path);
        return 2;
    }
    printf("replaying config=%s seed=0x%016" PRIx64 " fault=%u\n",
        cfg, seed, fault);

    /* Rebuild the same (hashable, default trial_post) configuration the
     * plan was recorded with. A full harness would look the name up in
     * a config registry. */
    struct theft_type_info ti = {
        .alloc = ex_alloc,
        .free = ex_free,
        .hash = ex_hash,
    };
    struct theft_run_config run_cfg = {
        .name = "replay_example",
        .prop1 = ex_prop,
        .type_info = { &ti },
        .trials = 4,
        .seed = seed,
        /* default trial_post => the library allocates its print env,
         * the allocation exercised by the init-leak repro. */
    };

    qa_reset();                 /* clean, empty recorder */
    qa_fault_single(fault);     /* fail exactly this attempt, once */

    enum theft_run_res res = theft_run(&run_cfg);

    printf("run_res=%d (%s) fault_hit=%s hits=%u\n",
        (int)res, theft_run_res_str(res),
        qa_fault_hit() ? "true" : "false", qa_fault_hits());
    printf("live_library_blocks=%zu live_user_objects=%zu violations=%zu\n",
        qa_live_count(), qa_user_live_count(), qa_violation_count());

    if (!qa_fault_hit() || qa_live_count() != 0
        || qa_user_live_count() != 0 || qa_violation_count() != 0) {
        qa_report(stderr);
        return 1;
    }
    puts("REPLAY_OK");
    return 0;
}
