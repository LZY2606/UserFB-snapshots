/*
 * Structured, self-contained decode of a contiguous, little-endian,
 * read-only RV64 I/A/C instruction stream.
 *
 * Every emitted record is produced by the shared production decoder
 * (rv_decode_canonical); no bit fields are re-decoded here and nothing is
 * parsed back from disassembly text. The block copies every byte it needs,
 * so the input buffer may be released after rv_code_block_decode() returns.
 */

#ifndef RV_STRUCT_DECODE_H
#define RV_STRUCT_DECODE_H

#include "riscv-disas.h"

/* Per-instruction record. Index equals decode order in the block. */
typedef struct {
    rv_decode dec;       /* canonical decode, before pseudo lifting */
    uint8_t   len;       /* raw instruction length: 2 or 4 */
    uint8_t   compressed;
    uint32_t  offset;    /* byte offset from block base */
} rv_code_rec;

typedef struct {
    uint64_t  base_pc;
    const uint8_t *code; /* internal copy; freed by rv_code_block_free */
    size_t    code_size;
    rv_code_rec *recs;
    size_t    count;
} rv_code_block;

/*
 * Decode size bytes starting at base_pc. Only normal 16/32-bit RV64
 * instructions are accepted. Returns 0 on success; on failure *err_pc (when
 * non-NULL) receives the PC at which decoding failed and the block is left
 * uninitialized.
 */
int rv_code_block_decode(rv_code_block *blk, uint64_t base_pc,
    const uint8_t *data, size_t size, uint64_t *err_pc);

/* Offset -> record index, or -1 when no instruction starts there. */
long rv_code_block_find(const rv_code_block *blk, uint64_t pc);

/* Target PC of a direct control-flow record, when it lands inside the block. */
int rv_code_rec_branch_target(const rv_code_block *blk, size_t idx,
    uint64_t *target_pc);

void rv_code_block_free(rv_code_block *blk);

#endif
