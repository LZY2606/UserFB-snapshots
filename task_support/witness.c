#include <capstone/capstone.h>
#include <stdio.h>
#include <string.h>
/* Existing decoding only: this witness does not calculate instruction semantics. */
static const unsigned char code[]={
 0x04,0xbf, /* itt eq */
 0x01,0x28, /* cmpeq r0,#1 */
 0x07,0x21, /* moveq r1,#7 */
 0x41,0xf2,0x34,0x20, /* movw r0,#0x1234 */
 0xc5,0xf2,0x78,0x60, /* movt r0,#0x5678 */
 0x0a,0x60, /* str r2,[r1] */
 0x23,0x68  /* ldr r3,[r4] */
};
int main(void) {
 csh h=0,it=0; cs_insn *a=0,*b=0; size_t n,i,remain=sizeof(code);
 const uint8_t *ptr=code; uint64_t pc=0x1000;
 if(cs_open(CS_ARCH_ARM,CS_MODE_THUMB,&h)||cs_open(CS_ARCH_ARM,CS_MODE_THUMB,&it)) return 1;
 if(cs_option(h,CS_OPT_DETAIL,CS_OPT_ON)||cs_option(it,CS_OPT_DETAIL,CS_OPT_ON)) return 2;
 n=cs_disasm(h,code,sizeof(code),pc,0,&a); b=cs_malloc(it);
 if(n!=7||!b) return 3;
 for(i=0;i<n;i++) {
  if(!cs_disasm_iter(it,&ptr,&remain,&pc,b)||b->id!=a[i].id||b->size!=a[i].size||
    b->detail->arm.cc!=a[i].detail->arm.cc||b->detail->arm.update_flags!=a[i].detail->arm.update_flags) return 4;
  printf("DECODE %llx %s %s cc=%u S=%u ops=%u\n",(unsigned long long)a[i].address,a[i].mnemonic,a[i].op_str,
     a[i].detail->arm.cc,a[i].detail->arm.update_flags,a[i].detail->arm.op_count);
 }
 if(remain||a[1].id!=ARM_INS_CMP||a[1].detail->arm.cc!=ARM_CC_EQ||!a[1].detail->arm.update_flags||
    a[2].id!=ARM_INS_MOV||a[2].detail->arm.cc!=ARM_CC_EQ||a[2].detail->arm.update_flags||
    (a[3].id!=ARM_INS_MOV || a[3].size!=4)||a[3].detail->arm.operands[1].imm!=0x1234||
    a[4].id!=ARM_INS_MOVT||a[4].detail->arm.operands[1].imm!=0x5678||
    a[5].id!=ARM_INS_STR||a[6].id!=ARM_INS_LDR) return 5;
 {cs_regs rd,wr;uint8_t nr,nw;size_t j;int cpsr=0;
  if(cs_regs_access(h,&a[2],rd,&nr,wr,&nw)) return 6;
  for(j=0;j<nr;j++) if(rd[j]==ARM_REG_CPSR)cpsr=1;
  printf("OLD_CONDITIONAL_CPSR_READ=%d\n",cpsr);
 }
 cs_free(a,n);cs_free(b,1);cs_close(&h);cs_close(&it);
 puts("THUMB_IR_DECODE_BASELINE_OK"); return 0;
}
