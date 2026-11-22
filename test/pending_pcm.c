/* Bounded pending PCM mode tests.

   Test data is produced by the real encoder (mono and stereo, impulse
   plus tonal content so both block sizes and long/short transitions
   occur).  The same legal packet sequence is decoded in the classic
   read-empty mode and in pending mode with batched submission and
   1/7/113 frame reads; frame counts, stream positions and every
   float sample must match exactly. */

#include <vorbis/vorbisenc.h>
#include "task_support/check.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef VORBIS_PENDING_TRACE
extern long vorbis_pending_trace_moved(vorbis_dsp_state *v);
#else
static long vorbis_pending_trace_moved(vorbis_dsp_state *v){
  (void)v;
  return 0;
}
#endif

static const int chunks[3]={1,7,113};

/* ---------------------------------------------------------------- */
/* encoded stream storage                                            */

typedef struct {
  ogg_packet *pkt;
  unsigned char **bytes;
  int count,cap;
  ogg_packet header[3];
  unsigned char *hbytes[3];
  long frames;
  int channels,rate;
  int shorts,longs,transitions;
} stream;

static void stream_free(stream *s){
  int i;
  for(i=0;i<s->count;i++)free(s->bytes[i]);
  for(i=0;i<3;i++)free(s->hbytes[i]);
  free(s->pkt);
  free(s->bytes);
  memset(s,0,sizeof(*s));
}

static int grab(ogg_packet *dst,unsigned char **dstbytes,const ogg_packet *src){
  unsigned char *copy=malloc(src->bytes>0?src->bytes:1);
  if(!copy)return 1;
  memcpy(copy,src->packet,src->bytes);
  *dst=*src;
  dst->packet=copy;
  *dstbytes=copy;
  return 0;
}

static int stream_push(stream *s,const ogg_packet *p){
  if(s->count==s->cap){
    int ncap=s->cap?s->cap*2:64;
    ogg_packet *np=realloc(s->pkt,ncap*sizeof(*np));
    unsigned char **nb;
    if(!np)return 1;
    s->pkt=np;
    nb=realloc(s->bytes,ncap*sizeof(*nb));
    if(!nb)return 1;
    s->bytes=nb;
    s->cap=ncap;
  }
  if(grab(&s->pkt[s->count],&s->bytes[s->count],p))return 1;
  s->count++;
  return 0;
}

static int stream_encode(stream *s,int channels,int rate,long frames){
  vorbis_info ei;
  vorbis_comment ec;
  vorbis_dsp_state vd;
  vorbis_block vb;
  ogg_packet h,c,setup,pkt;
  long pos=0;
  int i,ch,prev=-1;

  memset(s,0,sizeof(*s));
  s->channels=channels;
  s->rate=rate;
  s->frames=frames;

  vorbis_info_init(&ei);
  if(vorbis_encode_init_vbr(&ei,channels,rate,.35f))return 1;
  vorbis_comment_init(&ec);
  if(vorbis_analysis_init(&vd,&ei))return 1;
  if(vorbis_block_init(&vd,&vb))return 1;
  if(vorbis_analysis_headerout(&vd,&ec,&h,&c,&setup))return 1;
  if(grab(&s->header[0],&s->hbytes[0],&h))return 1;
  if(grab(&s->header[1],&s->hbytes[1],&c))return 1;
  if(grab(&s->header[2],&s->hbytes[2],&setup))return 1;

  for(;;){
    int n=frames-pos>997?997:(int)(frames-pos);
    if(n>0){
      float **in=vorbis_analysis_buffer(&vd,n);
      for(ch=0;ch<channels;ch++)
        for(i=0;i<n;i++){
          long k=pos+i;
          in[ch][i]=(float)(.12*sin(k*.039+ch*1.7)+.05*cos(k*.019+ch));
          if(k%3072<4)in[ch][i]+=(k%2?.7f:-.7f); /* impulses: force short blocks */
        }
      pos+=n;
    }
    if(vorbis_analysis_wrote(&vd,n))return 1;
    while(vorbis_analysis_blockout(&vd,&vb)==1){
      if(vorbis_analysis(&vb,NULL))return 1;
      if(vorbis_bitrate_addblock(&vb))return 1;
      while(vorbis_bitrate_flushpacket(&vd,&pkt))
        if(stream_push(s,&pkt))return 1;
    }
    if(n<=0)break;
  }

  /* block size census */
  {
    vorbis_info pi;
    vorbis_comment pc;
    vorbis_info_init(&pi);
    vorbis_comment_init(&pc);
    if(vorbis_synthesis_headerin(&pi,&pc,&s->header[0])||
       vorbis_synthesis_headerin(&pi,&pc,&s->header[1])||
       vorbis_synthesis_headerin(&pi,&pc,&s->header[2]))return 1;
    for(i=0;i<s->count;i++){
      int size=vorbis_packet_blocksize(&pi,&s->pkt[i]);
      if(size==vorbis_info_blocksize(&pi,0))s->shorts++;
      else if(size==vorbis_info_blocksize(&pi,1))s->longs++;
      else return 1;
      if(prev!=-1&&prev!=size)s->transitions++;
      prev=size;
    }
    vorbis_comment_clear(&pc);
    vorbis_info_clear(&pi);
  }

  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&ec);
  vorbis_info_clear(&ei);
  return 0;
}

static int make_info(stream *s,vorbis_info *vi,vorbis_comment *vc,int halfrate){
  vorbis_info_init(vi);
  vorbis_comment_init(vc);
  if(vorbis_synthesis_headerin(vi,vc,&s->header[0])||
     vorbis_synthesis_headerin(vi,vc,&s->header[1])||
     vorbis_synthesis_headerin(vi,vc,&s->header[2]))return 1;
  if(halfrate && vorbis_synthesis_halfrate(vi,1))return 1;
  return 0;
}

/* ---------------------------------------------------------------- */
/* decode output accumulator                                         */

typedef struct {
  float **ch;
  long frames,cap;
  int channels;
} pcmout;

static void pcmout_free(pcmout *o){
  int c;
  for(c=0;c<o->channels;c++)free(o->ch[c]);
  free(o->ch);
  memset(o,0,sizeof(*o));
}

static int pcm_append(pcmout *o,float **pcm,int n){
  int c;
  if(o->frames+n>o->cap){
    long ncap=o->cap?o->cap:1024;
    while(ncap<o->frames+n)ncap*=2;
    if(!o->ch){
      o->ch=calloc(o->channels,sizeof(*o->ch));
      if(!o->ch)return 1;
    }
    for(c=0;c<o->channels;c++){
      float *nb=realloc(o->ch[c],ncap*sizeof(*nb));
      if(!nb)return 1;
      o->ch[c]=nb;
    }
    o->cap=ncap;
  }
  if(!o->ch){
    o->ch=calloc(o->channels,sizeof(*o->ch));
    if(!o->ch)return 1;
  }
  for(c=0;c<o->channels;c++)
    memcpy(o->ch[c]+o->frames,pcm[c],n*sizeof(**pcm));
  o->frames+=n;
  return 0;
}

/* first differing frame, or -1; -2 on frame count mismatch */
static long pcm_diff(pcmout *a,pcmout *b,int *ch){
  long f;
  int c;
  if(a->frames!=b->frames)return -2;
  for(c=0;c<a->channels;c++)
    for(f=0;f<a->frames;f++)
      if(a->ch[c][f]!=b->ch[c][f]){
        *ch=c;
        return f;
      }
  return -1;
}

/* ---------------------------------------------------------------- */
/* decoders                                                          */

typedef struct {
  int rejections;
  int wrapped;
  long maxpend;
  int storage0,storage1;
  long moved;
  ogg_int64_t granule_tail;
  ogg_int64_t sequence_tail;
} dstats;

static int drain_chunk(vorbis_dsp_state *d,pcmout *o,long want){
  while(want>0){
    float **pcm;
    int n=vorbis_synthesis_pcmout(d,&pcm);
    int m;
    if(n<=0)break;
    m=n<want?n:(int)want;
    if(pcm_append(o,pcm,m))return 1;
    if(vorbis_synthesis_read(d,m))return 1;
    want-=m;
  }
  return 0;
}

/* classic mode: commit one block, read everything, repeat */
static int decode_classic(stream *s,pcmout *o,int halfrate,int trackonly,
                          dstats *st){
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state d;
  vorbis_block b;
  int i;
  if(make_info(s,&vi,&vc,halfrate))return 1;
  if(vorbis_synthesis_init(&d,&vi))return 1;
  if(vorbis_block_init(&d,&b))return 1;
  o->channels=s->channels;
  for(i=0;i<s->count;i++){
    if(i<trackonly){
      if(vorbis_synthesis_trackonly(&b,&s->pkt[i]))return 1;
      if(vorbis_synthesis_blockin(&d,&b))return 1;
    }else{
      if(vorbis_synthesis(&b,&s->pkt[i]))return 1;
      if(vorbis_synthesis_blockin(&d,&b))return 1;
      if(drain_chunk(&d,o,1L<<40))return 1;
    }
  }
  if(st){
    st->granule_tail=d.granulepos;
    st->sequence_tail=d.sequence;
  }
  vorbis_block_clear(&b);
  vorbis_dsp_clear(&d);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  return 0;
}

/* pending mode: batch submission, OV_EBACKPRESSURE retries, and
   1/7/113 frame reads */
static int decode_pending(stream *s,pcmout *o,int halfrate,int limit,
                          int trackonly,dstats *st){
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state d;
  vorbis_block b;
  int i,ci=0;
  if(make_info(s,&vi,&vc,halfrate))return 1;
  if(vorbis_synthesis_init(&d,&vi))return 1;
  if(vorbis_synthesis_set_pending_limit(&d,limit))return 1;
  if(st)st->storage0=d.pcm_storage;
  if(vorbis_block_init(&d,&b))return 1;
  o->channels=s->channels;
  for(i=0;i<s->count;i++){
    if(i<trackonly){
      if(vorbis_synthesis_trackonly(&b,&s->pkt[i]))return 1;
      if(vorbis_synthesis_blockin(&d,&b))return 1;
      if(vorbis_synthesis_pending(&d)!=0)return 1; /* no PCM from thin air */
    }else{
      int r;
      if(vorbis_synthesis(&b,&s->pkt[i]))return 1;
      while((r=vorbis_synthesis_blockin(&d,&b))==OV_EBACKPRESSURE){
        if(st)st->rejections++;
        if(drain_chunk(&d,o,chunks[ci++%3]))return 1;
      }
      if(r)return 1;
      if(st){
        long p=vorbis_synthesis_pending(&d);
        if(d.pcm_current<d.pcm_returned)st->wrapped=1;
        if(vorbis_synthesis_pcmout(&d,NULL)<p)st->wrapped=1;
        if(p>st->maxpend)st->maxpend=p;
      }
      if(drain_chunk(&d,o,chunks[ci++%3]))return 1;
    }
  }
  if(drain_chunk(&d,o,1L<<40))return 1;
  if(st){
    st->granule_tail=d.granulepos;
    st->sequence_tail=d.sequence;
    st->storage1=d.pcm_storage;
    st->moved=vorbis_pending_trace_moved(&d);
  }
  vorbis_block_clear(&b);
  vorbis_dsp_clear(&d);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  return 0;
}

/* ---------------------------------------------------------------- */
/* Queue group: backlog mechanics                                    */

static int q_mono_batch(void){
  stream s;
  pcmout ref={0},got={0};
  dstats cs={0},ps={0};
  int ch=-1;
  long d;
  TASK_CHECK(stream_encode(&s,1,44100,24577)==0);
  TASK_CHECK(s.shorts>0&&s.longs>0&&s.transitions>0);
  TASK_CHECK(decode_classic(&s,&ref,0,0,&cs)==0);
  TASK_CHECK(ref.frames==s.frames);
  TASK_CHECK(decode_pending(&s,&got,0,2048,0,&ps)==0);
  TASK_CHECK(ps.rejections>0);          /* at least one reject+retry */
  TASK_CHECK(ps.wrapped);               /* ring wrapped around */
  TASK_CHECK(ps.maxpend>0&&ps.maxpend<=2048);
  TASK_CHECK(ps.storage0==2048+2048);   /* limit + 2x long block output */
  TASK_CHECK(ps.storage1==ps.storage0); /* backlog storage does not grow */
  TASK_CHECK(ps.moved==0);              /* no backlog shuffling at all */
  TASK_CHECK(cs.granule_tail==ps.granule_tail);
  TASK_CHECK(cs.sequence_tail==ps.sequence_tail);
  TASK_CHECK(got.frames==ref.frames);
  d=pcm_diff(&got,&ref,&ch);
  if(d!=-1)fprintf(stderr,"mono mismatch frame=%ld ch=%d\n",d,ch);
  TASK_CHECK(d==-1);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

static int q_stereo_batch(void){
  stream s;
  pcmout ref={0},got={0};
  dstats cs={0},ps={0};
  int ch=-1;
  long d;
  TASK_CHECK(stream_encode(&s,2,44100,20011)==0);
  TASK_CHECK(s.shorts>0&&s.longs>0&&s.transitions>0);
  TASK_CHECK(decode_classic(&s,&ref,0,0,&cs)==0);
  TASK_CHECK(ref.frames==s.frames);
  TASK_CHECK(decode_pending(&s,&got,0,4096,0,&ps)==0);
  TASK_CHECK(ps.rejections>0);
  TASK_CHECK(ps.wrapped);
  TASK_CHECK(ps.maxpend>0&&ps.maxpend<=4096);
  TASK_CHECK(ps.storage0==4096+2048);
  TASK_CHECK(ps.storage1==ps.storage0);
  TASK_CHECK(ps.moved==0);
  TASK_CHECK(cs.granule_tail==ps.granule_tail);
  TASK_CHECK(got.frames==ref.frames);
  d=pcm_diff(&got,&ref,&ch);
  if(d!=-1)fprintf(stderr,"stereo mismatch frame=%ld ch=%d\n",d,ch);
  TASK_CHECK(d==-1);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

static int q_api_rules(void){
  stream s;
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state d,z,av;
  vorbis_block b;
  vorbis_info ei;
  vorbis_comment ec;
  vorbis_dsp_state d2;
  vorbis_block b2;
  vorbis_info vi2;
  vorbis_comment vc2;
  int n1,minlimit,i,r;
  long p0,sq0;
  ogg_int64_t g0;
  float **pcm;

  TASK_CHECK(stream_encode(&s,1,44100,8192)==0);
  TASK_CHECK(make_info(&s,&vi,&vc,0)==0);
  n1=vorbis_info_blocksize(&vi,1)/2;
  minlimit=vorbis_info_blocksize(&vi,1);

  /* before synthesis_init there is nothing to configure */
  memset(&z,0,sizeof(z));
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&z,4096)==OV_EINVAL);

  TASK_CHECK(vorbis_synthesis_init(&d,&vi)==0);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,-5)==OV_EINVAL);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,minlimit-1)==OV_EINVAL);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,1048577)==OV_EINVAL);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,0)==0); /* already off */
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,minlimit)==0);
  TASK_CHECK(d.pcm_storage==minlimit+2*n1);
  TASK_CHECK(vorbis_synthesis_pending(&d)==0);
  TASK_CHECK(vorbis_block_init(&d,&b)==0);

  /* an analysis dsp has no pending mode */
  vorbis_info_init(&ei);
  vorbis_comment_init(&ec);
  TASK_CHECK(vorbis_encode_init_vbr(&ei,1,44100,.35f)==0);
  TASK_CHECK(vorbis_analysis_init(&av,&ei)==0);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&av,4096)==OV_EINVAL);
  vorbis_dsp_clear(&av);
  vorbis_comment_clear(&ec);
  vorbis_info_clear(&ei);

  /* once a block is committed the limit is locked */
  TASK_CHECK(vorbis_synthesis(&b,&s.pkt[0])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d,&b)==0);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,4096)==OV_EINVAL);

  /* backlog accumulates across blocks */
  TASK_CHECK(vorbis_synthesis(&b,&s.pkt[1])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d,&b)==0);
  p0=vorbis_synthesis_pending(&d);
  TASK_CHECK(p0>0);
  r=vorbis_synthesis_pcmout(&d,&pcm);
  TASK_CHECK(r>0&&r<=p0);
  TASK_CHECK(vorbis_synthesis_pcmout(&d,NULL)==r); /* NULL: same segment length */

  /* read argument validation; failed reads must not move */
  TASK_CHECK(vorbis_synthesis_read(&d,-1)==OV_EINVAL);
  TASK_CHECK(vorbis_synthesis_read(&d,(int)p0+1)==OV_EINVAL);
  TASK_CHECK(vorbis_synthesis_pending(&d)==p0);
  TASK_CHECK(vorbis_synthesis_read(&d,0)==0);
  TASK_CHECK(vorbis_synthesis_pending(&d)==p0);

  /* lapout refuses while PCM is pending, state untouched */
  TASK_CHECK(vorbis_synthesis_lapout(&d,&pcm)==OV_EINVAL);
  TASK_CHECK(vorbis_synthesis_pending(&d)==p0);

  /* drained: lapout exposes the right lap again */
  TASK_CHECK(vorbis_synthesis_read(&d,(int)p0)==0);
  TASK_CHECK(vorbis_synthesis_pending(&d)==0);
  TASK_CHECK(vorbis_synthesis_lapout(&d,&pcm)>0);
  TASK_CHECK(vorbis_synthesis_pending(&d)==0); /* lap data is not pending */

  /* fill to the limit; a rejected block changes nothing and the same
     block is accepted after draining */
  for(i=2;i<s.count;i++){
    TASK_CHECK(vorbis_synthesis(&b,&s.pkt[i])==0);
    r=vorbis_synthesis_blockin(&d,&b);
    if(r==OV_EBACKPRESSURE)break;
    TASK_CHECK(r==0);
  }
  TASK_CHECK(r==OV_EBACKPRESSURE);
  p0=vorbis_synthesis_pending(&d);
  g0=d.granulepos;
  sq0=d.sequence;
  TASK_CHECK(vorbis_synthesis_blockin(&d,&b)==OV_EBACKPRESSURE);
  TASK_CHECK(vorbis_synthesis_pending(&d)==p0);
  TASK_CHECK(d.granulepos==g0&&d.sequence==sq0);
  while((r=vorbis_synthesis_blockin(&d,&b))==OV_EBACKPRESSURE)
    TASK_CHECK(vorbis_synthesis_read(&d,113)==0);
  TASK_CHECK(r==0);

  vorbis_block_clear(&b);
  vorbis_dsp_clear(&d);

  /* default mode (limit 0) is unchanged: unread data rejects */
  TASK_CHECK(make_info(&s,&vi2,&vc2,0)==0);
  TASK_CHECK(vorbis_synthesis_init(&d2,&vi2)==0);
  TASK_CHECK(vorbis_block_init(&d2,&b2)==0);
  TASK_CHECK(vorbis_synthesis(&b2,&s.pkt[0])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d2,&b2)==0);
  TASK_CHECK(vorbis_synthesis(&b2,&s.pkt[1])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d2,&b2)==0);
  TASK_CHECK(vorbis_synthesis(&b2,&s.pkt[2])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d2,&b2)==OV_EINVAL);
  p0=vorbis_synthesis_pending(&d2);
  TASK_CHECK(p0>0&&p0==vorbis_synthesis_pcmout(&d2,NULL));
  TASK_CHECK(vorbis_synthesis_read(&d2,(int)p0)==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d2,&b2)==0);
  vorbis_block_clear(&b2);
  vorbis_dsp_clear(&d2);
  vorbis_comment_clear(&vc2);
  vorbis_info_clear(&vi2);

  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  stream_free(&s);
  return 0;
}

/* ---------------------------------------------------------------- */
/* Timeline group: controlled granule/position scenarios             */

static int t_first_position_late(void){
  stream s;
  pcmout ref={0},got={0};
  dstats cs={0},ps={0};
  int ch=-1;
  long d;
  TASK_CHECK(stream_encode(&s,1,44100,12031)==0);
  /* first positions arrive late, and low: forces a beginning trim */
  s.pkt[0].granulepos=-1;
  s.pkt[1].granulepos=-1;
  s.pkt[2].granulepos=-1;
  s.pkt[3].granulepos-=200;
  TASK_CHECK(decode_classic(&s,&ref,0,0,&cs)==0);
  TASK_CHECK(ref.frames<s.frames); /* trim really happened */
  TASK_CHECK(decode_pending(&s,&got,0,2048,0,&ps)==0);
  TASK_CHECK(got.frames==ref.frames);
  d=pcm_diff(&got,&ref,&ch);
  if(d!=-1)fprintf(stderr,"late-pos mismatch frame=%ld ch=%d\n",d,ch);
  TASK_CHECK(d==-1);
  TASK_CHECK(cs.granule_tail==ps.granule_tail);
  /* the trim slid only this packet's fresh segment, never the backlog */
  TASK_CHECK(ps.moved>0&&ps.moved<=1024);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

/* classic-mode EOS trim for the last packet with a given granulepos:
   the granule shortfall clamped to what the last packet itself
   contributed, halfrate-shifted */
static long eos_trim(stream *s,vorbis_info *pi,int last,ogg_int64_t gp,int hs){
  long bsL=vorbis_packet_blocksize(pi,&s->pkt[last]);
  long bsP=vorbis_packet_blocksize(pi,&s->pkt[last-1]);
  long step_full=bsP/4+bsL/4;
  ogg_int64_t tracked=s->pkt[last-1].granulepos+step_full;
  ogg_int64_t avail=(ogg_int64_t)(step_full>>hs)<<hs;
  ogg_int64_t extra=tracked-gp;
  if(extra>avail)extra=avail;
  if(extra<0)extra=0;
  return (long)(extra>>hs);
}

static int t_short_eos(void){
  stream s;
  pcmout base={0},ref={0},got={0};
  dstats cs={0},ps={0};
  vorbis_info pi;
  vorbis_comment pc;
  int ch=-1,last;
  long d;
  ogg_int64_t gp_orig;
  TASK_CHECK(stream_encode(&s,2,44100,16001)==0);
  last=s.count-1;
  gp_orig=s.pkt[last].granulepos;
  TASK_CHECK(decode_classic(&s,&base,0,0,NULL)==0);
  TASK_CHECK(base.frames==s.frames);
  s.pkt[last].granulepos-=173; /* short final page */
  s.pkt[last].e_o_s=1;
  TASK_CHECK(decode_classic(&s,&ref,0,0,&cs)==0);
  /* the end trim is the granule shortfall clamped to what the last
     packet itself contributed; the unmodified stream already trims */
  vorbis_info_init(&pi);
  vorbis_comment_init(&pc);
  TASK_CHECK(vorbis_synthesis_headerin(&pi,&pc,&s.header[0])==0);
  TASK_CHECK(vorbis_synthesis_headerin(&pi,&pc,&s.header[1])==0);
  TASK_CHECK(vorbis_synthesis_headerin(&pi,&pc,&s.header[2])==0);
  TASK_CHECK(ref.frames==base.frames-
             (eos_trim(&s,&pi,last,gp_orig-173,0)-
              eos_trim(&s,&pi,last,gp_orig,0)));
  vorbis_comment_clear(&pc);
  vorbis_info_clear(&pi);
  TASK_CHECK(decode_pending(&s,&got,0,2048,0,&ps)==0);
  TASK_CHECK(got.frames==ref.frames);
  d=pcm_diff(&got,&ref,&ch);
  if(d!=-1)fprintf(stderr,"eos mismatch frame=%ld ch=%d\n",d,ch);
  TASK_CHECK(d==-1);
  TASK_CHECK(cs.granule_tail==ps.granule_tail);
  TASK_CHECK(ps.moved==0); /* end trims never move data */
  pcmout_free(&base);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

static int t_halfrate_odd(void){
  stream s;
  pcmout base={0},ref={0},got={0};
  dstats cs={0},ps={0};
  vorbis_info pi;
  vorbis_comment pc;
  int ch=-1,last;
  long d;
  ogg_int64_t gp_orig;
  TASK_CHECK(stream_encode(&s,1,44100,14003)==0);
  last=s.count-1;
  gp_orig=s.pkt[last].granulepos;
  /* unmodified halfrate decode to establish the full length */
  TASK_CHECK(decode_classic(&s,&base,1,0,NULL)==0);
  /* odd granule shortfall at half rate */
  s.pkt[last].granulepos-=173;
  s.pkt[last].e_o_s=1;
  TASK_CHECK(decode_classic(&s,&ref,1,0,&cs)==0);
  TASK_CHECK(decode_pending(&s,&got,1,2048,0,&ps)==0);
  TASK_CHECK(got.frames==ref.frames);
  d=pcm_diff(&got,&ref,&ch);
  if(d!=-1)fprintf(stderr,"halfrate mismatch frame=%ld ch=%d\n",d,ch);
  TASK_CHECK(d==-1);
  /* the trim is exactly the halfrate-shifted odd granule shortfall,
     clamped to the last packet's own contribution */
  vorbis_info_init(&pi);
  vorbis_comment_init(&pc);
  TASK_CHECK(vorbis_synthesis_headerin(&pi,&pc,&s.header[0])==0);
  TASK_CHECK(vorbis_synthesis_headerin(&pi,&pc,&s.header[1])==0);
  TASK_CHECK(vorbis_synthesis_headerin(&pi,&pc,&s.header[2])==0);
  TASK_CHECK(ref.frames==base.frames-
             (eos_trim(&s,&pi,last,gp_orig-173,1)-
              eos_trim(&s,&pi,last,gp_orig,1)));
  TASK_CHECK(cs.granule_tail==ps.granule_tail);
  vorbis_comment_clear(&pc);
  vorbis_info_clear(&pi);
  pcmout_free(&base);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

static int t_trackonly(void){
  stream s;
  pcmout ref={0},got={0};
  dstats cs={0},ps={0};
  int ch=-1;
  long d;
  TASK_CHECK(stream_encode(&s,1,44100,10007)==0);
  TASK_CHECK(decode_classic(&s,&ref,0,5,&cs)==0);
  TASK_CHECK(decode_pending(&s,&got,0,2048,5,&ps)==0);
  TASK_CHECK(got.frames==ref.frames);
  d=pcm_diff(&got,&ref,&ch);
  if(d!=-1)fprintf(stderr,"trackonly mismatch frame=%ld ch=%d\n",d,ch);
  TASK_CHECK(d==-1);
  TASK_CHECK(cs.granule_tail==ps.granule_tail);
  TASK_CHECK(cs.sequence_tail==ps.sequence_tail);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

static int t_restart_lapout(void){
  stream s;
  pcmout ref={0},got={0};
  vorbis_info vi,vi2;
  vorbis_comment vc,vc2;
  vorbis_dsp_state d,d2;
  vorbis_block b,b2;
  float **pcm,**pcm2;
  int i,r,laps1,laps2,storage0,ch;
  long p0;

  TASK_CHECK(stream_encode(&s,1,44100,12011)==0);
  TASK_CHECK(make_info(&s,&vi,&vc,0)==0);
  TASK_CHECK(vorbis_synthesis_init(&d,&vi)==0);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,2048)==0);
  storage0=d.pcm_storage;
  TASK_CHECK(vorbis_block_init(&d,&b)==0);

  /* build a backlog, then restart: backlog and lap history drop,
     limit and storage stay */
  for(i=0;i<s.count;i++){
    TASK_CHECK(vorbis_synthesis(&b,&s.pkt[i])==0);
    r=vorbis_synthesis_blockin(&d,&b);
    if(r==OV_EBACKPRESSURE)break;
    TASK_CHECK(r==0);
  }
  p0=vorbis_synthesis_pending(&d);
  TASK_CHECK(p0>0);
  TASK_CHECK(vorbis_synthesis_lapout(&d,&pcm)==OV_EINVAL);
  TASK_CHECK(vorbis_synthesis_pending(&d)==p0);
  TASK_CHECK(vorbis_synthesis_restart(&d)==0);
  TASK_CHECK(vorbis_synthesis_pending(&d)==0);
  TASK_CHECK(d.pcm_storage==storage0);
  /* reconfigure is legal right after a restart */
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,4096)==0);
  TASK_CHECK(d.pcm_storage==4096+2048);

  /* classic reference, keeping the final lap */
  TASK_CHECK(make_info(&s,&vi2,&vc2,0)==0);
  TASK_CHECK(vorbis_synthesis_init(&d2,&vi2)==0);
  TASK_CHECK(vorbis_block_init(&d2,&b2)==0);
  ref.channels=1;
  for(i=0;i<s.count;i++){
    TASK_CHECK(vorbis_synthesis(&b2,&s.pkt[i])==0);
    TASK_CHECK(vorbis_synthesis_blockin(&d2,&b2)==0);
    TASK_CHECK(drain_chunk(&d2,&ref,1L<<40)==0);
  }
  laps1=vorbis_synthesis_lapout(&d2,&pcm2);
  TASK_CHECK(laps1>0);

  /* pending decode of the whole stream after the restart */
  got.channels=1;
  {
    int ci=0;
    for(i=0;i<s.count;i++){
      TASK_CHECK(vorbis_synthesis(&b,&s.pkt[i])==0);
      while((r=vorbis_synthesis_blockin(&d,&b))==OV_EBACKPRESSURE)
        TASK_CHECK(drain_chunk(&d,&got,chunks[ci++%3])==0);
      TASK_CHECK(r==0);
      TASK_CHECK(drain_chunk(&d,&got,chunks[ci++%3])==0);
    }
    TASK_CHECK(drain_chunk(&d,&got,1L<<40)==0);
  }
  TASK_CHECK(got.frames==ref.frames);
  {
    int dch=-1;
    long dd=pcm_diff(&got,&ref,&dch);
    if(dd!=-1)fprintf(stderr,"restart mismatch frame=%ld ch=%d\n",dd,dch);
    TASK_CHECK(dd==-1);
  }

  /* drained, lapout serves the same right lap as classic mode */
  laps2=vorbis_synthesis_lapout(&d,&pcm);
  TASK_CHECK(laps2==laps1);
  for(ch=0;ch<1;ch++)
    TASK_CHECK(memcmp(pcm[ch],pcm2[ch],laps1*sizeof(**pcm))==0);
  TASK_CHECK(vorbis_synthesis_pending(&d)==0); /* lap not counted */

  vorbis_block_clear(&b2);
  vorbis_dsp_clear(&d2);
  vorbis_comment_clear(&vc2);
  vorbis_info_clear(&vi2);
  vorbis_block_clear(&b);
  vorbis_dsp_clear(&d);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

/* ---------------------------------------------------------------- */
/* Compatibility group                                               */

static int c_encode_roundtrip(void){
  stream s;
  pcmout out={0};
  long f;
  int c;
  double energy;
  TASK_CHECK(stream_encode(&s,1,44100,9001)==0);
  TASK_CHECK(s.shorts>0&&s.longs>0&&s.transitions>0);
  TASK_CHECK(decode_classic(&s,&out,0,0,NULL)==0);
  TASK_CHECK(out.frames==s.frames);
  energy=0;
  for(c=0;c<out.channels;c++)
    for(f=0;f<out.frames;f++)
      energy+=fabs(out.ch[c][f]);
  TASK_CHECK(energy>1);
  pcmout_free(&out);
  stream_free(&s);

  memset(&out,0,sizeof(out));
  TASK_CHECK(stream_encode(&s,2,44100,11003)==0);
  TASK_CHECK(s.shorts>0&&s.longs>0&&s.transitions>0);
  TASK_CHECK(decode_classic(&s,&out,0,0,NULL)==0);
  TASK_CHECK(out.frames==s.frames);
  energy=0;
  for(c=0;c<out.channels;c++)
    for(f=0;f<out.frames;f++)
      energy+=fabs(out.ch[c][f]);
  TASK_CHECK(energy>1);
  pcmout_free(&out);
  stream_free(&s);
  return 0;
}

static int c_oldmode_preserved(void){
  stream s;
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state d;
  vorbis_block b;
  float **pcm;
  long p0;
  int laps;
  TASK_CHECK(stream_encode(&s,1,44100,8192)==0);
  TASK_CHECK(make_info(&s,&vi,&vc,0)==0);
  TASK_CHECK(vorbis_synthesis_init(&d,&vi)==0);
  TASK_CHECK(vorbis_block_init(&d,&b)==0);
  /* classic two-fragment behavior: unread data rejects the next block */
  TASK_CHECK(vorbis_synthesis(&b,&s.pkt[0])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d,&b)==0);
  TASK_CHECK(vorbis_synthesis(&b,&s.pkt[1])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d,&b)==0);
  TASK_CHECK(vorbis_synthesis(&b,&s.pkt[2])==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d,&b)==OV_EINVAL);
  /* pending() mirrors the classic readable count */
  p0=vorbis_synthesis_pending(&d);
  TASK_CHECK(p0>0&&p0==vorbis_synthesis_pcmout(&d,NULL));
  /* limit cannot be enabled mid-stream */
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d,4096)==OV_EINVAL);
  /* drain, retry, and the classic lap still works */
  TASK_CHECK(vorbis_synthesis_read(&d,(int)p0)==0);
  TASK_CHECK(vorbis_synthesis_blockin(&d,&b)==0);
  while(vorbis_synthesis_pcmout(&d,&pcm)>0)
    TASK_CHECK(vorbis_synthesis_read(&d,vorbis_synthesis_pcmout(&d,NULL))==0);
  laps=vorbis_synthesis_lapout(&d,&pcm);
  TASK_CHECK(laps>0);
  vorbis_block_clear(&b);
  vorbis_dsp_clear(&d);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  stream_free(&s);
  return 0;
}

static int c_cross_exact_stereo(void){
  stream s;
  vorbis_info vi1,vi2;
  vorbis_comment vc1,vc2;
  vorbis_dsp_state d1,d2;
  vorbis_block b1,b2;
  pcmout ref={0},got={0};
  int i,ci=0,rej=0,r,ch=-1;
  long dd;
  TASK_CHECK(stream_encode(&s,2,44100,22050)==0);
  TASK_CHECK(s.shorts>0&&s.longs>0&&s.transitions>0);
  TASK_CHECK(make_info(&s,&vi1,&vc1,0)==0);
  TASK_CHECK(vorbis_synthesis_init(&d1,&vi1)==0);
  TASK_CHECK(vorbis_block_init(&d1,&b1)==0);
  TASK_CHECK(make_info(&s,&vi2,&vc2,0)==0);
  TASK_CHECK(vorbis_synthesis_init(&d2,&vi2)==0);
  TASK_CHECK(vorbis_synthesis_set_pending_limit(&d2,2048)==0);
  TASK_CHECK(vorbis_block_init(&d2,&b2)==0);
  ref.channels=got.channels=2;
  for(i=0;i<s.count;i++){
    TASK_CHECK(vorbis_synthesis(&b1,&s.pkt[i])==0);
    TASK_CHECK(vorbis_synthesis_blockin(&d1,&b1)==0);
    TASK_CHECK(drain_chunk(&d1,&ref,1L<<40)==0);
    TASK_CHECK(vorbis_synthesis(&b2,&s.pkt[i])==0);
    while((r=vorbis_synthesis_blockin(&d2,&b2))==OV_EBACKPRESSURE){
      rej++;
      TASK_CHECK(drain_chunk(&d2,&got,chunks[ci++%3])==0);
    }
    TASK_CHECK(r==0);
    /* identical stream position after every packet */
    TASK_CHECK(d1.granulepos==d2.granulepos);
    TASK_CHECK(drain_chunk(&d2,&got,chunks[ci++%3])==0);
  }
  TASK_CHECK(drain_chunk(&d2,&got,1L<<40)==0);
  TASK_CHECK(rej>0);
  TASK_CHECK(d1.sequence==d2.sequence);
  TASK_CHECK(ref.frames==s.frames&&got.frames==s.frames);
  dd=pcm_diff(&got,&ref,&ch);
  if(dd!=-1)fprintf(stderr,"cross mismatch frame=%ld ch=%d\n",dd,ch);
  TASK_CHECK(dd==-1);
  vorbis_block_clear(&b1);
  vorbis_dsp_clear(&d1);
  vorbis_comment_clear(&vc1);
  vorbis_info_clear(&vi1);
  vorbis_block_clear(&b2);
  vorbis_dsp_clear(&d2);
  vorbis_comment_clear(&vc2);
  vorbis_info_clear(&vi2);
  pcmout_free(&ref);
  pcmout_free(&got);
  stream_free(&s);
  return 0;
}

/* ---------------------------------------------------------------- */

const struct task_case task_cases[]={
  {"Queue","mono_batch",q_mono_batch},
  {"Queue","stereo_batch",q_stereo_batch},
  {"Queue","api_rules",q_api_rules},
  {"Timeline","first_position_late",t_first_position_late},
  {"Timeline","short_eos",t_short_eos},
  {"Timeline","halfrate_odd",t_halfrate_odd},
  {"Timeline","trackonly",t_trackonly},
  {"Timeline","restart_lapout",t_restart_lapout},
  {"Compatibility","encode_roundtrip",c_encode_roundtrip},
  {"Compatibility","oldmode_preserved",c_oldmode_preserved},
  {"Compatibility","cross_exact_stereo",c_cross_exact_stereo},
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
