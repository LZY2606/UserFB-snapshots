#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#define WDL_SIMPLEPITCHSHIFT_IMPLEMENT
#include "WDL/simple_pitchshift.h"
static int render(int block,double tempo){WDL_SimplePitchShifter s;s.set_srate(8000);s.set_nch(2);s.set_tempo(tempo);s.set_shift(1);int got=0;for(int p=0;p<16000;){int n=std::min(block,16000-p);auto b=s.GetBuffer(n);for(int i=0;i<n;i++){b[2*i]=sin((p+i)*.1);b[2*i+1]=-b[2*i];}s.BufferDone(n);double out[1024];int m;while((m=s.GetSamples(512,out))){for(int j=0;j<m;j++)if(!std::isfinite(out[2*j])||fabs(out[2*j]+out[2*j+1])>1e-10)return -1;got+=m;}p+=n;}return got;}
int main(){int a=render(64,1),b=render(1,1);if(a!=16000||a!=b)return 1;int c=render(64,3.7),d=render(1,3.7);if(c<1||d<1)return 2;printf("WDL_BASELINE_OK unity=%d tempo37_block64=%d block1=%d\n",a,c,d);if(c!=d)puts("WDL_TEMPO_DRIFT_REPRODUCED");}
