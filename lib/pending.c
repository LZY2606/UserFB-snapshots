/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2015             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

  function: bounded pending PCM mode for single-block synthesis

  The classic synthesis pipeline keeps a two-fragment PCM buffer and
  refuses a new block while previously produced samples are unread.
  The optional pending mode enabled through
  vorbis_synthesis_set_pending_limit() lets vorbis_synthesis_blockin()
  accept blocks while older PCM is still unread, up to a fixed per
  channel frame limit.  Unread samples and the not-yet-readable right
  window of the most recent block share one bounded ring buffer per
  channel (v->pcm); no second PCM queue, block or packet copies, or
  accumulated packet state is kept anywhere.

  The ring invariant is:

      pending_r <= pending_c <= pending_w
      backlog = pending_c - pending_r <= pending_limit
      pending window occupies [pending_w, pending_w + pending_wn)

  so newly formed samples are always appended immediately after the
  oldest unread data.  A short EOS tail retreats pending_c while the
  pending window stays where it was copied, exactly as the classic
  two-fragment buffer leaves the window in place when pcm_current is
  rewound; the trimmed gap is exposed through lapout (never through
  pcmout) and is closed by the next committed block.

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>
#include "vorbis/codec.h"
#include "codec_internal.h"
#include "window.h"
#include "misc.h"

#ifdef VORBIS_PENDING_TRACE
#define TRACE_MOVE(b,n) ((b)->pending_moved+=(long)(n))
#else
#define TRACE_MOVE(b,n) ((void)0)
#endif

/* map a logical stream position to a ring offset */
static long _pending_off(private_state *b,ogg_int64_t pos){
  ogg_int64_t o=pos%b->pending_size;
  if(o<0)o+=b->pending_size;
  return (long)o;
}

/* What would this block contribute if committed?  Mirrors the
   granule/sample accounting of vorbis_synthesis_blockin() exactly so
   that the capacity check is based on the frames this packet would
   actually add (first-block state, beginning trim, short EOS tail,
   granule-less packets and halfrate all included), not on a worst
   case window bound. */
typedef struct {
  int  first;       /* no PCM block committed since (re)init */
  int  trackonly;   /* vb->pcm == NULL */
  long step;        /* readable frames appended before trim */
  long trim;        /* frames trimmed from this block's contribution */
  int  trim_begin;  /* nonzero: trim the front of the contribution */
  ogg_int64_t sample_count;
  ogg_int64_t granulepos;
} pending_plan;

static void _pending_plan(vorbis_dsp_state *v,vorbis_block *vb,int hs,
                          pending_plan *p){
  codec_setup_info *ci=v->vi->codec_setup;
  private_state *b=v->backend_state;
  int lW=v->W,W=vb->W;
  long step_full=ci->blocksizes[lW]/4+ci->blocksizes[W]/4;
  ogg_int64_t sc,gp;

  p->first=(v->pcm_returned==-1);
  p->trackonly=(vb->pcm==NULL);
  p->trim=0;
  p->trim_begin=0;

  /* an out-of-sequence packet loses position tracking */
  if(v->sequence==-1 || v->sequence+1!=vb->sequence){
    sc=-1;
    gp=-1;
  }else{
    sc=b->sample_count;
    gp=v->granulepos;
  }

  if(sc==-1)
    sc=0;
  else
    sc+=step_full;

  p->step=p->first?0:(step_full>>hs);

  if(gp==-1){
    if(vb->granulepos!=-1){
      gp=vb->granulepos;
      if(sc>gp){
        /* short first page; trim beginning, or end if this is also
           the last page */
        ogg_int64_t extra=sc-gp;
        if(extra<0)extra=0;
        if(vb->eofflag){
          ogg_int64_t avail=(ogg_int64_t)p->step<<hs;
          if(extra>avail)extra=avail;
          if(extra<0)extra=0;
          p->trim=(long)(extra>>hs);
        }else{
          p->trim=(long)(extra>>hs);
          p->trim_begin=1;
          if(p->trim>p->step)p->trim=p->step;
        }
      }
    }
  }else{
    gp+=step_full;
    if(vb->granulepos!=-1 && gp!=vb->granulepos){
      if(gp>vb->granulepos){
        ogg_int64_t extra=gp-vb->granulepos;
        if(extra && vb->eofflag){
          /* partial last frame; the trim is limited to what this
             block alone would have contributed, never to older
             backlog */
          ogg_int64_t avail=(ogg_int64_t)p->step<<hs;
          if(extra>avail)extra=avail;
          if(extra<0)extra=0;
          p->trim=(long)(extra>>hs);
        }
      }
      gp=vb->granulepos;
    }
  }

  /* trackonly blocks advance sequence and position tracking but never
     create or remove PCM */
  if(p->trackonly)p->trim=0;

  p->sample_count=sc;
  p->granulepos=gp;
}

int _vorbis_pending_blockin(vorbis_dsp_state *v,vorbis_block *vb){
  vorbis_info *vi=v->vi;
  codec_setup_info *ci=vi->codec_setup;
  private_state *b=v->backend_state;
  int hs=ci->halfrate_flag;
  pending_plan p;
  int i,j;

  b->pending_configurable=0;

  _pending_plan(v,vb,hs,&p);

  if(!p.trackonly){
    long backlog=p.first?0:(long)(b->pending_c-b->pending_r);
    long added=p.step-p.trim;
    if(backlog+added>b->pending_limit)
      return OV_EBACKPRESSURE; /* nothing committed; caller may drain
                                  and retry the same block unchanged */
  }

  /* commit sequence and position tracking (shared by PCM and
     trackonly blocks) */
  v->lW=v->W;
  v->W=vb->W;
  v->nW=-1;
  v->sequence=vb->sequence;
  b->sample_count=p.sample_count;
  v->granulepos=p.granulepos;

  if(!p.trackonly){
    int n=ci->blocksizes[v->W]>>(hs+1);
    int n0=ci->blocksizes[0]>>(hs+1);
    int n1=ci->blocksizes[1]>>(hs+1);
    long S=b->pending_size;
    ogg_int64_t w,base,cnew,wnew;

    v->glue_bits+=vb->glue_bits;
    v->time_bits+=vb->time_bits;
    v->floor_bits+=vb->floor_bits;
    v->res_bits+=vb->res_bits;

    /* close a trimmed EOS gap: slide the pending window down to the
       end of the readable data so the stream stays contiguous.  This
       moves at most one window, never the backlog. */
    if(b->pending_w>b->pending_c){
      ogg_int64_t cw=b->pending_c;
      long src=_pending_off(b,b->pending_w);
      long dst=_pending_off(b,cw);
      int k;
      for(j=0;j<vi->channels;j++){
        float *ring=v->pcm[j];
        long s2=src,d2=dst;
        for(k=0;k<b->pending_wn;k++){
          ring[d2]=ring[s2];
          if(++d2==S)d2=0;
          if(++s2==S)s2=0;
        }
      }
      TRACE_MOVE(b,(long)b->pending_wn*vi->channels);
      b->pending_w=cw;
    }
    w=b->pending_w;

    if(p.first){
      /* The first block's overlap output is never readable (it is
         always overwritten before use, exactly as in the classic
         two-fragment buffer), so run it into the scratch region above
         the window home instead of into the readable stream. */
      b->pending_r=w;
      b->pending_c=w;
      base=w+n1;
      cnew=w;
    }else{
      base=w;
      cnew=w+p.step-p.trim;
    }

    /* the new right window lands right after this block's full
       contribution; a beginning trim pulls it (and the contribution)
       down to the new end of the readable data, an end trim leaves it
       in place, forming the lap gap */
    wnew=p.trim_begin?cnew:w+p.step;

    /* overlap/add the new block's left half against the pending right
       window (or scratch on the first block) */
    for(j=0;j<vi->channels;j++){
      float *ring=v->pcm[j];
      float *src=vb->pcm[j];
      if(v->lW){
        if(v->W){
          /* large/large */
          const float *win=_vorbis_window_get(b->window[1]-hs);
          long o=_pending_off(b,base);
          for(i=0;i<n1;i++){
            ring[o]=ring[o]*win[n1-i-1]+src[i]*win[i];
            if(++o==S)o=0;
          }
        }else{
          /* large/small */
          const float *win=_vorbis_window_get(b->window[0]-hs);
          long o=_pending_off(b,base+n1/2-n0/2);
          for(i=0;i<n0;i++){
            ring[o]=ring[o]*win[n0-i-1]+src[i]*win[i];
            if(++o==S)o=0;
          }
        }
      }else{
        if(v->W){
          /* small/large */
          const float *win=_vorbis_window_get(b->window[0]-hs);
          float *p2=src+n1/2-n0/2;
          long o=_pending_off(b,base);
          for(i=0;i<n0;i++){
            ring[o]=ring[o]*win[n0-i-1]+p2[i]*win[i];
            if(++o==S)o=0;
          }
          for(;i<n1/2+n0/2;i++){
            ring[o]=p2[i];
            if(++o==S)o=0;
          }
        }else{
          /* small/small */
          const float *win=_vorbis_window_get(b->window[0]-hs);
          long o=_pending_off(b,base);
          for(i=0;i<n0;i++){
            ring[o]=ring[o]*win[n0-i-1]+src[i]*win[i];
            if(++o==S)o=0;
          }
        }
      }
    }

    /* A beginning trim drops frames from the front of this block's
       contribution only; slide the kept tail down to the window home
       so the readable stream stays contiguous.  Older backlog is
       never touched. */
    if(p.trim_begin && p.trim>0){
      long count=p.step-p.trim;
      for(j=0;j<vi->channels;j++){
        float *ring=v->pcm[j];
        long dst=_pending_off(b,w);
        long src=_pending_off(b,w+p.trim);
        for(i=0;i<count;i++){
          ring[dst]=ring[src];
          if(++dst==S)dst=0;
          if(++src==S)src=0;
        }
      }
      TRACE_MOVE(b,count*vi->channels);
    }

    /* copy the new right window into place */
    for(j=0;j<vi->channels;j++){
      float *ring=v->pcm[j];
      float *src=vb->pcm[j]+n;
      long o=_pending_off(b,wnew);
      for(i=0;i<n;i++){
        ring[o]=src[i];
        if(++o==S)o=0;
      }
    }

    b->pending_c=cnew;
    b->pending_w=wnew;
    b->pending_wn=n;

    v->pcm_returned=(int)_pending_off(b,b->pending_r);
    v->pcm_current=(int)_pending_off(b,b->pending_c);
    v->centerW=(long)_pending_off(b,b->pending_w);
  }

  if(vb->eofflag)v->eofflag=1;
  return 0;
}

/* returns the earliest unread contiguous segment; pcm==NULL just
   reports its length */
int _vorbis_pending_pcmout(vorbis_dsp_state *v,float ***pcm){
  vorbis_info *vi=v->vi;
  private_state *b=v->backend_state;
  ogg_int64_t backlog;
  long off,contig;

  if(v->pcm_returned<0)return 0;
  backlog=b->pending_c-b->pending_r;
  if(backlog<=0)return 0;

  off=_pending_off(b,b->pending_r);
  contig=b->pending_size-off;
  if(contig>backlog)contig=(long)backlog;

  if(pcm){
    int i;
    for(i=0;i<vi->channels;i++)
      v->pcmret[i]=v->pcm[i]+off;
    *pcm=v->pcmret;
  }
  return (int)contig;
}

int _vorbis_pending_read(vorbis_dsp_state *v,int n){
  private_state *b=v->backend_state;
  ogg_int64_t backlog;

  if(n<0)return OV_EINVAL;
  if(v->pcm_returned<0)return n?OV_EINVAL:0;
  backlog=b->pending_c-b->pending_r;
  if(n>backlog)return OV_EINVAL;

  b->pending_r+=n;
  v->pcm_returned=(int)_pending_off(b,b->pending_r);
  return 0;
}

/* lapout exposes the trimmed tail plus the pending right window once
   the readable stream is drained, exactly the data the classic
   two-fragment buffer would serve; the lap data is not counted as
   pending */
int _vorbis_pending_lapout(vorbis_dsp_state *v,float ***pcm){
  vorbis_info *vi=v->vi;
  private_state *b=v->backend_state;
  long S=b->pending_size;
  ogg_int64_t gap;
  long off,L;
  int i,j;

  if(v->pcm_returned<0)return 0;
  if(b->pending_c>b->pending_r)return OV_EINVAL;

  gap=b->pending_w-b->pending_c;
  L=(long)(gap+b->pending_wn);
  if(L<=0)return 0;

  off=_pending_off(b,b->pending_c);
  if(off+L>S){
    /* the lap region straddles the ring boundary; relocate it into
       the free run right after it so it can be exposed contiguously.
       The free run is always at least L frames here. */
    long target=off+L-S;
    for(j=0;j<vi->channels;j++){
      float *ring=v->pcm[j];
      long dst=target,src=off;
      for(i=0;i<L;i++){
        ring[dst]=ring[src];
        if(++dst==S)dst=0;
        if(++src==S)src=0;
      }
    }
    TRACE_MOVE(b,(long)L*vi->channels);
    /* normalize positions onto the relocated region; the backlog is
       empty, so this loses no readable data */
    b->pending_r=target;
    b->pending_c=target;
    b->pending_w=target+gap;
    v->pcm_returned=(int)target;
    v->pcm_current=(int)target;
    v->centerW=(long)(target+gap);
    off=target;
  }

  if(pcm){
    for(j=0;j<vi->channels;j++)
      v->pcmret[j]=v->pcm[j]+off;
    *pcm=v->pcmret;
  }
  return (int)L;
}

int vorbis_synthesis_set_pending_limit(vorbis_dsp_state *v,int frames){
  private_state *b;
  vorbis_info *vi;
  codec_setup_info *ci;
  int hs,i;
  long n1,size;
  float **fresh;

  if(!v)return OV_EINVAL;
  b=v->backend_state;
  vi=v->vi;
  if(!b||!vi||!vi->codec_setup)return OV_EINVAL;
  ci=vi->codec_setup;

  /* the limit may only be set before any block is committed, or right
     after a restart */
  if(!b->pending_configurable)return OV_EINVAL;

  hs=ci->halfrate_flag;
  n1=ci->blocksizes[1]>>(hs+1);
  size=frames?(frames+2*n1):ci->blocksizes[1];

  if(frames){
    long minlimit=ci->blocksizes[1]>>hs;
    if(frames<minlimit||frames>1048576)return OV_EINVAL;
  }else{
    if(!b->pending_limit)return 0; /* already classic; nothing to do */
  }

  /* swap in fresh per channel storage; on any failure the previous
     configuration is left untouched */
  fresh=_ogg_calloc(vi->channels,sizeof(*fresh));
  if(!fresh)return OV_EFAULT;
  for(i=0;i<vi->channels;i++){
    fresh[i]=_ogg_calloc(size,sizeof(*fresh[i]));
    if(!fresh[i]){
      while(--i>=0)_ogg_free(fresh[i]);
      _ogg_free(fresh);
      return OV_EFAULT;
    }
  }
  for(i=0;i<vi->channels;i++){
    _ogg_free(v->pcm[i]);
    v->pcm[i]=fresh[i];
  }
  _ogg_free(fresh);

  v->pcm_storage=(int)size;
  b->pending_limit=frames;
  b->pending_size=frames?size:0;
  b->pending_r=b->pending_c=b->pending_w=0;
  b->pending_wn=0;
  return 0;
}

long vorbis_synthesis_pending(vorbis_dsp_state *v){
  private_state *b;
  if(!v)return 0;
  b=v->backend_state;
  if(!b)return 0;
  if(b->pending_limit){
    if(v->pcm_returned<0)return 0;
    return (long)(b->pending_c-b->pending_r);
  }
  if(v->pcm_returned>-1 && v->pcm_returned<v->pcm_current)
    return v->pcm_current-v->pcm_returned;
  return 0;
}

#ifdef VORBIS_PENDING_TRACE
/* read-only observation point for tests: frames relocated inside the
   ring (trims and lapout defragmentation).  Per blockin/read work is
   otherwise a bounded number of window-sized copies, independent of
   the backlog size. */
long vorbis_pending_trace_moved(vorbis_dsp_state *v){
  private_state *b=v?v->backend_state:NULL;
  return b?b->pending_moved:0;
}
#endif
