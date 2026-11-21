#ifndef TASK_AUDIO_FIXTURE_H
#define TASK_AUDIO_FIXTURE_H
#include <string.h>
#include "dumb.h"
/* Original synthetic 4-channel MOD, two looped 8-bit waveforms. */
static void task_mod_event(unsigned char *p,int sample,int period,int effect,int value) {
 p[0]=(unsigned char)((sample&0xf0)|(period>>8));p[1]=(unsigned char)period;
 p[2]=(unsigned char)((sample<<4)|effect);p[3]=(unsigned char)value;
}
static DUH *task_load_mod(void) {
 unsigned char b[1084+1024+128];memset(b,0,sizeof(b));
 memcpy(b,"generated-loop-audio",20);
 for(int s=0;s<2;s++) {
  unsigned char *h=b+20+30*s;h[23]=32;h[25]=64;h[29]=32;
  for(int i=0;i<64;i++)b[1084+1024+s*64+i]=(unsigned char)((i*37+s*41)%255-127);
 }
 b[950]=1;memcpy(b+1080,"M.K.",4);
 task_mod_event(b+1084,1,428,0,0);
 task_mod_event(b+1088,2,321,0,0);
 task_mod_event(b+1084+4*16,1,404,4,0x47);
 task_mod_event(b+1084+8*16,2,360,0xA,0x03);
 DUMBFILE *f=dumbfile_open_memory((const char*)b,sizeof(b));
 if(!f)return NULL;
 DUH *d=dumb_read_mod_quick(f,0);dumbfile_close(f);return d;
}
/* Original minimal IT: two looping samples, channel 0 random tremolo. */
static void task_it_u16(unsigned char *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void task_it_u32(unsigned char *p,unsigned v){for(int j=0;j<4;j++)p[j]=v>>(8*j);}
static DUH *task_load_random_it(void) {
 unsigned char b[1024];memset(b,0,sizeof(b));
 memcpy(b,"IMPM",4);memcpy(b+4,"random-tremolo-audio",20);
 task_it_u16(b+32,2);task_it_u16(b+36,2);task_it_u16(b+38,1);
 task_it_u16(b+40,0x214);task_it_u16(b+42,0x214);task_it_u16(b+44,1);
 b[48]=128;b[49]=48;b[50]=6;b[51]=125;b[52]=128;
 for(int i=0;i<64;i++){b[64+i]=i<2?i*64:0x80;b[128+i]=64;}
 b[192]=0;b[193]=255;
 task_it_u32(b+194,206);task_it_u32(b+198,286);task_it_u32(b+202,366);
 unsigned char packed[128];unsigned n=0;
 unsigned char row0[]={0x81,0x0b,60,1,19,0x43,0x82,0x03,55,2,0};
 unsigned char row1[]={0x81,0x08,18,0x47,0};
 memcpy(packed+n,row0,sizeof(row0));n+=sizeof(row0);
 memcpy(packed+n,row1,sizeof(row1));n+=sizeof(row1);
 for(int i=2;i<64;i++)packed[n++]=0;
 task_it_u16(b+366,n);task_it_u16(b+368,64);memcpy(b+374,packed,n);
 unsigned data=374+n;
 for(int s=0;s<2;s++) {
  unsigned char *h=b+206+s*80;memcpy(h,"IMPS",4);h[17]=64;h[18]=17;h[19]=64;h[46]=1;
  task_it_u32(h+48,64);task_it_u32(h+56,64);task_it_u32(h+60,8363);
  task_it_u32(h+72,data+s*64);
  for(int i=0;i<64;i++)b[data+s*64+i]=(unsigned char)((i*37+s*41)%255-127);
 }
 DUMBFILE *f=dumbfile_open_memory((const char*)b,data+128);if(!f)return NULL;
 DUH *d=dumb_read_it_quick(f);dumbfile_close(f);return d;
}

#endif
