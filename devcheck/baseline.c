#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include <stdio.h>
#include <assert.h>
#include <math.h>
int main(void) {
 mp3dec_t d;mp3dec_file_info_t f;
 assert(mp3dec_load(&d,"vectors/l3-si_block.bit",&f,NULL,NULL)==0);
 assert(f.samples==73728&&f.channels==1&&f.hz==44100);
 FILE*r=fopen("vectors/l3-si_block.pcm","rb");assert(r);
 double err=0,power=0;size_t count=0;for(size_t i=0;i<f.samples;i++) {int16_t v;if(fread(&v,2,1,r)!=1)break;count++;double e=f.buffer[i]-v;err+=e*e;power+=(double)v*v;}
 fclose(r);assert(count==72576);assert(power>0);assert(err==0||10*log10(32768.*32768.*count/err)>96);
 printf("MINIMP3_BASELINE_OK samples=%zu channels=%d snr_db=%.2f\n",f.samples,f.channels,err?10*log10(32768.*32768.*count/err):999.);
 free(f.buffer);
}
