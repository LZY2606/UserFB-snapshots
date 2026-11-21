#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef struct { unsigned char bytes[8192]; size_t bits; } task_bits;
static void ts_put(task_bits *b, unsigned v, unsigned n) {
  while(n--) { size_t k=b->bits++; assert(k<sizeof(b->bytes)*8);
    b->bytes[k>>3] |= ((v>>n)&1u) << (7-(k&7)); }
}
static void ts_ue(task_bits *b,unsigned v) {
  unsigned n=0,x=v+1,t=x; while(t>>=1)n++;
  ts_put(b,0,n);ts_put(b,x,n+1);
}
static void ts_se(task_bits *b,int v) {ts_ue(b,v>0?(unsigned)(2*v-1):(unsigned)(-2*v));}
static void ts_align(task_bits *b) {while(b->bits&7)ts_put(b,0,1);}
static void ts_tail(task_bits *b) {ts_put(b,1,1);ts_align(b);}
static size_t ts_nal(task_bits *b,unsigned header,unsigned char *out) {
  size_t size=(b->bits+7)/8,w=0;unsigned zeros=0;out[w++]=(unsigned char)header;
  for(size_t i=0;i<size;i++) {unsigned c=b->bytes[i];
    if(zeros==2&&c<=3){out[w++]=3;zeros=0;}
    out[w++]=(unsigned char)c; zeros=c==0?zeros+1:0;
  }return w;
}
static size_t ts_sps(unsigned char *out) {
  task_bits b={{0},0};
  ts_put(&b,66,8);ts_put(&b,0,8);ts_put(&b,10,8);ts_ue(&b,0);
  ts_ue(&b,0);ts_ue(&b,2);ts_ue(&b,1);ts_put(&b,0,1);
  ts_ue(&b,0);ts_ue(&b,0);ts_put(&b,1,1);ts_put(&b,1,1);
  ts_put(&b,0,1);ts_put(&b,0,1);ts_tail(&b);return ts_nal(&b,0x67,out);
}
static size_t ts_pps(unsigned char *out) {
  task_bits b={{0},0};
  ts_ue(&b,0);ts_ue(&b,0);ts_put(&b,0,1);ts_put(&b,0,1);ts_ue(&b,0);
  ts_ue(&b,0);ts_ue(&b,0);ts_put(&b,0,1);ts_put(&b,0,2);
  ts_se(&b,0);ts_se(&b,0);ts_se(&b,0);ts_put(&b,1,1);
  ts_put(&b,0,1);ts_put(&b,0,1);ts_tail(&b);return ts_nal(&b,0x68,out);
}
static size_t ts_picture(unsigned char *out,unsigned frame_num) {
  task_bits b={{0},0};unsigned idr=frame_num==0;
  ts_ue(&b,0);ts_ue(&b,idr?2:0);ts_ue(&b,0);ts_put(&b,frame_num,4);
  if(idr) {ts_ue(&b,0);ts_put(&b,0,1);ts_put(&b,0,1);}
  else {ts_put(&b,0,1);ts_put(&b,0,1);ts_put(&b,0,1);}
  ts_se(&b,0);ts_ue(&b,1);
  if(idr){ts_ue(&b,25);ts_align(&b);for(unsigned i=0;i<384;i++)ts_put(&b,128,8);}
  else ts_ue(&b,1); // one P-skip macroblock
  ts_tail(&b);return ts_nal(&b,idr?0x65:0x41,out);
}
static size_t ts_recovery(unsigned char *out,unsigned count) {
  task_bits p={{0},0},b={{0},0};
  ts_ue(&p,count);ts_put(&p,1,1);ts_put(&p,0,1);ts_put(&p,0,2);
  // SEI payload extension bits, then RBSP trailing bits.
  if(p.bits&7)ts_tail(&p);
  ts_put(&b,6,8);ts_put(&b,(unsigned)(p.bits/8),8);
  for(size_t i=0;i<p.bits/8;i++)ts_put(&b,p.bytes[i],8);
  ts_tail(&b);return ts_nal(&b,6,out);
}
#endif
