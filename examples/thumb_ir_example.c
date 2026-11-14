/* Example: lift a Thumb snippet to a cs_thumb_ir expression graph and
   evaluate it under concrete entry inputs.
   Build (from the repository root, after building libcapstone):
     cc -std=c99 -Iinclude examples/thumb_ir_example.c .task-build/libcapstone.a -o thumb_ir_example
*/

#include <stdio.h>
#include <string.h>

#include <capstone/capstone.h>
#include <capstone/thumb_ir.h>

/* Entry-memory bytes for the example: a tiny read-only "flash" model. */
static bool flash_read8(void *ud, uint32_t addr, uint8_t *out)
{
    const uint8_t *flash = (const uint8_t *)ud;
    if (addr < 0x1000 || addr >= 0x1000 + 16)
        return false; /* unmapped */
    *out = flash[addr - 0x1000];
    return true;
}

int main(void)
{
    /* movs r0, #5; adds r0, r0, r1; str r0, [r2]; ldr r3, [r2] */
    static const uint8_t code[] = {
        0x05, 0x20, /* movs r0, #5      */
        0x40, 0x18, /* adds r0, r0, r1  */
        0x10, 0x60, /* str r0, [r2]     */
        0x13, 0x68, /* ldr r3, [r2]     */
    };
    static const uint8_t flash[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    };
    cs_thumb_ir *g = NULL;
    cs_thumb_ir_error err;
    cs_thumb_ir_inputs in;
    cs_thumb_ir_insn_info ii;
    uint32_t i, v;
    uint8_t b;

    if (cs_thumb_ir_lift(code, sizeof(code), 0x8000, &g, &err) != CS_THUMB_IR_OK) {
        fprintf(stderr, "lift failed at 0x%x: %s\n", err.addr,
                cs_thumb_ir_strerror(err.code));
        return 1;
    }

    printf("lifted %u instructions into %u nodes\n",
           cs_thumb_ir_insn_count(g), cs_thumb_ir_node_count(g));
    for (i = 0; i < cs_thumb_ir_insn_count(g); i++) {
        if (cs_thumb_ir_get_insn(g, i, &ii))
            printf("  insn %u @0x%x: cond node=%u, %u root(s) modified\n",
                   i, ii.addr, ii.cond, ii.root_count);
    }

    /* Evaluate the graph with concrete entry state. */
    memset(&in, 0, sizeof(in));
    in.regs[1] = 7;      /* r1 */
    in.regs[2] = 0x1004; /* r2: store address */
    in.read8 = flash_read8;
    in.read8_ud = (void *)flash;

    if (cs_thumb_ir_eval_node(g, &in, cs_thumb_ir_root_reg(g, 0), &v, NULL) != CS_THUMB_IR_OK)
        return 2;
    printf("r0 = 0x%x (5 + 7)\n", v);

    if (cs_thumb_ir_eval_node(g, &in, cs_thumb_ir_root_reg(g, 3), &v, NULL) != CS_THUMB_IR_OK)
        return 3;
    printf("r3 = 0x%x (loaded back from [r2])\n", v);

    if (cs_thumb_ir_eval_node(g, &in, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_C), &v, NULL) != CS_THUMB_IR_OK)
        return 4;
    printf("C  = %u (carry of 5 + 7)\n", v);

    /* A neighbouring entry-memory byte still comes from the flash model. */
    if (cs_thumb_ir_eval_mem8(g, &in, 0x1000, &b, NULL) != CS_THUMB_IR_OK)
        return 5;
    printf("mem[0x1000] = 0x%x\n", b);

    cs_thumb_ir_free(g);
    return 0;
}
