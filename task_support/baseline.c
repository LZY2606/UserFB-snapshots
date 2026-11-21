#include <ogg/ogg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"baseline line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
  ogg_stream_state enc,dec;
  ogg_sync_state sync;
  ogg_packet packet,out;
  ogg_page page,readpage;
  unsigned char *data=(unsigned char *)malloc(70000);
  long sizes[3]={1,70000,3};
  int k,pages=0,packets=0,unknown=0,continued=0;
  CHECK(data!=NULL);
  for(k=0;k<70000;k++) data[k]=(unsigned char)(k*17+3);
  CHECK(ogg_stream_init(&enc,71)==0);
  CHECK(ogg_stream_init(&dec,71)==0);
  CHECK(ogg_sync_init(&sync)==0);
  for(k=0;k<3;k++) {
    memset(&packet,0,sizeof(packet));packet.packet=data;packet.bytes=sizes[k];
    packet.b_o_s=k==0;packet.e_o_s=k==2;packet.granulepos=k*960;
    CHECK(ogg_stream_packetin(&enc,&packet)==0);
    while(ogg_stream_flush(&enc,&page)) {
      long bytes=page.header_len+page.body_len;
      char *dest=ogg_sync_buffer(&sync,bytes);CHECK(dest!=NULL);
      memcpy(dest,page.header,(size_t)page.header_len);
      memcpy(dest+page.header_len,page.body,(size_t)page.body_len);
      unknown+=ogg_page_granulepos(&page)==-1;
      continued+=!!ogg_page_continued(&page);pages++;
      CHECK(ogg_sync_wrote(&sync,bytes)==0);
      CHECK(ogg_sync_pageout(&sync,&readpage)==1);
      CHECK(ogg_stream_pagein(&dec,&readpage)==0);
      while(ogg_stream_packetout(&dec,&out)==1) {
        CHECK(packets<3);CHECK(out.bytes==sizes[packets]);
        CHECK(memcmp(out.packet,data,(size_t)out.bytes)==0);
        CHECK(out.granulepos==packets*960);packets++;
      }
    }
  }
  CHECK(pages==4 && packets==3 && unknown==1 && continued==1);
  printf("OGG_ROUNDTRIP pages=%d packets=%d unknown=%d continued=%d\n",pages,packets,unknown,continued);
  CHECK(ogg_stream_clear(&enc)==0);CHECK(ogg_stream_clear(&dec)==0);
  CHECK(ogg_sync_clear(&sync)==0);free(data);return 0;
}
