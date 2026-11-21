#include "libresample.h"
#include <math.h>
#include <stdio.h>
int main(void) {
 float in[521],out[128]; int i,used=0,pos=0,total=0,guard; double energy=0;
 void *h=resample_open(1,48000.0/44100.0,48000.0/44100.0);
 if(!h)return 2;
 for(i=0;i<521;i++)in[i]=(float)(0.2*sin(i*0.11));
 for(guard=0;guard<10000;guard++){
  int n=resample_process(h,48000.0/44100.0,in+pos,521-pos,1,&used,out,128);
  if(n<0 || used<0 || used>521-pos)return 3;
  pos+=used;total+=n;for(i=0;i<n;i++){if(!isfinite(out[i]))return 4;energy+=out[i]*out[i];}
  if(n==0&&pos==521)break;
 }
 resample_close(h);
 if(pos!=521||total<560||total>580||energy<1.0||guard==10000)return 5;
 printf("DENSE_PRODUCTION input=%d output=%d energy=%.9f\n",pos,total,energy);
 puts("BASELINE_COMPLETE sparse_stream_api=absent");return 0;
}
