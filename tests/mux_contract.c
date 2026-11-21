/********************************************************************
 * tests/mux_contract.c

 Contract tests for the multi-stream Ogg multiplexer.  Registered with
 task_support/check.h; there is no main().  Real pages are produced by
 the library's own framer, and every assertion inspects actual output
 bytes, stable ordering and per-track decode results.
 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <ogg/ogg.h>
#include <ogg/mux.h>

#include "../task_support/check.h"

/* ------------------------------------------------------------------ */
/* Page construction helpers                                          */

typedef struct {
  ogg_page *pages;
  int count;
  int cap;
} pagevec;

static int pv_push(pagevec *pv, ogg_page *pg){
  if(pv->count==pv->cap){
    int cap=pv->cap?pv->cap*2:16;
    ogg_page *np=(ogg_page *)realloc(pv->pages,(size_t)cap*sizeof(*np));
    if(!np) return 0;
    pv->pages=np;
    pv->cap=cap;
  }
  pv->pages[pv->count].header=(unsigned char *)malloc((size_t)pg->header_len);
  if(!pv->pages[pv->count].header) return 0;
  pv->pages[pv->count].body=(unsigned char *)malloc(pg->body_len?
                                                   (size_t)pg->body_len:1);
  if(!pv->pages[pv->count].body){
    free(pv->pages[pv->count].header);
    return 0;
  }
  memcpy(pv->pages[pv->count].header,pg->header,(size_t)pg->header_len);
  if(pg->body_len)
    memcpy(pv->pages[pv->count].body,pg->body,(size_t)pg->body_len);
  pv->pages[pv->count].header_len=pg->header_len;
  pv->pages[pv->count].body_len=pg->body_len;
  pv->count++;
  return 1;
}

static void pv_free(pagevec *pv){
  int i;
  for(i=0;i<pv->count;i++){
    free(pv->pages[i].header);
    free(pv->pages[i].body);
  }
  free(pv->pages);
  pv->pages=NULL;
  pv->count=pv->cap=0;
}

/* Publish with the regular encoder until it yields nothing more. */
static int enc_flush(ogg_stream_state *os,pagevec *pv){
  ogg_page pg;
  while(ogg_stream_flush(os,&pg)){
    if(!pv_push(pv,&pg)) return 0;
  }
  return 1;
}

typedef struct {
  unsigned char *data;
  long len;
  ogg_int64_t granule;
  int bos;
  int eos;
} packetdesc;

static int packet_fill(packetdesc *pd,long len,ogg_int64_t granule,
                       int bos,int eos,unsigned seed){
  long i;
  pd->data=(unsigned char *)malloc(len?(size_t)len:1);
  if(!pd->data) return 0;
  for(i=0;i<len;i++)
    pd->data[i]=(unsigned char)((seed*37u+(unsigned)i*11u+7u)&0xffu);
  pd->len=len;
  pd->granule=granule;
  pd->bos=bos;
  pd->eos=eos;
  return 1;
}

/* Frame one track from an ordered packet list. */
static int frame_track(int serial,const packetdesc *pd,int n,pagevec *out){
  ogg_stream_state os;
  ogg_packet op;
  int i,ok=1;
  if(ogg_stream_init(&os,serial)!=0) return 0;
  for(i=0;i<n && ok;i++){
    memset(&op,0,sizeof(op));
    op.packet=pd[i].data;
    op.bytes=pd[i].len;
    op.b_o_s=pd[i].bos;
    op.e_o_s=pd[i].eos;
    op.granulepos=pd[i].granule;
    if(ogg_stream_packetin(&os,&op)!=0) ok=0;
    else ok=enc_flush(&os,out);
  }
  ogg_stream_clear(&os);
  return ok;
}

static void packet_free(packetdesc *pd){
  free(pd->data);
  pd->data=NULL;
}

/* ------------------------------------------------------------------ */
/* Collected output                                                   */

typedef struct {
  unsigned char **bytes;      /* each entry is header followed by body */
  long *len;
  int count;
  int cap;
} bytevec;

static int bv_take(bytevec *bv,unsigned char *owned,long n){
  if(bv->count==bv->cap){
    int cap=bv->cap?bv->cap*2:16;
    unsigned char **nb=(unsigned char **)realloc(bv->bytes,
                                                 (size_t)cap*sizeof(*nb));
    long *nl;
    if(!nb) return 0;
    bv->bytes=nb;
    nl=(long *)realloc(bv->len,(size_t)cap*sizeof(*nl));
    if(!nl) return 0;
    bv->len=nl;
    bv->cap=cap;
  }
  bv->bytes[bv->count]=owned;
  bv->len[bv->count]=n;
  bv->count++;
  return 1;
}

static void bv_free(bytevec *bv){
  int i;
  for(i=0;i<bv->count;i++) free(bv->bytes[i]);
  free(bv->bytes);
  free(bv->len);
  memset(bv,0,sizeof(*bv));
}

/* Drain every currently safe page; returns count delivered or -1. */
static int drain(ogg_mux_t *mux,bytevec *out){
  ogg_page pg;
  int n=0;
  for(;;){
    ogg_mux_result_t r=ogg_mux_next_page(mux,&pg);
    unsigned char *join;
    long total;
    if(r==OGG_MUX_AGAIN) return n;
    if(r!=OGG_MUX_OK) return -1;
    total=pg.header_len+pg.body_len;
    join=(unsigned char *)malloc(total?(size_t)total:1);
    if(!join) return -1;
    memcpy(join,pg.header,(size_t)pg.header_len);
    if(pg.body_len) memcpy(join+pg.header_len,pg.body,(size_t)pg.body_len);
    if(!bv_take(out,join,total)){
      free(join);
      return -1;
    }
    n++;
  }
}

static int bytes_are_page(const unsigned char *p,long n,const ogg_page *pg){
  if(n!=pg->header_len+pg->body_len) return 0;
  if(memcmp(p,pg->header,(size_t)pg->header_len)) return 0;
  return memcmp(p+pg->header_len,pg->body,(size_t)pg->body_len)==0;
}

static int bv_has_page(const bytevec *bv,const ogg_page *pg){
  int i;
  for(i=0;i<bv->count;i++)
    if(bytes_are_page(bv->bytes[i],bv->len[i],pg)) return 1;
  return 0;
}

static int bv_serial_at(const bytevec *bv,int index){
  if(index<0 || index>=bv->count) return -1;
  return ogg_page_serialno(&(ogg_page){bv->bytes[index],0,NULL,0});
}

/* Decode all delivered pages for one serial, checking payload, granule,
   EOS flag, and contiguous page numbering for that track. */
static int decode_and_match(const bytevec *bv,int serial,
                            const packetdesc *pd,int npkts){
  ogg_stream_state os;
  ogg_packet op;
  long last_pageno=-1;
  int i,got=0;
  if(ogg_stream_init(&os,serial)!=0) return 0;
  for(i=0;i<bv->count;i++){
    ogg_page pg;
    pg.header=bv->bytes[i];
    if(bv->len[i]<27 || memcmp(pg.header,"OggS",4)) continue;
    pg.header_len=pg.header[26]+27L;
    if(pg.header_len>bv->len[i]){
      ogg_stream_clear(&os);
      return 0;
    }
    pg.body=pg.header+pg.header_len;
    pg.body_len=bv->len[i]-pg.header_len;
    if(ogg_page_serialno(&pg)!=serial) continue;
    if(last_pageno>=0 && ogg_page_pageno(&pg)!=last_pageno+1){
      ogg_stream_clear(&os);
      return 0;
    }
    last_pageno=ogg_page_pageno(&pg);
    if(ogg_stream_pagein(&os,&pg)!=0){ ogg_stream_clear(&os); return 0; }
    for(;;){
      int r=ogg_stream_packetout(&os,&op);
      if(r==0) break;
      if(r<0){ ogg_stream_clear(&os); return 0; }
      if(got>=npkts){ ogg_stream_clear(&os); return 0; }
      if(op.bytes!=pd[got].len){ ogg_stream_clear(&os); return 0; }
      if(pd[got].len>0 &&
         memcmp(op.packet,pd[got].data,(size_t)pd[got].len)!=0){
        ogg_stream_clear(&os);
        return 0;
      }
      if(op.granulepos!=pd[got].granule){ ogg_stream_clear(&os); return 0; }
      if(!!op.e_o_s!=!!pd[got].eos){ ogg_stream_clear(&os); return 0; }
      got++;
    }
  }
  ogg_stream_clear(&os);
  return got==npkts;
}

/* ------------------------------------------------------------------ */
/* Ordering                                                           */

/* Arrival order must not decide interleave order. */
static int case_arrival_vs_time(void){
  int serials[2]={4101,4102};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[3],p1[3];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;
  int i;

  packet_fill(&p0[0],4,0,1,0,1);
  packet_fill(&p0[1],4,100,0,0,2);
  packet_fill(&p0[2],4,200,0,1,3);
  packet_fill(&p1[0],4,0,1,0,4);
  packet_fill(&p1[1],4,50,0,0,5);
  packet_fill(&p1[2],4,150,0,1,6);
  TASK_CHECK(frame_track(serials[0],p0,3,&v0));
  TASK_CHECK(frame_track(serials[1],p1,3,&v1));
  TASK_CHECK(v0.count==3 && v1.count==3);

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);

  /* Deliver track 1's BOS first; nothing may leave until track 0's BOS
     also arrives. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_AGAIN);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);

  /* Media arrives in reverse-ish order of time. */
  for(i=1;i<3;i++) TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[i])==OGG_MUX_OK);
  for(i=1;i<3;i++) TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[i])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],1000000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],1000000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);

  TASK_CHECK(drain(mux,&out)==6);
  /* BOS pages come out in registration order. */
  TASK_CHECK(bv_serial_at(&out,0)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,1)==serials[1]);
  /* Then increasing time: 1@50, 0@100, 1@150, 0@200. */
  TASK_CHECK(bv_serial_at(&out,2)==serials[1]);
  TASK_CHECK(bv_serial_at(&out,3)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,4)==serials[1]);
  TASK_CHECK(bv_serial_at(&out,5)==serials[0]);
  TASK_CHECK(bv_has_page(&out,&v0.pages[1]));
  TASK_CHECK(bv_has_page(&out,&v0.pages[2]));
  TASK_CHECK(bv_has_page(&out,&v1.pages[1]));
  TASK_CHECK(bv_has_page(&out,&v1.pages[2]));
  TASK_CHECK(decode_and_match(&out,serials[0],p0,3));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,3));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  for(i=0;i<3;i++){ packet_free(&p0[i]); packet_free(&p1[i]); }
  return 0;
}

/* Equal scheduling times break by registration order. */
static int case_equal_time_registration_order(void){
  int serials[2]={4151,4152};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[2],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;

  packet_fill(&p0[0],3,0,1,0,7);
  packet_fill(&p0[1],3,42,0,1,8);
  packet_fill(&p1[0],3,0,1,0,9);
  packet_fill(&p1[1],3,42,0,1,10);
  TASK_CHECK(frame_track(serials[0],p0,2,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  /* Submit track 1's equal-time page first to prove arrival is ignored. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],100)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],100)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);

  TASK_CHECK(drain(mux,&out)==4);
  TASK_CHECK(bv_serial_at(&out,2)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,3)==serials[1]);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,2));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* granule 0 on a non-BOS page is time zero, not "unknown". */
static int case_zero_granule_is_time_zero(void){
  int serials[2]={4201,4202};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[2],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;

  packet_fill(&p0[0],3,0,1,0,10);
  packet_fill(&p0[1],3,0,0,1,11);
  packet_fill(&p1[0],3,0,1,0,12);
  packet_fill(&p1[1],3,5,0,1,13);
  TASK_CHECK(frame_track(serials[0],p0,2,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],100)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],100)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);

  TASK_CHECK(drain(mux,&out)==4);
  TASK_CHECK(bv_serial_at(&out,2)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,3)==serials[1]);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,2));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* Different rational clocks, granules near 2^62, compared exactly. */
static int case_rational_clock_exact(void){
  int serials[2]={4301,4302};
  ogg_int64_t num[2]={3,7};
  ogg_int64_t den[2]={5,11};
  packetdesc p0[2],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;
  ogg_int64_t g=(ogg_int64_t)1<<62;

  packet_fill(&p0[0],2,0,1,0,20);
  packet_fill(&p0[1],2,g,0,1,21);
  packet_fill(&p1[0],2,0,1,0,22);
  packet_fill(&p1[1],2,g,0,1,23);
  TASK_CHECK(frame_track(serials[0],p0,2,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  /* Arrival: later track first.  Time: 3/5 < 7/11, so 0 then 1. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],INT64_MAX)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],INT64_MAX)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);

  TASK_CHECK(drain(mux,&out)==4);
  TASK_CHECK(bv_serial_at(&out,2)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,3)==serials[1]);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,2));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* Huge num/den with huge granule: ordering must not wrap. */
static int case_overflow_safe_compare(void){
  int serials[2]={4401,4402};
  ogg_int64_t num[2]={INT64_MAX,INT64_MAX-1};
  ogg_int64_t den[2]={INT64_MAX-1,INT64_MAX};
  packetdesc p0[2],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;
  ogg_int64_t g=INT64_MAX/4;

  packet_fill(&p0[0],2,0,1,0,30);
  packet_fill(&p0[1],2,g,0,1,31);
  packet_fill(&p1[0],2,0,1,0,32);
  packet_fill(&p1[1],2,g,0,1,33);
  TASK_CHECK(frame_track(serials[0],p0,2,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],INT64_MAX)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],INT64_MAX)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);

  TASK_CHECK(drain(mux,&out)==4);
  /* Track 0's ratio exceeds 1; track 1's is below 1, so track 1 first
     even though its page arrived last. */
  TASK_CHECK(bv_serial_at(&out,2)==serials[1]);
  TASK_CHECK(bv_serial_at(&out,3)==serials[0]);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,2));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* A continued packet's -1 page joins the next anchored page as one
   group: same scheduling time, original granule preserved, and the
   open group blocks all non-BOS output. */
static int case_unanchored_group(void){
  int serials[2]={4501,4502};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[3],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;
  int i;

  packet_fill(&p0[0],3,0,1,0,40);
  packet_fill(&p0[1],700,-1,0,0,41);  /* spans, no completed packet */
  packet_fill(&p0[2],2,10,0,1,42);
  packet_fill(&p1[0],3,0,1,0,43);
  packet_fill(&p1[1],3,5,0,1,44);
  TASK_CHECK(frame_track(serials[0],p0,3,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));
  TASK_CHECK(v0.count==3 && v1.count==2);
  TASK_CHECK(ogg_page_granulepos(&(ogg_page){
      v0.pages[1].header,v0.pages[1].header_len,
      v0.pages[1].body,v0.pages[1].body_len})==-1);

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  /* Watermarks sit below both pending times (5 and the group's 10). */
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],0)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],0)==OGG_MUX_OK);

  /* Track 0 holds an unanchored page, so only the two BOS pages may
     leave even though track 1's page is ready. */
  TASK_CHECK(drain(mux,&out)==2);
  TASK_CHECK(bv_serial_at(&out,0)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,1)==serials[1]);

  /* The anchor closes the group; the -1 page and the anchor schedule at
     10, after track 1's page at 5, and come out in their own order. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[2])==OGG_MUX_OK);
  /* That anchor is also track 0's EOS, so both tracks are finished and
     no watermark is needed any more. */
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==3);
  TASK_CHECK(out.count==5);
  TASK_CHECK(bv_serial_at(&out,2)==serials[1]);
  TASK_CHECK(bv_serial_at(&out,3)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,4)==serials[0]);
  TASK_CHECK(bytes_are_page(out.bytes[3],out.len[3],&v0.pages[1]));
  TASK_CHECK(bytes_are_page(out.bytes[4],out.len[4],&v0.pages[2]));
  TASK_CHECK(decode_and_match(&out,serials[0],p0,3));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));

  /* The written -1 page is byte-identical to the submitted one. */
  for(i=0;i<out.count;i++){
    ogg_page pg;
    pg.header=out.bytes[i];
    if(bv_serial_at(&out,i)==serials[0] &&
       ogg_page_pageno(&pg)==ogg_page_pageno(&(ogg_page){
         v0.pages[1].header,v0.pages[1].header_len,
         v0.pages[1].body,v0.pages[1].body_len})){
      TASK_CHECK(ogg_page_granulepos(&pg)==-1);
      TASK_CHECK(bytes_are_page(out.bytes[i],out.len[i],&v0.pages[1]));
    }
  }

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  for(i=0;i<3;i++) packet_free(&p0[i]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Frontier                                                           */

/* Media waits until every unfinished track promises a watermark no
   earlier than the candidate; equality is sufficient. */
static int case_wait_for_equal_frontier(void){
  int serials[2]={4601,4602};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[3],p1[3];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;

  packet_fill(&p0[0],3,0,1,0,50);
  packet_fill(&p0[1],3,10,0,0,51);
  packet_fill(&p0[2],3,30,0,1,52);
  packet_fill(&p1[0],3,0,1,0,53);
  packet_fill(&p1[1],3,20,0,0,54);
  packet_fill(&p1[2],3,40,0,1,55);
  TASK_CHECK(frame_track(serials[0],p0,3,&v0));
  TASK_CHECK(frame_track(serials[1],p1,3,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==2);            /* the two BOS pages */

  /* Track 0 promises 10; track 1 has said nothing. */
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],10)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_AGAIN);
  /* A watermark below the candidate does not help. */
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],9)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_AGAIN);
  /* Equality releases track 0's page (time 10). */
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],10)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==1);
  TASK_CHECK(out.count==3);
  TASK_CHECK(bv_serial_at(&out,2)==serials[0]);
  TASK_CHECK(bytes_are_page(out.bytes[2],out.len[2],&v0.pages[1]));

  /* Track 1's page at 20 waits on both unfinished tracks: track 0 has
     promised only 10, and track 1 itself has promised only 10. */
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_AGAIN);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],20)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_AGAIN);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],20)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==1);
  TASK_CHECK(out.count==4);
  TASK_CHECK(bv_serial_at(&out,3)==serials[1]);

  /* Lowering a watermark is refused; equal is idempotent. */
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],19)==OGG_MUX_INVALID);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],20)==OGG_MUX_OK);

  /* Finish both tracks; the EOS pages then flow. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[2])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[2])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==2);
  TASK_CHECK(out.count==6);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,3));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,3));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]); packet_free(&p0[2]);
  packet_free(&p1[0]); packet_free(&p1[1]); packet_free(&p1[2]);
  return 0;
}

/* A slow track's EOS lifts its watermark to infinity and unblocks the
   rest without waiting for more commitments. */
static int case_slow_track_eos(void){
  int serials[2]={4701,4702};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[3],p1[3];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;

  packet_fill(&p0[0],3,0,1,0,60);
  packet_fill(&p0[1],3,10,0,0,61);
  packet_fill(&p0[2],3,20,0,1,62);
  packet_fill(&p1[0],3,0,1,0,63);
  packet_fill(&p1[1],3,100,0,0,64);
  packet_fill(&p1[2],3,110,0,1,65);
  TASK_CHECK(frame_track(serials[0],p0,3,&v0));
  TASK_CHECK(frame_track(serials[1],p1,3,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==2);

  /* Track 0 promises 10, track 1 is silent and unfinished. */
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],10)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_AGAIN);

  /* Track 1's EOS arrives with time 110; its EOS makes it infinite. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[2])==OGG_MUX_OK);
  /* Track 1's own watermark requirement is gone, but track 0 is still
     unfinished and has promised only 10, while the earliest candidate
     is track 0's page at 10: that one is safe and is delivered. */
  TASK_CHECK(drain(mux,&out)==1);
  TASK_CHECK(out.count==3);
  TASK_CHECK(bv_serial_at(&out,2)==serials[0]);
  /* Track 1's pages at 100/110 still need track 0's promise >= 100.
     Track 0 cannot promise 100 and then submit its page at 20, so its
     EOS must arrive first; that makes track 0 infinite too. */
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_AGAIN);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[2])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  /* Methodically 0@20, then 1@100, 1@110. */
  TASK_CHECK(drain(mux,&out)==3);
  TASK_CHECK(out.count==6);
  TASK_CHECK(bv_serial_at(&out,3)==serials[0]);
  TASK_CHECK(bv_serial_at(&out,4)==serials[1]);
  TASK_CHECK(bv_serial_at(&out,5)==serials[1]);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,3));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,3));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]); packet_free(&p0[2]);
  packet_free(&p1[0]); packet_free(&p1[1]); packet_free(&p1[2]);
  return 0;
}

/* Rejected frontier/granule submissions leave the queue and watermark
   intact; a corrected page for the same page number is accepted. */
static int case_rejected_submissions_preserve_state(void){
  int serials[2]={4801,4802};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc good[3],fbad[3],abad[3],b[3];
  pagevec vg={0},vf={0},va={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;
  int i;

  packet_fill(&good[0],3,0,1,0,70);
  packet_fill(&good[1],10,10,0,0,71);
  packet_fill(&good[2],2,40,0,1,72);
  packet_fill(&fbad[0],3,0,1,0,70);
  packet_fill(&fbad[1],10,10,0,0,71);
  packet_fill(&fbad[2],2,30,0,1,72);   /* time equals the watermark */
  packet_fill(&abad[0],3,0,1,0,70);
  packet_fill(&abad[1],10,10,0,0,71);
  packet_fill(&abad[2],2,8,0,1,72);    /* anchor steps backwards */
  packet_fill(&b[0],3,0,1,0,73);
  packet_fill(&b[1],3,5,0,0,74);
  packet_fill(&b[2],3,50,0,1,75);

  TASK_CHECK(frame_track(serials[0],good,3,&vg));
  TASK_CHECK(frame_track(serials[0],fbad,3,&vf));
  TASK_CHECK(frame_track(serials[0],abad,3,&va));
  TASK_CHECK(frame_track(serials[1],b,3,&v1));
  TASK_CHECK(ogg_page_pageno(&vf.pages[2])==ogg_page_pageno(&vg.pages[2]));
  TASK_CHECK(ogg_page_pageno(&va.pages[2])==ogg_page_pageno(&vg.pages[2]));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],5)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],5)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[1])==OGG_MUX_OK);

  /* A backwards anchor on the next page number is rejected, leaving no
     trace; the corrected page with that same page number is taken. */
  TASK_CHECK(ogg_mux_submit_page(mux,&va.pages[2])==OGG_MUX_BAD_GRANULE);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[2])==OGG_MUX_OK);

  /* On a fresh pair, a page whose time equals the committed watermark
     is rejected; a later page with the same page number is accepted. */
  ogg_mux_destroy(mux);
  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],5)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],5)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],30)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],29)==OGG_MUX_INVALID);
  TASK_CHECK(ogg_mux_bytes(mux)==
             (size_t)(vg.pages[0].header_len+vg.pages[0].body_len+
                      vg.pages[1].header_len+vg.pages[1].body_len+
                      v1.pages[0].header_len+v1.pages[0].body_len));
  TASK_CHECK(ogg_mux_submit_page(mux,&vf.pages[2])==OGG_MUX_FRONTIER);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[2])==OGG_MUX_OK);

  /* Track B's page at time 5 must be submitted before its watermark is
     raised past 5. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_FRONTIER);
  ogg_mux_destroy(mux);

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[2])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&vg.pages[2])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==6);
  TASK_CHECK(decode_and_match(&out,serials[0],good,3));
  TASK_CHECK(decode_and_match(&out,serials[1],b,3));
  for(i=0;i<3;i++) TASK_CHECK(bv_has_page(&out,&vg.pages[i]));
  for(i=0;i<3;i++) TASK_CHECK(bv_has_page(&out,&v1.pages[i]));

  ogg_mux_destroy(mux);
  pv_free(&vg); pv_free(&vf); pv_free(&va); pv_free(&v1);
  bv_free(&out);
  for(i=0;i<3;i++){
    packet_free(&good[i]); packet_free(&fbad[i]); packet_free(&abad[i]);
    packet_free(&b[i]);
  }
  return 0;
}

/* end_input without every EOS reports incomplete, not a successful
   drain, and the instance stays usable. */
static int case_end_input_incomplete(void){
  int serials[2]={4901,4902};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[1],p1[2];
  pagevec v0={0},v1={0};
  ogg_mux_t *mux;

  packet_fill(&p0[0],3,0,1,0,80);
  packet_fill(&p1[0],3,0,1,0,81);
  packet_fill(&p1[1],3,5,0,1,82);
  TASK_CHECK(frame_track(serials[0],p0,1,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_INCOMPLETE);
  /* The failed close did not lock the instance. */
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],10)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_next_page(mux,&(ogg_page){0})==OGG_MUX_OK);

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1);
  packet_free(&p0[0]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Ownership                                                          */

/* Accepted pages are copied, so the caller may reuse its buffer at
   once. */
static int case_submit_copies_input(void){
  int serials[2]={5001,5002};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[3],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;
  unsigned char *scratch;
  long scratch_len=0;
  int i;

  packet_fill(&p0[0],3,0,1,0,90);
  packet_fill(&p0[1],8,10,0,0,91);
  packet_fill(&p0[2],2,20,0,1,92);
  packet_fill(&p1[0],3,0,1,0,93);
  packet_fill(&p1[1],2,100,0,1,94);
  TASK_CHECK(frame_track(serials[0],p0,3,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  for(i=0;i<v0.count;i++)
    if(v0.pages[i].header_len+v0.pages[i].body_len>scratch_len)
      scratch_len=v0.pages[i].header_len+v0.pages[i].body_len;
  for(i=0;i<v1.count;i++)
    if(v1.pages[i].header_len+v1.pages[i].body_len>scratch_len)
      scratch_len=v1.pages[i].header_len+v1.pages[i].body_len;
  scratch=(unsigned char *)malloc((size_t)scratch_len);
  TASK_CHECK(scratch!=NULL);

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);

#define SUBMIT_VIA_SCRATCH(v,idx) do { \
    ogg_page pg_; \
    pg_.header=scratch; pg_.header_len=(v).pages[(idx)].header_len; \
    pg_.body=scratch+(v).pages[(idx)].header_len; \
    pg_.body_len=(v).pages[(idx)].body_len; \
    memcpy(scratch,(v).pages[(idx)].header,(size_t)(v).pages[(idx)].header_len); \
    memcpy(scratch+(v).pages[(idx)].header_len,(v).pages[(idx)].body, \
           (size_t)(v).pages[(idx)].body_len); \
    TASK_CHECK(ogg_mux_submit_page(mux,&pg_)==OGG_MUX_OK); \
    memset(scratch,0xEE,(size_t)scratch_len); \
  } while(0)

  for(i=0;i<v0.count;i++) SUBMIT_VIA_SCRATCH(v0,i);
  for(i=0;i<v1.count;i++) SUBMIT_VIA_SCRATCH(v1,i);

  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==5);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,3));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));
  for(i=0;i<v0.count;i++) TASK_CHECK(bv_has_page(&out,&v0.pages[i]));
  for(i=0;i<v1.count;i++) TASK_CHECK(bv_has_page(&out,&v1.pages[i]));
#undef SUBMIT_VIA_SCRATCH

  ogg_mux_destroy(mux);
  free(scratch);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  for(i=0;i<3;i++) packet_free(&p0[i]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* Capacity is enforced on held header+body bytes, including unanchored
   pages; rejection consumes nothing and a retry after raising the limit
   yields the original stream. */
static int case_capacity_reject_retry(void){
  int serials[1]={5101};
  ogg_int64_t num[1]={1},den[1]={1};
  packetdesc p[2];
  pagevec pv={0};
  bytevec out={0};
  ogg_mux_t *mux;
  long first_len;

  packet_fill(&p[0],3,0,1,0,100);
  packet_fill(&p[1],2000,10,0,1,101);
  TASK_CHECK(frame_track(serials[0],p,2,&pv));
  TASK_CHECK(pv.count==2);
  first_len=pv.pages[0].header_len+pv.pages[0].body_len;

  mux=ogg_mux_create(1,serials,num,den,(size_t)first_len+8);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_bytes(mux)==(size_t)first_len);

  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[1])==OGG_MUX_CAPACITY);
  TASK_CHECK(ogg_mux_bytes(mux)==(size_t)first_len);
  /* Lowering below held bytes is refused. */
  TASK_CHECK(ogg_mux_set_limit(mux,1)==OGG_MUX_CAPACITY);
  TASK_CHECK(ogg_mux_set_limit(mux,(size_t)first_len)==OGG_MUX_OK);
  /* The identical page is accepted after raising the limit. */
  TASK_CHECK(ogg_mux_set_limit(mux,1<<20)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==2);
  TASK_CHECK(bytes_are_page(out.bytes[0],out.len[0],&pv.pages[0]));
  TASK_CHECK(bytes_are_page(out.bytes[1],out.len[1],&pv.pages[1]));
  TASK_CHECK(decode_and_match(&out,serials[0],p,2));

  ogg_mux_destroy(mux);
  pv_free(&pv); bv_free(&out);
  packet_free(&p[0]); packet_free(&p[1]);
  return 0;
}

/* An unanchored page also counts toward the byte limit even though it
   cannot be scheduled yet. */
static int case_capacity_counts_unanchored(void){
  int serials[1]={5151};
  ogg_int64_t num[1]={1},den[1]={1};
  packetdesc p[3];
  pagevec pv={0};
  ogg_mux_t *mux;

  packet_fill(&p[0],3,0,1,0,101);
  packet_fill(&p[1],600,-1,0,0,102);
  packet_fill(&p[2],2,10,0,1,103);
  TASK_CHECK(frame_track(serials[0],p,3,&pv));
  TASK_CHECK(pv.count==3);

  /* Enough for BOS plus the -1 page, not the anchoring page. */
  mux=ogg_mux_create(1,serials,num,den,(size_t)(pv.pages[0].header_len+
      pv.pages[0].body_len+pv.pages[1].header_len+pv.pages[1].body_len));
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[2])==OGG_MUX_CAPACITY);
  TASK_CHECK(ogg_mux_set_limit(mux,(1<<20))==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[2])==OGG_MUX_OK);

  ogg_mux_destroy(mux);
  pv_free(&pv);
  packet_free(&p[0]); packet_free(&p[1]); packet_free(&p[2]);
  return 0;
}

/* The borrowed page is valid until the next state change. */
static int case_borrowed_page_lifetime(void){
  int serials[1]={5201};
  ogg_int64_t num[1]={1},den[1]={1};
  packetdesc p[2];
  pagevec pv={0};
  ogg_mux_t *mux;
  ogg_page pg;

  packet_fill(&p[0],3,0,1,0,110);
  packet_fill(&p[1],3,10,0,1,111);
  TASK_CHECK(frame_track(serials[0],p,2,&pv));

  mux=ogg_mux_create(1,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);

  TASK_CHECK(ogg_mux_next_page(mux,&pg)==OGG_MUX_OK);
  TASK_CHECK(bytes_are_page((const unsigned char *)pg.header,
                            pg.header_len+pg.body_len,&pv.pages[0]));
  /* Reading again with no state change is not promised, but the next
     fetch must produce a correct second page. */
  TASK_CHECK(ogg_mux_next_page(mux,&pg)==OGG_MUX_OK);
  TASK_CHECK(bytes_are_page((const unsigned char *)pg.header,
                            pg.header_len+pg.body_len,&pv.pages[1]));
  TASK_CHECK(ogg_mux_next_page(mux,&pg)==OGG_MUX_AGAIN);

  ogg_mux_destroy(mux);
  pv_free(&pv);
  packet_free(&p[0]); packet_free(&p[1]);
  return 0;
}

/* Registration rules and malformed submissions are rejected without
   disturbing the instance. */
static int case_bad_input_recovery(void){
  int dup_serials[2]={5301,5301};
  int serials[2]={5301,5302};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  ogg_int64_t badnum[2]={1,0},badden[2]={1,-1};
  packetdesc p0[2],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;

  TASK_CHECK(ogg_mux_create(2,dup_serials,num,den,1<<20)==NULL);
  TASK_CHECK(ogg_mux_create(2,serials,num,den,0)==NULL);
  TASK_CHECK(ogg_mux_create(2,serials,badnum,den,1<<20)==NULL);
  TASK_CHECK(ogg_mux_create(2,serials,num,badden,1<<20)==NULL);
  TASK_CHECK(ogg_mux_create(0,serials,num,den,1<<20)==NULL);

  packet_fill(&p0[0],3,0,1,0,120);
  packet_fill(&p0[1],3,10,0,1,121);
  packet_fill(&p1[0],3,0,1,0,122);
  packet_fill(&p1[1],3,20,0,1,123);
  TASK_CHECK(frame_track(serials[0],p0,2,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[0])==OGG_MUX_OK);
  {
    ogg_stream_state tmp;
    ogg_packet op;
    ogg_page fresh;
    unsigned char body[3]={1,2,3};
    ogg_stream_init(&tmp,9999);
    memset(&op,0,sizeof(op)); op.packet=body; op.bytes=3; op.b_o_s=1;
    ogg_stream_packetin(&tmp,&op);
    TASK_CHECK(ogg_stream_flush(&tmp,&fresh));
    TASK_CHECK(ogg_mux_submit_page(mux,&fresh)==OGG_MUX_BAD_SERIAL);
    ogg_stream_clear(&tmp);
  }
  /* Track 0's first page must be BOS; a stray non-BOS page is refused. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_NEED_BOS);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[0])==OGG_MUX_BAD_PAGE);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_OK);
  /* Duplicate/replayed page number. */
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_BAD_PAGE);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_BAD_PAGE);
  TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[1])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,7777,1)==OGG_MUX_BAD_SERIAL);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],-1)==OGG_MUX_INVALID);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[1])==OGG_MUX_INVALID);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],2000)==OGG_MUX_INVALID);
  TASK_CHECK(drain(mux,&out)==4);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,2));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* Empty packets survive intact, and a negative granule other than -1 is
   rejected. */
static int case_empty_packets(void){
  int serials[2]={5401,5402};
  ogg_int64_t num[2]={1,1},den[2]={1,1};
  packetdesc p0[3],p1[2];
  pagevec v0={0},v1={0};
  bytevec out={0};
  ogg_mux_t *mux;
  int i;

  packet_fill(&p0[0],3,0,1,0,130);
  packet_fill(&p0[1],0,10,0,0,131);
  packet_fill(&p0[2],3,20,0,1,132);
  packet_fill(&p1[0],3,0,1,0,133);
  packet_fill(&p1[1],0,50,0,1,134);
  TASK_CHECK(frame_track(serials[0],p0,3,&v0));
  TASK_CHECK(frame_track(serials[1],p1,2,&v1));

  mux=ogg_mux_create(2,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  for(i=0;i<v0.count;i++)
    TASK_CHECK(ogg_mux_submit_page(mux,&v0.pages[i])==OGG_MUX_OK);
  for(i=0;i<v1.count;i++)
    TASK_CHECK(ogg_mux_submit_page(mux,&v1.pages[i])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[1],1000)==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==5);
  TASK_CHECK(decode_and_match(&out,serials[0],p0,3));
  TASK_CHECK(decode_and_match(&out,serials[1],p1,2));
  for(i=0;i<v0.count;i++) TASK_CHECK(bv_has_page(&out,&v0.pages[i]));
  for(i=0;i<v1.count;i++) TASK_CHECK(bv_has_page(&out,&v1.pages[i]));

  ogg_mux_destroy(mux);
  pv_free(&v0); pv_free(&v1); bv_free(&out);
  packet_free(&p0[0]); packet_free(&p0[1]); packet_free(&p0[2]);
  packet_free(&p1[0]); packet_free(&p1[1]);
  return 0;
}

/* A continued packet's earlier page with granule -1 followed by a
   non-negative page is delivered as a unit; the -1 page never becomes a
   negative time and never moves before its own track's BOS.  Also
   checks that an EOS granule may be huge and does not disturb order. */
static int case_continued_and_huge_eos(void){
  int serials[1]={5501};
  ogg_int64_t num[1]={5};
  ogg_int64_t den[1]={3};
  packetdesc p[4];
  pagevec pv={0};
  bytevec out={0};
  ogg_mux_t *mux;
  long i;

  packet_fill(&p[0],3,0,1,0,140);
  packet_fill(&p[1],510,-1,0,0,141);    /* two full 255 segments */
  packet_fill(&p[2],2,INT64_MAX,0,0,142);
  packet_fill(&p[3],1,INT64_MAX,0,1,143);
  TASK_CHECK(frame_track(serials[0],p,4,&pv));
  /* Pages: BOS, at least one -1 continuation page, then the anchors.
     At least one non-BOS page before the last must carry -1. */
  {
    int negatives=0;
    TASK_CHECK(pv.count>=4);
    for(i=1;i<pv.count-1;i++){
      ogg_page pg;
      pg.header=pv.pages[i].header;
      pg.header_len=pv.pages[i].header_len;
      pg.body=pv.pages[i].body;
      pg.body_len=pv.pages[i].body_len;
      if(ogg_page_granulepos(&pg)==-1) negatives++;
    }
    TASK_CHECK(negatives>=1);
  }

  mux=ogg_mux_create(1,serials,num,den,1<<20);
  TASK_CHECK(mux!=NULL);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[0])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[1])==OGG_MUX_OK);
  /* Only the BOS page is safe: the -1 page waits for its anchor. */
  TASK_CHECK(drain(mux,&out)==1);
  TASK_CHECK(ogg_mux_advance_frontier(mux,serials[0],0)==OGG_MUX_OK);
  for(i=2;i<pv.count;i++)
    TASK_CHECK(ogg_mux_submit_page(mux,&pv.pages[i])==OGG_MUX_OK);
  TASK_CHECK(ogg_mux_end_input(mux)==OGG_MUX_OK);
  TASK_CHECK(drain(mux,&out)==pv.count-1);
  TASK_CHECK(out.count==pv.count);
  /* The group's scheduling time is huge, but each page's written
     granule is untouched. */
  for(i=0;i<pv.count;i++)
    TASK_CHECK(bytes_are_page(out.bytes[i],out.len[i],&pv.pages[i]));
  TASK_CHECK(decode_and_match(&out,serials[0],p,4));

  ogg_mux_destroy(mux);
  pv_free(&pv); bv_free(&out);
  for(i=0;i<4;i++) packet_free(&p[i]);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Cross-process tool chain (POSIX only)                              */

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int case_pipe_tool_chain(void){
  const char *helper="./.task-build/ogg/mux_pipe";
  const char *tool="./.task-build/ogg/oggmux";
  int status=0;
  pid_t pid;

  TASK_CHECK(access(helper,X_OK)==0);   /* built by CMake */
  TASK_CHECK(access(tool,X_OK)==0);
  pid=fork();
  TASK_CHECK(pid>=0);
  if(pid==0){
    execl(helper,helper,tool,(char *)NULL);
    _exit(127);
  }
  TASK_CHECK(waitpid(pid,&status,0)==pid);
  TASK_CHECK(WIFEXITED(status));
  TASK_CHECK(WEXITSTATUS(status)==0);
  return 0;
}
#else
static int case_pipe_tool_chain(void){
  TASK_CHECK(0);   /* the harness requires POSIX fork/waitpid */
  return 0;
}
#endif

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  {"Ordering","arrival_vs_time",case_arrival_vs_time},
  {"Ordering","equal_time_registration_order",
   case_equal_time_registration_order},
  {"Ordering","zero_granule_is_time_zero",case_zero_granule_is_time_zero},
  {"Ordering","rational_clock_exact",case_rational_clock_exact},
  {"Ordering","overflow_safe_compare",case_overflow_safe_compare},
  {"Ordering","unanchored_group",case_unanchored_group},
  {"Frontier","wait_for_equal_frontier",case_wait_for_equal_frontier},
  {"Frontier","slow_track_eos",case_slow_track_eos},
  {"Frontier","rejected_submissions_preserve_state",
   case_rejected_submissions_preserve_state},
  {"Frontier","end_input_incomplete",case_end_input_incomplete},
  {"Ownership","submit_copies_input",case_submit_copies_input},
  {"Ownership","capacity_reject_retry",case_capacity_reject_retry},
  {"Ownership","capacity_counts_unanchored",case_capacity_counts_unanchored},
  {"Ownership","borrowed_page_lifetime",case_borrowed_page_lifetime},
  {"Ownership","bad_input_recovery",case_bad_input_recovery},
  {"Ownership","empty_packets",case_empty_packets},
  {"Ownership","continued_and_huge_eos",case_continued_and_huge_eos},
  {"Ownership","pipe_tool_chain",case_pipe_tool_chain}
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
