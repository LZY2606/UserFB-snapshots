/* Intra zero-AC syntax adapted from an earlier public baseline/start.c.
 * That scaffold and the upstream at c871f2be are only input material.
 * This writer adds dimensions, DC values, sequence ends and a PS envelope. */
#include "fixture.h"
#include <assert.h>
#include <string.h>
void fixture_bits(fixture_t *b,unsigned value,unsigned count) {
  assert(count<=32 && b->bit+count<=sizeof(b->bytes)*8);
  for(unsigned n=count;n;n--,b->bit++) {
    unsigned mask=1u<<(7-b->bit%8);
    b->bytes[b->bit/8]=(uint8_t)((b->bytes[b->bit/8]&~mask)|(((value>>(n-1))&1)?mask:0));
  }
}
void fixture_start(fixture_t *b,unsigned code) {
  while(b->bit%8) fixture_bits(b,0,1);
  fixture_bits(b,1,24); fixture_bits(b,code,8);
}
size_t fixture_size(const fixture_t *b) {return (b->bit+7)/8;}
void fixture_sequence(fixture_t *b,unsigned w,unsigned h,unsigned rate,unsigned aspect) {
  fixture_start(b,0xb3);fixture_bits(b,w,12);fixture_bits(b,h,12);
  fixture_bits(b,aspect,4);fixture_bits(b,rate,4);
  fixture_bits(b,1,18);fixture_bits(b,1,1);fixture_bits(b,20,10);
  fixture_bits(b,0,1);fixture_bits(b,0,1);fixture_bits(b,0,1);
  /* Valid user data gives the upstream conservative header look-ahead room. */
  fixture_start(b,0xb2);for(unsigned i=0;i<160;i++)fixture_bits(b,0x55,8);
}
static void dc(fixture_t *b,int delta,int luma) {
  static const unsigned code[]={4,0,1,5,6,14,30,62,126};
  static const unsigned len[]={3,2,2,3,3,4,5,6,7};
  unsigned mag=(unsigned)(delta<0?-delta:delta),n=0;
  for(unsigned v=mag;v;v>>=1)n++;
  assert(n<=8);
  if(luma)fixture_bits(b,code[n],len[n]);
  else { assert(n==0);fixture_bits(b,0,2); }
  if(n)fixture_bits(b,delta<0?((1u<<n)-1-mag):mag,n);
  fixture_bits(b,2,2); /* end_of_block */
}
void fixture_intra(fixture_t *b,unsigned w,unsigned h,unsigned tr,unsigned gray) {
  assert(gray<=255);
  fixture_start(b,0);fixture_bits(b,tr,10);fixture_bits(b,1,3);
  fixture_bits(b,65535,16);fixture_bits(b,0,1);
  for(unsigned row=0;row<(h+15)/16;row++) {
    int previous=128;
    fixture_start(b,row+1);fixture_bits(b,8,5);fixture_bits(b,0,1);
    for(unsigned col=0;col<(w+15)/16;col++) {
      fixture_bits(b,1,1);fixture_bits(b,1,1); /* address increment, intra MB */
      for(unsigned block=0;block<6;block++) {
        int delta=block==0?(int)gray-previous:0;
        dc(b,delta,block<4);if(block<4)previous=(int)gray;
      }
    }
  }
}
void fixture_end(fixture_t *b) {fixture_start(b,0xb7);fixture_bits(b,0,32);}
void fixture_ps(fixture_t *b,const fixture_t *es) {
  size_t n=fixture_size(es);assert(n+1<65536);
  fixture_start(b,0xba);fixture_bits(b,2,4);fixture_bits(b,0,3);fixture_bits(b,1,1);
  fixture_bits(b,0,15);fixture_bits(b,1,1);fixture_bits(b,0,15);fixture_bits(b,1,1);
  fixture_bits(b,1,1);fixture_bits(b,1000,22);fixture_bits(b,1,1);
  fixture_start(b,0xbb);fixture_bits(b,9,16);
  fixture_bits(b,1,1);fixture_bits(b,1000,22);fixture_bits(b,1,1);
  fixture_bits(b,0,6);fixture_bits(b,1,5);fixture_bits(b,1,5);
  fixture_bits(b,0xff,8);fixture_bits(b,0xe0,8);fixture_bits(b,3,2);fixture_bits(b,1,1);fixture_bits(b,512,13);
  fixture_start(b,0xe0);fixture_bits(b,(unsigned)n+1,16);fixture_bits(b,0x0f,8);
  for(size_t i=0;i<n;i++)fixture_bits(b,es->bytes[i],8);
  fixture_start(b,0xb9);fixture_bits(b,0,32);
}
