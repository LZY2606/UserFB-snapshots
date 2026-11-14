#include <stdio.h>
#include <string.h>
#include "udis86.h"
#include "gather-inputs.h"
int main(void){
 const unsigned char scalar[]={0x48,0x8b,0x44,0x8b,0x10};
 const unsigned char vex[]={0xc5,0xec,0x58,0xcb};
 ud_t u;const struct ud_operand *o;
 ud_init(&u);ud_set_mode(&u,64);ud_set_syntax(&u,UD_SYN_INTEL);ud_set_input_buffer(&u,scalar,sizeof scalar);
 if(ud_disassemble(&u)!=sizeof scalar||ud_insn_mnemonic(&u)!=UD_Imov)return 1;
 o=ud_insn_opr(&u,1);if(!o||o->type!=UD_OP_MEM||o->base!=UD_R_RBX||o->index!=UD_R_RCX||o->scale!=4||o->offset!=8||o->lval.sbyte!=16)return 2;
 printf("SCALAR_OK %s\n",ud_insn_asm(&u));
 ud_set_input_buffer(&u,vex,sizeof vex);if(ud_disassemble(&u)!=sizeof vex||ud_insn_mnemonic(&u)!=UD_Ivaddps)return 3;
 if(ud_insn_opr(&u,0)->base!=UD_R_YMM1||ud_insn_opr(&u,1)->base!=UD_R_YMM2||ud_insn_opr(&u,2)->base!=UD_R_YMM3)return 4;
 printf("VEX_OK %s\n",ud_insn_asm(&u));
 ud_set_input_buffer(&u,gather_inputs,sizeof gather_inputs);ud_disassemble(&u);
 printf("GATHER_STATUS %s mnemonic=%s\n",ud_insn_mnemonic(&u)==UD_Iinvalid?"unsupported":"decoded",ud_lookup_mnemonic(ud_insn_mnemonic(&u)));
 puts("BASELINE_OK scalar_index=rcx vector=ymm");return 0;
}
