#include "sbc.h"
#include <stdio.h>
#include <stdint.h>
int main(void){for(int bands=4;bands<=8;bands+=4){sbc_t e={0},d={0};struct sbc_frame f={.freq=SBC_FREQ_48K,.mode=SBC_MODE_STEREO,.bam=SBC_BAM_LOUDNESS,.nblocks=16,.nsubbands=bands,.bitpool=32},got;int16_t a[128],b[128],l[128],r[128];unsigned char buf[1024];uint32_t hash=0;for(int j=0;j<4;j++){for(int i=0;i<16*bands;i++){a[i]=(i*367+j*103)%12000-6000;b[i]=(i*983-j*51)%16000-8000;}unsigned n=sbc_get_frame_size(&f);if(!n||sbc_encode(&e,a,1,b,1,&f,buf,n)||sbc_decode(&d,buf,n,&got,l,1,r,1))return 1;for(int i=0;i<16*bands;i++)hash=hash*33u+(uint16_t)l[i]+(uint16_t)r[i];}if(!hash)return 2;printf("SBC_BASE bands=%d frames=4 samples=%d bytes=%u hash=%08x\n",bands,64*bands,sbc_get_frame_size(&f),hash);}return 0;}
