#include <vorbis/vorbisenc.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#define COUNT 24577
static int drain(vorbis_dsp_state *d,long *frames,double *energy){
 float **pcm;int n,i;
 while((n=vorbis_synthesis_pcmout(d,&pcm))>0){
  for(i=0;i<n;i++){if(!isfinite(pcm[0][i]))return 1;*energy+=fabs(pcm[0][i]);}
  *frames+=n;if(vorbis_synthesis_read(d,n))return 1;
 }
 return n<0;
}
int main(void){
 vorbis_info enc,info,probe_info;vorbis_comment ec,dc,pc;
 vorbis_dsp_state e,d,p;vorbis_block eb,db,pb;ogg_packet h,c,s,packet;
 long frames=0;double energy=0;int pos=0,packets=0,shorts=0,longs=0,transitions=0,prev=-1,rejected=0;
 vorbis_info_init(&enc);vorbis_info_init(&info);vorbis_info_init(&probe_info);
 vorbis_comment_init(&ec);vorbis_comment_init(&dc);vorbis_comment_init(&pc);
 if(vorbis_encode_init_vbr(&enc,1,44100,.35f)||vorbis_analysis_init(&e,&enc)||vorbis_block_init(&e,&eb))return 2;
 if(vorbis_analysis_headerout(&e,&ec,&h,&c,&s))return 3;
 if(vorbis_synthesis_headerin(&info,&dc,&h)||vorbis_synthesis_headerin(&info,&dc,&c)||vorbis_synthesis_headerin(&info,&dc,&s))return 4;
 if(vorbis_synthesis_headerin(&probe_info,&pc,&h)||vorbis_synthesis_headerin(&probe_info,&pc,&c)||vorbis_synthesis_headerin(&probe_info,&pc,&s))return 5;
 if(vorbis_synthesis_init(&d,&info)||vorbis_block_init(&d,&db)||vorbis_synthesis_init(&p,&probe_info)||vorbis_block_init(&p,&pb))return 6;
 do{
  int n=COUNT-pos;if(n>997)n=997;
  if(n){float **input=vorbis_analysis_buffer(&e,n);int i;
   for(i=0;i<n;i++){int k=pos+i;input[0][i]=(float)(.12*sin(k*.039)+.05*cos(k*.019));if(k%3072<4)input[0][i]+=(k%2?.7f:-.7f);}
   pos+=n;
  }
  if(vorbis_analysis_wrote(&e,n))return 7;
  while(vorbis_analysis_blockout(&e,&eb)==1){
   if(vorbis_analysis(&eb,NULL)||vorbis_bitrate_addblock(&eb))return 8;
   while(vorbis_bitrate_flushpacket(&e,&packet)){
    int size=vorbis_packet_blocksize(&info,&packet);packets++;
    if(size==vorbis_info_blocksize(&info,0))shorts++;else if(size==vorbis_info_blocksize(&info,1))longs++;else return 9;
    if(prev!=-1&&prev!=size)transitions++;prev=size;
    if(!rejected){
     int before=vorbis_synthesis_pcmout(&p,NULL),r;
     if(vorbis_synthesis(&pb,&packet))return 10;
     r=vorbis_synthesis_blockin(&p,&pb);
     if(before>0){if(r!=OV_EINVAL)return 11;rejected=1;printf("VORBIS_UNREAD pending=%d blockin=%d packet=%d\n",before,r,packets);}
     else if(r)return 12;
    }
    if(vorbis_synthesis(&db,&packet)||vorbis_synthesis_blockin(&d,&db)||drain(&d,&frames,&energy))return 13;
   }
  }
  if(!n)break;
 }while(1);
 if(frames!=COUNT||energy<1||!shorts||!longs||!transitions||!rejected)return 14;
 printf("VORBIS_BASELINE_OK packets=%d frames=%ld energy=%.6f short=%d long=%d transitions=%d\n",packets,frames,energy,shorts,longs,transitions);
 vorbis_block_clear(&pb);vorbis_block_clear(&db);vorbis_block_clear(&eb);
 vorbis_dsp_clear(&p);vorbis_dsp_clear(&d);vorbis_dsp_clear(&e);
 vorbis_info_clear(&probe_info);vorbis_info_clear(&info);vorbis_info_clear(&enc);
 vorbis_comment_clear(&pc);vorbis_comment_clear(&dc);vorbis_comment_clear(&ec);return 0;
}
