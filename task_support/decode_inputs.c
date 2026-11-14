#include <stdio.h>
#include "udis86.h"
#include "amx-inputs.h"
int main(void) {
 const unsigned char plain[]={0xd9,0xe8,0xd9,0xc9,0xdd,0xd8,0x48,0x8b,0x44,0x88,0x10};
 ud_t u; unsigned n=0; ud_init(&u);ud_set_mode(&u,64);ud_set_syntax(&u,UD_SYN_INTEL);ud_set_input_buffer(&u,plain,sizeof plain);
 while(ud_disassemble(&u)) { ++n; printf("PLAIN %s\n",ud_insn_asm(&u)); if(u.mnemonic==UD_Iinvalid) return 1; }
 if(n!=4) return 2;
 /* Observation of the requested feature's absence, never a target pass. */
 ud_init(&u);ud_set_mode(&u,64);ud_set_input_buffer(&u,amx_inputs,sizeof amx_inputs);
 if(!ud_disassemble(&u)) return 3;
 printf("AMX_UPSTREAM_FIRST %s length=%u\n",ud_lookup_mnemonic(u.mnemonic),ud_insn_len(&u));
 puts("AMX_EXISTING_DECODE_BASELINE_OK");return 0;
}
