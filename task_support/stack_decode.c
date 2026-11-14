#include <assert.h>
#include <stdio.h>
#include "udis86.h"
int main(void){
 const unsigned char code[]={0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,0x48,0x8b,0x45,0xf8,0xc9,0xc3};
 ud_t u;ud_init(&u);ud_set_mode(&u,64);ud_set_input_buffer(&u,code,sizeof code);
 const enum ud_mnemonic_code m[]={UD_Ipush,UD_Imov,UD_Isub,UD_Imov,UD_Ileave,UD_Iret};
 const unsigned len[]={1,3,4,4,1,1};
 for(unsigned i=0;i<6;i++){assert(ud_disassemble(&u)==len[i]);assert(ud_insn_mnemonic(&u)==m[i]);
 if(i==3){const struct ud_operand *o=ud_insn_opr(&u,1);assert(o->type==UD_OP_MEM && o->base==UD_R_RBP && o->offset==8 && o->lval.sbyte==-8);}}
 assert(!ud_disassemble(&u));puts("STACK_DECODE_OK instructions=6 displacement=-8");return 0;
}
