/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REDISTRIBUTION OF THIS LIBRARY SOURCE IS   *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2018             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: decode Ogg pages into packets, tracking which byte ranges
           of the original input each packet was assembled from

 note: The decode logic below intentionally mirrors the
       ogg_stream_pagein()/ogg_stream_packetout() state machine in
       framing.c so that source-tracked streams behave bit-for-bit
       identically to plain ogg_stream_state streams; the source
       records are carried alongside the lacing fifo.

 ********************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <ogg/ogg.h>
#include <ogg/source.h>

#define OGG_SOURCE_INT64_MAX ((ogg_int64_t)(((ogg_uint64_t)1<<63)-1))

/* One source record per lacing fifo entry: the input range that
   segment's payload was read from.  Hole markers and zero-length
   segments carry bytes==0 and contribute nothing to a packet's
   spans. */
typedef struct {
  ogg_int64_t offset; /* absolute input offset of the segment payload */
  long bytes;         /* payload bytes covered (== lacing value)      */
  long page;          /* id of the submitting page; spans never merge
                         across pages                                  */
} ogg_source_rec;

struct ogg_source_stream {
  ogg_stream_state os;        /* the real decode state */
  ogg_source_rec *recs;       /* parallel to os.lacing_vals */
  long recs_storage;
  long next_page;             /* page id source for recs */
  ogg_source_span *spans;     /* result buffer handed out via info */
  long spans_storage;
};

ogg_source_stream *ogg_source_stream_new(int serialno){
  ogg_source_stream *ss=_ogg_malloc(sizeof(*ss));
  if(!ss) return NULL;
  memset(ss,0,sizeof(*ss));
  if(ogg_stream_init(&ss->os,serialno)){
    _ogg_free(ss);
    return NULL;
  }
  return ss;
}

void ogg_source_stream_free(ogg_source_stream *ss){
  if(ss){
    ogg_stream_clear(&ss->os);
    if(ss->recs)_ogg_free(ss->recs);
    if(ss->spans)_ogg_free(ss->spans);
    _ogg_free(ss);
  }
}

int ogg_source_stream_reset(ogg_source_stream *ss, int serialno){
  int ret;
  if(!ss) return -1;
  ret=ogg_stream_reset_serialno(&ss->os,serialno);
  /* release all tracking state */
  if(ss->recs)_ogg_free(ss->recs);
  if(ss->spans)_ogg_free(ss->spans);
  ss->recs=NULL;
  ss->recs_storage=0;
  ss->spans=NULL;
  ss->spans_storage=0;
  ss->next_page=0;
  return ret;
}

/* Expand the lacing fifo of the embedded stream state together with
   the parallel source records.  Mirrors _os_lacing_expand(); the
   record array additionally grows whenever it lags behind the lacing
   storage (it starts unallocated while the lacing fifo does not). */
static int _source_lacing_expand(ogg_source_stream *ss,long needed){
  ogg_stream_state *os=&ss->os;
  if(os->lacing_storage-needed<=os->lacing_fill){
    long lacing_storage;
    void *ret;
    if(os->lacing_storage>LONG_MAX-needed){ /* LONG_MAX guard as in framing.c */
      ogg_stream_clear(os);
      return -1;
    }
    lacing_storage=os->lacing_storage+needed;
    if(lacing_storage<LONG_MAX-32)lacing_storage+=32;
    ret=_ogg_realloc(os->lacing_vals,lacing_storage*sizeof(*os->lacing_vals));
    if(!ret){
      ogg_stream_clear(os);
      return -1;
    }
    os->lacing_vals=ret;
    ret=_ogg_realloc(os->granule_vals,lacing_storage*sizeof(*os->granule_vals));
    if(!ret){
      ogg_stream_clear(os);
      return -1;
    }
    os->granule_vals=ret;
    os->lacing_storage=lacing_storage;
  }
  if(ss->recs_storage<os->lacing_storage){
    void *ret=_ogg_realloc(ss->recs,os->lacing_storage*sizeof(*ss->recs));
    if(!ret){
      ogg_stream_clear(os);
      _ogg_free(ss->recs);
      ss->recs=NULL;
      ss->recs_storage=0;
      return -1;
    }
    ss->recs=ret;
    ss->recs_storage=os->lacing_storage;
  }
  return 0;
}

/* Mirrors _os_body_expand(). */
static int _source_body_expand(ogg_source_stream *ss,long needed){
  ogg_stream_state *os=&ss->os;
  if(os->body_storage-needed<=os->body_fill){
    long body_storage;
    void *ret;
    if(os->body_storage>LONG_MAX-needed){
      ogg_stream_clear(os);
      return -1;
    }
    body_storage=os->body_storage+needed;
    if(body_storage<LONG_MAX-1024)body_storage+=1024;
    ret=_ogg_realloc(os->body_data,body_storage*sizeof(*os->body_data));
    if(!ret){
      ogg_stream_clear(os);
      return -1;
    }
    os->body_storage=body_storage;
    os->body_data=ret;
  }
  return 0;
}

int ogg_source_stream_pagein(ogg_source_stream *ss, ogg_page *og,
                             ogg_int64_t offset){
  ogg_stream_state *os;
  unsigned char *header;
  unsigned char *body;
  long           bodysize;
  int            segptr=0;

  int version;
  int continued;
  int bos;
  int eos;
  ogg_int64_t granulepos;
  int serialno;
  long pageno;
  int segments;

  ogg_int64_t segpos; /* input offset of the next kept segment */
  long page;

  if(!ss || !og || !og->header) return -1;
  os=&ss->os;
  if(ogg_stream_check(os)) return -1;

  header=og->header;
  body=og->body;
  bodysize=og->body_len;

  /* Validate the offset before any state is modified: the offset
     itself must be nonnegative and the whole page (capture pattern
     through end of body) must fit in a signed 64 bit offset. */
  if(offset<0) return -1;
  if(og->header_len<0 || bodysize<0) return -1;
  if((ogg_uint64_t)og->header_len+(ogg_uint64_t)bodysize>
     (ogg_uint64_t)OGG_SOURCE_INT64_MAX-(ogg_uint64_t)offset)
    return -1;

  version=ogg_page_version(og);
  continued=ogg_page_continued(og);
  bos=ogg_page_bos(og);
  eos=ogg_page_eos(og);
  granulepos=ogg_page_granulepos(og);
  serialno=ogg_page_serialno(og);
  pageno=ogg_page_pageno(og);
  segments=header[26];

  /* clean up 'returned data' (mirrors ogg_stream_pagein) */
  {
    long lr=os->lacing_returned;
    long br=os->body_returned;

    /* body data */
    if(br){
      os->body_fill-=br;
      if(os->body_fill)
        memmove(os->body_data,os->body_data+br,os->body_fill);
      os->body_returned=0;
    }

    if(lr){
      /* segment table */
      if(os->lacing_fill-lr){
        memmove(os->lacing_vals,os->lacing_vals+lr,
                (os->lacing_fill-lr)*sizeof(*os->lacing_vals));
        memmove(os->granule_vals,os->granule_vals+lr,
                (os->lacing_fill-lr)*sizeof(*os->granule_vals));
        /* the source records of not-yet-returned packets and partial
           packets move along with the lacing values */
        memmove(ss->recs,ss->recs+lr,
                (os->lacing_fill-lr)*sizeof(*ss->recs));
      }
      os->lacing_fill-=lr;
      os->lacing_packet-=lr;
      os->lacing_returned=0;
    }
  }

  /* check the serial number */
  if(serialno!=os->serialno)return(-1);
  if(version>0)return(-1);

  /* from here on the page is accepted and may contribute source */
  page=ss->next_page++;

  if(_source_lacing_expand(ss,segments+1)) return -1;

  /* are we in sequence? */
  if(pageno!=os->pageno){
    int i;

    /* unroll previous partial packet (if any); its source records
       are dropped along with the lacing values */
    for(i=os->lacing_packet;i<os->lacing_fill;i++)
      os->body_fill-=os->lacing_vals[i]&0xff;
    os->lacing_fill=os->lacing_packet;

    /* make a note of dropped data in segment table */
    if(os->pageno!=-1){
      os->lacing_vals[os->lacing_fill]=0x400;
      ss->recs[os->lacing_fill].offset=0;
      ss->recs[os->lacing_fill].bytes=0;
      ss->recs[os->lacing_fill].page=page;
      os->lacing_fill++;
      os->lacing_packet++;
    }
  }

  /* the first kept segment's payload sits at the page body start */
  segpos=offset+og->header_len;

  /* are we a 'continued packet' page?  If so, we may need to skip
     some segments */
  if(continued){
    if(os->lacing_fill<1 ||
       (os->lacing_vals[os->lacing_fill-1]&0xff)<255 ||
       os->lacing_vals[os->lacing_fill-1]==0x400){
      bos=0;
      for(;segptr<segments;segptr++){
        int val=header[27+segptr];
        body+=val;
        bodysize-=val;
        segpos+=val; /* skipped partial-packet payload: no source */
        if(val<255){
          segptr++;
          break;
        }
      }
    }
  }

  if(bodysize){
    if(_source_body_expand(ss,bodysize)) return -1;
    memcpy(os->body_data+os->body_fill,body,bodysize);
    os->body_fill+=bodysize;
  }

  {
    int saved=-1;
    while(segptr<segments){
      int val=header[27+segptr];
      os->lacing_vals[os->lacing_fill]=val;
      os->granule_vals[os->lacing_fill]=-1;

      ss->recs[os->lacing_fill].offset=segpos;
      ss->recs[os->lacing_fill].bytes=val;
      ss->recs[os->lacing_fill].page=page;
      segpos+=val;

      if(bos){
        os->lacing_vals[os->lacing_fill]|=0x100;
        bos=0;
      }

      if(val<255)saved=os->lacing_fill;

      os->lacing_fill++;
      segptr++;

      if(val<255)os->lacing_packet=os->lacing_fill;
    }

    /* set the granulepos on the last granuleval of the last full packet */
    if(saved!=-1){
      os->granule_vals[saved]=granulepos;
    }

  }

  if(eos){
    os->e_o_s=1;
    if(os->lacing_fill>0)
      os->lacing_vals[os->lacing_fill-1]|=0x200;
  }

  os->pageno=pageno+1;

  return(0);
}

/* Assemble the span list covering lacing entries [start,end] into the
   stream-owned buffer.  Adjacent segments of the same page merge;
   segments from different pages never do. */
static int _source_fill_info(ogg_source_stream *ss,int start,int end,
                             ogg_source_info *info){
  size_t count=0;
  long last_page=-1;
  int i;

  if(end-start+1>ss->spans_storage){
    long storage=end-start+1;
    void *ret;
    if(storage<32)storage=32;
    ret=_ogg_realloc(ss->spans,storage*sizeof(*ss->spans));
    if(!ret) return -1;
    ss->spans=ret;
    ss->spans_storage=storage;
  }

  for(i=start;i<=end;i++){
    ogg_source_rec *r=ss->recs+i;
    if(r->bytes<=0) continue; /* hole markers and nil segments */
    if(count && r->page==last_page &&
       ss->spans[count-1].offset+ss->spans[count-1].bytes==r->offset){
      ss->spans[count-1].bytes+=r->bytes;
    }else{
      ss->spans[count].offset=r->offset;
      ss->spans[count].bytes=r->bytes;
      count++;
    }
    last_page=r->page;
  }

  info->spans=count?ss->spans:NULL;
  info->count=count;
  return 0;
}

/* Mirrors _packetout() in framing.c, additionally reporting source
   spans.  adv!=0 consumes the packet (packetout), adv==0 does not
   (packetpeek); hole markers are consumed in both cases, exactly like
   the original. */
static int _source_packetout(ogg_source_stream *ss,ogg_packet *op,
                             ogg_source_info *info,int adv){
  ogg_stream_state *os=&ss->os;
  int ptr=os->lacing_returned;

  if(info){
    info->spans=NULL;
    info->count=0;
  }

  if(os->lacing_packet<=ptr)return(0);

  if(os->lacing_vals[ptr]&0x400){
    /* we need to tell the codec there's a gap; it might need to
       handle previous packet dependencies. */
    os->lacing_returned++;
    os->packetno++;
    return(-1);
  }

  if(!op && !adv && !info)return(1); /* just using peek as an
                                        inexpensive way to ask if
                                        there's a whole packet
                                        waiting */

  /* Gather the whole packet. We'll have no holes or a partial packet */
  {
    int start=ptr;
    int size=os->lacing_vals[ptr]&0xff;
    long bytes=size;
    int eos=os->lacing_vals[ptr]&0x200; /* last packet of the stream? */
    int bos=os->lacing_vals[ptr]&0x100; /* first packet of the stream? */

    while(size==255){
      int val=os->lacing_vals[++ptr];
      size=val&0xff;
      if(val&0x200)eos=0x200;
      bytes+=size;
    }

    if(op){
      op->e_o_s=eos;
      op->b_o_s=bos;
      op->packet=os->body_data+os->body_returned;
      op->packetno=os->packetno;
      op->granulepos=os->granule_vals[ptr];
      op->bytes=bytes;
    }

    if(info){
      if(_source_fill_info(ss,start,ptr,info)){
        info->spans=NULL;
        info->count=0;
      }
    }

    if(adv){
      os->body_returned+=bytes;
      os->lacing_returned=ptr+1;
      os->packetno++;
    }
  }
  return(1);
}

int ogg_source_stream_packetout(ogg_source_stream *ss,ogg_packet *op,
                                ogg_source_info *info){
  if(info){
    info->spans=NULL;
    info->count=0;
  }
  if(!ss || ogg_stream_check(&ss->os)) return 0;
  return _source_packetout(ss,op,info,1);
}

int ogg_source_stream_packetpeek(ogg_source_stream *ss,ogg_packet *op,
                                 ogg_source_info *info){
  if(info){
    info->spans=NULL;
    info->count=0;
  }
  if(!ss || ogg_stream_check(&ss->os)) return 0;
  return _source_packetout(ss,op,info,0);
}
