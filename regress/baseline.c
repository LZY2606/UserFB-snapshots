#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include <stdio.h>
#include <string.h>
int main(void){
 drwav w,r;void *data=NULL;size_t size=0;drwav_int16 in[]={10,-20,30,-40,50,-60},out[6]={0};
 drwav_data_format fmt;memset(&fmt,0,sizeof(fmt));fmt.container=drwav_container_riff;fmt.format=DR_WAVE_FORMAT_PCM;fmt.channels=2;fmt.sampleRate=8000;fmt.bitsPerSample=16;
 if(!drwav_init_memory_write(&w,&data,&size,&fmt,NULL))return 1;
 if(drwav_write_pcm_frames(&w,3,in)!=3||drwav_uninit(&w)!=DRWAV_SUCCESS)return 2;
 if(!drwav_init_memory(&r,data,size,NULL))return 3;
 if(r.totalPCMFrameCount!=3||drwav_read_pcm_frames_s16(&r,3,out)!=3||memcmp(in,out,sizeof(in)))return 4;
 drwav_uninit(&r);drwav_free(data,NULL);data=NULL;size=0;
 fmt.format=DR_WAVE_FORMAT_ADPCM;fmt.bitsPerSample=4;
 if(drwav_init_memory_write(&w,&data,&size,&fmt,NULL)){drwav_uninit(&w);drwav_free(data,NULL);return 5;}
 puts("MSADPCM_BASELINE_OK pcm_stereo_roundtrip=3 compressed_writer_rejected=true");return 0;
}
