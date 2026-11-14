/*
** MW_RECOVERY.C
** Generic trusted-ledger metadata rebuild engine.
** See mw_recovery.h for the trust model.  This file contains no
** memwatch-specific knowledge and never converts a token to a pointer.
*/
#include <stdlib.h>
#include <string.h>
#include "mw_recovery.h"

typedef struct {
    mwFact     fact;
    mwObsHead  observed;   /* snapshot taken at prepare time */
    mwObsHead  wanted;     /* canonical header to publish on commit */
    unsigned   diffmask;
    int        readable;   /* zero if the observed header was missing */
} mwPlanEntry;

struct mwPlan {
    unsigned      generation;
    unsigned      ledger_version;
    unsigned      chain_flags;
    size_t        count;
    size_t        discrepancies;
    mwObsEnds     observed_ends;
    mwObsEnds     wanted_ends;
    mwPlanEntry  *entries;
    mwObsHead    *writes;   /* contiguous write set, indexed like entries */
};

/* ---- fact validation -------------------------------------------------- */

static int extent_wraps(mwToken base, size_t len)
{
    if (len == 0) return 1;
    return (mwToken)(base + (mwToken)len) < base;
}

static int fact_well_formed(const mwFact *f)
{
    if (f->identity == MW_TOKEN_NULL) return 0;
    if (f->state != MW_ENTRY_LIVE && f->state != MW_ENTRY_NML) return 0;
    if (extent_wraps(f->system_base, f->system_len)) return 0;
    /* user extent must lie inside the system extent, without wrapping */
    if (f->user_base < f->system_base) return 0;
    if (extent_wraps(f->user_base, f->user_len)) return 0;
    if (f->user_base + (mwToken)f->user_len >
        f->system_base + (mwToken)f->system_len) return 0;
    return 1;
}

static int extents_overlap(const mwFact *a, const mwFact *b)
{
    mwToken a_end = a->system_base + (mwToken)a->system_len;
    mwToken b_end = b->system_base + (mwToken)b->system_len;
    if (a->system_base == b->system_base) return 1;
    if (a->system_base < b->system_base) return b->system_base < a_end;
    return a->system_base < b_end;
}

int mwMirrorValidate(const mwMirror *mirror)
{
    size_t i, j;
    if (mirror == NULL) return MW_RC_INVALID;
    if (mirror->fact_count == 0) return MW_RC_OK;
    if (mirror->facts == NULL) return MW_RC_INVALID;
    for (i = 0; i < mirror->fact_count; ++i) {
        if (!fact_well_formed(&mirror->facts[i])) return MW_RC_INVALID;
        for (j = 0; j < i; ++j) {
            if (mirror->facts[i].identity == mirror->facts[j].identity)
                return MW_RC_INVALID; /* duplicate descriptor */
            if (extents_overlap(&mirror->facts[i], &mirror->facts[j]))
                return MW_RC_INVALID; /* overlapping system extents */
        }
    }
    return MW_RC_OK;
}

/* ---- plan construction ------------------------------------------------- */

static int find_entry(const mwPlan *plan, mwToken id)
{
    size_t i;
    for (i = 0; i < plan->count; ++i)
        if (plan->entries[i].fact.identity == id) return (int)i;
    return -1;
}

static unsigned field_diffs(const mwObsHead *obs, const mwObsHead *want)
{
    unsigned mask = 0;
    if (obs->prev != want->prev) mask |= MW_DIFF_PREV;
    if (obs->next != want->next) mask |= MW_DIFF_NEXT;
    if (obs->source_token != want->source_token)
        mask |= MW_DIFF_SOURCE;
    if (obs->count != want->count) mask |= MW_DIFF_COUNT;
    if (obs->check != want->check) mask |= MW_DIFF_CHECK;
    if (obs->size != want->size) mask |= MW_DIFF_SIZE;
    if (obs->line != want->line) mask |= MW_DIFF_LINE;
    if (obs->flag != want->flag) mask |= MW_DIFF_FLAG;
    return mask;
}

/*
** Chain analysis driven entirely by the observed links.  The canonical
** fact set is used only to decide whether an observed token names a
** member; the links themselves are never trusted or followed to build
** the canonical chain.
*/
static void analyze_chain(mwPlan *plan)
{
    size_t i, steps;
    mwToken cur;
    char *seen;
    unsigned flags = 0;

    if (plan->count == 0) {
        if (plan->observed_ends.head != MW_TOKEN_NULL ||
            plan->observed_ends.tail != MW_TOKEN_NULL)
            flags |= MW_CHAIN_BAD_HEAD | MW_CHAIN_BAD_TAIL;
        plan->chain_flags = flags;
        return;
    }

    if (plan->observed_ends.head != plan->entries[0].fact.identity)
        flags |= MW_CHAIN_BAD_HEAD;
    if (plan->observed_ends.tail !=
        plan->entries[plan->count - 1].fact.identity)
        flags |= MW_CHAIN_BAD_TAIL;

    seen = (char *)calloc(plan->count, 1);
    if (seen == NULL) { /* analysis degrades, never blocks the plan */
        plan->chain_flags = flags;
        return;
    }

    cur = plan->observed_ends.head;
    for (steps = 0; steps <= plan->count; ++steps) {
        int idx;
        if (cur == MW_TOKEN_NULL) break;
        idx = find_entry(plan, cur);
        if (idx < 0) {
            flags |= MW_CHAIN_FOREIGN_LINK;
            break;
        }
        if (seen[idx]) {
            flags |= MW_CHAIN_CYCLE;
            break;
        }
        seen[idx] = 1;
        cur = plan->entries[idx].observed.next;
    }
    if (steps > plan->count) flags |= MW_CHAIN_CYCLE;

    for (i = 0; i < plan->count; ++i) {
        mwToken t = plan->entries[i].observed.next;
        if (t != MW_TOKEN_NULL && find_entry(plan, t) < 0)
            flags |= MW_CHAIN_FOREIGN_LINK;
        t = plan->entries[i].observed.prev;
        if (t != MW_TOKEN_NULL && find_entry(plan, t) < 0)
            flags |= MW_CHAIN_FOREIGN_LINK;
        if (!seen[i]) flags |= MW_CHAIN_UNREACHABLE;
    }

    /* duplicate linkage: two members claim the same successor */
    for (i = 0; i < plan->count; ++i) {
        size_t j;
        mwToken t = plan->entries[i].observed.next;
        if (t == MW_TOKEN_NULL) continue;
        for (j = i + 1; j < plan->count; ++j)
            if (plan->entries[j].observed.next == t) {
                flags |= MW_CHAIN_DUPLICATE_LINK;
                break;
            }
    }

    free(seen);
    plan->chain_flags = flags;
}

int mwPlanBuild(const mwFact *facts, size_t fact_count,
                unsigned ledger_version, unsigned generation,
                const mwPlanAdapter *adapter, mwPlan **out_plan)
{
    mwPlan *plan;
    size_t i;

    if (out_plan == NULL) return MW_RC_INVALID;
    *out_plan = NULL;
    if (adapter == NULL || adapter->read_fields == NULL ||
        adapter->read_ends == NULL || adapter->canon_check == NULL)
        return MW_RC_INVALID;

    plan = (mwPlan *)calloc(1, sizeof(*plan));
    if (plan == NULL) return MW_RC_NOMEM;
    plan->generation = generation;
    plan->ledger_version = ledger_version;

    if (fact_count) {
        plan->entries = (mwPlanEntry *)calloc(fact_count,
                                              sizeof(mwPlanEntry));
        if (plan->entries == NULL) {
            free(plan);
            return MW_RC_NOMEM;
        }
        plan->writes = (mwObsHead *)calloc(fact_count,
                                           sizeof(mwObsHead));
        if (plan->writes == NULL) {
            free(plan->entries);
            free(plan);
            return MW_RC_NOMEM;
        }
    }
    plan->count = fact_count;

    /* Stage 1: validate every canonical fact before publishing. */
    for (i = 0; i < fact_count; ++i) {
        size_t j;
        if (!fact_well_formed(&facts[i])) goto invalid;
        if (adapter->validate_token &&
            !adapter->validate_token(adapter->ctx, facts[i].identity))
            goto invalid;
        for (j = 0; j < i; ++j) {
            if (facts[i].identity == facts[j].identity) goto invalid;
            if (extents_overlap(&facts[i], &facts[j])) goto invalid;
        }
    }

    /* Stage 2: snapshot observations.  Failures are recorded, fatal. */
    if (!adapter->read_ends(adapter->ctx, &plan->observed_ends))
        goto invalid;
    for (i = 0; i < fact_count; ++i) {
        mwPlanEntry *e = &plan->entries[i];
        e->fact = facts[i];
        e->readable = adapter->read_fields(adapter->ctx,
                                           facts[i].identity,
                                           &e->observed);
        if (!e->readable) {
            memset(&e->observed, 0, sizeof(e->observed));
            e->observed.identity = facts[i].identity;
        }
    }

    /* Stage 3: canonical headers.  Order is the fact-array order,
    ** which the producer defines by descending allocation count. */
    for (i = 0; i < fact_count; ++i) {
        mwPlanEntry *e = &plan->entries[i];
        const mwFact *f = &e->fact;
        memset(&e->wanted, 0, sizeof(e->wanted));
        e->wanted.identity = f->identity;
        e->wanted.prev = i ? plan->entries[i - 1].fact.identity
                           : MW_TOKEN_NULL;
        e->wanted.next = i + 1 < fact_count
                       ? plan->entries[i + 1].fact.identity
                       : MW_TOKEN_NULL;
        e->wanted.count = f->count;
        e->wanted.size = f->user_len;
        e->wanted.line = f->line;
        e->wanted.source_token = f->source_token;
        e->wanted.flag = f->state == MW_ENTRY_NML ? 1u : 0u;
        e->wanted.check = adapter->canon_check(adapter->ctx, f,
                                               &e->wanted);
    }

    /* Stage 4: itemized discrepancies. */
    plan->discrepancies = 0;
    for (i = 0; i < fact_count; ++i) {
        mwPlanEntry *e = &plan->entries[i];
        if (!e->readable)
            e->diffmask = MW_DIFF_MISSING;
        else
            e->diffmask = field_diffs(&e->observed, &e->wanted);
        if (e->diffmask) plan->discrepancies++;
    }

    if (fact_count) {
        plan->wanted_ends.head = plan->entries[0].fact.identity;
        plan->wanted_ends.tail = plan->entries[fact_count - 1]
                                     .fact.identity;
    }

    /* materialize the contiguous, already-validated write set */
    for (i = 0; i < fact_count; ++i)
        plan->writes[i] = plan->entries[i].wanted;

    analyze_chain(plan);

    *out_plan = plan;
    return MW_RC_OK;

invalid:
    free(plan->writes);
    free(plan->entries);
    free(plan);
    return MW_RC_INVALID;
}

/* ---- second-stage verification ---------------------------------------- */

int mwPlanVerify(const mwPlan *plan, const mwPlanAdapter *adapter,
                 unsigned generation, unsigned ledger_version)
{
    mwObsEnds ends;
    size_t i;

    if (plan == NULL || adapter == NULL) return MW_RC_INVALID;
    if (generation != plan->generation ||
        ledger_version != plan->ledger_version)
        return MW_RC_STALE;
    if (!adapter->read_ends(adapter->ctx, &ends)) return MW_RC_INVALID;
    if (ends.head != plan->observed_ends.head ||
        ends.tail != plan->observed_ends.tail)
        return MW_RC_STALE;

    for (i = 0; i < plan->count; ++i) {
        const mwPlanEntry *e = &plan->entries[i];
        mwObsHead now;
        if (!adapter->read_fields(adapter->ctx, e->fact.identity, &now))
            return MW_RC_STALE;
        /* field-by-field: struct padding is not part of the contract */
        if (now.identity != e->observed.identity ||
            now.prev != e->observed.prev ||
            now.next != e->observed.next ||
            now.count != e->observed.count ||
            now.line != e->observed.line ||
            now.flag != e->observed.flag ||
            now.size != e->observed.size ||
            now.check != e->observed.check ||
            now.source_token != e->observed.source_token)
            return MW_RC_STALE;
    }
    return MW_RC_OK;
}

int mwPlanCommit(mwPlan *plan, const mwPlanAdapter *adapter,
                 unsigned generation, unsigned ledger_version,
                 int (*apply_cb)(void *, const mwObsHead *, size_t,
                                 const mwObsEnds *))
{
    int rc;

    if (plan == NULL || adapter == NULL || apply_cb == NULL)
        return MW_RC_INVALID;

    /* Full re-validation first; only then is the prepared write set
    ** handed over.  This function allocates nothing. */
    rc = mwPlanVerify(plan, adapter, generation, ledger_version);
    if (rc != MW_RC_OK) return rc;

    rc = apply_cb(adapter->ctx, plan->writes, plan->count,
                  &plan->wanted_ends) ? MW_RC_OK : MW_RC_INVALID;
    return rc;
}

/* ---- preview accessors ------------------------------------------------- */

size_t mwPlanItemCount(const mwPlan *plan)
{
    return plan ? plan->count : 0;
}

const mwPlanItem *mwPlanItemAt(const mwPlan *plan, size_t index)
{
    static mwPlanItem view;
    const mwPlanEntry *e;
    if (plan == NULL || index >= plan->count) return NULL;
    e = &plan->entries[index];
    view.identity = e->fact.identity;
    view.count = e->fact.count;
    view.size = e->fact.user_len;
    view.source_token = e->fact.source_token;
    view.line = e->fact.line;
    view.state = e->fact.state;
    view.diffmask = e->diffmask;
    view.observed_check = e->observed.check;
    view.expected_check = e->wanted.check;
    return &view;
}

unsigned mwPlanGeneration(const mwPlan *plan)
{
    return plan ? plan->generation : 0;
}

unsigned mwPlanLedgerVersion(const mwPlan *plan)
{
    return plan ? plan->ledger_version : 0;
}

unsigned mwPlanChainFlags(const mwPlan *plan)
{
    return plan ? plan->chain_flags : 0;
}

size_t mwPlanDiscrepancyCount(const mwPlan *plan)
{
    return plan ? plan->discrepancies : 0;
}

const mwObsEnds *mwPlanObservedEnds(const mwPlan *plan)
{
    return plan ? &plan->observed_ends : NULL;
}

const mwObsEnds *mwPlanWantedEnds(const mwPlan *plan)
{
    return plan ? &plan->wanted_ends : NULL;
}

void mwPlanRelease(mwPlan *plan)
{
    if (plan == NULL) return;
    free(plan->writes);
    free(plan->entries);
    free(plan);
}

/* ---- plain-array mirror adapter --------------------------------------- */

static int mirror_read_fields(void *ctx, mwToken id, mwObsHead *out)
{
    const mwMirror *m = (const mwMirror *)ctx;
    size_t idx;
    if (id == MW_TOKEN_NULL || id > (mwToken)m->head_count) return 0;
    if (m->blocked == id) return 0; /* simulated unreadable header */
    idx = (size_t)(id - 1);
    *out = m->heads[idx];
    out->identity = id;
    return 1;
}

static int mirror_read_ends(void *ctx, mwObsEnds *out)
{
    *out = ((const mwMirror *)ctx)->ends;
    return 1;
}

static long mirror_canon_check(void *ctx, const mwFact *fact,
                               const mwObsHead *wanted)
{
    (void)ctx;
    (void)wanted;
    return (long)(0xFE0180UL ^ (unsigned long)fact->count
                  ^ (unsigned long)fact->user_len
                  ^ (unsigned long)fact->line);
}

static int mirror_validate_token(void *ctx, mwToken token)
{
    const mwMirror *m = (const mwMirror *)ctx;
    return token != MW_TOKEN_NULL && token <= (mwToken)m->head_count;
}

static const mwPlanAdapter mirror_adapter = {
    mirror_read_fields,
    mirror_read_ends,
    mirror_canon_check,
    mirror_validate_token,
    NULL
};

int mwMirrorPrepare(mwMirror *mirror, mwPlan **out_plan)
{
    mwPlanAdapter ad;
    int rc;
    if (mirror == NULL || out_plan == NULL) return MW_RC_INVALID;
    rc = mwMirrorValidate(mirror);
    if (rc != MW_RC_OK) return rc;
    ad = mirror_adapter;
    ad.ctx = mirror;
    return mwPlanBuild(mirror->facts, mirror->fact_count,
                       mirror->ledger_version, mirror->generation,
                       &ad, out_plan);
}

static int mirror_apply(void *ctx, const mwObsHead *writes,
                        size_t write_count, const mwObsEnds *ends)
{
    mwMirror *m = (mwMirror *)ctx;
    size_t i;
    for (i = 0; i < write_count; ++i) {
        size_t idx = (size_t)(writes[i].identity - 1);
        if (idx >= m->head_count) return 0;
        m->heads[idx] = writes[i];
    }
    m->ends = *ends;
    return 1;
}

int mwMirrorCommit(mwMirror *mirror, mwPlan *plan)
{
    mwPlanAdapter ad;
    int rc;
    if (mirror == NULL || plan == NULL) return MW_RC_INVALID;
    ad = mirror_adapter;
    ad.ctx = mirror;
    rc = mwPlanCommit(plan, &ad, mirror->generation,
                      mirror->ledger_version, mirror_apply);
    if (rc == MW_RC_OK) {
        /* a published rebuild is itself a new ledger generation */
        mirror->ledger_version++;
        mirror->generation++;
    }
    return rc;
}

void mwMirrorTouch(mwMirror *mirror)
{
    if (mirror == NULL) return;
    mirror->ledger_version++;
    mirror->generation++;
}

/* EOF MW_RECOVERY.C */
