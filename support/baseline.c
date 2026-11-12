#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "mpeg2.h"
int main(void) {
 FILE *f=fopen("support/ipb.m2v","rb"); uint8_t b[73]; size_t n=1;
 mpeg2_accel(0); mpeg2dec_t *d=mpeg2_init(); if(!f||!d)return 2;
 const mpeg2_info_t *i=mpeg2_info(d); unsigned frames=0; uint32_t h=2166136261u;
 while(n) { mpeg2_state_t s=mpeg2_parse(d);
  if(s==STATE_BUFFER){n=fread(b,1,sizeof b,f); mpeg2_buffer(d,b,b+n);}
  if((s==STATE_SLICE||s==STATE_END||s==STATE_INVALID_END)&&i->display_fbuf){
   for(int p=0;p<3;p++) { unsigned w=p?i->sequence->chroma_width:i->sequence->width;
    unsigned hh=p?i->sequence->chroma_height:i->sequence->height;
    for(unsigned x=0;x<w*hh;x++)h=(h^i->display_fbuf->buf[p][x])*16777619u;
   } frames++;
  }
 }
 fclose(f);mpeg2_close(d); printf("MPEG2_BASE frames=%u hash=%08x\n",frames,h);
 return frames==6 ? 0:3;
}
