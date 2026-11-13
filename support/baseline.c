#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed line %d\n",__LINE__);exit(1);}}while(0)
int main(void){
 AVCodecContext *e=avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_EXR));
 AVCodecContext *d=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_EXR));
 AVFrame *in=av_frame_alloc(),*out=av_frame_alloc();AVPacket *p=av_packet_alloc();
 e->width=17;e->height=33;e->pix_fmt=AV_PIX_FMT_GBRPF32;e->time_base=(AVRational){1,25};e->thread_count=1;d->thread_count=1;
 CHECK(av_opt_set(e->priv_data,"compression","zip16",0)==0);
 CHECK(av_opt_set(e->priv_data,"format","float",0)==0);
 CHECK(avcodec_open2(e,e->codec,NULL)==0 && avcodec_open2(d,d->codec,NULL)==0);
 in->width=e->width;in->height=e->height;in->format=e->pix_fmt;CHECK(av_frame_get_buffer(in,32)==0);
 for(int c=0;c<3;c++)for(int y=0;y<33;y++)for(int x=0;x<17;x++)((float*)(in->data[c]+y*in->linesize[c]))[x]=(float)((x+3*y+5*c)%32)/32;
 CHECK(avcodec_send_frame(e,in)==0 && avcodec_receive_packet(e,p)==0);
 CHECK(avcodec_send_packet(d,p)==0 && avcodec_receive_frame(d,out)==0);
 CHECK(out->width==17 && out->height==33 && out->format==in->format);
 for(int c=0;c<3;c++)for(int y=0;y<33;y++)CHECK(!memcmp(in->data[c]+y*in->linesize[c],out->data[c]+y*out->linesize[c],17*4));
 printf("BASELINE_EXR_ZIP16_17x33_RGB_EXACT\n");
 av_packet_free(&p);av_frame_free(&in);av_frame_free(&out);avcodec_free_context(&e);avcodec_free_context(&d);return 0;
}
