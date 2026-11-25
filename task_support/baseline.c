#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "voAAC.h"
#include "cmnMemory.h"
#include "aacenc_core.h"
static void need(int yes, const char *s) {if(!yes){fprintf(stderr,"BASELINE_FAIL %s\n",s);exit(2);}}
int main(void) {
 VO_AUDIO_CODECAPI api={0}; VO_HANDLE h=0; VO_MEM_OPERATOR mem={0};
 VO_CODEC_INIT_USERDATA u={0}; AACENC_PARAM p={48000,96000,2,1};
 int16_t pcm[2048]; uint8_t bytes[8192]; uint32_t rng=91, hash=2166136261u;
 unsigned frames=0, total=0, windows[4]={0}; FILE *f=fopen(".task-build/baseline.aac","wb");
 need(f!=0,"file"); mem.Alloc=cmnMemAlloc;mem.Copy=cmnMemCopy;mem.Free=cmnMemFree;mem.Set=cmnMemSet;mem.Check=cmnMemCheck;
 u.memflag=VO_IMF_USERMEMOPERATOR;u.memData=&mem;
 need(voGetAACEncAPI(&api)==0,"api");need(api.Init(&h,VO_AUDIO_CodingAAC,&u)==0,"init");
 need(api.SetParam(h,VO_PID_AAC_ENCPARAM,&p)==0,"params");
 for(unsigned k=0;k<24;k++) {
  for(unsigned i=0;i<2048;i++) {rng=rng*1664525u+1013904223u; pcm[i]=(int16_t)((int32_t)(rng>>17)-16384); if(k%6<3)pcm[i]/=32;}
  VO_CODECBUFFER in={0},out={0};VO_AUDIO_OUTPUTINFO info={0};
  in.Buffer=(VO_PBYTE)pcm;in.Length=sizeof(pcm);out.Buffer=bytes;out.Length=sizeof(bytes);
  need(api.SetInputData(h,&in)==0,"input");need(api.GetOutputData(h,&out,&info)==0,"encode");
  need(out.Length>7 && out.Length<=sizeof(bytes),"length");
  unsigned len=((unsigned)(bytes[3]&3)<<11)|((unsigned)bytes[4]<<3)|(bytes[5]>>5);
  need(bytes[0]==255 && (bytes[1]&246)==240 && len==out.Length,"adts");
  need(((bytes[2]>>2)&15)==3,"48khz");need(info.InputUsed==sizeof(pcm),"consumption");
  int w=((AAC_ENCODER*)h)->psyOut.psyOutChannel[0].windowSequence;
  need(w>=0 && w<4,"window");windows[w]++;
  for(unsigned j=7;j<len;j++)hash=(hash^bytes[j])*16777619u;
  need(fwrite(bytes,1,len,f)==len,"write");frames++;total+=len;
 }
 need(fclose(f)==0,"close");need(api.Uninit(h)==0,"uninit");
 need(frames==24 && total>200 && hash!=2166136261u,"audio");
 printf("AAC_BASELINE frames=%u bytes=%u payload_fnv=%08x windows=%u,%u,%u,%u\n",frames,total,hash,windows[0],windows[1],windows[2],windows[3]);
 return 0;
}
