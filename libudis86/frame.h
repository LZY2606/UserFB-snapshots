/*
 * frame.h - static x86-64 frame-state analysis over a caller CFG.
 *
 * The analyzer only decodes bytes and combines abstract values.  It never
 * executes the code, reads host registers or memory, or reconstructs control
 * flow from the instruction stream.
 */
#ifndef UD_FRAME_H
#define UD_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UD_FRAME_NO_BLOCK ((size_t)-1)
#define UD_FRAME_NO_OPERAND (~0u)

/*
 * Abstract value of RSP or RBP, or the abstract address of a stack slot.
 *
 * UD_FRAME_UNREACHED is the data-flow bottom state: no processed predecessor
 * has reached the point yet.  At a reported instruction the block is reachable,
 * so register values are normally either UNKNOWN or RELATIVE.
 */
enum ud_frame_value_kind {
    UD_FRAME_UNREACHED = 0,
    UD_FRAME_UNKNOWN   = 1,
    UD_FRAME_RELATIVE  = 2
};

typedef struct ud_frame_value {
    int kind;
    int64_t offset; /* valid when kind == UD_FRAME_RELATIVE; value is S+offset */
} ud_frame_value_t;

typedef struct ud_frame_state {
    ud_frame_value_t rsp;
    ud_frame_value_t rbp;
} ud_frame_state_t;

/* Half-open buffer range [begin, end) and caller-provided successor indices. */
typedef struct ud_frame_block {
    size_t begin;
    size_t end;
    const size_t *successors;
    size_t successor_count;
} ud_frame_block_t;

typedef struct ud_frame_graph {
    const ud_frame_block_t *blocks;
    size_t block_count;
    size_t entry;
} ud_frame_graph_t;

enum ud_frame_error_code {
    UD_FRAME_OK = 0,
    UD_FRAME_ERR_ARGUMENT,
    UD_FRAME_ERR_GRAPH,
    UD_FRAME_ERR_RANGE,
    UD_FRAME_ERR_EDGE,
    UD_FRAME_ERR_DECODE,
    UD_FRAME_ERR_UNSUPPORTED,
    UD_FRAME_ERR_NOMEM
};

typedef struct ud_frame_error {
    int code;
    size_t block;  /* UD_FRAME_NO_BLOCK when not associated with a block */
    size_t offset; /* byte offset in the input buffer, when applicable */
    char message[96];
} ud_frame_error_t;

const char *ud_frame_error_string(int code);

enum ud_frame_access_kind {
    UD_FRAME_ACCESS_EXPLICIT = 0, /* a MOV memory operand */
    UD_FRAME_ACCESS_PUSH,
    UD_FRAME_ACCESS_POP,
    UD_FRAME_ACCESS_CALL,
    UD_FRAME_ACCESS_RET,
    UD_FRAME_ACCESS_LEAVE,
    UD_FRAME_ACCESS_LEA /* address calculation only; no memory access */
};

/*
 * direction is a bit mask.  Read/write records have one bit set; LEA address
 * calculations use direction == 0.
 */
#define UD_FRAME_ACCESS_READ  1u
#define UD_FRAME_ACCESS_WRITE 2u

typedef struct ud_frame_access {
    size_t block_index;       /* original caller block number */
    size_t block_begin;       /* stable ordering key */
    size_t insn_offset;
    unsigned int length;
    unsigned int operand_index; /* 0..3 for explicit MOV, NO_OPERAND otherwise */
    int kind;
    unsigned int direction;
    unsigned int width;       /* bytes */
    unsigned int width_bits;
    int address_known;
    int64_t address;          /* S+address when address_known is true */
} ud_frame_access_t;

typedef struct ud_frame_insn {
    size_t block_index;
    size_t block_begin;
    size_t offset;
    unsigned int length;
    unsigned int mnemonic; /* enum ud_mnemonic_code from udis86 itab.h */
    ud_frame_state_t before;
    size_t access_start;
    size_t access_count;
    ud_frame_access_t *accesses; /* points into ud_frame_report.accesses */
} ud_frame_insn_t;

typedef struct ud_frame_block_info {
    size_t index;
    size_t begin;
    size_t end;
    int reachable;
    ud_frame_state_t entry_state;
    ud_frame_state_t exit_state;
} ud_frame_block_info_t;

typedef struct ud_frame_report {
    size_t insn_count;
    ud_frame_insn_t *insns;

    size_t access_count;
    ud_frame_access_t *accesses;

    size_t block_count;
    ud_frame_block_info_t *blocks; /* sorted by (begin, end, index) */
} ud_frame_report_t;

/*
 * Analyze code[0, code_size) using the caller's graph.  On success a freshly
 * allocated report is returned and must be released with
 * ud_frame_report_free().  On failure NULL is returned, no partial report is
 * published, and error (if non-NULL) is filled in.
 *
 * The code and graph pointers are not retained in the report.
 */
ud_frame_report_t *ud_frame_analyze(const uint8_t *code,
                                   size_t code_size,
                                   const ud_frame_graph_t *graph,
                                   ud_frame_error_t *error);

void ud_frame_report_free(ud_frame_report_t *report);

/* Short aliases for callers that prefer the object-free naming. */
void ud_frame_free(ud_frame_report_t *report);
void ud_frame_analyze_free(ud_frame_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* UD_FRAME_H */
