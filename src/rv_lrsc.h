/*
 * Read-only static LR/SC retry-region analysis for RV64 I/A/C.
 *
 * The analyzer never executes input. It recovers a control-flow graph from
 * direct jumps, conditional branches and fall-through edges, tracks
 * entry-register-relative addresses and SC success/failure status through
 * lossless register copies, and reports, for every reachable LR, the same-
 * width SC candidates with a structural verdict and the decisive evidence.
 *
 * Results own all data they expose (records are copied out of the code
 * block), so the source block and buffer may be released afterwards.
 */

#ifndef RV_LRSC_H
#define RV_LRSC_H

#include "rv_struct_decode.h"

typedef enum {
    RV_LRSC_VERDICT_UNKNOWN = 0,       /* proof gap; runtime may still work */
    RV_LRSC_VERDICT_PROVEN = 1,        /* static structural conditions hold */
    RV_LRSC_VERDICT_UNCONSTRAINED = 2  /* a reachable instruction/edge violates
                                           the stated region constraints */
} rv_lrsc_verdict;

typedef enum {
    RV_LRSC_REASON_NONE = 0,
    /* hard constraint violations -> unconstrained */
    RV_LRSC_REASON_INTERVENING_LR,     /* another LR between LR and SC */
    RV_LRSC_REASON_WIDTH_MISMATCH,     /* LR and SC widths (.w/.d) differ */
    RV_LRSC_REASON_FORBIDDEN_INSN,     /* mem/amo/fence/system/muldiv/jalr */
    RV_LRSC_REASON_BACKWARD_EDGE,      /* backward edge or loop inside region */
    RV_LRSC_REASON_REGION_TOO_LONG,    /* region exceeds 16 instructions */
    RV_LRSC_REASON_EXTERNAL_ENTRY,     /* region body entered bypassing LR */
    RV_LRSC_REASON_UNREACHABLE_NODE,   /* region node not reachable from LR */
    /* proof gaps -> unknown */
    RV_LRSC_REASON_NO_SC,              /* no SC found before the window ends */
    RV_LRSC_REASON_ADDR_MISMATCH,      /* SC address differs across paths */
    RV_LRSC_REASON_ADDR_UNKNOWN,       /* SC/LR address lineage untrackable */
    RV_LRSC_REASON_STATUS_CLOBBERED,   /* SC result overwritten before test */
    RV_LRSC_REASON_STATUS_FOREIGN,     /* branch tests another SC's status */
    RV_LRSC_REASON_NO_STATUS_BRANCH,   /* no branch on the SC status found */
    RV_LRSC_REASON_EDGE_UNPROVEN       /* retry/success edges not both proven */
} rv_lrsc_reason_kind;

typedef struct {
    rv_lrsc_reason_kind kind;
    uint64_t pc;         /* decisive instruction PC (0 if not applicable) */
    uint64_t edge_from;  /* branch PC for edge reasons, else 0 */
    uint64_t edge_to;    /* edge target for edge reasons, else 0 */
} rv_lrsc_reason;

typedef struct {
    rv_lrsc_verdict verdict;
    uint8_t  width;             /* 4 for .w, 8 for .d */
    rv_code_rec lr_rec;         /* full owned copies of key instructions */
    rv_code_rec sc_rec;
    rv_code_rec branch_rec;     /* status-testing branch; pc 0 if none */

    uint64_t region_start_pc;
    uint64_t region_end_pc;
    uint64_t region_pcs[16];    /* contiguous region, region_count entries */
    uint8_t  region_count;

    /* proven certificate edges; 0 when not proven */
    uint64_t fail_edge_from;    /* branch taking the non-zero (retry) edge */
    uint64_t fail_edge_to;      /* must equal lr_rec.dec.pc */
    uint64_t success_edge_from; /* branch taking the zero (leave) edge */
    uint64_t success_edge_to;   /* first PC outside the region */

    rv_lrsc_reason *reasons;
    size_t   reason_count;
} rv_lrsc_candidate;

typedef struct {
    uint64_t *entries;          /* owned copy */
    size_t    entry_count;
    size_t    reached_count;    /* instructions reachable from any entry */

    rv_lrsc_candidate *candidates;
    size_t    candidate_count;

    rv_lrsc_reason *violations; /* block-level unconstrained evidence */
    size_t    violation_count;
} rv_lrsc_result;

/*
 * Analyze blk from the given entry PCs (every entry must decode inside blk).
 * Returns NULL only on allocation error or invalid arguments.
 */
rv_lrsc_result *rv_lrsc_analyze(const rv_code_block *blk,
    const uint64_t *entries, size_t entry_count);

void rv_lrsc_result_free(rv_lrsc_result *res);

const char *rv_lrsc_verdict_name(rv_lrsc_verdict v);
const char *rv_lrsc_reason_name(rv_lrsc_reason_kind k);

#endif
