#include <assert.h>
#include <stdio.h>
#include <Zydis/Zydis.h>
#include "compact-inputs.h"
int main(void){
 ZydisDecoder dec;ZydisDecodedInstruction i;ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
 const ZydisMnemonic expect[]={ZYDIS_MNEMONIC_KMOVW,ZYDIS_MNEMONIC_KANDW,ZYDIS_MNEMONIC_KNOTW,ZYDIS_MNEMONIC_KSHIFTLW,ZYDIS_MNEMONIC_VPCOMPRESSD,ZYDIS_MNEMONIC_VPCOMPRESSD,ZYDIS_MNEMONIC_VPEXPANDD,ZYDIS_MNEMONIC_VPCOMPRESSD,ZYDIS_MNEMONIC_VPEXPANDD,ZYDIS_MNEMONIC_VMOVDQU32};
 assert(ZYAN_SUCCESS(ZydisDecoderInit(&dec,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64)));
 for(size_t n=0;n<sizeof(compact_spans)/sizeof(compact_spans[0]);n++){
  const struct fixture_span *s=&compact_spans[n];
  assert(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec,compact_bytes+s->offset,s->length,&i,op)));
  assert(i.mnemonic==expect[n] && i.length==s->length);
  if(n==0)assert(op[0].reg.value==ZYDIS_REGISTER_K1 && op[1].reg.value==ZYDIS_REGISTER_K2);
  if(n==4)assert(i.avx.vector_length==512 && i.avx.mask.reg==ZYDIS_REGISTER_K1 && op[0].reg.value==ZYDIS_REGISTER_ZMM3 && op[2].reg.value==ZYDIS_REGISTER_ZMM2);
  if(n==5)assert(i.avx.vector_length==256 && op[0].reg.value==ZYDIS_REGISTER_YMM4);
  if(n==7){ZyanU64 a;assert(op[0].type==ZYDIS_OPERAND_TYPE_MEMORY && op[0].mem.base==ZYDIS_REGISTER_RIP);assert(ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&i,&op[0],0x1000+s->offset,&a)));assert(a==0x2000);}
  if(n==8){ZyanU64 a;assert(op[2].type==ZYDIS_OPERAND_TYPE_MEMORY && op[2].mem.base==ZYDIS_REGISTER_RIP);assert(ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&i,&op[2],0x1000+s->offset,&a)));assert(a==0x2004);}
  if(n==9)assert(op[0].reg.value==ZYDIS_REGISTER_XMM31 && op[2].reg.value==ZYDIS_REGISTER_XMM17 && i.avx.vector_length==128);
 }
 puts("ZYDIS_COMPACTION_EXISTING_BASELINE_OK instructions=10 regs=31 rip=1 widths=128,256,512");return 0;
}
