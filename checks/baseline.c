#include "sonic.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(int argc,char **argv) {
  short mono[4096], stereo[8192], out[16000]; int i,n,pm,ps;
  sonicStream m=sonicCreateStream(48000,1), s=sonicCreateStream(48000,2);
  assert(m && s);sonicSetQuality(m,1);sonicSetQuality(s,1);
  for(i=0;i<4096;i++){mono[i]=(short)lrint(10000*sin(i*6.283185307179586*220/48000));stereo[2*i]=mono[i];stereo[2*i+1]=-mono[i];}
  pm=findPitchPeriod(m,mono,1);ps=findPitchPeriod(s,stereo,1);
  printf("C_PERIOD_OBSERVED mono=%d antiphase=%d\n",pm,ps);
  assert(abs(pm-218)<=1);
  if(argc>1 && abs(ps-pm)>1){fprintf(stderr,"ANTIPHASE_PERIOD_MISMATCH\n");return 1;}
  sonicSetSpeed(s,1.5f);assert(sonicWriteShortToStream(s,stereo,4096));assert(sonicFlushStream(s));
  n=sonicReadShortFromStream(s,out,8000);assert(n>1000 && n<4096);
  for(i=0;i<n;i++)assert(abs(out[2*i]+out[2*i+1])<=1);
  printf("SONIC_C_PUBLIC_PATH_OK frames=%d\n",n);
  sonicDestroyStream(m);sonicDestroyStream(s);return 0;
}
