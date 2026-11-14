/* Created normal RV64 I/A/C words, not executable test code. */
#include <stdint.h>
#include <stdio.h>
static void emit(uint32_t x, unsigned bytes) { unsigned i;for(i=0;i<bytes;i++)printf("%02x",(unsigned)(x>>(i*8))&255); }
static uint32_t branch(int displacement) { uint32_t i=(uint32_t)displacement&8191;return 0x1063|(6u<<15)|((i>>12)&1)<<31|((i>>5)&63)<<25|((i>>1)&15)<<8|((i>>11)&1)<<7; }
int main(void) {
 emit((2u<<27)|(1u<<26)|(10u<<15)|(3u<<12)|(5u<<7)|0x2f,4); /* lr.d.aq t0,(a0) */
 emit((10u<<15)|(11u<<7)|0x13,4); /* addi a1,a0,0 */
 emit((5u<<7)|(1u<<2)|1u,2); /* c.addi t0,1 */
 emit((3u<<27)|(1u<<25)|(5u<<20)|(11u<<15)|(3u<<12)|(6u<<7)|0x2f,4);
 emit(branch(-14),4); /* bne t1,zero,start */
 emit((1u<<20)|(12u<<7)|0x13,4);putchar('\n');return 0;
}
