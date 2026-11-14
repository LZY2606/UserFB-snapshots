/* Deterministic normal-function fixture: existing single-program public API only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libmpegts.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"BASELINE_FAIL line=%d %s\n",__LINE__,#x);exit(1);}}while(0)
static uint8_t *trace; static int64_t *ticks; static size_t length,packets;
static uint32_t crc32mpeg(const uint8_t *s,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;i++){c^=(uint32_t)s[i]<<24;for(int j=0;j<8;j++)c=(c<<1)^((c&0x80000000u)?0x04c11db7u:0);}return c;}
static uint64_t stamp(const uint8_t *s){return ((uint64_t)(s[0]&14)<<29)|((uint64_t)s[1]<<22)|((uint64_t)(s[2]&254)<<14)|((uint64_t)s[3]<<7)|(s[4]>>1);}
static void collect(ts_writer_t *w,ts_frame_t *f,int n){uint8_t *out=NULL;int len=0;int64_t *clock=NULL;CHECK(ts_write_frames(w,f,n,&out,&len,&clock)==0);CHECK(len>=0&&len%188==0);if(!len)return;trace=realloc(trace,length+len);ticks=realloc(ticks,(packets+len/188)*sizeof(*ticks));CHECK(trace&&ticks&&clock);memcpy(trace+length,out,len);memcpy(ticks+packets,clock,(size_t)len/188*sizeof(*ticks));length+=len;packets+=len/188;}
int main(void){
 ts_stream_t s={0};s.pid=0x100;s.stream_format=LIBMPEGTS_VIDEO_MPEG2;s.stream_id=LIBMPEGTS_STREAM_ID_MPEGVIDEO;
 ts_program_t p={0};p.pmt_pid=0x1000;p.program_num=7;p.pcr_pid=0x110;p.num_streams=1;p.streams=&s;
 ts_main_t m={0};m.num_programs=1;m.programs=&p;m.ts_id=23;m.muxrate=2000000;m.cbr=1;m.lowlatency=1;m.ts_type=TS_TYPE_GENERIC;m.pcr_period=20;m.pat_period=80;
 ts_writer_t *w=ts_create_writer();CHECK(w);CHECK(ts_setup_transport_stream(w,&m)==0);CHECK(ts_setup_mpegvideo_stream(w,s.pid,LIBMPEGTS_MPEG2_LEVEL_MAIN,LIBMPEGTS_MPEG2_PROFILE_MAIN,1000000,100000,3)==0);
 uint8_t body[640];
 for(int i=0;i<8;i++){for(size_t j=0;j<sizeof(body);j++)body[j]=(uint8_t)(j*17+i*31);ts_frame_t f={0};f.pid=s.pid;f.data=body;f.size=sizeof(body);f.dts=9000+i*3600;f.pts=f.dts;f.cpb_initial_arrival_time=(int64_t)(i*3600+900)*300;f.cpb_final_arrival_time=(int64_t)(i*3600+3600)*300;f.random_access=i==0;collect(w,&f,1);memset(body,0xee,sizeof(body));}
 collect(w,NULL,0);CHECK(ts_close_writer(w)==0);CHECK(packets>100);
 int pat=0,pmt=0,pcrs=0,pes=0,cc[8192],have[8192]={0};uint8_t payload[4096];size_t used=0;uint64_t fingerprint=14695981039346656037ULL;
 for(size_t k=0;k<packets;k++){
  const uint8_t *b=trace+k*188;CHECK(b[0]==0x47);int pid=((b[1]&31)<<8)|b[2],af=(b[3]>>4)&3,pos=4;CHECK(af);
  if(af&1){if(have[pid]&&pid!=0x1fff)CHECK((b[3]&15)==((cc[pid]+1)&15));cc[pid]=b[3]&15;have[pid]=1;}
  if(af&2){CHECK(b[4]<=183);pos=5+b[4];if(b[4]>=7&&(b[5]&16)){
    CHECK(pid==0x110);uint64_t base=((uint64_t)b[6]<<25)|((uint64_t)b[7]<<17)|((uint64_t)b[8]<<9)|((uint64_t)b[9]<<1)|(b[10]>>7),ext=((b[10]&1)<<8)|b[11];CHECK(ext<300);uint64_t v=base*300+ext;int64_t d=ticks[k]-(int64_t)v;CHECK(llabs(d-19224)<=1);pcrs++;
  }}
  CHECK(ticks[k]==270000000LL+((int64_t)(k+1)*188*8*27000000+1000000)/2000000);
  if(!(af&1)||pos==188)continue;
  if(pid==0||pid==0x1000){CHECK(b[1]&64);int start=pos+1+b[pos],n=3+(((b[start+1]&15)<<8)|b[start+2]);CHECK(start+n<=188);CHECK(crc32mpeg(b+start,n)==0);if(pid==0){CHECK(b[start]==0);CHECK(b[start+8]==0&&b[start+9]==7);pat++;}else{CHECK(b[start]==2);CHECK((((b[start+8]&31)<<8)|b[start+9])==0x110);pmt++;}}
  if(pid==0x100){if(b[1]&64){
    if(pes){CHECK(used==sizeof(body));for(size_t j=0;j<used;j++)CHECK(payload[j]==(uint8_t)(j*17+(pes-1)*31));}
    CHECK(b[pos]==0&&b[pos+1]==0&&b[pos+2]==1);CHECK(stamp(b+pos+9)==(uint64_t)(900000+9000+pes*3600));pes++;used=0;pos+=9+b[pos+8];
  }CHECK(used+188-pos<=sizeof(payload));memcpy(payload+used,b+pos,188-pos);used+=188-pos;}
 }
 CHECK(pes==8&&used==sizeof(body));for(size_t j=0;j<used;j++)CHECK(payload[j]==(uint8_t)(j*17+7*31));CHECK(pat>=2&&pmt==pat&&pcrs>=8);
 for(size_t i=0;i<length;i++){fingerprint^=trace[i];fingerprint*=1099511628211ULL;}
 CHECK(fingerprint==0x1a31e3a3681f738eULL && length==80088);
 printf("SINGLE_PROGRAM_OK packets=%zu pat=%d pmt=%d pcr=%d pes=%d bytes=%zu fnv=%016llx\n",packets,pat,pmt,pcrs,pes,length,(unsigned long long)fingerprint);
 FILE *file=fopen(".task-build/single.ts","wb");CHECK(file);CHECK(fwrite(trace,1,length,file)==length);CHECK(fclose(file)==0);free(trace);free(ticks);return 0;
}
