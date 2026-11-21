#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "mpeg2.h"
#include "fixture.h"
static uint64_t digest=UINT64_C(14695981039346656037);
static void add(uint8_t byte){digest=(digest^byte)*UINT64_C(1099511628211);}
int main(void) {
 mpeg2_accel(0);
 mpeg2dec_t *d=mpeg2_init();
 if(!d) return 2;
 const mpeg2_info_t *info=mpeg2_info(d);
 size_t offset=0;
 int frames=0,types[5]={0},sequences=0;
 for(unsigned steps=0;steps<100000;steps++) {
  mpeg2_state_t s=mpeg2_parse(d);
  if(s==STATE_BUFFER) {
   if(offset==sizeof(task_video)) break;
   size_t n=37;
   if(n>sizeof(task_video)-offset)n=sizeof(task_video)-offset;
   mpeg2_buffer(d,(uint8_t*)task_video+offset,(uint8_t*)task_video+offset+n);
   offset+=n;
  } else if(s==STATE_SEQUENCE) {
   sequences++;
   printf("SEQUENCE coded=%ux%u chroma=%ux%u\n",info->sequence->width,info->sequence->height,info->sequence->chroma_width,info->sequence->chroma_height);
  } else if(s==STATE_INVALID || s==STATE_INVALID_END) {
   fprintf(stderr,"unexpected parser state %d\n",s);mpeg2_close(d);return 3;
  } else if((s==STATE_SLICE||s==STATE_END) && info->display_fbuf) {
   if(!info->sequence||!info->display_picture)return 4;
   const mpeg2_sequence_t *q=info->sequence;
   int type=info->display_picture->flags&PIC_MASK_CODING_TYPE;
   if(type<1||type>4)return 5;
   types[type]++;
   for(int p=0;p<3;p++) {
    unsigned w=p?q->chroma_width:q->width,h=p?q->chroma_height:q->height;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)add(info->display_fbuf->buf[p][y*w+x]);
   }
   printf("DISPLAY n=%d type=%d temporal=%u\n",frames,type,info->display_picture->temporal_reference);
   frames++;
  }
 }
 mpeg2_close(d);
 if(sequences!=1||frames!=9||types[1]!=1||!types[2]||!types[3])return 6;
 printf("BASELINE_OK frames=%d I=%d P=%d B=%d fnv64=%016llx\n",frames,types[1],types[2],types[3],(unsigned long long)digest);
 return 0;
}
