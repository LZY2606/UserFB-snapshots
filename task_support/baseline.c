/* Deterministic ordinary MPEG-1 Layer III fixture. No ADU implementation. */
#include "fixed.h"
#include "bit.h"
#include "stream.h"
#include "frame.h"
#include "synth.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define COUNT 6
#define FRAMESIZE 417
#define PREFIX 36
#define SLOT (FRAMESIZE-PREFIX)
#define CHECK(x) do { if(!(x)){fprintf(stderr,"baseline failure line=%d: %s\n",__LINE__,#x);return 1;} }while(0)
static void bits(unsigned char *p,unsigned *at,unsigned n,unsigned value) {
 while(n--) { unsigned char mask=(unsigned char)(1u<<(7-(*at%8)));
  if((value>>n)&1u) p[*at/8]|=mask; else p[*at/8]&=(unsigned char)~mask;
  ++*at;
 }
}
static void fixture(unsigned char *mp3) {
 static const unsigned back[COUNT]={0,150,300,100,280,200};
 unsigned char tape[COUNT*SLOT]; unsigned f,j;
 for(j=0;j<sizeof tape;j++) tape[j]=(unsigned char)(j*37u+19u);
 for(f=0;f<COUNT;f++) {
  unsigned char *p=mp3+f*FRAMESIZE; unsigned at=0,gr,ch,start=f*SLOT-back[f],audio_at=0;
  memset(p,0,FRAMESIZE); p[0]=0xff;p[1]=0xfb;p[2]=0x90;p[3]=0;
  bits(p+4,&at,9,back[f]);bits(p+4,&at,3,0);bits(p+4,&at,8,0);
  for(gr=0;gr<2;gr++) for(ch=0;ch<2;ch++) {
   bits(p+4,&at,12,3);bits(p+4,&at,9,1);bits(p+4,&at,8,ch?224:230);
   bits(p+4,&at,4,0);bits(p+4,&at,1,0);
   bits(p+4,&at,5,1);bits(p+4,&at,5,0);bits(p+4,&at,5,0);
   bits(p+4,&at,4,0);bits(p+4,&at,3,0);bits(p+4,&at,3,0);
   bits(tape+start,&audio_at,2,1);bits(tape+start,&audio_at,1,(f+gr+ch)&1u);
  }
  if(at!=256) abort();
 }
 for(f=0;f<COUNT;f++) memcpy(mp3+f*FRAMESIZE+PREFIX,tape+f*SLOT,SLOT);
}
static int pcm16(FILE *out,mad_fixed_t value) {
 int64_t v=(int64_t)value+(1L<<(MAD_F_FRACBITS-16));
 if(v>=MAD_F_ONE) v=MAD_F_ONE-1;
 if(v< -MAD_F_ONE) v= -MAD_F_ONE;
 v>>=(MAD_F_FRACBITS+1-16);
 return fputc((int)(v&255),out)!=EOF && fputc((int)((v>>8)&255),out)!=EOF;
}
int main(void) {
 unsigned char mp3[COUNT*FRAMESIZE+MAD_BUFFER_GUARD];
 struct mad_stream stream; struct mad_frame frame; struct mad_synth synth;
 unsigned frames=0,samples=0,nonzero=0,asymmetric=0,i,ch; uint64_t hash=UINT64_C(1469598103934665603);
 FILE *encoded,*reference;
 memset(mp3,0,sizeof mp3); fixture(mp3);
 encoded=fopen(".task-build/production.mp3","wb");CHECK(encoded);
 CHECK(fwrite(mp3,1,COUNT*FRAMESIZE,encoded)==COUNT*FRAMESIZE);CHECK(fclose(encoded)==0);
 reference=fopen(".task-build/reference.pcm","wb");CHECK(reference);
 mad_stream_init(&stream);mad_frame_init(&frame);mad_synth_init(&synth);
 mad_stream_buffer(&stream,mp3,sizeof mp3);
 while(frames<COUNT) {
  if(mad_frame_decode(&frame,&stream)!=0) {
   fprintf(stderr,"frame=%u mad_error=%s\n",frames,mad_stream_errorstr(&stream)); return 1;
  }
  CHECK(frame.header.layer==MAD_LAYER_III && frame.header.samplerate==44100);
  mad_synth_frame(&synth,&frame);
  CHECK(synth.pcm.channels==2 && synth.pcm.length==1152);
  for(i=0;i<synth.pcm.length;i++) {
   if(synth.pcm.samples[0][i]!=synth.pcm.samples[1][i]) ++asymmetric;
   for(ch=0;ch<2;ch++) {
    int32_t v=synth.pcm.samples[ch][i]; if(v)++nonzero;
    hash^=(uint32_t)v;hash*=UINT64_C(1099511628211);
    CHECK(pcm16(reference,v));
   }
  }
  samples+=synth.pcm.length;++frames;
 }
 CHECK(fclose(reference)==0);
 CHECK(nonzero>0 && asymmetric>0 && samples==6912);
 mad_frame_finish(&frame);mad_stream_finish(&stream);
 printf("MAD_MP3_BASELINE_OK frames=%u samples=%u reservoir_frames=5 nonzero=%u asymmetric=%u hash=%016llx\n",frames,samples,nonzero,asymmetric,(unsigned long long)hash);
 return 0;
}
