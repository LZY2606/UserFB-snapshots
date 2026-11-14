#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "codec2.h"
int main(void) {
 struct CODEC2 *enc=codec2_create(CODEC2_MODE_3200),*dec=codec2_create(CODEC2_MODE_3200);
 assert(enc&&dec);int n=codec2_samples_per_frame(enc),bytes=codec2_bytes_per_frame(enc);assert(n==160&&bytes==8);
 short in[160],out[160];unsigned char bits[8];uint32_t h=2166136261u;long long energy=0;
 for(int f=0;f<12;f++){
  for(int i=0;i<n;i++){int t=f*n+i;in[i]=(short)(2400*sin(.071*t)+900*cos(.133*t));}
  codec2_encode(enc,bits,in);codec2_decode(dec,out,bits);
  for(int i=0;i<n;i++){energy+=(long long)out[i]*out[i];h=(h^(uint16_t)out[i])*16777619u;}
 }
 assert(energy>1000000);codec2_destroy(enc);codec2_destroy(dec);
 printf("CODEC2_BASELINE_OK frames=12 samples=1920 bytes_per_frame=8 nonzero_energy=1 pcm=%08x\n",h);return 0;
}
