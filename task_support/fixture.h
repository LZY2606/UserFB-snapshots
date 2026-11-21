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

/*
 * Richer IT generator shared by the seek tests.
 *
 *  - random_volume / random_pan are written into a new-format instrument so
 *    the per-note random volume/pan path is exercised.
 *  - waveform_effect is 3 (S3x vibrato), 4 (S4x tremolo) or 5 (S5x panbrello)
 *    and waveform 3 selects the random waveform.  Those effects are re-issued
 *    on later rows so the random waveform keeps changing.
 *  - use_nna retriggers the note on a channel so the previous voice is pushed
 *    into the NNA pool (each voice keeps its own smooth-effect state).
 *
 * The layout follows the same minimal IT that task_load_random_it() builds.
 */
static DUH *task_load_it_full(int random_volume, int random_pan,
                              int waveform_effect, int waveform, int use_nna) {
 unsigned char b[4096];memset(b,0,sizeof(b));
 memcpy(b,"IMPM",4);memcpy(b+4,"deterministic-seek-fixture",20);
 task_it_u16(b+32,1);            /* n_orders */
 task_it_u16(b+34,1);            /* n_instruments */
 task_it_u16(b+36,2);            /* n_samples */
 task_it_u16(b+38,1);            /* n_patterns */
 task_it_u16(b+40,0x214);task_it_u16(b+42,0x214);task_it_u16(b+44,0x0d);
 b[48]=128;b[49]=48;b[50]=6;b[51]=125;b[52]=128;
 for(int i=0;i<64;i++){b[64+i]=(unsigned char)(i==0?32:(i==1?96:128));b[128+i]=64;}
 b[192]=0;                       /* order 0: pattern 0 */

 /* Component pointer table follows the order list. */
 unsigned ptr_off=193;
 unsigned int_off=ptr_off+4+8+4; /* 1 instrument ptr + 2 sample ptrs + 1 pattern ptr */
 unsigned smp_off=int_off+554;
 unsigned pat_off=smp_off+2*80;

 /* Build packed pattern data first so we can size the pattern. */
 unsigned char packed[512];unsigned n=0;
 /* Packed row format: a channel byte with bit 7 set is followed by a mask
  * byte (1=note,2=instrument,4=volpan,8=effect). */
 unsigned char r0i0[]={0x81,0x03,60,1};            /* ch0 note+instrument */
 unsigned char r0i1[]={0x82,0x03,67,1};            /* ch1 note+instrument */
 memcpy(packed+n,r0i0,sizeof(r0i0));n+=sizeof(r0i0);
 memcpy(packed+n,r0i1,sizeof(r0i1));n+=sizeof(r0i1);
 if(waveform_effect>=3&&waveform_effect<=5){
  unsigned char fx[4]={0x81,0x08,(unsigned char)waveform_effect,
                       (unsigned char)waveform};
  memcpy(packed+n,fx,4);n+=4;
 }
 packed[n++]=0;
 /* Row 1: re-issue the waveform effect and optionally retrigger for NNA. */
 if(use_nna){
  unsigned char r1[]={0x82,0x01,61};                /* ch1 note-only retrigger */
  memcpy(packed+n,r1,sizeof(r1));n+=sizeof(r1);
 }
 if(waveform_effect>=3&&waveform_effect<=5){
  unsigned char fx[4]={0x81,0x08,(unsigned char)waveform_effect,
                       (unsigned char)waveform};
  memcpy(packed+n,fx,4);n+=4;
 }
 packed[n++]=0;
 for(int i=2;i<64;i++)packed[n++]=0;

 unsigned dat_off=pat_off+8+n;
 task_it_u32(b+ptr_off,int_off);
 task_it_u32(b+ptr_off+4,smp_off);
 task_it_u32(b+ptr_off+8,smp_off+80);
 task_it_u32(b+ptr_off+12,pat_off);

 /* New-format instrument. */
 {
  unsigned char *p=b+int_off;
  memcpy(p,"IMPI",4);p+=4;
  memset(p,0,13);p+=13;                    /* filename */
  *p++=0xFF;                               /* new note action: keep default */
  *p++=0;                                  /* dup check type */
  *p++=0;                                  /* dup check action */
  task_it_u16(p,use_nna?512:0);p+=2;       /* fadeout */
  *p++=32;                                 /* pp separation */
  *p++=6;                                  /* pp centre */
  *p++=128;                                /* global volume */
  *p++=128;                                /* default pan */
  *p++=(unsigned char)random_volume;
  *p++=(unsigned char)random_pan;
  task_it_u16(p,0x214);p+=2;               /* tracker version */
  task_it_u16(p,0);p+=2;                   /* number of samples */
  memset(p,0,26);p+=26;                    /* name */
  *p++=0;*p++=0;                           /* filter cutoff / resonance */
  memset(p,0,4);p+=4;                      /* midi channel/program/bank */

  /* The reader consumes 4+13+3+2+7+4+26+2+4 = 64 bytes before the note map. */
  for(int i=0;i<120;i++){*p++=(unsigned char)i;*p++=(unsigned char)(i>=119?1:(i==60?1:2));}
  /* Three envelopes.  Enable a volume envelope with a sustain loop when
   * fadeout is nonzero, so NNA voices and envelope ticks are exercised.
   * Envelope layout: flags,nodes,loop_start,loop_end,sus_loop_start,
   * sus_loop_end, then 25 (y,t) pairs, padded so each is 82 bytes. */
  for(int e=0;e<3;e++){
   if(e==0&&use_nna){
    *p++=0x01|0x04;                        /* ON | SUSTAIN_LOOP */
    *p++=2;                                /* two nodes */
    *p++=0;*p++=1;                         /* loop start/end */
    *p++=0;*p++=1;                         /* sustain loop start/end */
    *p++=64;task_it_u16(p,0);p+=2;         /* node 0: y=64, t=0 */
    *p++=32;task_it_u16(p,256);p+=2;       /* node 1: y=32, t=256 */
    memset(p,0,82-6-50);p+=82-6-50;
   } else {
    *p++=0;*p++=0;*p++=0;*p++=0;*p++=0;*p++=0;
    memset(p,0,76);p+=76;
   }
  }
 }
 /* Samples. */
 for(int s=0;s<2;s++) {
  unsigned char *h=b+smp_off+s*80;
  memcpy(h,"IMPS",4);
  h[17]=64;                                /* sample global volume */
  h[18]=0x11;                              /* EXISTS|LOOP */
  h[19]=64;                                /* default volume */
  h[46]=1;                                 /* convert: uncompressed */
  h[47]=32;                                /* default pan (0..64) */
  task_it_u32(h+48,64);task_it_u32(h+56,64);task_it_u32(h+60,8363);
  /* h+64..h+79: suspend loops + sample pointer; the reader ignores the
   * pointer and reads data where the header leaves off, so keep it zero. */
  for(int i=0;i<64;i++)b[dat_off+s*64+i]=(unsigned char)((i*37+s*41)%255-127);
 }
 task_it_u16(b+pat_off,(unsigned short)n);task_it_u16(b+pat_off+2,64);
 memcpy(b+pat_off+8,packed,n);
 DUMBFILE *f=dumbfile_open_memory((const char*)b,dat_off+128);
 if(!f)return NULL;
 DUH *d=dumb_read_it_quick(f);dumbfile_close(f);return d;
}

/*
 * Minimal IT whose only sample is a 16-bit mono ping-pong loop that jumps
 * between two note regions, so sample auto-vibrato (random waveform) and
 * backwards resampling are exercised.  Panning alternates left/right.
 */
static DUH *task_load_it_pingpong(void) {
 unsigned char b[2048];memset(b,0,sizeof(b));
 memcpy(b,"IMPM",4);memcpy(b+4,"pingpong-vibrato-fixture",20);
 task_it_u16(b+32,1);task_it_u16(b+34,0);task_it_u16(b+36,1);
 task_it_u16(b+38,1);task_it_u16(b+40,0x214);task_it_u16(b+42,0x214);
 task_it_u16(b+44,0x09);          /* STEREO|LINEAR (no instruments) */
 b[48]=128;b[49]=48;b[50]=6;b[51]=125;b[52]=128;
 for(int i=0;i<64;i++){b[64+i]=(unsigned char)(i==0?24:(i==1?104:128));b[128+i]=64;}
 b[192]=0;
 unsigned ptr_off=193;
 unsigned smp_off=ptr_off+4+4;     /* 1 sample ptr + 1 pattern ptr */
 unsigned pat_off=smp_off+80;
 unsigned char packed[128];unsigned n=0;
 unsigned char r0[]={0x81,0x03,60,1};       /* note 60, sample 1 */
 memcpy(packed+n,r0,sizeof(r0));n+=sizeof(r0);
 packed[n++]=0;
 for(int i=1;i<64;i++)packed[n++]=0;
 unsigned dat_off=pat_off+8+n;
 task_it_u32(b+ptr_off,smp_off);
 task_it_u32(b+ptr_off+4,pat_off);
 {
  unsigned char *h=b+smp_off;
  memcpy(h,"IMPS",4);
  h[17]=64; h[18]=0x51;                     /* EXISTS | LOOP | PINGPONG */
  h[19]=64; h[46]=1; h[47]=32;              /* uncompressed, signed LE */
  task_it_u32(h+48,128);                    /* length, in sample points */
  task_it_u32(h+52,16);                     /* loop start */
  task_it_u32(h+56,112);                    /* loop end */
  task_it_u32(h+60,8363);                   /* C5 speed */
  task_it_u32(h+72,dat_off);                /* sample data offset */
  h[76]=8; h[77]=2;                         /* auto-vibrato speed/depth */
  h[78]=8; h[79]=3;                         /* rate, random waveform */
  for(int i=0;i<128;i++){
   short v=(short)(((i*37+11)%255-127)*256);
   b[dat_off+i*2+0]=(unsigned char)(v&0xFF);
   b[dat_off+i*2+1]=(unsigned char)((v>>8)&0xFF);
  }
 }
 task_it_u16(b+pat_off,(unsigned short)n);task_it_u16(b+pat_off+2,64);
 memcpy(b+pat_off+8,packed,n);
 DUMBFILE *f=dumbfile_open_memory((const char*)b,dat_off+128*2);
 if(!f)return NULL;
 DUH *d=dumb_read_it_quick(f);dumbfile_close(f);return d;
}

#endif
