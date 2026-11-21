/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2026             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: multi-stream Ogg page multiplexer

 ********************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <ogg/mux.h>

/* ------------------------------------------------------------------
   Exact rational time comparison.

   A scheduling time is granule * num / den.  All values are
   non-negative and may use the full signed 64-bit range, so a naive
   product overflows.  We compare 192-bit cross products using 32-bit
   limbs; no floating point is involved and no intermediate saturates.
   ------------------------------------------------------------------ */

typedef struct {
  ogg_uint64_t hi;
  ogg_uint64_t lo;
} mux_u128;

typedef struct {
  ogg_uint64_t hi;
  ogg_uint64_t mid;
  ogg_uint64_t lo;
} mux_u192;

static mux_u128 mux_mul64(ogg_uint64_t a, ogg_uint64_t b){
  ogg_uint64_t a0=a&(ogg_uint64_t)0xffffffffu;
  ogg_uint64_t a1=a>>32;
  ogg_uint64_t b0=b&(ogg_uint64_t)0xffffffffu;
  ogg_uint64_t b1=b>>32;
  ogg_uint64_t p00=a0*b0;
  ogg_uint64_t p01=a0*b1;
  ogg_uint64_t p10=a1*b0;
  ogg_uint64_t p11=a1*b1;
  ogg_uint64_t mid=p01+p10;
  ogg_uint64_t mid_carry=mid<p01;
  mux_u128 r;
  r.lo=p00+(mid<<32);
  r.hi=p11+(mid>>32)+(mid_carry<<32)+(r.lo<p00);
  return r;
}

static mux_u192 mux_mul128_64(mux_u128 a, ogg_uint64_t b){
  mux_u128 lomul=mux_mul64(a.lo,b);
  mux_u128 himul=mux_mul64(a.hi,b);
  ogg_uint64_t mid=lomul.hi+himul.lo;
  ogg_uint64_t mid_carry=mid<lomul.hi;
  mux_u192 r;
  r.lo=lomul.lo;
  r.mid=mid;
  r.hi=himul.hi+mid_carry;
  return r;
}

static int mux_u192_cmp(mux_u192 a,mux_u192 b){
  if(a.hi!=b.hi) return a.hi<b.hi?-1:1;
  if(a.mid!=b.mid) return a.mid<b.mid?-1:1;
  if(a.lo!=b.lo) return a.lo<b.lo?-1:1;
  return 0;
}

/* compare g1*num1/den1 with g2*num2/den2 (all values non-negative) */
static int mux_time_cmp(ogg_uint64_t g1,ogg_uint64_t num1,ogg_uint64_t den1,
                        ogg_uint64_t g2,ogg_uint64_t num2,ogg_uint64_t den2){
  mux_u192 a=mux_mul128_64(mux_mul64(g1,num1),den2);
  mux_u192 b=mux_mul128_64(mux_mul64(g2,num2),den1);
  return mux_u192_cmp(a,b);
}

/* true when granule*num/den is strictly greater than the watermark,
   itself measured in whole ticks (granule*num > ticks*den) */
static int mux_time_after_ticks(ogg_uint64_t granule,ogg_uint64_t num,
                                ogg_uint64_t den,ogg_uint64_t ticks){
  mux_u128 a=mux_mul64(granule,num);
  mux_u128 b=mux_mul64(ticks,den);
  if(a.hi!=b.hi) return a.hi>b.hi;
  return a.lo>b.lo;
}

/* true when a watermark of ticks ticks is at least granule*num/den */
static int mux_ticks_cover_time(ogg_uint64_t ticks,ogg_uint64_t granule,
                                ogg_uint64_t num,ogg_uint64_t den){
  mux_u128 a=mux_mul64(ticks,den);
  mux_u128 b=mux_mul64(granule,num);
  if(a.hi!=b.hi) return a.hi>b.hi;
  return a.lo>=b.lo;
}

/* ------------------------------------------------------------------ */

typedef struct mux_page {
  struct mux_page *next;
  unsigned char *data;        /* header followed by body */
  long header_len;
  long body_len;
  ogg_int64_t granule;        /* granule as carried by the original page */
  ogg_int64_t anchor;         /* anchor granule used for scheduling */
  ogg_int64_t pageno;
  int is_bos;
  int is_eos;
  int anchored;               /* anchor is valid */
} mux_page;

typedef struct {
  int serial;
  ogg_uint64_t num;
  ogg_uint64_t den;

  mux_page *head;
  mux_page *tail;
  size_t queued_bytes;

  int bos_received;
  int bos_delivered;
  int eos_accepted;

  int have_anchor;
  ogg_int64_t last_anchor;    /* highest anchor granule so far */

  int have_frontier;
  ogg_int64_t frontier;       /* ticks, in this track's time base */

  long next_pageno;

  int unanchored;             /* count of trailing pages awaiting an anchor */
} mux_track;

struct ogg_mux_state {
  int ntracks;
  mux_track *tracks;
  size_t limit;
  size_t held;                /* queued pages plus the borrowed page */
  int all_bos;
  int input_ended;

  /* Storage for the page most recently returned by next_page. */
  unsigned char *pin_data;
  long pin_header_len;
  long pin_body_len;
};

static size_t mux_pin_bytes(const ogg_mux_t *mux){
  if(!mux || !mux->pin_data) return 0;
  return (size_t)mux->pin_header_len+(size_t)mux->pin_body_len;
}

static void mux_release_pin(ogg_mux_t *mux){
  if(mux && mux->pin_data){
    mux->held-=mux_pin_bytes(mux);
    free(mux->pin_data);
    mux->pin_data=NULL;
    mux->pin_header_len=0;
    mux->pin_body_len=0;
  }
}

static void mux_page_free(mux_page *p){
  if(p){
    free(p->data);
    free(p);
  }
}

static void mux_free_queue(mux_page *p){
  while(p){
    mux_page *next=p->next;
    mux_page_free(p);
    p=next;
  }
}

static mux_track *mux_find_track(ogg_mux_t *mux,int serial){
  int i;
  for(i=0;i<mux->ntracks;i++)
    if(mux->tracks[i].serial==serial) return &mux->tracks[i];
  return NULL;
}

static void mux_unlink_head(mux_track *t){
  mux_page *p=t->head;
  if(!p) return;
  t->head=p->next;
  if(!t->head) t->tail=NULL;
  if(!p->anchored) t->unanchored--;
  t->queued_bytes-=(size_t)p->header_len+(size_t)p->body_len;
  p->next=NULL;
}

ogg_mux_t *ogg_mux_create(int track_count,const int *serials,
                          const ogg_int64_t *num,const ogg_int64_t *den,
                          size_t limit){
  ogg_mux_t *mux;
  int i,j;

  if(track_count<=0 || !serials || !num || !den || limit==0) return NULL;

  for(i=0;i<track_count;i++){
    if(num[i]<=0 || den[i]<=0) return NULL;
    for(j=0;j<i;j++)
      if(serials[i]==serials[j]) return NULL;
  }

  mux=(ogg_mux_t *)calloc(1,sizeof(*mux));
  if(!mux) return NULL;
  mux->tracks=(mux_track *)calloc((size_t)track_count,sizeof(*mux->tracks));
  if(!mux->tracks){
    free(mux);
    return NULL;
  }
  mux->ntracks=track_count;
  mux->limit=limit;
  for(i=0;i<track_count;i++){
    mux->tracks[i].serial=serials[i];
    mux->tracks[i].num=(ogg_uint64_t)num[i];
    mux->tracks[i].den=(ogg_uint64_t)den[i];
    mux->tracks[i].next_pageno=0;
  }
  return mux;
}

void ogg_mux_destroy(ogg_mux_t *mux){
  int i;
  if(!mux) return;
  for(i=0;i<mux->ntracks;i++)
    mux_free_queue(mux->tracks[i].head);
  free(mux->tracks);
  mux_release_pin(mux);
  free(mux);
}

int ogg_mux_track_count(const ogg_mux_t *mux){
  return mux?mux->ntracks:0;
}

size_t ogg_mux_bytes(const ogg_mux_t *mux){
  return mux?mux->held:0;
}

ogg_mux_result_t ogg_mux_set_limit(ogg_mux_t *mux,size_t limit){
  if(!mux) return OGG_MUX_INVALID;
  if(limit<mux->held) return OGG_MUX_CAPACITY;
  if(limit!=mux->limit){
    mux_release_pin(mux);
    mux->limit=limit;
  }
  return OGG_MUX_OK;
}

ogg_mux_result_t ogg_mux_advance_frontier(ogg_mux_t *mux,int serial,
                                          ogg_int64_t ticks){
  mux_track *t;
  if(!mux) return OGG_MUX_INVALID;
  if(mux->input_ended) return OGG_MUX_INVALID;
  t=mux_find_track(mux,serial);
  if(!t) return OGG_MUX_BAD_SERIAL;
  if(ticks<0) return OGG_MUX_INVALID;

  /* A track that has accepted EOS behaves as an infinite watermark. */
  if(t->eos_accepted) return OGG_MUX_OK;

  if(t->have_frontier && ticks<t->frontier) return OGG_MUX_INVALID;

  if(!t->have_frontier || ticks!=t->frontier){
    mux_release_pin(mux);
    t->frontier=ticks;
    t->have_frontier=1;
  }
  return OGG_MUX_OK;
}

ogg_mux_result_t ogg_mux_submit_page(ogg_mux_t *mux,const ogg_page *page){
  mux_track *t;
  mux_page *rec;
  ogg_int64_t granule;
  long pageno;
  size_t page_bytes;
  size_t base;
  int is_bos,is_eos;
  unsigned char *data;
  int nseg;
  long body_sum;
  int k;

  if(!mux || !page) return OGG_MUX_INVALID;
  if(!page->header || page->header_len<27 || page->body_len<0)
    return OGG_MUX_BAD_PAGE;
  if(page->body_len>0 && !page->body) return OGG_MUX_BAD_PAGE;
  if(memcmp(page->header,"OggS",4)!=0 || page->header[4]!=0)
    return OGG_MUX_BAD_PAGE;

  /* Structural integrity: the header must describe exactly the body we
     were handed.  (CRC validity is the caller's responsibility.) */
  nseg=page->header[26];
  if(page->header_len!=27+nseg) return OGG_MUX_BAD_PAGE;
  body_sum=0;
  for(k=0;k<nseg;k++) body_sum+=(long)page->header[27+k];
  if(body_sum!=page->body_len) return OGG_MUX_BAD_PAGE;

  if(mux->input_ended) return OGG_MUX_INVALID;

  t=mux_find_track(mux,ogg_page_serialno(page));
  if(!t) return OGG_MUX_BAD_SERIAL;

  is_bos=ogg_page_bos(page);
  is_eos=ogg_page_eos(page);
  pageno=ogg_page_pageno(page);
  granule=ogg_page_granulepos(page);

  if(granule<-1) return OGG_MUX_BAD_GRANULE;
  if(t->eos_accepted) return OGG_MUX_BAD_PAGE;
  if(is_bos && t->bos_received) return OGG_MUX_BAD_PAGE;

  if(!t->bos_received){
    if(!is_bos) return OGG_MUX_NEED_BOS;
    if(pageno!=0) return OGG_MUX_BAD_PAGE;
  }else if(pageno!=t->next_pageno){
    return OGG_MUX_BAD_PAGE;   /* gap, replay or out-of-order page */
  }

  if(!is_bos){
    if(granule<0){
      if(is_eos) return OGG_MUX_BAD_GRANULE;
    }else{
      /* Anchors never move backwards. */
      if(t->have_anchor && granule<t->last_anchor)
        return OGG_MUX_BAD_GRANULE;
      /* Pages submitted after a watermark commitment must schedule
         strictly later than it: granule*num/den > ticks. */
      if(t->have_frontier &&
         !mux_time_after_ticks((ogg_uint64_t)granule,t->num,t->den,
                               (ogg_uint64_t)t->frontier))
        return OGG_MUX_FRONTIER;
    }
  }

  page_bytes=(size_t)page->header_len+(size_t)page->body_len;
  if(page_bytes>mux->limit) return OGG_MUX_CAPACITY;
  base=mux->held;   /* a successful submit releases the borrowed page */
  base-=mux_pin_bytes(mux);
  if(base>mux->limit || page_bytes>mux->limit-base) return OGG_MUX_CAPACITY;

  data=(unsigned char *)malloc(page_bytes?page_bytes:1);
  if(!data) return OGG_MUX_NOMEM;
  memcpy(data,page->header,(size_t)page->header_len);
  memcpy(data+page->header_len,page->body,(size_t)page->body_len);

  rec=(mux_page *)calloc(1,sizeof(*rec));
  if(!rec){
    free(data);
    return OGG_MUX_NOMEM;
  }
  rec->data=data;
  rec->header_len=page->header_len;
  rec->body_len=page->body_len;
  rec->granule=granule;
  rec->anchor=granule;
  rec->pageno=pageno;
  rec->is_bos=is_bos;
  rec->is_eos=is_eos;

  /* All validation passed; only now does the instance change. */
  mux_release_pin(mux);

  if(is_bos){
    t->bos_received=1;
    rec->anchored=1;
  }else if(granule<0){
    rec->anchored=0;
    t->unanchored++;
  }else{
    rec->anchored=1;
    if(t->unanchored){
      /* Close the trailing run of pages that were waiting for an
         anchor: they all schedule at this page's time, but keep their
         original (negative) granule when written out.  The run is
         identified by walking past the anchored prefix once. */
      mux_page *p=t->head;
      int remaining=t->unanchored;
      while(p && p->anchored) p=p->next;
      while(p && remaining--){
        p->anchor=granule;
        p->anchored=1;
        p=p->next;
      }
      t->unanchored=0;
    }
    t->last_anchor=granule;
    t->have_anchor=1;
  }

  if(is_eos) t->eos_accepted=1;

  rec->next=NULL;
  if(t->tail) t->tail->next=rec;
  else t->head=rec;
  t->tail=rec;
  t->queued_bytes+=page_bytes;
  mux->held+=page_bytes;
  t->next_pageno=pageno+1;

  return OGG_MUX_OK;
}

/* Pick the non-BOS page whose group schedules earliest, breaking ties
   by registration order.  Returns NULL when no track has a page. */
static mux_track *mux_pick_candidate(ogg_mux_t *mux){
  mux_track *best=NULL;
  int i;
  for(i=0;i<mux->ntracks;i++){
    mux_track *t=&mux->tracks[i];
    mux_page *p=t->head;
    if(!p) continue;
    if(!best){
      best=t;
    }else{
      mux_page *bp=best->head;
      int cmp=mux_time_cmp((ogg_uint64_t)p->anchor,t->num,t->den,
                           (ogg_uint64_t)bp->anchor,best->num,best->den);
      if(cmp<0) best=t;
      /* cmp==0: keep the earlier registered track */
    }
  }
  return best;
}

static ogg_mux_result_t mux_deliver_head(ogg_mux_t *mux,mux_track *t){
  mux_page *p=t->head;
  if(!p) return OGG_MUX_AGAIN;

  mux_release_pin(mux);
  mux_unlink_head(t);

  /* Transfer ownership of the page bytes to the borrow slot.  'held'
     keeps counting them until the slot is released. */
  mux->pin_data=p->data;
  mux->pin_header_len=p->header_len;
  mux->pin_body_len=p->body_len;
  p->data=NULL;
  mux_page_free(p);
  return OGG_MUX_OK;
}

static void mux_fill_page(ogg_mux_t *mux,ogg_page *page){
  page->header=mux->pin_data;
  page->header_len=mux->pin_header_len;
  page->body=mux->pin_data+mux->pin_header_len;
  page->body_len=mux->pin_body_len;
}

ogg_mux_result_t ogg_mux_next_page(ogg_mux_t *mux,ogg_page *page){
  int i;

  if(!mux || !page) return OGG_MUX_INVALID;

  if(!mux->all_bos){
    int complete=1;
    for(i=0;i<mux->ntracks;i++)
      if(!mux->tracks[i].bos_received) complete=0;
    if(!complete) return OGG_MUX_AGAIN;
    mux->all_bos=1;
  }

  /* All BOS pages leave first, in registration order. */
  for(i=0;i<mux->ntracks;i++){
    mux_track *t=&mux->tracks[i];
    if(!t->bos_delivered){
      mux_page *p=t->head;
      if(!p || !p->is_bos) return OGG_MUX_AGAIN;
      t->bos_delivered=1;
      if(mux_deliver_head(mux,t)!=OGG_MUX_OK) return OGG_MUX_AGAIN;
      mux_fill_page(mux,page);
      return OGG_MUX_OK;
    }
  }

  /* An open unanchored group anywhere holds back all non-BOS output. */
  for(i=0;i<mux->ntracks;i++)
    if(mux->tracks[i].unanchored) return OGG_MUX_AGAIN;

  {
    mux_track *best=mux_pick_candidate(mux);
    mux_page *cand;
    int j;
    if(!best) return OGG_MUX_AGAIN;
    cand=best->head;

    /* Every track that has not accepted EOS must have promised a
       watermark no earlier than the candidate group's scheduling
       time.  Watermarks are in each track's own time base. */
    for(j=0;j<mux->ntracks;j++){
      mux_track *t=&mux->tracks[j];
      if(t->eos_accepted) continue;
      if(!t->have_frontier) return OGG_MUX_AGAIN;
      if(!mux_ticks_cover_time((ogg_uint64_t)t->frontier,
                               (ogg_uint64_t)cand->anchor,
                               best->num,best->den))
        return OGG_MUX_AGAIN;
    }

    if(mux_deliver_head(mux,best)!=OGG_MUX_OK) return OGG_MUX_AGAIN;
    mux_fill_page(mux,page);
    return OGG_MUX_OK;
  }
}

ogg_mux_result_t ogg_mux_end_input(ogg_mux_t *mux){
  int i;
  if(!mux) return OGG_MUX_INVALID;
  if(mux->input_ended) return OGG_MUX_OK;
  for(i=0;i<mux->ntracks;i++)
    if(!mux->tracks[i].eos_accepted) return OGG_MUX_INCOMPLETE;
  mux_release_pin(mux);
  mux->input_ended=1;
  return OGG_MUX_OK;
}
