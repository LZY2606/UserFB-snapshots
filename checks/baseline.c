#include <assert.h>
#include <stdio.h>
#include "udis86.h"
int main(void) {
  const unsigned char code[]={0xb4,0x7f,0x89,0xc8,0x48,0x0f,0x44,0xc3,0xc3};
  ud_t u; unsigned n=0;
  ud_init(&u); ud_set_mode(&u,64); ud_set_input_buffer(&u,code,sizeof code);
  while(ud_disassemble(&u)) {
    const struct ud_operand *o=ud_insn_opr(&u,0);
    if(n==0) assert(u.mnemonic==UD_Imov && o->base==UD_R_AH && o->size==8);
    if(n==1) assert(u.mnemonic==UD_Imov && o->base==UD_R_EAX && o->size==32);
    if(n==2) assert(u.mnemonic==UD_Icmovz && o->base==UD_R_RAX && o->size==64);
    if(n==3) assert(u.mnemonic==UD_Iret);
    n++;
  }
  assert(n==4); puts("UDIS_DECODE_OK instructions=4");
}
