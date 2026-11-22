/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2015             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

  Bounded pending PCM example.

  A player that decodes a batch of packets up front and then lets the
  sound card pull small chunks normally has to stop at the first
  unread block: vorbis_synthesis_blockin() refuses a new block while
  earlier PCM is still pending.  Setting a bounded pending limit with
  vorbis_synthesis_set_pending_limit() lets the native synthesizer
  keep a bounded backlog instead; a block that would overflow the
  backlog is rejected with OV_EBACKPRESSURE and can be retried
  unchanged after the consumer reads some audio.

  This example needs no audio device: it encodes a synthetic signal
  in memory, then decodes it in batches while a simulated sound card
  drains fixed 113-frame chunks.

  Build (from the top level, after the library):
    cc -Iinclude pending_example.c -Llib/.libs -lvorbis -lvorbisenc -lm

 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vorbis/vorbisenc.h>

#define FRAMES 22050          /* half a second at 44.1kHz */
#define PENDING_LIMIT 2048    /* backlog budget in frames per channel */
#define CARD_CHUNK 113        /* sound card pull size */

static int encode_tone(ogg_packet **out,int *outn){
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  ogg_packet header[3],op;
  ogg_packet *pkts=NULL;
  int count=0,cap=0,pos=0,i;

  vorbis_info_init(&vi);
  if(vorbis_encode_init_vbr(&vi,1,44100,.4f))return 1;
  vorbis_comment_init(&vc);
  if(vorbis_analysis_init(&vd,&vi))return 1;
  if(vorbis_block_init(&vd,&vb))return 1;
  {
    ogg_packet h,c,s;
    if(vorbis_analysis_headerout(&vd,&vc,&h,&c,&s))return 1;
    header[0]=h;
    header[1]=c;
    header[2]=s;
  }
  /* deep-copy the headers; the originals live in encoder storage */
  for(i=0;i<3;i++){
    unsigned char *copy=malloc(header[i].bytes);
    if(!copy)return 1;
    memcpy(copy,header[i].packet,header[i].bytes);
    header[i].packet=copy;
  }

  for(;;){
    int n=FRAMES-pos>512?512:FRAMES-pos;
    if(n>0){
      float **in=vorbis_analysis_buffer(&vd,n);
      for(i=0;i<n;i++){
        int k=pos+i;
        in[0][i]=(float)(.15*sin(k*.05)+.04*cos(k*.013));
        if(k%3000<3)in[0][i]+=(k%2?.6f:-.6f); /* clicks for short blocks */
      }
      pos+=n;
    }
    if(vorbis_analysis_wrote(&vd,n))return 1;
    while(vorbis_analysis_blockout(&vd,&vb)==1){
      if(vorbis_analysis(&vb,NULL))return 1;
      if(vorbis_bitrate_addblock(&vb))return 1;
      while(vorbis_bitrate_flushpacket(&vd,&op)){
        ogg_packet copy=op;
        if(count==cap){
          cap=cap?cap*2:64;
          pkts=realloc(pkts,cap*sizeof(*pkts));
          if(!pkts)return 1;
        }
        copy.packet=malloc(op.bytes);
        if(!copy.packet)return 1;
        memcpy(copy.packet,op.packet,op.bytes);
        pkts[count++]=copy;
      }
    }
    if(n<=0)break;
  }

  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);

  /* stash the headers in front of the audio packets */
  {
    ogg_packet *all=malloc((count+3)*sizeof(*all));
    if(!all)return 1;
    memcpy(all,header,3*sizeof(*all));
    memcpy(all+3,pkts,count*sizeof(*pkts));
    free(pkts);
    *out=all;
    *outn=count+3;
  }
  return 0;
}

int main(void){
  ogg_packet *pkts;
  int npkts,i;
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  long produced=0,consumed=0;
  int backpressure=0;

  if(encode_tone(&pkts,&npkts)){
    fprintf(stderr,"encode failed\n");
    return 1;
  }

  vorbis_info_init(&vi);
  vorbis_comment_init(&vc);
  for(i=0;i<3;i++)
    if(vorbis_synthesis_headerin(&vi,&vc,&pkts[i]))return 1;
  if(vorbis_synthesis_init(&vd,&vi))return 1;

  /* enable the bounded backlog before the first audio block */
  if(vorbis_synthesis_set_pending_limit(&vd,PENDING_LIMIT)){
    fprintf(stderr,"pending limit rejected\n");
    return 1;
  }
  if(vorbis_block_init(&vd,&vb))return 1;

  for(i=3;i<npkts;i++){
    int ret;
    if(vorbis_synthesis(&vb,&pkts[i]))continue; /* non-audio packet */
    /* submit; if the backlog is full the sound card must catch up */
    while((ret=vorbis_synthesis_blockin(&vd,&vb))==OV_EBACKPRESSURE){
      float **pcm;
      int n=vorbis_synthesis_pcmout(&vd,&pcm);
      backpressure++;
      if(n>0){
        consumed+=n;
        vorbis_synthesis_read(&vd,n);
      }
    }
    if(ret){
      fprintf(stderr,"blockin failed: %d\n",ret);
      return 1;
    }
    produced++;
    /* the simulated sound card is slow: it pulls only one small
       chunk per submitted packet, so the backlog fills up and
       OV_EBACKPRESSURE above is exercised */
    if(vorbis_synthesis_pending(&vd)>=CARD_CHUNK){
      float **pcm;
      int n=vorbis_synthesis_pcmout(&vd,&pcm);
      if(n>CARD_CHUNK)n=CARD_CHUNK;
      if(n>0){
        /* 'pcm' now points at the oldest n unread frames; play them */
        consumed+=n;
        vorbis_synthesis_read(&vd,n);
      }
    }
  }

  /* drain the rest */
  for(;;){
    float **pcm;
    int n=vorbis_synthesis_pcmout(&vd,&pcm);
    if(n<=0)break;
    consumed+=n;
    vorbis_synthesis_read(&vd,n);
  }

  printf("decoded %d packets, %ld frames delivered, "
         "backpressure retries: %d, left pending: %ld\n",
         npkts-3,consumed,backpressure,vorbis_synthesis_pending(&vd));

  if(consumed!=FRAMES){
    fprintf(stderr,"frame count mismatch: got %ld, want %d\n",consumed,FRAMES);
    return 1;
  }

  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  for(i=0;i<npkts;i++)free(pkts[i].packet);
  free(pkts);
  (void)produced;
  return 0;
}
