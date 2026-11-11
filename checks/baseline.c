#include "compressor.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
 sf_compressor_state_st a,b;
 sf_sample_st x[96],y[96],z[96];
 for(int i=0;i<96;i++) x[i]=(sf_sample_st){.L=(i==32?0.9f:0.1f),.R=-0.05f};
 sf_defaultcomp(&a,48000);sf_defaultcomp(&b,48000);
 sf_compressor_process(&a,96,x,y);
 for(int i=0;i<3;i++)sf_compressor_process(&b,32,x+32*i,z+32*i);
 for(int i=0;i<96;i++)assert(y[i].L==z[i].L && isfinite(y[i].L));
 sf_sample_st sentinel={.L=123,.R=456}; sf_defaultcomp(&a,48000);
 sf_compressor_process(&a,1,x,&sentinel);
 assert(sentinel.L==123 && sentinel.R==456);
 assert(a.delaywritepos==0);
 printf("baseline: 96=3x32; size1 leaves sentinel; predelay=%d latency=%d\n",a.delaybufsize,a.delaybufsize-1);
}
