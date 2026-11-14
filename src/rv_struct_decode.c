#include "rv_struct_decode.h"

#include <stdlib.h>
#include <string.h>

int rv_code_block_decode(rv_code_block *blk, uint64_t base_pc,
    const uint8_t *data, size_t size, uint64_t *err_pc)
{
    size_t cap = 0, count = 0, off = 0;
    rv_code_rec *recs = NULL;
    uint8_t *copy;

    memset(blk, 0, sizeof(*blk));
    if (size == 0 || (base_pc + size) < base_pc) {
        if (err_pc) *err_pc = base_pc;
        return -1;
    }
    copy = malloc(size);
    if (!copy) return -1;
    memcpy(copy, data, size);

    while (off < size) {
        rv_inst inst;
        size_t len;
        rv_code_rec rec;

        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            rv_code_rec *nrecs = realloc(recs, ncap * sizeof(*recs));
            if (!nrecs) {
                free(recs);
                free(copy);
                if (err_pc) *err_pc = base_pc + off;
                return -1;
            }
            recs = nrecs;
            cap = ncap;
        }

        inst_fetch(copy + off, &inst, &len);
        if (len != 2 && len != 4) {
            free(recs);
            free(copy);
            if (err_pc) *err_pc = base_pc + off;
            return -1;
        }
        if (off + len > size) {
            free(recs);
            free(copy);
            if (err_pc) *err_pc = base_pc + off;
            return -1;
        }

        memset(&rec, 0, sizeof(rec));
        rec.offset = (uint32_t)off;
        rec.len = (uint8_t)len;
        rec.compressed = (len == 2);
        rv_decode_canonical(&rec.dec, rv64, base_pc + off, inst);
        if (rec.dec.op == rv_op_illegal) {
            free(recs);
            free(copy);
            if (err_pc) *err_pc = base_pc + off;
            return -1;
        }
        recs[count++] = rec;
        off += len;
    }

    blk->base_pc = base_pc;
    blk->code = copy;
    blk->code_size = size;
    blk->recs = recs;
    blk->count = count;
    return 0;
}

long rv_code_block_find(const rv_code_block *blk, uint64_t pc)
{
    size_t i;
    if (pc < blk->base_pc) return -1;
    for (i = 0; i < blk->count; i++) {
        if (blk->recs[i].dec.pc == pc) return (long)i;
    }
    return -1;
}

int rv_code_rec_branch_target(const rv_code_block *blk, size_t idx,
    uint64_t *target_pc)
{
    const rv_code_rec *r = &blk->recs[idx];
    uint64_t t;
    switch (r->dec.op) {
    case rv_op_jal:
    case rv_op_beq: case rv_op_bne:
    case rv_op_blt: case rv_op_bge:
    case rv_op_bltu: case rv_op_bgeu:
        t = r->dec.pc + (int64_t)r->dec.imm;
        break;
    default:
        return -1;
    }
    if (t < blk->base_pc || t >= blk->base_pc + blk->code_size) return -1;
    if (rv_code_block_find(blk, t) < 0) return -1;
    *target_pc = t;
    return 0;
}

void rv_code_block_free(rv_code_block *blk)
{
    if (!blk) return;
    free((void *)blk->code);
    free(blk->recs);
    memset(blk, 0, sizeof(*blk));
}
