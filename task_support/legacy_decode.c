#include <stdio.h>
#include <string.h>
#include "riscv-disas.h"
#include "lrsc-inputs.h"
int main(void) {
 const size_t lens[]={4,4,2,4,4,4};const char *names[]={"lr.d","mv","addi","sc.d","bnez","addi"};
 size_t off=0,i; for(i=0;i<6;i++) {
  rv_inst word;size_t len;char text[160];
  inst_fetch(lrsc_inputs+off,&word,&len);
  if(len!=lens[i]) return 1;
  disasm_inst(text,sizeof text,rv64,0x1000+off,word);
  printf("NORMAL %04zx %zu %s\n",off,len,text);
  if(!strstr(text,names[i])) return 2;
  off+=len;
 }
 if(off!=sizeof lrsc_inputs)return 3;
 puts("RV64_EXISTING_DECODE_BASELINE_OK");return 0;
}
