#include "emu2413.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
int main(void){
 OPLL *o=OPLL_new(3579545,44100);assert(o);
 OPLL_writeReg(o,0x30,0x10);OPLL_writeReg(o,0x10,0x93);OPLL_writeReg(o,0x20,0x19);
 OPLL_writeReg(o,0x16,0x51);OPLL_writeReg(o,0x17,0x87);OPLL_writeReg(o,0x18,0xc1);
 OPLL_writeReg(o,0x26,0x09);OPLL_writeReg(o,0x27,0x09);OPLL_writeReg(o,0x28,0x09);
 OPLL_writeReg(o,0x36,0x01);OPLL_writeReg(o,0x37,0x11);OPLL_writeReg(o,0x38,0x11);OPLL_writeReg(o,0x0e,0x3f);
 int64_t energy=0;int32_t out[2];
 for(int i=0;i<8192;i++){OPLL_calcStereo(o,out);assert(out[0]==out[1]);energy+=(int64_t)out[0]*out[0];}
 assert(energy>100000);OPLL_delete(o);printf("OPLL_AUDIO_OK energy=%lld\n",(long long)energy);
}
