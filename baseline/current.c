#include "x86emu.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 x86emu_t *e=x86emu_new(X86EMU_PERM_RWX,0);assert(e);
 const unsigned char code[]={0xb8,0x34,0x12,0xa3,0x00,0x02,0xb9,3,0,0xbe,0,2,0xbf,0x10,2,0xf3,0xa4,0xf4};
 x86emu_set_seg_register(e,e->x86.R_CS_SEL,0);x86emu_set_seg_register(e,e->x86.R_DS_SEL,0);
 x86emu_set_seg_register(e,e->x86.R_ES_SEL,0);x86emu_set_seg_register(e,e->x86.R_SS_SEL,0);
 e->x86.R_EIP=0x100;e->max_instr=100;
 for(unsigned i=0;i<sizeof code;i++)x86emu_write_byte(e,0x100+i,code[i]);
 x86emu_write_byte(e,0x202,0x56);unsigned rs=x86emu_run(e,X86EMU_RUN_MAX_INSTR);
 assert(!(rs & X86EMU_RUN_MAX_INSTR));assert(e->x86.R_AX==0x1234);
 assert(e->x86.R_CX==0 && e->x86.R_SI==0x203 && e->x86.R_DI==0x213);
 assert(x86emu_read_word(e,0x210)==0x1234);assert(x86emu_read_byte(e,0x212)==0x56);
 x86emu_done(e);puts("X86EMU_BASELINE_OK mov=1234 rep=3 destination=341256");return 0;
}
