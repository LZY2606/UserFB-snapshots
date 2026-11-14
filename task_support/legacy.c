#include <assert.h>
#include <stdio.h>
#include "rabbitizer.h"
#include "pic-inputs.h"
static void decode(RabbitizerInstruction *i,uint32_t w,unsigned off){RabbitizerInstruction_init(i,w,0x00401000+off);RabbitizerInstruction_processUniqueId(i);}
int main(void){
 RabbitizerInstruction i[8],adjust;RabbitizerRegistersTracker t;RabbitizerLoPairingInfo lo;
 const RabbitizerInstrId expected[]={RABBITIZER_INSTR_ID_cpu_lui,RABBITIZER_INSTR_ID_cpu_addiu,RABBITIZER_INSTR_ID_cpu_addu,RABBITIZER_INSTR_ID_cpu_lw,RABBITIZER_INSTR_ID_cpu_jalr,RABBITIZER_INSTR_ID_cpu_addiu,RABBITIZER_INSTR_ID_cpu_addu,RABBITIZER_INSTR_ID_cpu_lw};
 for(unsigned n=0;n<8;n++){decode(&i[n],pic_words[n],4*n);assert(i[n].uniqueId==expected[n]);}
 assert(RabbitizerInstruction_getProcessedImmediate(&i[1])==-16);
 assert(RabbitizerInstruction_getProcessedImmediate(&i[3])==-32752);
 assert(RabbitizerInstruction_isFunctionCall(&i[4]));
 RabbitizerRegistersTracker_init(&t,NULL);
 RabbitizerRegistersTracker_processLui(&t,&i[0],0,NULL);
 lo=RabbitizerRegistersTracker_preprocessLoAndGetInfo(&t,&i[1],4);
 assert(lo.shouldProcess && lo.instrOffset==0 && lo.value==0x12340000 && !lo.isGpRel);
 RabbitizerRegistersTracker_processLo(&t,&i[1],0x1233fff0,4);
 assert(t.registers[28].hasLoValue && t.registers[28].value==0x1233fff0);
 RabbitizerRegistersTracker_processGpLoad(&t,&i[3],12);
 decode(&adjust,0x27390000,32);
 lo=RabbitizerRegistersTracker_preprocessLoAndGetInfo(&t,&adjust,32);
 assert(lo.shouldProcess && lo.isGpGot && lo.instrOffset==12 && (uint32_t)lo.value==(uint32_t)-32752);
 RabbitizerRegistersTracker_destroy(&t);RabbitizerInstruction_destroy(&adjust);
 for(unsigned n=0;n<8;n++)RabbitizerInstruction_destroy(&i[n]);
 puts("RABBITIZER_PIC_EXISTING_BASELINE_OK words=8 hi_lo=1 gp_load=1 signed_offset=-32752");return 0;
}
