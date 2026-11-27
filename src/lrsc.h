/*
 * Static LR/SC retry-region analysis (read-only, never executes input).
 *
 * The analyzer owns a private copy of every byte it inspects. Results are
 * plain value structures indexed by PC, so callers can keep a report alive
 * independently of the original buffer and get byte-identical output for
 * identical input. Nothing here judges what real hardware would do at run
 * time; proven only means the static structural conditions below hold.
 */
#ifndef LRSC_ANALYSIS_H
#define LRSC_ANALYSIS_H

#include "riscv-disas.h"

/* Verdict for one LR -> SC candidate. */
typedef enum {
    LRSC_PROVEN = 0,       /* every required static condition is certified */
    LRSC_UNCONSTRAINED,    /* a reachable instruction or edge violates a rule */
    LRSC_UNKNOWN           /* association of address or status cannot be proved */
} lrsc_verdict;

/* Why a candidate is not proven. LRSC_REASON_NONE accompanies proven. */
typedef enum {
    LRSC_REASON_NONE = 0,
    LRSC_REASON_ILLEGAL_INST,     /* reachable illegal instruction */
    LRSC_REASON_MIXED_WIDTH,      /* LR and SC do not have equal width */
    LRSC_REASON_INTERVENING_LR,   /* another LR sits inside the region */
    LRSC_REASON_FOREIGN_SC,       /* another SC sits inside the region */
    LRSC_REASON_FORBIDDEN_OP,     /* op outside arithmetic/logic/forward control */
    LRSC_REASON_MEMORY_OP,        /* load/store/AMO other than the LR/SC pair */
    LRSC_REASON_FENCE,            /* FENCE or FENCE.I inside the region */
    LRSC_REASON_SYSTEM,           /* SYSTEM instruction inside the region */
    LRSC_REASON_MULDIV,           /* M-extension multiply/divide inside region */
    LRSC_REASON_BACKWARD_EDGE,    /* backward edge other than the retry tail */
    LRSC_REASON_LOOP,             /* cycle reachable inside the region */
    LRSC_REASON_REGION_TOO_LONG,  /* more than 16 instructions in the region */
    LRSC_REASON_EXTERNAL_ENTRY,   /* region node has an incoming edge outside */
    LRSC_REASON_EDGE_OUTSIDE,     /* direct edge leaves the code image */
    LRSC_REASON_ADDR_OVERWRITTEN, /* address register redefined before the SC */
    LRSC_REASON_ADDR_MERGE,       /* merged paths disagree on the address */
    LRSC_REASON_ADDR_UNKNOWN,     /* address provenance cannot be expressed */
    LRSC_REASON_STATUS_LOST,      /* SC result is overwritten before the branch */
    LRSC_REASON_STATUS_FOREIGN,   /* branch tests a status this SC does not own */
    LRSC_REASON_STATUS_MERGE,     /* merged paths disagree on the status token */
    LRSC_REASON_NO_STATUS_BRANCH, /* no zero-compare branch on the SC result */
    LRSC_REASON_FAIL_EDGE,        /* failure edge does not return to this LR */
    LRSC_REASON_SUCCESS_EDGE,     /* success edge does not leave the region */
    LRSC_REASON_COUNT
} lrsc_reason;

typedef enum {
    LRSC_EDGE_FALLTHROUGH = 0,
    LRSC_EDGE_BRANCH,
    LRSC_EDGE_JUMP
} lrsc_edge_kind;

typedef struct {
    uint64_t pc;
    lrsc_edge_kind kind;
} lrsc_edge;

/* One decoded instruction plus its recovered direct successors. */
typedef struct {
    rv_decode dec;        /* canonical decode, before pseudo lifting */
    uint16_t len;         /* raw instruction length: 2 or 4 bytes */
    uint16_t nsucc;
    uint32_t flags;
    lrsc_edge succ[2];
} lrsc_node;

#define LRSC_NODE_ENTRY      0x1u
#define LRSC_NODE_REACHABLE  0x2u
#define LRSC_NODE_EDGE_OUT   0x4u

/* A certified retry region holds at most 16 instructions. */
#define LRSC_CERT_REGION_MAX 16

/* One candidate retry region, anchored at one reachable LR. */
typedef struct {
    lrsc_verdict verdict;
    lrsc_reason reason;
    uint64_t lr_pc;
    uint64_t sc_pc;
    uint64_t reason_pc;       /* decisive instruction/edge PC, 0 when none */
    uint64_t branch_pc;       /* status branch PC once proved, else 0 */
    uint64_t fail_target;     /* proved failure edge target (the LR), else 0 */
    uint64_t success_target;  /* proved success exit target, else 0 */
    uint32_t region_count;    /* instructions covered by the certificate */
    uint64_t region_pc[LRSC_CERT_REGION_MAX]; /* PCs in linear order */
} lrsc_candidate;

typedef struct lrsc_report lrsc_report;

/* Read-only accessors; out-pointers may be NULL. */
const lrsc_node *lrsc_report_nodes(const lrsc_report *r, size_t *count);
const lrsc_candidate *lrsc_report_candidates(const lrsc_report *r, size_t *count);
const uint8_t *lrsc_report_code(const lrsc_report *r, size_t *len);
uint64_t lrsc_report_base_pc(const lrsc_report *r);
size_t lrsc_report_reachable_count(const lrsc_report *r);
void lrsc_report_free(lrsc_report *r);

const char *lrsc_verdict_name(lrsc_verdict v);
const char *lrsc_reason_name(lrsc_reason reason);

/*
 * Decode and analyze one contiguous, little-endian, read-only code image.
 * Every entry PC must be a valid instruction boundary inside code. The
 * routine copies code/entries and never writes through the supplied pointers.
 * Returns NULL only when allocation fails.
 */
lrsc_report *lrsc_analyze(rv_isa isa, uint64_t base_pc,
                          const uint8_t *code, size_t len,
                          const uint64_t *entries, size_t nentries);

#endif
