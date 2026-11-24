#include "alloc_fault.h"

#include <stdlib.h>
#include <string.h>

/*
 * Allocation fault injector with a double ledger (library blocks and
 * test-owned argument objects). Everything the recorder needs is in
 * fixed-capacity static storage: this translation unit is compiled
 * with the normal allocator and never calls malloc on its own behalf,
 * so an injected fault cannot recursively break the bookkeeping.
 */

struct qa_record {
    uint32_t id;
    void    *addr;
    size_t   size;
    bool     live;
    uint32_t alloc_seq;
    uint32_t free_seq;
    qa_op_t  alloc_op;
};

static struct qa_record s_records[QA_MAX_RECORDS];
static size_t   s_record_count;
static uint32_t s_next_id;

static struct qa_event s_events[QA_MAX_EVENTS];
static size_t   s_event_count;
static uint32_t s_next_seq;
static uint32_t s_attempt;                 /* positive-length attempts */

static struct qa_violation s_violations[QA_MAX_VIOLATIONS];
static size_t   s_violation_count;

static struct qa_user_obj s_users[QA_MAX_USER_OBJECTS];
static size_t   s_user_count;
static size_t   s_user_allocs;
static size_t   s_user_frees;
static uint32_t s_user_next_seq;       /* user-ledger monotonic sequence */
static unsigned s_user_fail_left;

static uint32_t s_fault_pos[QA_MAX_PLAN_FAULTS];
static size_t   s_fault_count;
static unsigned s_fault_hits;

static bool     s_budget_exceeded;
static const char *s_stage;

const char *qa_op_str(qa_op_t op) {
    switch (op) {
    case QA_OP_MALLOC: return "malloc";
    case QA_OP_CALLOC: return "calloc";
    case QA_OP_REALLOC: return "realloc";
    case QA_OP_FREE:   return "free";
    default:           return "unknown";
    }
}

static void record_violation(uint32_t seq, qa_op_t op, void *addr,
        const char *kind) {
    if (s_violation_count < QA_MAX_VIOLATIONS) {
        s_violations[s_violation_count++] = (struct qa_violation) {
            .seq = seq, .op = op, .addr = addr, .kind = kind,
        };
    }
    s_budget_exceeded = s_violation_count >= QA_MAX_VIOLATIONS
        ? true : s_budget_exceeded;
}

static void record_event(qa_op_t op, size_t size, bool ok,
        uint32_t id, uint32_t new_id, void *addr, void *old_addr,
        uint32_t attempt) {
    if (s_event_count >= QA_MAX_EVENTS) {
        s_budget_exceeded = true;
        return;
    }
    s_events[s_event_count++] = (struct qa_event) {
        .seq = s_next_seq,
        .op = op,
        .size = size,
        .ok = ok,
        .id = id,
        .new_id = new_id,
        .addr = addr,
        .old_addr = old_addr,
        .attempt = attempt,
        .stage = s_stage,
    };
}

static struct qa_record *find_live(void *addr) {
    if (addr == NULL) { return NULL; }
    /* Identities are never inherited on address reuse, so only a live
     * record can own an address. */
    for (size_t i = 0; i < s_record_count; i++) {
        if (s_records[i].live && s_records[i].addr == addr) {
            return &s_records[i];
        }
    }
    return NULL;
}

static struct qa_record *find_any(void *addr) {
    if (addr == NULL) { return NULL; }
    for (size_t i = 0; i < s_record_count; i++) {
        if (s_records[i].addr == addr) { return &s_records[i]; }
    }
    return NULL;
}

static bool fault_due_now(uint32_t attempt) {
    for (size_t i = 0; i < s_fault_count; i++) {
        if (s_fault_pos[i] == attempt) {
            s_fault_hits++;
            return true;
        }
    }
    return false;
}

void qa_reset(void) {
    memset(s_records, 0, sizeof(s_records));
    memset(s_events, 0, sizeof(s_events));
    memset(s_violations, 0, sizeof(s_violations));
    memset(s_users, 0, sizeof(s_users));
    s_record_count = 0;
    s_event_count = 0;
    s_violation_count = 0;
    s_user_count = 0;
    s_user_allocs = 0;
    s_user_frees = 0;
    s_user_next_seq = 1;
    s_next_id = 1;
    s_next_seq = 1;
    s_attempt = 0;
    s_fault_hits = 0;
    s_budget_exceeded = false;
    s_stage = "init";
    s_fault_count = 0;
    s_user_fail_left = 0;
}

void qa_fault_reset(void) {
    s_fault_count = 0;
    s_fault_hits = 0;
}

void qa_fault_single(uint32_t attempt_pos) {
    qa_fault_reset();
    s_fault_pos[0] = attempt_pos;
    s_fault_count = 1;
}

void qa_fault_pair(uint32_t first, uint32_t second) {
    qa_fault_reset();
    s_fault_pos[0] = first;
    s_fault_pos[1] = second;
    s_fault_count = 2;
}

bool qa_fault_hit(void) { return s_fault_hits > 0; }
unsigned qa_fault_hits(void) { return s_fault_hits; }
uint32_t qa_attempt_count(void) { return s_attempt; }

void qa_set_stage(const char *stage) { s_stage = stage; }
const char *qa_stage(void) { return s_stage; }

void *qa_malloc(size_t size) {
    uint32_t seq = s_next_seq++;
    uint32_t attempt = 0;
    bool forced_fail = false;

    if (size > 0) {
        attempt = ++s_attempt;
        forced_fail = fault_due_now(attempt);
    }

    if (forced_fail) {
        record_event(QA_OP_MALLOC, size, false, 0, 0, NULL, NULL, attempt);
        return NULL;
    }

    void *p = malloc(size);
    if (p == NULL) {
        record_event(QA_OP_MALLOC, size, false, 0, 0, NULL, NULL, attempt);
        return NULL;
    }

    uint32_t id = 0;
    if (s_record_count < QA_MAX_RECORDS) {
        id = s_next_id++;
        s_records[s_record_count++] = (struct qa_record) {
            .id = id, .addr = p, .size = size, .live = true,
            .alloc_seq = seq, .alloc_op = QA_OP_MALLOC,
        };
    } else {
        s_budget_exceeded = true;
        record_violation(seq, QA_OP_MALLOC, p, "record table full");
    }
    record_event(QA_OP_MALLOC, size, true, id, 0, p, NULL, attempt);
    return p;
}

void *qa_calloc(size_t nmemb, size_t size) {
    uint32_t seq = s_next_seq++;
    uint32_t attempt = 0;

    /* Reject multiplying sizes that wrap, instead of relying on the
     * platform allocator's behavior. */
    if (size != 0 && nmemb > (size_t)-1 / size) {
        if (nmemb > 0 && size > 0) { attempt = ++s_attempt; }
        record_violation(seq, QA_OP_CALLOC, NULL, "calloc size overflow");
        record_event(QA_OP_CALLOC, nmemb * size, false, 0, 0, NULL, NULL,
            attempt);
        return NULL;
    }

    size_t total = nmemb * size;
    bool forced_fail = false;
    if (total > 0) {
        attempt = ++s_attempt;
        forced_fail = fault_due_now(attempt);
    }

    if (forced_fail) {
        record_event(QA_OP_CALLOC, total, false, 0, 0, NULL, NULL, attempt);
        return NULL;
    }

    void *p = calloc(nmemb, size);
    if (p == NULL) {
        record_event(QA_OP_CALLOC, total, false, 0, 0, NULL, NULL, attempt);
        return NULL;
    }

    uint32_t id = 0;
    if (s_record_count < QA_MAX_RECORDS) {
        id = s_next_id++;
        s_records[s_record_count++] = (struct qa_record) {
            .id = id, .addr = p, .size = total, .live = true,
            .alloc_seq = seq, .alloc_op = QA_OP_CALLOC,
        };
    } else {
        s_budget_exceeded = true;
        record_violation(seq, QA_OP_CALLOC, p, "record table full");
    }
    record_event(QA_OP_CALLOC, total, true, id, 0, p, NULL, attempt);
    return p;
}

void *qa_realloc(void *ptr, size_t size) {
    uint32_t seq = s_next_seq++;

    if (ptr == NULL) {
        /* realloc(NULL, n) behaves like malloc. */
        uint32_t attempt = 0;
        bool forced_fail = false;
        if (size > 0) {
            attempt = ++s_attempt;
            forced_fail = fault_due_now(attempt);
        }
        if (forced_fail) {
            record_event(QA_OP_REALLOC, size, false, 0, 0, NULL, NULL,
                attempt);
            return NULL;
        }
        void *p = realloc(NULL, size);
        if (p == NULL) {
            record_event(QA_OP_REALLOC, size, false, 0, 0, NULL, NULL,
                attempt);
            return NULL;
        }
        uint32_t id = 0;
        if (s_record_count < QA_MAX_RECORDS) {
            id = s_next_id++;
            s_records[s_record_count++] = (struct qa_record) {
                .id = id, .addr = p, .size = size, .live = true,
                .alloc_seq = seq, .alloc_op = QA_OP_REALLOC,
            };
        } else {
            s_budget_exceeded = true;
            record_violation(seq, QA_OP_REALLOC, p, "record table full");
        }
        record_event(QA_OP_REALLOC, size, true, 0, id, p, NULL, attempt);
        return p;
    }

    struct qa_record *rec = find_live(ptr);
    if (rec == NULL) {
        /* Unknown or already-freed address: never hand it to the system
         * allocator, which could abort. */
        const char *kind = find_any(ptr) ? "realloc of freed block"
                                         : "realloc of unknown address";
        record_violation(seq, QA_OP_REALLOC, ptr, kind);
        record_event(QA_OP_REALLOC, size, false, 0, 0, NULL, ptr, 0);
        return NULL;
    }

    if (size == 0) {
        /* Treat like free. */
        free(ptr);
        rec->live = false;
        rec->free_seq = seq;
        record_event(QA_OP_REALLOC, 0, true, rec->id, 0, NULL, ptr, 0);
        return NULL;
    }

    uint32_t attempt = ++s_attempt;
    if (fault_due_now(attempt)) {
        /* The old block, its bytes and its identity stay valid. */
        record_event(QA_OP_REALLOC, size, false, rec->id, 0, ptr, ptr,
            attempt);
        return NULL;
    }

    void *np = realloc(ptr, size);
    if (np == NULL) {
        /* Real allocation failure: old block and identity remain valid. */
        record_event(QA_OP_REALLOC, size, false, rec->id, 0, ptr, ptr,
            attempt);
        return NULL;
    }

    uint32_t old_id = rec->id;
    rec->live = false;
    rec->free_seq = seq;

    uint32_t new_id = 0;
    if (s_record_count < QA_MAX_RECORDS) {
        new_id = s_next_id++;
        s_records[s_record_count++] = (struct qa_record) {
            .id = new_id, .addr = np, .size = size, .live = true,
            .alloc_seq = seq, .alloc_op = QA_OP_REALLOC,
        };
    } else {
        s_budget_exceeded = true;
        record_violation(seq, QA_OP_REALLOC, np, "record table full");
    }
    record_event(QA_OP_REALLOC, size, true, old_id, new_id, np, ptr,
        attempt);
    return np;
}

void qa_free(void *ptr) {
    uint32_t seq = s_next_seq++;
    if (ptr == NULL) {
        /* free(NULL) is legal and does nothing. */
        record_event(QA_OP_FREE, 0, true, 0, 0, NULL, NULL, 0);
        return;
    }

    struct qa_record *rec = find_live(ptr);
    if (rec == NULL) {
        const char *kind = find_any(ptr) ? "double free"
                                         : "free of unknown address";
        record_violation(seq, QA_OP_FREE, ptr, kind);
        record_event(QA_OP_FREE, 0, false, 0, 0, ptr, NULL, 0);
        return;
    }

    free(ptr);
    rec->live = false;
    rec->free_seq = seq;
    record_event(QA_OP_FREE, rec->size, true, rec->id, 0, ptr, NULL, 0);
}

/* ---- Test-owned object ledger ------------------------------------ */

static struct qa_user_obj *find_user(void *addr) {
    if (addr == NULL) { return NULL; }
    /* Address reuse must not inherit an old identity: only a live
     * record can own the address. */
    for (size_t i = 0; i < s_user_count; i++) {
        if (s_users[i].live && s_users[i].addr == addr) {
            return &s_users[i];
        }
    }
    return NULL;
}

static struct qa_user_obj *find_user_any(void *addr) {
    if (addr == NULL) { return NULL; }
    for (size_t i = 0; i < s_user_count; i++) {
        if (s_users[i].addr == addr) { return &s_users[i]; }
    }
    return NULL;
}

void qa_user_fail_next(unsigned count) { s_user_fail_left = count; }

void *qa_user_malloc(size_t size, const char *tag, uint64_t checksum) {
    if (s_user_fail_left > 0) {
        s_user_fail_left--;
        return NULL;
    }
    void *p = malloc(size);
    if (p == NULL) { return NULL; }
    if (s_user_count >= QA_MAX_USER_OBJECTS) {
        s_budget_exceeded = true;
        free(p);
        return NULL;
    }
    uint32_t uid = (uint32_t)s_user_count + 1;
    s_users[s_user_count++] = (struct qa_user_obj) {
        .id = uid,
        .addr = p,
        .size = size,
        .tag = tag,
        .live = true,
        .alloc_seq = s_user_next_seq++,
        .checksum = checksum,
    };
    s_user_allocs++;
    return p;
}

void *qa_user_calloc(size_t nmemb, size_t size, const char *tag) {
    if (s_user_fail_left > 0) {
        s_user_fail_left--;
        return NULL;
    }
    void *p = calloc(nmemb, size);
    if (p == NULL) { return NULL; }
    if (s_user_count >= QA_MAX_USER_OBJECTS) {
        s_budget_exceeded = true;
        free(p);
        return NULL;
    }
    uint32_t uid2 = (uint32_t)s_user_count + 1;
    s_users[s_user_count++] = (struct qa_user_obj) {
        .id = uid2,
        .addr = p,
        .size = nmemb * size,
        .tag = tag,
        .live = true,
        .alloc_seq = s_user_next_seq++,
        .checksum = 0,
    };
    s_user_allocs++;
    return p;
}

void qa_user_free(void *ptr, const char *tag) {
    if (ptr == NULL) { return; }
    struct qa_user_obj *u = find_user(ptr);
    if (u == NULL) {
        struct qa_user_obj *any = find_user_any(ptr);
        record_violation(s_next_seq++, QA_OP_FREE, ptr,
            any ? "double free of user object"
                : "free of unknown user object");
        return;
    }
    (void)tag;
    free(ptr);
    u->live = false;
    u->free_seq = s_user_next_seq++;
    s_user_frees++;
}

/* ---- Introspection ----------------------------------------------- */

size_t qa_event_count(void) { return s_event_count; }
const struct qa_event *qa_event_at(size_t i) {
    return i < s_event_count ? &s_events[i] : NULL;
}

size_t qa_live_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < s_record_count; i++) {
        if (s_records[i].live) { n++; }
    }
    return n;
}

size_t qa_violation_count(void) { return s_violation_count; }
const struct qa_violation *qa_violation_at(size_t i) {
    return i < s_violation_count ? &s_violations[i] : NULL;
}

bool qa_budget_exceeded(void) { return s_budget_exceeded; }

size_t qa_user_live_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < s_user_count; i++) {
        if (s_users[i].live) { n++; }
    }
    return n;
}

const struct qa_user_obj *qa_user_live_at(size_t i) {
    size_t seen = 0;
    for (size_t j = 0; j < s_user_count; j++) {
        if (!s_users[j].live) { continue; }
        if (seen == i) { return &s_users[j]; }
        seen++;
    }
    return NULL;
}

size_t qa_user_total_allocs(void) { return s_user_allocs; }
size_t qa_user_total_frees(void) { return s_user_frees; }

const struct qa_user_obj *qa_user_find(void *addr) {
    struct qa_user_obj *u = find_user(addr);
    return u;
}

/* ---- Replayable plan output -------------------------------------- */

void qa_plan_write(FILE *f, const struct qa_plan *plan,
        const struct qa_outcome *outcome) {
    fprintf(f, "PLAN config=%s seed=0x%016llx faults=",
        plan->config, (unsigned long long)plan->seed);
    for (size_t i = 0; i < plan->fault_count; i++) {
        fprintf(f, "%s%u", i == 0 ? "" : ",", plan->fault_seq[i]);
    }
    fprintf(f, "\n");
    fprintf(f,
        "RESULT run_res=%d fault_hit=%s hits=%u live_blocks=%zu "
        "live_user=%zu violations=%zu incomplete=%s last_stage=%s\n",
        outcome->run_result,
        outcome->fault_hit ? "true" : "false",
        outcome->fault_hits,
        outcome->live_blocks,
        outcome->live_user_objects,
        outcome->violations,
        outcome->incomplete ? "true" : "false",
        outcome->last_stage ? outcome->last_stage : "(none)");
    for (size_t i = 0; i < s_event_count; i++) {
        const struct qa_event *e = &s_events[i];
        fprintf(f,
            "EVENT seq=%u attempt=%u op=%s size=%zu ok=%s id=%u new_id=%u "
            "addr=%p stage=%s\n",
            e->seq, e->attempt, qa_op_str(e->op), e->size,
            e->ok ? "true" : "false", e->id, e->new_id,
            e->addr, e->stage ? e->stage : "(none)");
    }
    for (size_t i = 0; i < s_record_count; i++) {
        if (!s_records[i].live) { continue; }
        fprintf(f, "LEAK id=%u op=%s size=%zu addr=%p alloc_seq=%u\n",
            s_records[i].id, qa_op_str(s_records[i].alloc_op),
            s_records[i].size, s_records[i].addr,
            s_records[i].alloc_seq);
    }
    for (size_t i = 0; i < s_violation_count; i++) {
        fprintf(f, "VIOLATION seq=%u op=%s addr=%p kind=%s\n",
            s_violations[i].seq, qa_op_str(s_violations[i].op),
            s_violations[i].addr, s_violations[i].kind);
    }
    fputs("ENDPLAN\n", f);
}

size_t qa_report(FILE *f) {
    size_t live = qa_live_count();
    for (size_t i = 0; i < s_record_count; i++) {
        if (s_records[i].live) {
            fprintf(f,
                "LEAK id=%u op=%s size=%zu addr=%p alloc_seq=%u\n",
                s_records[i].id, qa_op_str(s_records[i].alloc_op),
                s_records[i].size, s_records[i].addr,
                s_records[i].alloc_seq);
        }
    }
    for (size_t i = 0; i < s_violation_count; i++) {
        fprintf(f, "VIOLATION seq=%u op=%s addr=%p kind=%s\n",
            s_violations[i].seq, qa_op_str(s_violations[i].op),
            s_violations[i].addr, s_violations[i].kind);
    }
    for (size_t i = 0; i < s_user_count; i++) {
        if (s_users[i].live) {
            fprintf(f, "USER_LEAK id=%u tag=%s addr=%p size=%zu\n",
                s_users[i].id,
                s_users[i].tag ? s_users[i].tag : "(null)",
                s_users[i].addr, s_users[i].size);
        }
    }
    return live + s_violation_count + qa_user_live_count();
}
