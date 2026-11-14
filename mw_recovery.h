/*
** MW_RECOVERY.H
** Trusted-ledger driven metadata rebuild engine.
**
** This module is independent of memwatch.c: it never dereferences a
** pointer on its own.  All access to observed header fields goes through
** an adapter supplied by the caller.  The same engine, the same plan
** generation, the same second-stage verification and the same write-set
** application code serve both the live MEMWATCH heap and the plain
** integer-token array mirror used by the contract tests.
**
** Trust model:
**  - The fact array (mwFact) is the canonical truth.  It is owned by the
**    caller and copied into the plan at prepare time; later changes to
**    the caller's arrays cannot mutate a published plan.
**  - The observed header array is untrusted input.  It is read through
**    the adapter, snapshotted, diffed against the facts and never used
**    to derive canonical identities, sizes or origins.
**  - Tokens are integers.  The engine never converts a token to a
**    pointer; only the adapter decides what a token means.
*/
#ifndef MW_RECOVERY_H
#define MW_RECOVERY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t mwToken;

#define MW_TOKEN_NULL ((mwToken)0)

/* Engine status codes. */
#define MW_RC_OK        0   /* plan built / verified / applied */
#define MW_RC_STALE     1   /* observations or ledger moved since prepare */
#define MW_RC_INVALID   2   /* facts or observed fields are not well formed */
#define MW_RC_NOMEM     3   /* internal storage could not be obtained */
#define MW_RC_NOTREADY  4   /* engine has no usable ledger to plan against */

/* Entry states remembered by the trusted fact array. */
#define MW_ENTRY_LIVE   1
#define MW_ENTRY_NML    2

/* Per-field discrepancy bits reported by a prepared plan. */
#define MW_DIFF_PREV    0x0001u
#define MW_DIFF_NEXT    0x0002u
#define MW_DIFF_SOURCE  0x0004u
#define MW_DIFF_COUNT   0x0008u
#define MW_DIFF_CHECK   0x0010u
#define MW_DIFF_SIZE    0x0020u
#define MW_DIFF_LINE    0x0040u
#define MW_DIFF_FLAG    0x0080u
#define MW_DIFF_MISSING 0x0100u /* observed header could not be read */

/* Chain-level anomaly bits, derived without trusting the links. */
#define MW_CHAIN_BAD_HEAD       0x0001u
#define MW_CHAIN_BAD_TAIL       0x0002u
#define MW_CHAIN_DUPLICATE_LINK 0x0004u
#define MW_CHAIN_CYCLE          0x0008u
#define MW_CHAIN_UNREACHABLE    0x0010u
#define MW_CHAIN_FOREIGN_LINK   0x0020u /* link points outside the fact set */

/*
** One observed management header, as copied through the adapter.
** Every value is diagnostic: none of it is trusted.
*/
typedef struct {
    mwToken identity;
    mwToken prev;
    mwToken next;
    long    count;
    long    line;
    unsigned flag;
    size_t  size;
    long    check;
    mwToken source_token; /* controlled source id, or a raw pointer token;
                             never dereferenced by the engine */
} mwObsHead;

typedef struct {
    mwToken head;
    mwToken tail;
} mwObsEnds;

/*
** One canonical fact.  Describes a single system allocation that is
** still owned by the heap: the raw system extent, the user extent,
** the allocation action count, the controlled source number/line and
** the LIVE/NML state.
*/
typedef struct {
    mwToken system_base;
    size_t  system_len;
    mwToken user_base;
    size_t  user_len;
    long    count;
    mwToken source_token; /* controlled source number (mirror) or a
                             pointer token (real heap); never followed */
    int     line;
    int     state;      /* MW_ENTRY_LIVE or MW_ENTRY_NML */
    mwToken identity;   /* token of the management header for this fact */
} mwFact;

/*
** Adapter between the generic engine and a concrete heap layout.
** The real heap adapter maps tokens to mwData pointers; the mirror
** adapter maps tokens to array indices.  The engine calls these; it
** never touches storage directly.
*/
typedef struct {
    /* Copy the observed header named by id.  Return 0 on failure. */
    int  (*read_fields)(void *ctx, mwToken id, mwObsHead *out);
    /* Copy the observed head/tail tokens. */
    int  (*read_ends)(void *ctx, mwObsEnds *out);
    /* Canonical check value for a fact and its desired header. */
    long (*canon_check)(void *ctx, const mwFact *fact,
                        const mwObsHead *wanted);
    /* Validate that a token names a readable header slot. */
    int  (*validate_token)(void *ctx, mwToken token);
    void *ctx;
} mwPlanAdapter;

/* Opaque plan. */
typedef struct mwPlan mwPlan;

/* One preview record: canonical fact versus the prepared snapshot. */
typedef struct {
    mwToken  identity;
    long     count;
    size_t   size;
    mwToken  source_token;
    int      line;
    int      state;
    unsigned diffmask;
    long     observed_check;
    long     expected_check;
} mwPlanItem;

/*
** Build a plan from canonical facts.  Never modifies observed storage.
** Facts are validated (duplicate identities, overlapping extents,
** size wraparound, bad states/tokens) before anything is published.
** On any failure *out_plan is set to NULL and no partial plan escapes.
*/
int  mwPlanBuild(const mwFact *facts, size_t fact_count,
                 unsigned ledger_version, unsigned generation,
                 const mwPlanAdapter *adapter,
                 mwPlan **out_plan);

/*
** Re-validate a prepared plan against live observations:
** generation and ledger version must match, every observed field and
** both chain ends must still equal the prepared snapshot.  Performs no
** writes and no allocation.
*/
int  mwPlanVerify(const mwPlan *plan, const mwPlanAdapter *adapter,
                  unsigned generation, unsigned ledger_version);

/*
** Verify first, then hand the complete write set to apply_cb once.
** Allocation is forbidden during commit; the callback only stores the
** already validated values.  Returns MW_RC_STALE/MW_RC_INVALID without
** invoking the callback if the second stage fails.
*/
int  mwPlanCommit(mwPlan *plan, const mwPlanAdapter *adapter,
                  unsigned generation, unsigned ledger_version,
                  int (*apply_cb)(void *ctx,
                                  const mwObsHead *writes,
                                  size_t write_count,
                                  const mwObsEnds *ends));

/* Free a plan (or accept NULL).  Never dereferences stored tokens. */
void mwPlanRelease(mwPlan *plan);

/* Read-only preview accessors. */
size_t         mwPlanItemCount(const mwPlan *plan);
const mwPlanItem *mwPlanItemAt(const mwPlan *plan, size_t index);
unsigned       mwPlanGeneration(const mwPlan *plan);
unsigned       mwPlanLedgerVersion(const mwPlan *plan);
unsigned       mwPlanChainFlags(const mwPlan *plan);
size_t         mwPlanDiscrepancyCount(const mwPlan *plan);
const mwObsEnds *mwPlanObservedEnds(const mwPlan *plan);
const mwObsEnds *mwPlanWantedEnds(const mwPlan *plan);

/*
** Plain-array mirror.
**
** The descriptor array supplies canonical facts; the header array
** supplies the observed fields under analysis.  All addresses are
** integer tokens; source numbers are controlled integers and are never
** dereferenced.  The mirror runs the exact same engine as the real heap.
*/
typedef struct {
    mwFact    *facts;      /* trusted descriptors */
    size_t     fact_count;
    mwObsHead *heads;      /* observed headers under analysis */
    size_t     head_count;
    mwObsEnds  ends;
    mwToken    blocked;    /* identity whose header is unreadable, or 0 */
    unsigned   ledger_version;
    unsigned   generation;
} mwMirror;

/* Validate descriptor legality: duplicates, overlaps, wraparound. */
int  mwMirrorValidate(const mwMirror *mirror);

/* Prepare against a mirror; mirrors use identity == array index + 1. */
int  mwMirrorPrepare(mwMirror *mirror, mwPlan **out_plan);

/* Verify and atomatically apply the plan to the mirror arrays. */
int  mwMirrorCommit(mwMirror *mirror, mwPlan *plan);

/* Simulate a structural heap change between prepare and commit. */
void mwMirrorTouch(mwMirror *mirror);

#ifdef __cplusplus
}
#endif

#endif /* MW_RECOVERY_H */
