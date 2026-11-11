/* Public-domain, inert byte arrays for decoder observations only. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "distorm.h"
#include "mnemonics.h"

static unsigned decode(const unsigned char *p, unsigned n, uint64_t address,
                       _DInst *out, unsigned capacity)
{
    _CodeInfo ci;
    unsigned used = 0;
    memset(&ci, 0, sizeof(ci));
    ci.code = p; ci.codeLen = (int)n; ci.codeOffset = address;
    ci.dt = Decode64Bits; ci.features = DF_FILL_EFLAGS;
    assert(distorm_decompose(&ci, out, capacity, &used) == DECRES_SUCCESS);
    assert(used != 0);
    for (unsigned i = 0; i < used; ++i) assert(out[i].flags != FLAG_NOT_DECODABLE);
    return used;
}

int main(void)
{
    unsigned char block[132];
    _DInst d[256];
    unsigned used;
    int32_t ext_disp = 0x2000 - (0x1000 + 9);
    memset(block, 0x90, sizeof(block));
    block[0] = 0x75; block[1] = 124;
    block[2] = 0x75; block[3] = 127;
    block[4] = 0xe9;
    for (unsigned i = 0; i < 4; ++i) block[5+i] = (unsigned char)((uint32_t)ext_disp >> (i*8));
    block[131] = 0xc3;
    used = decode(block, sizeof(block), 0x1000, d, 256);
    assert(used > 120);
    assert(d[0].size == 2 && d[0].ops[0].type == O_PC);
    assert(INSTRUCTION_GET_TARGET(&d[0]) == 0x107e);
    assert(INSTRUCTION_GET_TARGET(&d[1]) == 0x1083);
    assert(d[2].size == 5 && INSTRUCTION_GET_TARGET(&d[2]) == 0x2000);
    printf("DECODE_BRANCHES_OK instructions=%u deltas=124,127 external=0x2000\n", used);

    /* The displacement is followed by a separate immediate. */
    const unsigned char mov[] = {0x48,0xc7,0x05,0x78,0x56,0x34,0x12,0xef,0xbe,0xad,0xde};
    assert(decode(mov, sizeof(mov), 0x4000, d, 256) == 1);
    assert(d[0].opcode == I_MOV && d[0].size == 11);
    assert((d[0].flags & FLAG_RIP_RELATIVE) && d[0].dispSize == 32);
    assert(d[0].disp == 0x12345678 && d[0].imm.dword == 0xdeadbeef);
    assert(INSTRUCTION_GET_RIP_TARGET(&d[0]) == 0x12349683);
    puts("RIP_IMMEDIATE_OK mov_size=11 disp=0x12345678 imm=0xdeadbeef");

    const unsigned char simd[] = {0x66,0x0f,0x3a,0x0f,0x05,0xf0,0xff,0xff,0xff,0x07};
    assert(decode(simd, sizeof(simd), 0x5000, d, 256) == 1);
    assert(d[0].opcode == I_PALIGNR && d[0].size == 10);
    assert((d[0].flags & FLAG_RIP_RELATIVE) && (int64_t)d[0].disp == -16);
    assert(d[0].imm.byte == 7 && INSTRUCTION_GET_RIP_TARGET(&d[0]) == 0x4ffa);
    puts("SIMD_RIP_OK size=10 target=0x4ffa immediate=7");

    const unsigned char loop[] = {0x67,0xe2,0xfd};
    assert(decode(loop, sizeof(loop), 0x6000, d, 256) == 1);
    assert(d[0].opcode == I_LOOP && FLAG_GET_ADDRSIZE(d[0].flags) == Decode32Bits);
    assert(INSTRUCTION_GET_TARGET(&d[0]) == 0x6000);
    puts("LOOP_COUNTER_OK address_size=32 target=0x6000");
    puts("BASELINE_OK");
    return 0;
}
