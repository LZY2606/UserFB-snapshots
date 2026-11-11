/********************************************************************
 *                                                                  *
 * Tests for the source-tracking decode stream (ogg/source.h).      *
 *                                                                  *
 * Packets are decoded through ogg_source_stream and a plain        *
 * ogg_stream_state in lockstep; payloads, packetno and granulepos  *
 * must agree.  Independently, every packet is reassembled from     *
 * the recorded source spans and the raw file image, and the spans  *
 * are checked to cover payload bytes only.                         *
 *                                                                  *
 ********************************************************************/
#include <ogg/ogg.h>
#include <ogg/source.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INT64_MAX_LOCAL ((ogg_int64_t)(((ogg_uint64_t)1<<63)-1))

/* ---------------------------------------------------------------- */
/* page lists produced by the encoder                               */

typedef struct {
  unsigned char *header;
  long header_len;
  unsigned char *body;
  long body_len;
  long offset; /* absolute offset in the test file image, set by lay_page */
} page_rec;

typedef struct {
  page_rec *pages;
  int count;
} page_list;

static unsigned char pattern_byte(int packet_index,long j){
  return (unsigned char)(packet_index*37+j*11+5);
}

static void collect_pages(ogg_stream_state *enc,page_list *pl){
  ogg_page og;
  while(ogg_stream_flush(enc,&og)){
    page_rec r;
    r.header_len=og.header_len;
    r.body_len=og.body_len;
    r.header=malloc(r.header_len);
    assert(r.header);
    memcpy(r.header,og.header,r.header_len);
    r.body=malloc(r.body_len?r.body_len:1);
    assert(r.body);
    memcpy(r.body,og.body,r.body_len);
    r.offset=-1;
    pl->pages=realloc(pl->pages,(pl->count+1)*sizeof(*pl->pages));
    assert(pl->pages);
    pl->pages[pl->count++]=r;
  }
}

/* Encode packets with the given sizes into a page list.  When
   flush_each is set every packet gets its own page(s); otherwise the
   encoder packs as it sees fit (multiple packets share pages). */
static page_list encode_packets(const long *sizes,int n,int serial,int flush_each){
  ogg_stream_state enc;
  page_list pl={NULL,0};
  int i;
  assert(ogg_stream_init(&enc,serial)==0);
  for(i=0;i<n;i++){
    ogg_packet op;
    unsigned char *buf=malloc(sizes[i]?sizes[i]:1);
    long j;
    assert(buf);
    for(j=0;j<sizes[i];j++)buf[j]=pattern_byte(i,j);
    memset(&op,0,sizeof(op));
    op.packet=buf;
    op.bytes=sizes[i];
    op.e_o_s=(i==n-1);
    op.granulepos=(i+1)*1000;
    assert(ogg_stream_packetin(&enc,&op)==0);
    if(flush_each)collect_pages(&enc,&pl);
    free(buf);
  }
  collect_pages(&enc,&pl);
  ogg_stream_clear(&enc);
  return pl;
}

static void free_page_list(page_list *pl){
  int i;
  for(i=0;i<pl->count;i++){
    free(pl->pages[i].header);
    free(pl->pages[i].body);
  }
  free(pl->pages);
  pl->pages=NULL;
  pl->count=0;
}

static ogg_page page_of(const page_rec *r){
  ogg_page og;
  og.header=r->header;
  og.header_len=r->header_len;
  og.body=r->body;
  og.body_len=r->body_len;
  return og;
}

/* ---------------------------------------------------------------- */
/* the "file": pages are laid out back to back, offsets recorded    */

static unsigned char *file_image;
static long file_len;

/* geometry of laid pages, for payload-only span validation */
typedef struct { long offset,header_len,body_len; } laid_rec;
static laid_rec *laid;
static int laid_count;

static void reset_file(void){
  free(file_image); file_image=NULL; file_len=0;
  free(laid); laid=NULL; laid_count=0;
}

static void lay_page(page_rec *p){
  p->offset=file_len;
  file_image=realloc(file_image,file_len+p->header_len+p->body_len);
  assert(file_image);
  memcpy(file_image+file_len,p->header,p->header_len);
  file_len+=p->header_len;
  memcpy(file_image+file_len,p->body,p->body_len);
  file_len+=p->body_len;
  laid=realloc(laid,(laid_count+1)*sizeof(*laid));
  assert(laid);
  laid[laid_count].offset=p->offset;
  laid[laid_count].header_len=p->header_len;
  laid[laid_count].body_len=p->body_len;
  laid_count++;
}

/* ---------------------------------------------------------------- */
/* independent span validation against the raw file image           */

static void validate_spans(const ogg_packet *op,const ogg_source_info *info){
  long pos=0,total=0;
  size_t s;
  if(info->count==0){
    /* an empty packet has no source */
    assert(info->spans==NULL);
    assert(op->bytes==0);
    return;
  }
  assert(info->spans!=NULL);
  for(s=0;s<info->count;s++){
    const ogg_source_span *sp=&info->spans[s];
    long k;
    int pi;
    assert(sp->bytes>0);
    /* the span must lie entirely within a single page's body:
       payload only, no header/lacing bytes, no skipped data */
    for(pi=0;pi<laid_count;pi++){
      long bstart=laid[pi].offset+laid[pi].header_len;
      long bend=bstart+laid[pi].body_len;
      if((long)sp->offset>=bstart && (long)sp->offset+sp->bytes<=bend)break;
    }
    assert(pi<laid_count);
    /* adjacent spans may be contiguous in the input only across a
       page boundary; within a page they must have been merged */
    if(s>0){
      const ogg_source_span *prev=&info->spans[s-1];
      if(prev->offset+prev->bytes==sp->offset){
        for(pi=0;pi<laid_count;pi++)
          if(laid[pi].offset+laid[pi].header_len+laid[pi].body_len==
             (long)(prev->offset+prev->bytes))break;
        assert(pi<laid_count);
        for(pi=0;pi<laid_count;pi++)
          if(laid[pi].offset==(long)sp->offset)break;
        assert(pi<laid_count);
      }
    }
    /* reassemble the packet from the raw input ranges */
    total+=sp->bytes;
    for(k=0;k<sp->bytes;k++)
      assert(file_image[(long)sp->offset+k]==op->packet[pos++]);
  }
  assert(total==op->bytes);
  assert(pos==op->bytes);
}

/* ---------------------------------------------------------------- */
/* dual decode: source stream and plain stream in lockstep          */

typedef struct {
  ogg_source_stream *ss;
  ogg_stream_state dec;
} dual;

static void dual_init(dual *d,int serial){
  d->ss=ogg_source_stream_new(serial);
  assert(d->ss);
  assert(ogg_stream_init(&d->dec,serial)==0);
}

static void dual_free(dual *d){
  ogg_source_stream_free(d->ss);
  ogg_stream_clear(&d->dec);
}

static void dual_pagein(dual *d,page_rec *p){
  ogg_page og=page_of(p);
  assert(ogg_source_stream_pagein(d->ss,&og,p->offset)==0);
  assert(ogg_stream_pagein(&d->dec,&og)==0);
}

/* drain both decoders; returns the number of packets taken */
static int dual_drain(dual *d){
  int n=0;
  for(;;){
    ogg_packet o1,o2;
    ogg_source_info info;
    int r1=ogg_source_stream_packetout(d->ss,&o1,&info);
    int r2=ogg_stream_packetout(&d->dec,&o2);
    assert(r1==r2);
    if(r1==0)break;
    if(r1==-1){
      /* holes report no source */
      assert(info.spans==NULL);
      assert(info.count==0);
      continue;
    }
    assert(o1.bytes==o2.bytes);
    assert(o1.packetno==o2.packetno);
    assert(o1.granulepos==o2.granulepos);
    assert(o1.b_o_s==o2.b_o_s);
    assert(o1.e_o_s==o2.e_o_s);
    assert(memcmp(o1.packet,o2.packet,o1.bytes)==0);
    validate_spans(&o1,&info);
    n++;
  }
  return n;
}

/* ---------------------------------------------------------------- */

static void test_reassembly(void){
  /* a 70000 byte packet spanning two pages, with a page of another
     serial interleaved between the two halves */
  {
    long sa[]={70000};
    long sb[]={5,6,7};
    page_list A=encode_packets(sa,1,73,0);
    page_list B=encode_packets(sb,3,99,0);
    dual da,db;
    long j;
    ogg_packet o1,o2;
    ogg_source_info info;

    assert(A.count==2);
    assert(B.count==2); /* initial header page holds packet 0 alone */

    reset_file();
    lay_page(&A.pages[0]);
    lay_page(&B.pages[0]); /* foreign serial between the halves */
    lay_page(&B.pages[1]);
    lay_page(&A.pages[1]);

    dual_init(&da,73);
    dual_init(&db,99);

    dual_pagein(&da,&A.pages[0]);
    assert(dual_drain(&da)==0); /* packet not complete yet */

    dual_pagein(&db,&B.pages[0]);
    dual_pagein(&db,&B.pages[1]);
    assert(dual_drain(&db)==3); /* the foreign stream is unaffected */

    dual_pagein(&da,&A.pages[1]);
    assert(ogg_source_stream_packetout(da.ss,&o1,&info)==1);
    assert(ogg_stream_packetout(&da.dec,&o2)==1);
    assert(o1.bytes==70000 && o2.bytes==70000);
    assert(o1.packetno==0);
    assert(o1.granulepos==1000);
    /* exactly two spans: one per page, never merged across pages,
       and never stretched over the interleaved foreign page */
    assert(info.count==2);
    assert(info.spans[0].offset==A.pages[0].offset+A.pages[0].header_len);
    assert(info.spans[0].bytes==A.pages[0].body_len);
    assert(info.spans[1].offset==A.pages[1].offset+A.pages[1].header_len);
    assert(info.spans[1].bytes==A.pages[1].body_len);
    assert(info.spans[0].bytes+info.spans[1].bytes==70000);
    validate_spans(&o1,&info);
    assert(memcmp(o1.packet,o2.packet,70000)==0);
    for(j=0;j<70000;j++)
      assert(o1.packet[j]==pattern_byte(0,j));
    assert(ogg_source_stream_packetout(da.ss,&o1,&info)==0);
    assert(info.spans==NULL && info.count==0);

    dual_free(&da);
    dual_free(&db);
    free_page_list(&A);
    free_page_list(&B);
  }

  /* mixed sizes on shared pages: empty packets, 255 multiples,
     several packets per page */
  {
    long sizes[]={0,17,255,256,510,254,1,1020,0,100,255,3};
    page_list L=encode_packets(sizes,12,73,0);
    dual d;
    int i,total=0,multi=0;

    reset_file();
    for(i=0;i<L.count;i++)lay_page(&L.pages[i]);

    /* the encoding must actually pack multiple packets per page */
    for(i=0;i<L.count;i++){
      ogg_page og=page_of(&L.pages[i]);
      if(ogg_page_packets(&og)>1)multi=1;
    }
    assert(multi);

    dual_init(&d,73);
    for(i=0;i<L.count;i++){
      dual_pagein(&d,&L.pages[i]);
      total+=dual_drain(&d);
    }
    assert(total==12);
    dual_free(&d);
    free_page_list(&L);
  }

  /* compaction on pagein moves taken packets out of the fifo; the
     source of packets still buffered must survive untouched */
  {
    long sizes[]={300,2000,2000,2000,2000,2000};
    page_list L=encode_packets(sizes,6,73,0);
    dual d;
    ogg_packet o1,o2;
    ogg_source_info info;
    int i,total;

    assert(L.count==3); /* packet 0 alone, packets 1..4, packet 5 */
    reset_file();
    for(i=0;i<L.count;i++)lay_page(&L.pages[i]);

    dual_init(&d,73);
    dual_pagein(&d,&L.pages[0]);
    assert(dual_drain(&d)==1);
    dual_pagein(&d,&L.pages[1]);
    /* take only the first of the four buffered packets */
    assert(ogg_source_stream_packetout(d.ss,&o1,&info)==1);
    assert(o1.bytes==2000);
    validate_spans(&o1,&info);
    assert(ogg_stream_packetout(&d.dec,&o2)==1);
    /* this pagein compacts the taken packet away */
    dual_pagein(&d,&L.pages[2]);
    total=dual_drain(&d);
    assert(total==4);
    dual_free(&d);
    free_page_list(&L);
  }

  /* the source of a half packet pending across such a compaction
     must survive as well */
  {
    long sizes[]={300,2000,70000};
    page_list L=encode_packets(sizes,3,73,0);
    dual d;
    ogg_packet o1,o2;
    ogg_source_info info;
    int i;

    assert(L.count==3); /* packet 0; packet 1 + big start; big tail */
    reset_file();
    for(i=0;i<L.count;i++)lay_page(&L.pages[i]);

    dual_init(&d,73);
    dual_pagein(&d,&L.pages[0]);
    assert(dual_drain(&d)==1);
    dual_pagein(&d,&L.pages[1]);
    /* take the small packet, leave the half of the big one pending */
    assert(ogg_source_stream_packetout(d.ss,&o1,&info)==1);
    assert(o1.bytes==2000);
    validate_spans(&o1,&info);
    assert(ogg_stream_packetout(&d.dec,&o2)==1);
    /* compaction drops the taken packet's records, keeps the rest */
    dual_pagein(&d,&L.pages[2]);
    assert(ogg_source_stream_packetout(d.ss,&o1,&info)==1);
    assert(ogg_stream_packetout(&d.dec,&o2)==1);
    assert(o1.bytes==70000);
    assert(info.count==2);
    assert(info.spans[0].offset==
           L.pages[1].offset+L.pages[1].header_len+2000);
    assert(info.spans[0].bytes==L.pages[1].body_len-2000);
    assert(info.spans[1].offset==L.pages[2].offset+L.pages[2].header_len);
    assert(info.spans[1].bytes==L.pages[2].body_len);
    validate_spans(&o1,&info);
    dual_free(&d);
    free_page_list(&L);
  }

  printf("SOURCE_REASSEMBLY_OK\n");
}

static void test_holes(void){
  /* a missing page in the middle: hole reported, the following
     complete packet keeps only its own source */
  {
    long sizes[]={1000,2000,3000,4000,5000};
    page_list L=encode_packets(sizes,5,73,1); /* one page per packet */
    dual d;
    ogg_packet o1,o2;
    ogg_source_info info;
    int r1,r2;
    int i;

    assert(L.count==5);
    reset_file();
    for(i=0;i<5;i++)lay_page(&L.pages[i]);

    dual_init(&d,73);
    dual_pagein(&d,&L.pages[0]);
    assert(dual_drain(&d)==1);
    dual_pagein(&d,&L.pages[1]);
    assert(dual_drain(&d)==1);

    /* pages[2] (packet 3000) is lost */
    dual_pagein(&d,&L.pages[3]);

    r1=ogg_source_stream_packetout(d.ss,&o1,&info);
    r2=ogg_stream_packetout(&d.dec,&o2);
    assert(r1==-1 && r2==-1);
    assert(info.spans==NULL && info.count==0);

    r1=ogg_source_stream_packetout(d.ss,&o1,&info);
    r2=ogg_stream_packetout(&d.dec,&o2);
    assert(r1==1 && r2==1);
    assert(o1.bytes==4000);
    assert(o1.packetno==o2.packetno);
    assert(o1.granulepos==o2.granulepos);
    assert(info.count==1);
    assert(info.spans[0].offset==L.pages[3].offset+L.pages[3].header_len);
    assert(info.spans[0].bytes==4000);
    validate_spans(&o1,&info);

    dual_pagein(&d,&L.pages[4]);
    assert(dual_drain(&d)==1);
    dual_free(&d);
    free_page_list(&L);
  }

  /* the first page of a spanning packet is missing: the continuation
     page has no beginning and its partial payload is skipped */
  {
    long sizes[]={70000,50};
    page_list L=encode_packets(sizes,2,73,1);
    dual d;
    ogg_packet o1,o2;
    ogg_source_info info;
    int i;

    assert(L.count==3); /* big packet takes two pages, small one */
    reset_file();
    for(i=0;i<3;i++)lay_page(&L.pages[i]);

    dual_init(&d,73);
    /* pages[0] never arrives; the fresh stream expected page 0, so
       the continuation page reports a hole and its partial payload
       is skipped */
    dual_pagein(&d,&L.pages[1]);
    {
      int r1=ogg_source_stream_packetout(d.ss,&o1,&info);
      int r2=ogg_stream_packetout(&d.dec,&o2);
      assert(r1==-1 && r2==-1);
      assert(info.spans==NULL && info.count==0);
    }
    assert(dual_drain(&d)==0);
    dual_pagein(&d,&L.pages[2]);

    assert(ogg_source_stream_packetout(d.ss,&o1,&info)==1);
    assert(ogg_stream_packetout(&d.dec,&o2)==1);
    assert(o1.bytes==50);
    assert(o1.packetno==o2.packetno && o1.packetno==1);
    /* only the small packet's own page is a source */
    assert(info.count==1);
    assert(info.spans[0].offset==L.pages[2].offset+L.pages[2].header_len);
    assert(info.spans[0].bytes==50);
    validate_spans(&o1,&info);
    dual_free(&d);
    free_page_list(&L);
  }

  /* the middle page of a spanning packet is missing: the buffered
     partial packet is rolled back, a hole is reported, and
     packetpeek consumes the hole just like packetout */
  {
    long sizes[]={70000,50};
    page_list L=encode_packets(sizes,2,73,1);
    dual d;
    ogg_packet o1,o2;
    ogg_source_info info;
    int i;

    assert(L.count==3);
    reset_file();
    for(i=0;i<3;i++)lay_page(&L.pages[i]);

    dual_init(&d,73);
    dual_pagein(&d,&L.pages[0]);
    assert(dual_drain(&d)==0); /* partial packet buffered */

    /* pages[1] is lost; pages[2] completes a different packet */
    dual_pagein(&d,&L.pages[2]);

    /* peek reports the hole and, as in the original interface,
       advances past it */
    assert(ogg_source_stream_packetpeek(d.ss,&o1,&info)==-1);
    assert(info.spans==NULL && info.count==0);
    assert(ogg_stream_packetpeek(&d.dec,&o2)==-1);

    /* the hole is gone now; peek sees the packet without consuming */
    assert(ogg_source_stream_packetpeek(d.ss,&o1,&info)==1);
    assert(o1.bytes==50);
    assert(info.count==1);
    assert(info.spans[0].offset==L.pages[2].offset+L.pages[2].header_len);
    assert(ogg_source_stream_packetpeek(d.ss,&o1,&info)==1);

    assert(ogg_source_stream_packetout(d.ss,&o1,&info)==1);
    assert(ogg_stream_packetout(&d.dec,&o2)==1);
    assert(o1.bytes==50);
    assert(o1.packetno==o2.packetno);
    /* the rolled-back partial packet leaves no source behind */
    assert(info.count==1);
    assert(info.spans[0].offset==L.pages[2].offset+L.pages[2].header_len);
    assert(info.spans[0].bytes==50);
    validate_spans(&o1,&info);
    dual_free(&d);
    free_page_list(&L);
  }

  printf("SOURCE_HOLES_OK\n");
}

static void test_peek_reset(void){
  long sizes[]={300,400,0,70000};
  page_list L=encode_packets(sizes,4,73,1);
  dual d;
  ogg_packet o1,o2;
  ogg_source_info i1,i2;
  int i;

  assert(L.count==5); /* 300, 400, nil, then the 70000 in two pages */
  reset_file();
  for(i=0;i<L.count;i++)lay_page(&L.pages[i]);

  dual_init(&d,73);
  for(i=0;i<L.count;i++)dual_pagein(&d,&L.pages[i]);

  /* peek reports a complete packet without consuming it */
  assert(ogg_source_stream_packetpeek(d.ss,&o1,&i1)==1);
  assert(o1.bytes==300);
  assert(i1.count==1);
  assert(ogg_source_stream_packetpeek(d.ss,&o1,&i2)==1);
  assert(i2.count==i1.count);
  assert(i2.spans[0].offset==i1.spans[0].offset);
  assert(i2.spans[0].bytes==i1.spans[0].bytes);
  assert(ogg_source_stream_packetout(d.ss,&o1,&i1)==1);
  assert(o1.bytes==300 && o1.packetno==0);
  validate_spans(&o1,&i1);
  assert(ogg_stream_packetout(&d.dec,&o2)==1);

  /* peek with a NULL packet pointer still reports the source and
     still does not consume */
  assert(ogg_source_stream_packetpeek(d.ss,NULL,&i1)==1);
  assert(i1.count==1);
  assert(i1.spans[0].bytes==400);
  assert(ogg_source_stream_packetpeek(d.ss,&o1,&i2)==1);
  assert(o1.bytes==400);
  /* packetout with a NULL packet pointer consumes as usual, and the
     source of the consumed packet is still available */
  assert(ogg_source_stream_packetout(d.ss,NULL,&i1)==1);
  assert(i1.count==1);
  assert(i1.spans[0].bytes==400);
  assert(ogg_stream_packetout(&d.dec,&o2)==1);
  assert(o2.bytes==400);

  /* a NULL info pointer is fine too (nil packet here) */
  assert(ogg_source_stream_packetout(d.ss,&o1,NULL)==1);
  assert(o1.bytes==0);
  assert(ogg_stream_packetout(&d.dec,&o2)==1);

  /* both NULL: plain consumption */
  assert(ogg_source_stream_packetout(d.ss,NULL,NULL)==1);
  assert(ogg_stream_packetout(&d.dec,&o2)==1);
  assert(o2.bytes==70000);

  /* stream is empty now; 0 comes with cleared source */
  assert(ogg_source_stream_packetpeek(d.ss,NULL,&i1)==0);
  assert(i1.spans==NULL && i1.count==0);
  assert(ogg_source_stream_packetout(d.ss,&o1,&i1)==0);
  assert(i1.spans==NULL && i1.count==0);

  /* reset releases all tracking state and re-arms the stream */
  assert(ogg_source_stream_reset(d.ss,77)==0);
  assert(ogg_stream_reset_serialno(&d.dec,77)==0);
  assert(ogg_source_stream_packetout(d.ss,&o1,&i1)==0);
  assert(i1.spans==NULL && i1.count==0);
  {
    long s2[]={42,70000};
    page_list L2=encode_packets(s2,2,77,1);
    assert(L2.count==3);
    reset_file();
    for(i=0;i<L2.count;i++)lay_page(&L2.pages[i]);
    dual_pagein(&d,&L2.pages[0]);
    assert(dual_drain(&d)==1);
    dual_pagein(&d,&L2.pages[1]);
    assert(dual_drain(&d)==0);
    dual_pagein(&d,&L2.pages[2]);
    assert(ogg_source_stream_packetout(d.ss,&o1,&i1)==1);
    assert(ogg_stream_packetout(&d.dec,&o2)==1);
    assert(o1.bytes==70000);
    assert(o1.packetno==1 && o2.packetno==1); /* numbering restarted */
    assert(i1.count==2);
    validate_spans(&o1,&i1);
    free_page_list(&L2);
  }

  dual_free(&d);
  free_page_list(&L);
  printf("SOURCE_PEEK_RESET_OK\n");
}

static void test_offsets(void){
  long sizes[]={300,400};
  page_list L=encode_packets(sizes,2,73,1);
  ogg_source_stream *ss;
  ogg_packet o1;
  ogg_source_info info;
  ogg_page pg;
  int i;

  assert(L.count==2);
  reset_file();
  for(i=0;i<2;i++)lay_page(&L.pages[i]);

  ss=ogg_source_stream_new(73);
  assert(ss);

  /* a negative offset is rejected before any state change */
  pg=page_of(&L.pages[0]);
  assert(ogg_source_stream_pagein(ss,&pg,-1)==-1);

  /* an offset whose header+body extent would pass the signed 64 bit
     limit is rejected as well */
  assert(ogg_source_stream_pagein(ss,&pg,
         INT64_MAX_LOCAL-(pg.header_len+pg.body_len)+1)==-1);

  /* a page from another serial is rejected and adds no source */
  {
    long fs[]={9};
    page_list F=encode_packets(fs,1,99,1);
    ogg_page fg=page_of(&F.pages[0]);
    assert(ogg_source_stream_pagein(ss,&fg,0)==-1);
    free_page_list(&F);
  }

  /* a nonzero page version is rejected and adds no source */
  {
    unsigned char *h=malloc(L.pages[0].header_len);
    ogg_page vg;
    assert(h);
    memcpy(h,L.pages[0].header,L.pages[0].header_len);
    h[4]=1; /* version */
    vg.header=h;
    vg.header_len=L.pages[0].header_len;
    vg.body=L.pages[0].body;
    vg.body_len=L.pages[0].body_len;
    assert(ogg_source_stream_pagein(ss,&vg,0)==-1);
    free(h);
  }

  /* none of the rejected pages produced anything */
  assert(ogg_source_stream_packetout(ss,&o1,&info)==0);
  assert(info.spans==NULL && info.count==0);

  /* the stream still decodes afterwards */
  assert(ogg_source_stream_pagein(ss,&pg,L.pages[0].offset)==0);

  /* rejected offsets stay rejected between valid pages */
  pg=page_of(&L.pages[1]);
  assert(ogg_source_stream_pagein(ss,&pg,INT64_MAX_LOCAL)==-1);
  assert(ogg_source_stream_pagein(ss,&pg,-100)==-1);
  assert(ogg_source_stream_pagein(ss,&pg,L.pages[1].offset)==0);

  assert(ogg_source_stream_packetout(ss,&o1,&info)==1);
  assert(o1.bytes==300);
  assert(o1.packetno==0);
  validate_spans(&o1,&info);
  assert(ogg_source_stream_packetout(ss,&o1,&info)==1);
  assert(o1.bytes==400);
  assert(o1.packetno==1);
  validate_spans(&o1,&info);
  assert(ogg_source_stream_packetout(ss,&o1,&info)==0);
  ogg_source_stream_free(ss);

  /* an offset ending exactly at the int64 limit is legal */
  ss=ogg_source_stream_new(73);
  assert(ss);
  pg=page_of(&L.pages[0]);
  {
    ogg_int64_t off=INT64_MAX_LOCAL-(pg.header_len+pg.body_len);
    assert(ogg_source_stream_pagein(ss,&pg,off)==0);
    assert(ogg_source_stream_packetout(ss,&o1,&info)==1);
    assert(info.count==1);
    assert(info.spans[0].offset==off+pg.header_len);
    assert(info.spans[0].bytes==300);
  }
  ogg_source_stream_free(ss);

  free_page_list(&L);
  printf("SOURCE_OFFSETS_OK\n");
}

/* write the sample file: a single 70000 byte packet on two pages */
static void write_sample(const char *path){
  long s[]={70000};
  page_list L=encode_packets(s,1,73,0);
  FILE *f;
  int i;
  assert(L.count==2);
  f=fopen(path,"wb");
  assert(f);
  for(i=0;i<L.count;i++){
    assert(fwrite(L.pages[i].header,1,L.pages[i].header_len,f)==
           (size_t)L.pages[i].header_len);
    assert(fwrite(L.pages[i].body,1,L.pages[i].body_len,f)==
           (size_t)L.pages[i].body_len);
  }
  fclose(f);
  free_page_list(&L);
}

int main(int argc,char **argv){
  if(argc>1)write_sample(argv[1]);
  test_reassembly();
  test_holes();
  test_peek_reset();
  test_offsets();
  reset_file();
  return 0;
}
