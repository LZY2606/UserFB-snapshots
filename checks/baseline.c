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

 // a single frame is now processed completely instead of being left as a sentinel
 sf_sample_st sentinel={.L=123,.R=456};
 sf_defaultcomp(&a,48000);
 sf_compressor_process(&a,1,x,&sentinel);
 assert(isfinite(sentinel.L) && isfinite(sentinel.R));
 assert(sentinel.L!=123 || sentinel.R!=456);
 assert(a.delaywritepos==1 && a.spupos==1 && a.totalframes==1);

 // a zero length call with NULL pointers is legal and advances no state at all
 sf_defaultcomp(&a,48000);
 sf_compressor_process(&a,0,NULL,NULL);
 assert(a.delaywritepos==0 && a.spupos==0 && a.totalframes==0);
 assert(sf_compressor_drain(&a,1,NULL)==0); // no input means no tail

 // latency follows the ring buffer definition, and draining emits exactly that many frames
 sf_defaultcomp(&a,48000);
 int D=sf_compressor_latency(&a);
 assert(D==a.delaybufsize-1);
 sf_compressor_process(&a,96,x,y);
 sf_sample_st tail[SF_COMPRESSOR_MAXDELAY];
 int got=0,w;
 while((w=sf_compressor_drain(&a,7,tail+got))>0){
   got+=w;
   assert(got<=D);
 }
 assert(got==D);
 assert(sf_compressor_drain(&a,7,tail)==0); // repeated calls after exhaustion return zero
 printf("baseline: 96=3x32; size1 processed; size0 no-op; predelay=%d latency=%d\n",
   a.delaybufsize,D);
}
