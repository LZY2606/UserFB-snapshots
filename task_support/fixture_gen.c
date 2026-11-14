/* Benign instruction samples; generates words, never executes them. */
#include <stdint.h>
#include <stdio.h>
#define I(op,rs,rt,imm) (((uint32_t)(op)<<26)|((uint32_t)(rs)<<21)|((uint32_t)(rt)<<16)|((uint32_t)(imm)&65535u))
#define R(rs,rt,rd,fun) (((uint32_t)(rs)<<21)|((uint32_t)(rt)<<16)|((uint32_t)(rd)<<11)|(fun))
int main(void){
 uint32_t w[]={I(15,0,28,0x1234),I(9,28,28,-16),R(28,25,28,33),I(35,28,25,-32752),R(25,0,31,9),I(9,0,4,7),R(28,0,16,33),I(35,28,25,16)};
 puts("#include <stdint.h>\nstatic const uint32_t pic_words[]={");
 for(unsigned i=0;i<8;i++)printf("0x%08xu,%s",w[i],i==7?"\n":"");
 puts("};");return 0;
}
