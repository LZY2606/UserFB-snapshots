/*Regression cases for th_decode_fork().
  Uses real encoder output to verify that forked decoder contexts can take
   divergent non-keyframe suffixes, that reference frames are shared
   read-only and detached on write, that ancestors can be freed in any
   order, and that each branch keeps its own granule clock.*/
#include "check.h"
#include <theora/theoraenc.h>
#include <theora/theoradec.h>
#include <string.h>
#include <stdlib.h>

#define FW 64
#define FH 48
#define CW (FW/2)
#define CH (FH/2)
/*Frames encoded per stream.*/
#define NF 8
/*The two encoded streams are identical before this frame and diverge
   (non-keyframe packets only) from this frame on.*/
#define FORK_AT 4

typedef struct{
  unsigned char *data;
  long           bytes;
}pkt_t;

static pkt_t g_hdr[3];
static int   g_hdr_ready;
static pkt_t g_stream[2][NF];
static int   g_stream_ready;

static unsigned char g_y[FW*FH];
static unsigned char g_u[CW*CH];
static unsigned char g_v[CW*CH];

static void save_pkt(pkt_t *_dst,const ogg_packet *_op){
  _dst->data=(unsigned char *)malloc((size_t)_op->bytes);
  if(_dst->data==NULL)exit(2);
  memcpy(_dst->data,_op->packet,(size_t)_op->bytes);
  _dst->bytes=_op->bytes;
}

/*Encodes two streams with identical headers and an identical prefix; the
   frames from FORK_AT on differ between the two variants.*/
static void encode_streams(void){
  int variant;
  if(g_stream_ready)return;
  for(variant=0;variant<2;variant++){
    th_info         info;
    th_comment      comment;
    th_enc_ctx     *enc;
    th_ycbcr_buffer input;
    ogg_packet      op;
    int             f;
    int             i;
    int             n;
    th_info_init(&info);
    th_comment_init(&comment);
    info.frame_width=info.pic_width=FW;
    info.frame_height=info.pic_height=FH;
    info.fps_numerator=25;
    info.fps_denominator=1;
    info.aspect_numerator=info.aspect_denominator=1;
    info.pixel_fmt=TH_PF_420;
    info.quality=42;
    info.keyframe_granule_shift=6;
    enc=th_encode_alloc(&info);
    if(enc==NULL)exit(2);
    while(th_encode_flushheader(enc,&comment,&op)>0){
      if(!g_hdr_ready){
        static int nh;
        if(nh<3)save_pkt(g_hdr+nh,&op);
        nh++;
      }
    }
    g_hdr_ready=1;
    for(f=0;f<NF;f++){
      int v;
      v=f<FORK_AT?0:variant+1;
      for(i=0;i<FW*FH;i++){
        g_y[i]=(unsigned char)(40+(i%FW)/2+(i/FW)/3+f*2+v*2);
      }
      for(i=0;i<CW*CH;i++){
        g_u[i]=(unsigned char)(100+i%13+f+v);
        g_v[i]=(unsigned char)(160-i%11-f+v);
      }
      input[0]=(th_img_plane){FW,FH,FW,g_y};
      input[1]=(th_img_plane){CW,CH,CW,g_u};
      input[2]=(th_img_plane){CW,CH,CW,g_v};
      if(th_encode_ycbcr_in(enc,input)!=0)exit(2);
      n=0;
      while(th_encode_packetout(enc,f==NF-1,&op)>0){
        if(n==0)save_pkt(g_stream[variant]+f,&op);
        n++;
      }
      if(n!=1)exit(2);
    }
    th_encode_free(enc);
    th_info_clear(&info);
    th_comment_clear(&comment);
  }
  g_stream_ready=1;
}

/*Builds a decoder from the saved headers.
  The setup information is released before this function returns, so any
   fork made from the resulting context must not depend on it.*/
static th_dec_ctx *make_decoder(void){
  th_info        info;
  th_comment     comment;
  th_setup_info *setup;
  th_dec_ctx    *dec;
  ogg_packet     op;
  int            i;
  th_info_init(&info);
  th_comment_init(&comment);
  setup=NULL;
  for(i=0;i<3;i++){
    memset(&op,0,sizeof(op));
    op.packet=g_hdr[i].data;
    op.bytes=g_hdr[i].bytes;
    op.b_o_s=(i==0);
    if(th_decode_headerin(&info,&comment,&setup,&op)<=0){
      th_setup_free(setup);
      th_comment_clear(&comment);
      th_info_clear(&info);
      return NULL;
    }
  }
  dec=th_decode_alloc(&info,setup);
  th_setup_free(setup);
  th_comment_clear(&comment);
  th_info_clear(&info);
  return dec;
}

static int frames_equal(th_ycbcr_buffer _a,th_ycbcr_buffer _b){
  int pli;
  int y;
  for(pli=0;pli<3;pli++){
    if(_a[pli].width!=_b[pli].width||_a[pli].height!=_b[pli].height)return 0;
    for(y=0;y<_a[pli].height;y++){
      if(memcmp(_a[pli].data+y*(ptrdiff_t)_a[pli].stride,
       _b[pli].data+y*(ptrdiff_t)_b[pli].stride,
       (size_t)_a[pli].width))return 0;
    }
  }
  return 1;
}

/*A pixel snapshot of an output frame, so we can prove a shared frame was
   not disturbed by a sibling's decoding.*/
typedef struct{
  unsigned char *pix[3];
  int            w[3];
  int            h[3];
}snap_t;

static void snap_take(snap_t *_s,th_ycbcr_buffer _img){
  int pli;
  int y;
  for(pli=0;pli<3;pli++){
    _s->w[pli]=_img[pli].width;
    _s->h[pli]=_img[pli].height;
    _s->pix[pli]=(unsigned char *)malloc((size_t)(_s->w[pli]*_s->h[pli]));
    if(_s->pix[pli]==NULL)exit(2);
    for(y=0;y<_s->h[pli];y++){
      memcpy(_s->pix[pli]+y*(ptrdiff_t)_s->w[pli],
       _img[pli].data+y*(ptrdiff_t)_img[pli].stride,(size_t)_s->w[pli]);
    }
  }
}

static int snap_matches(const snap_t *_s,th_ycbcr_buffer _img){
  int pli;
  int y;
  for(pli=0;pli<3;pli++){
    if(_img[pli].width!=_s->w[pli]||_img[pli].height!=_s->h[pli])return 0;
    for(y=0;y<_s->h[pli];y++){
      if(memcmp(_s->pix[pli]+y*(ptrdiff_t)_s->w[pli],
       _img[pli].data+y*(ptrdiff_t)_img[pli].stride,(size_t)_s->w[pli])){
        return 0;
      }
    }
  }
  return 1;
}

static void snap_clear(snap_t *_s){
  int pli;
  for(pli=0;pli<3;pli++){
    free(_s->pix[pli]);
    _s->pix[pli]=NULL;
  }
}

static int feed_pkt(th_dec_ctx *_dec,const pkt_t *_p,ogg_int64_t *_gp){
  ogg_packet op;
  memset(&op,0,sizeof(op));
  op.packet=_p->data;
  op.bytes=_p->bytes;
  return th_decode_packetin(_dec,&op,_gp);
}

static int feed_dup(th_dec_ctx *_dec,ogg_int64_t *_gp){
  ogg_packet op;
  memset(&op,0,sizeof(op));
  op.packet=(unsigned char *)"";
  op.bytes=0;
  return th_decode_packetin(_dec,&op,_gp);
}

/*Feeds the same packet to a context under test and to a sequential
   reference decoder, and checks that the return codes, granule positions,
   and decoded pixels all agree.*/
static int check_step(th_dec_ctx *_dec,th_dec_ctx *_ref,const pkt_t *_p){
  ogg_int64_t     gd;
  ogg_int64_t     gr;
  th_ycbcr_buffer od;
  th_ycbcr_buffer orf;
  int             rd;
  int             rr;
  gd=gr=-2;
  rd=feed_pkt(_dec,_p,&gd);
  rr=feed_pkt(_ref,_p,&gr);
  TASK_CHECK(rd==rr);
  TASK_CHECK(gd==gr);
  if(rd==0){
    TASK_CHECK(th_decode_ycbcr_out(_dec,od)==0);
    TASK_CHECK(th_decode_ycbcr_out(_ref,orf)==0);
    TASK_CHECK(frames_equal(od,orf));
  }
  return 0;
}

static void dummy_stripe(void *_ctx,th_ycbcr_buffer _buf,int _yfrag0,
 int _yfrag_end){
  (void)_ctx;
  (void)_buf;
  (void)_yfrag0;
  (void)_yfrag_end;
}

/*The classic preview scenario: decode a shared prefix, fork, then let each
   branch decode a different non-keyframe suffix.*/
static int case_divergent_suffixes(void){
  th_dec_ctx    *src;
  th_dec_ctx    *forked;
  th_dec_ctx    *ref0;
  th_dec_ctx    *ref1;
  th_ycbcr_buffer a;
  th_ycbcr_buffer b;
  ogg_packet      op;
  ogg_int64_t     gs;
  ogg_int64_t     gr0;
  ogg_int64_t     gr1;
  int             i;
  int             pli;
  encode_streams();
  src=make_decoder();
  ref0=make_decoder();
  ref1=make_decoder();
  TASK_CHECK(src!=NULL&&ref0!=NULL&&ref1!=NULL);
  /*The two streams share an identical encoded prefix...*/
  for(i=0;i<FORK_AT;i++){
    TASK_CHECK(g_stream[0][i].bytes==g_stream[1][i].bytes);
    TASK_CHECK(!memcmp(g_stream[0][i].data,g_stream[1][i].data,
     (size_t)g_stream[0][i].bytes));
  }
  /*...and diverge through non-keyframe packets only.*/
  for(i=FORK_AT;i<NF;i++){
    memset(&op,0,sizeof(op));
    op.packet=g_stream[1][i].data;
    op.bytes=g_stream[1][i].bytes;
    TASK_CHECK(th_packet_iskeyframe(&op)==0);
  }
  gs=gr0=gr1=-2;
  for(i=0;i<FORK_AT;i++){
    TASK_CHECK(feed_pkt(src,g_stream[0]+i,&gs)==0);
    TASK_CHECK(feed_pkt(ref0,g_stream[0]+i,&gr0)==0);
    TASK_CHECK(feed_pkt(ref1,g_stream[1]+i,&gr1)==0);
    TASK_CHECK(gs==gr0&&gs==gr1);
  }
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  /*Before either branch advances, both report the same frame through the
     same plane pointers, and share the same clock.*/
  TASK_CHECK(th_decode_ycbcr_out(src,a)==0);
  TASK_CHECK(th_decode_ycbcr_out(forked,b)==0);
  for(pli=0;pli<3;pli++)TASK_CHECK(a[pli].data==b[pli].data);
  TASK_CHECK(frames_equal(a,b));
  TASK_CHECK(th_granule_frame(src,gs)==th_granule_frame(forked,gs));
  TASK_CHECK(th_granule_time(src,gs)==th_granule_time(forked,gs));
  /*The source keeps playing stream 0; the fork previews stream 1.*/
  for(i=FORK_AT;i<NF;i++){
    if(check_step(src,ref0,g_stream[0]+i))return 1;
    if(check_step(forked,ref1,g_stream[1]+i))return 1;
  }
  /*The branches really did diverge.*/
  TASK_CHECK(th_decode_ycbcr_out(src,a)==0);
  TASK_CHECK(th_decode_ycbcr_out(forked,b)==0);
  TASK_CHECK(!frames_equal(a,b));
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref0);
  th_decode_free(ref1);
  return 0;
}

/*A fork of a fork, with the intermediate branch taking the same suffix as
   the source and the nested fork taking a different one.*/
static int case_nested_forks(void){
  th_dec_ctx    *src;
  th_dec_ctx    *a;
  th_dec_ctx    *b;
  th_dec_ctx    *ref0;
  th_dec_ctx    *ref0b;
  th_dec_ctx    *ref1;
  th_ycbcr_buffer os;
  th_ycbcr_buffer oa;
  ogg_int64_t     g;
  int             i;
  encode_streams();
  src=make_decoder();
  ref0=make_decoder();
  ref0b=make_decoder();
  ref1=make_decoder();
  TASK_CHECK(src!=NULL&&ref0!=NULL&&ref0b!=NULL&&ref1!=NULL);
  for(i=0;i<3;i++){
    if(check_step(src,ref0,g_stream[0]+i))return 1;
    TASK_CHECK(feed_pkt(ref0b,g_stream[0]+i,&g)==0);
    TASK_CHECK(feed_pkt(ref1,g_stream[1]+i,&g)==0);
  }
  a=th_decode_fork(src);
  TASK_CHECK(a!=NULL);
  /*Advance the source and the first fork with the same packet; both must
     stay pixel-identical to sequential decoding.*/
  if(check_step(src,ref0,g_stream[0]+3))return 1;
  if(check_step(a,ref0b,g_stream[0]+3))return 1;
  TASK_CHECK(feed_pkt(ref1,g_stream[1]+3,&g)==0);
  TASK_CHECK(th_decode_ycbcr_out(src,os)==0);
  TASK_CHECK(th_decode_ycbcr_out(a,oa)==0);
  TASK_CHECK(frames_equal(os,oa));
  /*Nested fork: fork the already-forked context.*/
  b=th_decode_fork(a);
  TASK_CHECK(b!=NULL);
  for(i=4;i<NF;i++){
    if(check_step(src,ref0,g_stream[0]+i))return 1;
    if(check_step(a,ref0b,g_stream[0]+i))return 1;
    if(check_step(b,ref1,g_stream[1]+i))return 1;
  }
  th_decode_free(b);
  th_decode_free(a);
  th_decode_free(src);
  th_decode_free(ref0);
  th_decode_free(ref0b);
  th_decode_free(ref1);
  return 0;
}

/*Forking must work both when GOLD and PREV alias the same buffer (right
   after a lone keyframe) and when they are distinct buffers.*/
static int case_ref_aliasing(void){
  th_dec_ctx *src;
  th_dec_ctx *forked;
  th_dec_ctx *ref;
  th_dec_ctx *refb;
  ogg_int64_t g;
  int         i;
  encode_streams();
  /*Aliased case: only a keyframe has been decoded, so GOLD==PREV.*/
  src=make_decoder();
  ref=make_decoder();
  refb=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL&&refb!=NULL);
  TASK_CHECK(feed_pkt(src,g_stream[0],&g)==0);
  TASK_CHECK(feed_pkt(ref,g_stream[0],&g)==0);
  TASK_CHECK(feed_pkt(refb,g_stream[0],&g)==0);
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  for(i=1;i<4;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    if(check_step(forked,refb,g_stream[0]+i))return 1;
  }
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  th_decode_free(refb);
  /*Distinct case: an inter frame has moved PREV away from GOLD.*/
  src=make_decoder();
  ref=make_decoder();
  refb=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL&&refb!=NULL);
  for(i=0;i<2;i++){
    TASK_CHECK(feed_pkt(src,g_stream[0]+i,&g)==0);
    TASK_CHECK(feed_pkt(ref,g_stream[0]+i,&g)==0);
    TASK_CHECK(feed_pkt(refb,g_stream[0]+i,&g)==0);
  }
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  for(i=2;i<6;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    if(check_step(forked,refb,g_stream[0]+i))return 1;
  }
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  th_decode_free(refb);
  return 0;
}

/*Forking is also allowed before the first video packet was submitted.*/
static int case_fork_before_packets(void){
  th_dec_ctx *src;
  th_dec_ctx *forked;
  th_dec_ctx *ref;
  th_dec_ctx *refb;
  int         i;
  encode_streams();
  src=make_decoder();
  TASK_CHECK(src!=NULL);
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  ref=make_decoder();
  refb=make_decoder();
  TASK_CHECK(ref!=NULL&&refb!=NULL);
  for(i=0;i<NF;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    if(check_step(forked,refb,g_stream[0]+i))return 1;
  }
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  th_decode_free(refb);
  return 0;
}

/*Ancestors may be freed while their forks keep decoding and forking.*/
static int case_free_ancestors(void){
  th_dec_ctx *src;
  th_dec_ctx *a;
  th_dec_ctx *b;
  th_dec_ctx *ref;
  int         i;
  encode_streams();
  src=make_decoder();
  ref=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL);
  for(i=0;i<4;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
  }
  a=th_decode_fork(src);
  TASK_CHECK(a!=NULL);
  /*Free the parent first; the child must keep decoding bit-exactly.*/
  th_decode_free(src);
  for(i=4;i<6;i++){
    if(check_step(a,ref,g_stream[0]+i))return 1;
  }
  b=th_decode_fork(a);
  TASK_CHECK(b!=NULL);
  /*Free the middle generation; the grandchild still decodes and matches.*/
  th_decode_free(a);
  for(i=6;i<NF;i++){
    if(check_step(b,ref,g_stream[0]+i))return 1;
  }
  th_decode_free(b);
  th_decode_free(ref);
  return 0;
}

/*Frames shared at fork time are read-only: advancing one branch must not
   disturb the picture or the plane pointers of the other.*/
static int case_shared_frame_readonly(void){
  th_dec_ctx    *src;
  th_dec_ctx    *forked;
  th_dec_ctx    *ref;
  th_dec_ctx    *refb;
  th_ycbcr_buffer os;
  th_ycbcr_buffer of;
  th_ycbcr_buffer ob;
  unsigned char  *src_ptrs[3];
  snap_t          snap_s;
  ogg_int64_t     g;
  int             i;
  int             pli;
  encode_streams();
  src=make_decoder();
  ref=make_decoder();
  refb=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL&&refb!=NULL);
  for(i=0;i<4;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    TASK_CHECK(feed_pkt(refb,g_stream[0]+i,&g)==0);
  }
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  TASK_CHECK(th_decode_ycbcr_out(src,os)==0);
  TASK_CHECK(th_decode_ycbcr_out(forked,of)==0);
  for(pli=0;pli<3;pli++){
    TASK_CHECK(os[pli].data==of[pli].data);
    src_ptrs[pli]=os[pli].data;
  }
  snap_take(&snap_s,os);
  /*Advance the fork first; the source's picture and pointers stay put.*/
  for(i=4;i<6;i++){
    if(check_step(forked,refb,g_stream[0]+i))return 1;
  }
  TASK_CHECK(th_decode_ycbcr_out(src,os)==0);
  for(pli=0;pli<3;pli++)TASK_CHECK(os[pli].data==src_ptrs[pli]);
  TASK_CHECK(snap_matches(&snap_s,os));
  /*Now advance the source to the end; the fork's current frame (still the
     shared state it had decoded to) is unaffected.*/
  for(i=4;i<NF;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
  }
  TASK_CHECK(th_decode_ycbcr_out(forked,of)==0);
  TASK_CHECK(th_decode_ycbcr_out(refb,ob)==0);
  TASK_CHECK(frames_equal(of,ob));
  /*The fork can still continue decoding from the shared state.*/
  for(i=6;i<NF;i++){
    if(check_step(forked,refb,g_stream[0]+i))return 1;
  }
  snap_clear(&snap_s);
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  th_decode_free(refb);
  return 0;
}

/*Contexts with post-processing or a stripe callback cannot be forked, and
   stay usable after the rejected fork.*/
static int case_rejected_forks(void){
  th_dec_ctx        *src;
  th_dec_ctx        *forked;
  th_dec_ctx        *ref;
  th_stripe_callback cb;
  th_ycbcr_buffer    os;
  th_ycbcr_buffer    of;
  ogg_int64_t        g;
  int                lvl;
  int                i;
  encode_streams();
  TASK_CHECK(th_decode_fork(NULL)==NULL);
  src=make_decoder();
  ref=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL);
  for(i=0;i<3;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
  }
  /*Enabling post-processing makes the fork fail...*/
  lvl=3;
  TASK_CHECK(th_decode_ctl(src,TH_DECCTL_SET_PPLEVEL,&lvl,sizeof(lvl))==0);
  TASK_CHECK(th_decode_fork(src)==NULL);
  /*...until it is disabled again (no post-processing ever ran).*/
  lvl=0;
  TASK_CHECK(th_decode_ctl(src,TH_DECCTL_SET_PPLEVEL,&lvl,sizeof(lvl))==0);
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  th_decode_free(forked);
  /*A stripe callback also blocks forking.*/
  cb.ctx=NULL;
  cb.stripe_decoded=dummy_stripe;
  TASK_CHECK(th_decode_ctl(src,TH_DECCTL_SET_STRIPE_CB,&cb,sizeof(cb))==0);
  TASK_CHECK(th_decode_fork(src)==NULL);
  cb.stripe_decoded=NULL;
  TASK_CHECK(th_decode_ctl(src,TH_DECCTL_SET_STRIPE_CB,&cb,sizeof(cb))==0);
  /*The source is still a perfectly good decoder after the rejections.*/
  for(i=3;i<6;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
  }
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  for(i=6;i<NF;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    TASK_CHECK(feed_pkt(forked,g_stream[0]+i,&g)==0);
  }
  TASK_CHECK(th_decode_ycbcr_out(src,os)==0);
  TASK_CHECK(th_decode_ycbcr_out(forked,of)==0);
  TASK_CHECK(frames_equal(os,of));
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  return 0;
}

/*Each branch keeps its own granule clock; adjusting one does not leak into
   the other.*/
static int case_granule_clock(void){
  th_dec_ctx    *src;
  th_dec_ctx    *forked;
  th_dec_ctx    *ref;
  th_ycbcr_buffer os;
  th_ycbcr_buffer of;
  ogg_int64_t     gs;
  ogg_int64_t     gf;
  ogg_int64_t     gr;
  ogg_int64_t     ng;
  int             rs;
  int             rf;
  int             i;
  encode_streams();
  src=make_decoder();
  ref=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL);
  gs=gr=-2;
  for(i=0;i<FORK_AT;i++){
    TASK_CHECK(feed_pkt(src,g_stream[0]+i,&gs)==0);
    TASK_CHECK(feed_pkt(ref,g_stream[0]+i,&gr)==0);
    TASK_CHECK(gs==gr);
  }
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  /*The fork inherits the exact clock of the source.*/
  TASK_CHECK(th_granule_frame(src,gs)==th_granule_frame(forked,gs));
  TASK_CHECK(th_granule_time(src,gs)==th_granule_time(forked,gs));
  for(i=FORK_AT;i<6;i++){
    rs=feed_pkt(src,g_stream[0]+i,&gs);
    rf=feed_pkt(forked,g_stream[0]+i,&gf);
    TASK_CHECK(rs==rf&&gs==gf);
    TASK_CHECK(feed_pkt(ref,g_stream[0]+i,&gr)==rs&&gr==gs);
  }
  /*Rewinding the fork's clock leaves the source's clock untouched.*/
  ng=(ogg_int64_t)2<<6;
  TASK_CHECK(th_decode_ctl(forked,TH_DECCTL_SET_GRANPOS,&ng,sizeof(ng))==0);
  TASK_CHECK(feed_pkt(forked,g_stream[0]+6,&gf)==0);
  TASK_CHECK(feed_pkt(src,g_stream[0]+6,&gs)==0);
  TASK_CHECK(feed_pkt(ref,g_stream[0]+6,&gr)==0);
  TASK_CHECK(gs==gr);
  TASK_CHECK(gf!=gs);
  TASK_CHECK(th_granule_frame(forked,gf)==
   th_granule_frame(forked,ng));
  /*Both still decode the same pixels; only the timestamps differ.*/
  TASK_CHECK(feed_pkt(forked,g_stream[0]+7,&gf)==0);
  if(check_step(src,ref,g_stream[0]+7))return 1;
  TASK_CHECK(th_decode_ycbcr_out(src,os)==0);
  TASK_CHECK(th_decode_ycbcr_out(forked,of)==0);
  TASK_CHECK(frames_equal(os,of));
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  return 0;
}

/*Duplicate (dropped) frame packets keep the picture and advance each
   branch's clock independently.*/
static int case_dup_frames(void){
  th_dec_ctx    *src;
  th_dec_ctx    *forked;
  th_dec_ctx    *ref;
  th_dec_ctx    *refd;
  th_ycbcr_buffer out;
  snap_t          snap;
  ogg_int64_t     gs;
  ogg_int64_t     gd;
  ogg_int64_t     gt;
  int             rs;
  int             rd;
  int             i;
  encode_streams();
  src=make_decoder();
  ref=make_decoder();
  refd=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL&&refd!=NULL);
  gs=-2;
  for(i=0;i<4;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    TASK_CHECK(feed_pkt(refd,g_stream[0]+i,&gs)==0);
  }
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  TASK_CHECK(th_decode_ycbcr_out(src,out)==0);
  snap_take(&snap,out);
  /*A duplicate frame packet keeps the picture and bumps the clock by one
     frame in both the source and the sequential reference.*/
  rs=feed_dup(src,&gs);
  rd=feed_dup(ref,&gd);
  TASK_CHECK(rs==TH_DUPFRAME&&rd==TH_DUPFRAME);
  TASK_CHECK(gs==gd);
  TASK_CHECK(th_decode_ycbcr_out(src,out)==0);
  TASK_CHECK(snap_matches(&snap,out));
  /*The fork runs its own clock: give it two duplicates (mirrored in its
     own reference).*/
  TASK_CHECK(feed_dup(forked,&gt)==TH_DUPFRAME);
  TASK_CHECK(feed_dup(forked,&gd)==TH_DUPFRAME);
  TASK_CHECK(feed_dup(refd,&gt)==TH_DUPFRAME);
  TASK_CHECK(feed_dup(refd,&gt)==TH_DUPFRAME);
  TASK_CHECK(gd==gt);
  TASK_CHECK(gd!=gs);
  TASK_CHECK(th_decode_ycbcr_out(forked,out)==0);
  TASK_CHECK(snap_matches(&snap,out));
  /*Both branches continue with real packets against their references.*/
  for(i=4;i<NF;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    if(check_step(forked,refd,g_stream[0]+i))return 1;
  }
  snap_clear(&snap);
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  th_decode_free(refd);
  return 0;
}

/*Post-processing enabled on a fork after forking uses the fork's own
   buffers and does not affect the source.*/
static int case_pp_after_fork(void){
  th_dec_ctx *src;
  th_dec_ctx *forked;
  th_dec_ctx *ref;
  th_dec_ctx *refpp;
  ogg_int64_t g;
  int         lvl;
  int         i;
  encode_streams();
  src=make_decoder();
  ref=make_decoder();
  refpp=make_decoder();
  TASK_CHECK(src!=NULL&&ref!=NULL&&refpp!=NULL);
  for(i=0;i<FORK_AT;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    TASK_CHECK(feed_pkt(refpp,g_stream[0]+i,&g)==0);
  }
  forked=th_decode_fork(src);
  TASK_CHECK(forked!=NULL);
  lvl=0;
  TASK_CHECK(th_decode_ctl(forked,TH_DECCTL_GET_PPLEVEL_MAX,&lvl,
   sizeof(lvl))==0);
  TASK_CHECK(lvl>0);
  /*Enable post-processing on the fork and on its reference only.*/
  lvl=2;
  TASK_CHECK(th_decode_ctl(forked,TH_DECCTL_SET_PPLEVEL,&lvl,
   sizeof(lvl))==0);
  TASK_CHECK(th_decode_ctl(refpp,TH_DECCTL_SET_PPLEVEL,&lvl,
   sizeof(lvl))==0);
  for(i=FORK_AT;i<NF;i++){
    if(check_step(src,ref,g_stream[0]+i))return 1;
    if(check_step(forked,refpp,g_stream[0]+i))return 1;
  }
  th_decode_free(forked);
  th_decode_free(src);
  th_decode_free(ref);
  th_decode_free(refpp);
  return 0;
}

const struct task_case task_cases[]={
  {"branches","divergent_suffixes",case_divergent_suffixes},
  {"branches","nested_forks",case_nested_forks},
  {"branches","ref_aliasing",case_ref_aliasing},
  {"branches","fork_before_packets",case_fork_before_packets},
  {"ownership","free_ancestors",case_free_ancestors},
  {"ownership","shared_frame_readonly",case_shared_frame_readonly},
  {"ownership","rejected_forks",case_rejected_forks},
  {"timeline","granule_clock",case_granule_clock},
  {"timeline","dup_frames",case_dup_frames},
  {"timeline","pp_after_fork",case_pp_after_fork}
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
