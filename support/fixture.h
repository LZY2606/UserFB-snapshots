#ifndef MAD_FIXTURE_H
#define MAD_FIXTURE_H
#include <string.h>
/* All frames are mono MPEG-2.5 Layer III, 16 kb/s at 8 kHz, no reservoir.
   Only one table-1 Huffman pair at spectral coordinates (40,41) is nonzero.
   Switching sequence is normal, start, short/mixed, short/mixed, stop, normal. */
static void putbits(unsigned char *p,unsigned *bit,unsigned value,unsigned n){
 while(n--){p[*bit/8]|=((value>>n)&1)<<(7-*bit%8);++*bit;}
}
static unsigned make_stream(unsigned char *p,int mixed){
 memset(p,0,6*144+8);
 for(unsigned f=0;f<6;f++){
  unsigned char *d=p+f*144;d[0]=255;d[1]=227;d[2]=40;d[3]=192;
  unsigned bit=0;unsigned block=mixed?(f==1?1:(f==2||f==3?2:(f==4?3:0))):0;
  unsigned active=(f==2||f==3);unsigned part=active?23:0;
  putbits(d+4,&bit,0,8);putbits(d+4,&bit,0,1);
  putbits(d+4,&bit,part,12);putbits(d+4,&bit,active?21:0,9);
  putbits(d+4,&bit,190,8);putbits(d+4,&bit,0,9);
  putbits(d+4,&bit,block!=0,1);
  if(block){putbits(d+4,&bit,block,2);putbits(d+4,&bit,block==2,1);
   putbits(d+4,&bit,1,5);putbits(d+4,&bit,1,5);putbits(d+4,&bit,0,9);
  }else {putbits(d+4,&bit,1,5);putbits(d+4,&bit,1,5);putbits(d+4,&bit,1,5);putbits(d+4,&bit,0,4);putbits(d+4,&bit,0,3);}
  putbits(d+4,&bit,0,1);putbits(d+4,&bit,0,1);
  bit=0;if(active){for(unsigned j=0;j<20;j++)putbits(d+13,&bit,1,1);putbits(d+13,&bit,2,3);}
 }
 return 6*144+8;
}
#endif
