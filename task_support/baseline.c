#include "speex_resampler.h"
#include <math.h>
#include <stdio.h>
int main(void) {
 float input[4096], a[256],b[256],first_a[1],first_b[1];
 SpeexResamplerState *left,*right; int error=0; unsigned i; double peak=0,energy=0;
 spx_uint32_t ia=4096,ib=4096,oa=1,ob=1,used;
 for(i=0;i<4096;i++)input[i]=(float)(.4*sin(2*3.141592653589793*.017*i));
 left=speex_resampler_init_frac(1,2,3,32000,48000,10,&error);if(!left||error)return 1;
 right=speex_resampler_init_frac(1,2,3,32000,48000,10,&error);if(!right||error)return 2;
 if(speex_resampler_skip_zeros(left)||speex_resampler_skip_zeros(right))return 3;
 if(speex_resampler_process_float(left,0,input,&ia,first_a,&oa)||speex_resampler_process_float(right,0,input,&ib,first_b,&ob))return 4;
 if(oa!=1||ob!=1||ia!=ib||first_a[0]!=first_b[0])return 5;used=ia;
 if(speex_resampler_set_rate_frac(right,5,7,32000,48000)||speex_resampler_set_rate_frac(right,2,3,32000,48000))return 6;
 ia=ib=4096-used;oa=ob=256;
 if(speex_resampler_process_float(left,0,input+used,&ia,a,&oa)||speex_resampler_process_float(right,0,input+used,&ib,b,&ob))return 7;
 if(oa!=256||ob!=256)return 8;
 for(i=0;i<256;i++){double d=fabs(a[i]-b[i]);if(!isfinite(a[i])||!isfinite(b[i]))return 9;if(d>peak)peak=d;energy+=a[i]*a[i];}
 if(peak<.001||energy<1)return 10;
 printf("SPEEX_BASELINE channels=1 output=256 roundtrip_maxdiff=%.9f energy=%.9f\n",peak,energy);
 speex_resampler_destroy(left);speex_resampler_destroy(right);return 0;
}
