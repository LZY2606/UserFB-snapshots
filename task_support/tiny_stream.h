/* Deterministic flat floor smoke stream; no interior control points.
 * One codebook, one floor1, residue1, mono, no coupling, 256-sample blocks.
 * Packet buffers are bounded here by these fixed constants. */
#ifndef TINY_STREAM_H
#define TINY_STREAM_H
#include <stdint.h>
#include <string.h>
struct tiny_packet { unsigned char b[255]; unsigned pos; };
static void tb(struct tiny_packet*p,unsigned v,unsigned n){for(unsigned i=0;i<n;i++,p->pos++){if(v&(1u<<i))p->b[p->pos/8]|=1u<<(p->pos%8);}}
static void magic(struct tiny_packet*p,unsigned type){tb(p,type,8);const char*s="vorbis";for(int i=0;i<6;i++)tb(p,s[i],8);}
static unsigned page(unsigned char*out,const struct tiny_packet*p,unsigned seq,unsigned flags,unsigned granule){unsigned n=(p->pos+7)/8;memset(out,0,n+28);memcpy(out,"OggS",4);out[5]=flags;for(unsigned i=0;i<4;i++){out[6+i]=granule>>(8*i);out[14+i]=7u>>(8*i);out[18+i]=seq>>(8*i);}out[26]=1;out[27]=n;memcpy(out+28,p->b,n);uint32_t crc=0;for(unsigned i=0;i<n+28;i++){crc^=(uint32_t)out[i]<<24;for(int k=0;k<8;k++)crc=(crc&0x80000000u)?(crc<<1)^0x04c11db7u:crc<<1;}for(int k=0;k<4;k++)out[22+k]=crc>>(8*k);return n+28;}
static unsigned tiny_stream(unsigned char*out){struct tiny_packet p={{0},0};unsigned at=0;magic(&p,1);tb(&p,0,32);tb(&p,1,8);tb(&p,8000,32);tb(&p,0,32);tb(&p,0,32);tb(&p,0,32);tb(&p,0x88,8);tb(&p,1,8);at+=page(out+at,&p,0,2,0);
memset(&p,0,sizeof p);magic(&p,3);tb(&p,0,32);tb(&p,0,32);tb(&p,1,8);at+=page(out+at,&p,1,0,0);
memset(&p,0,sizeof p);magic(&p,5);tb(&p,0,8);tb(&p,0x564342,24);tb(&p,1,16);tb(&p,1,24);tb(&p,0,1);tb(&p,0,1);tb(&p,0,5);tb(&p,1,4);tb(&p,0,32);tb(&p,0x60100000,32);tb(&p,0,4);tb(&p,0,1);tb(&p,1,1);
tb(&p,0,6);tb(&p,0,16);tb(&p,0,6);tb(&p,1,16);tb(&p,0,5);tb(&p,0,2);tb(&p,6,4);
tb(&p,0,6);tb(&p,1,16);tb(&p,0,24);tb(&p,32,24);tb(&p,0,24);tb(&p,0,6);tb(&p,0,8);tb(&p,1,3);tb(&p,0,1);tb(&p,0,8);
tb(&p,0,6);tb(&p,0,16);tb(&p,0,1);tb(&p,0,1);tb(&p,0,2);tb(&p,0,8);tb(&p,0,8);tb(&p,0,8);
tb(&p,0,6);tb(&p,0,1);tb(&p,0,16);tb(&p,0,16);tb(&p,0,8);tb(&p,1,1);at+=page(out+at,&p,2,0,0);
for(unsigned j=0;j<3;j++){memset(&p,0,sizeof p);tb(&p,0,1);tb(&p,1,1);tb(&p,200,8);tb(&p,200,8);for(int k=0;k<64;k++)tb(&p,0,1);at+=page(out+at,&p,3+j,j==2?4:0,j*128);}return at;}
#endif
