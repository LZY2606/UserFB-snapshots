#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dumb.h"
#include "fixture.h"
int main(void) {
 dumb_resampling_quality=DUMB_RQ_FIR;
 DUH *d=task_load_mod();if(!d)return 1;
 DUH_SIGRENDERER *a=duh_start_sigrenderer(d,0,2,0);
 DUH_SIGRENDERER *b=duh_start_sigrenderer(d,0,2,8002);
 sample_t **whole=allocate_sample_buffer(2,4257),**late=allocate_sample_buffer(2,256);
 if(!a||!b||!whole||!late)return 2;
 memset(whole[0],0,2*4257*sizeof(sample_t));memset(late[0],0,2*256*sizeof(sample_t));
 long n=duh_sigrenderer_generate_samples(a,1.0f,2.0f,4257,whole);
 long m=duh_sigrenderer_generate_samples(b,1.0f,2.0f,256,late);
 unsigned different=0,nonzero=0;long long maxdiff=0;
 for(int i=0;i<512;i++) {
  long long x=whole[0][8002+i],y=late[0][i],v=llabs(x-y);
  if(v){different++;if(v>maxdiff)maxdiff=v;}
  if(x)nonzero++;
 }
 printf("BASELINE_OK sequential_frames=%ld located_frames=%ld nonzero=%u\n",n,m,nonzero);
 printf("CURRENT_SEEK_DIFF scalar_samples=%u max_abs=%lld output_frame=4001 delta=2\n",different,maxdiff);
 duh_end_sigrenderer(a);duh_end_sigrenderer(b);destroy_sample_buffer(whole);destroy_sample_buffer(late);unload_duh(d);
 /* This is an observation of the old time-based seek, not a requirement to keep its difference. */
 return n==4257&&m==256&&nonzero>0?0:3;
}
