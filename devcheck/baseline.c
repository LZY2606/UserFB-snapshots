#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "libtta.h"
static unsigned char data[65536];static size_t pos,len;
static TTAint32 rd(TTA_io_callback*c,TTAuint8*p,TTAuint32 n){(void)c;size_t z=len-pos;if(z>n)z=n;memset(p,0,n);memcpy(p,data+pos,z);pos+=z;return z;}
static TTAint32 wr(TTA_io_callback*c,TTAuint8*p,TTAuint32 n){(void)c;assert(pos+n<sizeof data);memcpy(data+pos,p,n);pos+=n;if(pos>len)len=pos;return n;}
static TTAint64 sk(TTA_io_callback*c,TTAint64 n){(void)c;assert(n>=0&&n<(TTAint64)sizeof data);pos=n;return pos;}
int main(void){
 TTA_io_callback io={rd,wr,sk};TTA_info info={1,2,16,245,600},got;
 short pcm[1204],out[1204];for(int i=0;i<1200;i++)pcm[i]=(i*73)%12000-6000;
 memset(pcm+1200,0,8);memset(out,0,sizeof out);
 tta_encoder_new(&io);assert(tta_encoder_init_set_info(&info,0)==0);
 assert(tta_encoder_process_stream((unsigned char*)pcm,2400,0)==0);
 assert(tta_encoder_finalize()==0);tta_encoder_done();pos=0;
 tta_decoder_new(&io);assert(tta_decoder_init_get_info(&got)==0);
 assert(got.nch==2&&got.samples==600&&got.bps==16);
 int n=tta_decoder_process_stream((unsigned char*)out,2400,0);
 assert(n==600&&!memcmp(pcm,out,2400));tta_decoder_done();
 printf("TTA_BASELINE_OK frames=3 samples=600 stereo_pcm=equal bytes=%zu\n",len);
}
