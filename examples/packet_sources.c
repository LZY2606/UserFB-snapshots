/********************************************************************
 *                                                                  *
 * Example: decode an Ogg file with the source-tracking stream and  *
 * print, for every packet, how many input byte ranges it was       *
 * assembled from.                                                  *
 *                                                                  *
 * usage: packet_sources <file.ogg>                                 *
 *                                                                  *
 ********************************************************************/
#include <ogg/ogg.h>
#include <ogg/source.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv){
  FILE *f;
  ogg_sync_state oy;
  ogg_source_stream *ss=NULL;
  long long written=0; /* total bytes handed to the sync layer */
  int eof=0;

  if(argc<2){
    fprintf(stderr,"usage: %s <file.ogg>\n",argv[0]);
    return 1;
  }
  f=fopen(argv[1],"rb");
  if(!f){
    fprintf(stderr,"cannot open %s\n",argv[1]);
    return 1;
  }

  ogg_sync_init(&oy);

  while(!eof){
    char *buf=ogg_sync_buffer(&oy,4096);
    size_t n=fread(buf,1,4096,f);
    if(n==0)eof=1;
    ogg_sync_wrote(&oy,(long)n);
    written+=n;

    for(;;){
      ogg_page og;
      /* absolute input offset of the next unconsumed byte; if a page
         is returned below, this is where its capture pattern sits */
      ogg_int64_t page_offset=written-(oy.fill-oy.returned);
      int r=ogg_sync_pageout(&oy,&og);
      if(r==0)break;    /* need more data */
      if(r<0)continue;  /* skipped garbage; recompute the offset */

      if(!ss){
        ss=ogg_source_stream_new(ogg_page_serialno(&og));
        if(!ss){
          fprintf(stderr,"out of memory\n");
          return 1;
        }
      }

      /* pages of other serials are rejected by pagein */
      if(ogg_source_stream_pagein(ss,&og,page_offset)==0){
        for(;;){
          ogg_packet op;
          ogg_source_info info;
          int pr=ogg_source_stream_packetout(ss,&op,&info);
          if(pr==0)break;         /* need more data */
          if(pr<0)continue;       /* hole in the stream */
          printf("packet=%lld bytes=%ld spans=%zu\n",
                 (long long)op.packetno,op.bytes,info.count);
        }
      }
    }
  }

  ogg_source_stream_free(ss);
  ogg_sync_clear(&oy);
  fclose(f);
  return 0;
}
