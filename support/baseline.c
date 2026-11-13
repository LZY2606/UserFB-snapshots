#include "rabbitizer.h"
#include <stdio.h>
int main(void) {
 RabbitizerInstruction i;
 RabbitizerInstruction_init(&i,0x51090002,0x80001000);
 RabbitizerInstruction_processUniqueId(&i);
 int likely=RabbitizerInstrDescriptor_isBranchLikely(i.descriptor);
 int delay=RabbitizerInstruction_hasDelaySlot(&i);
 unsigned target=RabbitizerInstruction_getBranchVramGeneric(&i);
 printf("BEQL likely=%d delay=%d target=%08x\n",likely,delay,target);
 RabbitizerInstruction_destroy(&i);
 RabbitizerInstruction_init(&i,0x08000001,0x0ffffffc);
 RabbitizerInstruction_processUniqueId(&i);
 unsigned jump=RabbitizerInstruction_getInstrIndexAsVram(&i);
 printf("J region target=%08x\n",jump); RabbitizerInstruction_destroy(&i);
 return likely!=1 || delay!=1 || target!=0x8000100c || jump!=0x10000004;
}
