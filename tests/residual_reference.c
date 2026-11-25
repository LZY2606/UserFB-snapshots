/********************************************************************
 * Independent residual/token regression for the Theora decoder.
 *
 * The production decoder groups DCT tokens by coefficient position and
 *  reconstructs fragment by fragment inside pipelined MCUs.  This test builds
 *  legal short bitstreams with its own C generator, computes every expected
 *  DC differential, predictor history, zig-zag coefficient event and DC-only
 *  pixel value with an independent model, and checks them through the public
 *  th_decode_packetin() entry point.
 *
 * Expectations are never derived from production decoder outputs.  The only
 *  production data read back are geometry/dequantizer tables (format facts
 *  derived from the parsed headers) and the OC_TASK_OBSERVE observation hooks
 *  (raw pre-prediction differentials, pre-transform coefficients, pre-filter
 *  pixels), which are compared against independently computed values.
 *
 * The token writer uses the public TH_VP31_HUFF_CODES bit patterns but never
 *  the production internal token table, OC_DCT_CODE_WORD, ti0 or eob_runs.
 ********************************************************************/

#include <theora/theoraenc.h>
#include <theora/theoradec.h>
#include "decint.h"
#include "task_support/check.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Observation hook wiring (only present in the OC_TASK_OBSERVE build).   */
/* ====================================================================== */

typedef struct oc_task_obs oc_task_obs;
struct oc_task_obs{
  void (*dc_hook)(ptrdiff_t _fragi,int _pli,int _refi,int _diff,int _pred,
   int _recon_dc);
  void (*coeff_hook)(ptrdiff_t _fragi,int _pli,int _last_zzi,
   const ogg_int16_t _coeffs[64]);
  void (*pixel_hook)(ptrdiff_t _fragi,int _pli,const unsigned char *_dst,
   int _ystride);
  void *ctx;
};
extern oc_task_obs oc_task_obs_hook;

#define MAX_RECORDS 2048

typedef struct{
  ptrdiff_t fragi;
  int       pli;
  int       refi;
  int       diff;
  int       pred;
  int       recon_dc;
}dc_rec;

typedef struct{
  ptrdiff_t   fragi;
  int         pli;
  int         last_zzi;
  ogg_int16_t coeffs[64];
}coeff_rec;

typedef struct{
  ptrdiff_t      fragi;
  int            pli;
  unsigned char  px[64];
}pixel_rec;

typedef struct{
  dc_rec    dc[MAX_RECORDS];
  coeff_rec coeff[MAX_RECORDS];
  pixel_rec pixel[MAX_RECORDS];
  int       ndc;
  int       ncoeff;
  int       npixel;
}obs_store;

static obs_store OBS;

static void obs_reset(void){
  memset(&OBS,0,sizeof(OBS));
}

static void on_dc(ptrdiff_t fragi,int pli,int refi,int diff,int pred,
 int recon_dc){
  dc_rec *r;
  if(OBS.ndc>=MAX_RECORDS)return;
  r=&OBS.dc[OBS.ndc++];
  r->fragi=fragi;r->pli=pli;r->refi=refi;r->diff=diff;r->pred=pred;
  r->recon_dc=recon_dc;
}

static void on_coeff(ptrdiff_t fragi,int pli,int last_zzi,
 const ogg_int16_t coeffs[64]){
  coeff_rec *r;
  if(OBS.ncoeff>=MAX_RECORDS)return;
  r=&OBS.coeff[OBS.ncoeff++];
  r->fragi=fragi;r->pli=pli;r->last_zzi=last_zzi;
  memcpy(r->coeffs,coeffs,64*sizeof(*coeffs));
}

static void on_pixel(ptrdiff_t fragi,int pli,const unsigned char *dst,
 int ystride){
  pixel_rec *r;
  int row,col;
  if(OBS.npixel>=MAX_RECORDS)return;
  r=&OBS.pixel[OBS.npixel++];
  r->fragi=fragi;r->pli=pli;
  for(row=0;row<8;row++)for(col=0;col<8;col++)
    r->px[row*8+col]=dst[row*ystride+col];
}

static void obs_attach(void){
  oc_task_obs_hook.dc_hook=on_dc;
  oc_task_obs_hook.coeff_hook=on_coeff;
  oc_task_obs_hook.pixel_hook=on_pixel;
  oc_task_obs_hook.ctx=NULL;
}

static void obs_detach(void){
  memset(&oc_task_obs_hook,0,sizeof(oc_task_obs_hook));
}

static dc_rec *obs_find_dc(ptrdiff_t fragi){
  int i;
  for(i=0;i<OBS.ndc;i++)if(OBS.dc[i].fragi==fragi)return &OBS.dc[i];
  return NULL;
}

static coeff_rec *obs_find_coeff(ptrdiff_t fragi){
  int i;
  for(i=0;i<OBS.ncoeff;i++)if(OBS.coeff[i].fragi==fragi)
    return &OBS.coeff[i];
  return NULL;
}

static pixel_rec *obs_find_pixel(ptrdiff_t fragi){
  int i;
  for(i=0;i<OBS.npixel;i++)if(OBS.pixel[i].fragi==fragi)
    return &OBS.pixel[i];
  return NULL;
}

/* ====================================================================== */
/* MSB-first bit writer.                                                  */
/* ====================================================================== */

typedef struct{
  unsigned char  *buf;
  size_t          cap;
  size_t          len;
  unsigned long long window;
  int             nbits;
}bitw;

static void bw_init(bitw *bw,unsigned char *buf,size_t cap){
  bw->buf=buf;bw->cap=cap;bw->len=0;bw->window=0;bw->nbits=0;
}

static void bw_drain(bitw *bw){
  while(bw->nbits>=8){
    bw->buf[bw->len++]=(unsigned char)(bw->window>>56);
    bw->window<<=8;
    bw->nbits-=8;
  }
}

static void bw_put(bitw *bw,unsigned long long value,int n){
  if(n==0)return;
  /* A 64-bit window always has room for one symbol (the longest Huffman code
     plus its extra bits is well under 56 bits) after the previous drain. */
  bw->window|=(value&((1ULL<<n)-1))<<(64-bw->nbits-n);
  bw->nbits+=n;
  bw_drain(bw);
}

static size_t bw_finish(bitw *bw){
  if(bw->nbits>0){
    bw->buf[bw->len++]=(unsigned char)(bw->window>>56);
    bw->window=0;bw->nbits=0;
  }
  return bw->len;
}

static void bw_huff(bitw *bw,int table,int token){
  th_huff_code c=TH_VP31_HUFF_CODES[table][token];
  bw_put(bw,c.pattern,c.nbits);
}

/* ====================================================================== */
/* Independent format constants and tables.                               */
/* ====================================================================== */

/* Independent zig-zag scan: zig-zag position -> natural coefficient index,
  and its inverse.  Written directly, not imported from production. */
static const unsigned char MY_FZIGZAG[64]={
   0, 1, 8,16, 9, 2, 3,10,
  17,24,32,25,18,11, 4, 5,
  12,19,26,33,40,48,41,34,
  27,20,13, 6, 7,14,21,28,
  35,42,49,56,57,50,43,36,
  29,22,15,23,30,37,44,51,
  58,59,52,45,38,31,39,46,
  53,60,61,54,47,55,62,63
};

static unsigned char my_izigzag(int natural){
  int z;
  for(z=0;z<64;z++)if(MY_FZIGZAG[z]==natural)return (unsigned char)z;
  return 0;
}

/* External spec DCT token indices. */
enum{
  T_EOB1=0,T_EOB2=1,T_EOB3=2,
  T_REP_RUN0=3,T_REP_RUN1=4,T_REP_RUN2=5,T_REP_RUN3=6,
  T_SHORT_ZRL=7,T_ZRL=8,
  T_ONE=9,T_MINUS_ONE=10,T_TWO=11,T_MINUS_TWO=12
};

/* Reference frame identities (mirror the bitstream OC_FRAME_* semantics,
  kept local on purpose). */
#define REF_GOLD 0
#define REF_PREV 1
#define REF_SELF 2
#define REF_NONE 3

/* Macro block modes we emit through the scheme-7 CLC alphabet. */
#define MODE_INTER_NOMV  0
#define MODE_INTRA       1
#define MODE_GOLDEN_NOMV 5

/* ====================================================================== */
/* Independent coefficient value/run tokenizer (external tokens only).    */
/* ====================================================================== */

typedef struct{ int token; unsigned eb; int neb; }val_enc;

static int my_abs(int x){return x<0?-x:x;}

/* Map a signed quantized coefficient to an external value token plus extra
  bits.  The category ranges follow the public Theora token specification
  (verified against TH_VP31_HUFF_CODES decoding); no production internal code
  word table is consulted.  Each token's extra-bit range splits into a low
  positive half and a high negative half. */
static val_enc encode_value(int v){
  int a=my_abs(v);
  int neg=v<0;
  val_enc e;
  if(a==1){e.token=neg?T_MINUS_ONE:T_ONE;e.eb=0;e.neb=0;return e;}
  if(a==2){e.token=neg?T_MINUS_TWO:T_TWO;e.eb=0;e.neb=0;return e;}
  /* CAT2: tokens 13..16, one extra bit, magnitudes 3..6. */
  if(a<=6){
    e.token=13+(a-3);e.neb=1;e.eb=neg?1:0;return e;
  }
  /* Each wider category is a single external token whose extra-bit range
     splits into a low positive half and a high negative half. */
  if(a<=8){e.token=17;e.neb=2;e.eb=neg?(unsigned)(2+(a-7)):(unsigned)(a-7);return e;}
  if(a<=12){e.token=18;e.neb=3;e.eb=neg?(unsigned)(4+(a-9)):(unsigned)(a-9);return e;}
  if(a<=20){e.token=19;e.neb=4;e.eb=neg?(unsigned)(8+(a-13)):(unsigned)(a-13);return e;}
  if(a<=36){e.token=20;e.neb=5;e.eb=neg?(unsigned)(16+(a-21)):(unsigned)(a-21);return e;}
  if(a<=68){e.token=21;e.neb=6;e.eb=neg?(unsigned)(32+(a-37)):(unsigned)(a-37);return e;}
  e.token=22;e.neb=10;
  e.eb=neg?(unsigned)(512+(a-69)):(unsigned)(a-69);
  return e;
}

static void emit_value(bitw *bw,int huff_group,int v){
  val_enc e=encode_value(v);
  bw_huff(bw,huff_group,e.token);
  if(e.neb)bw_put(bw,e.eb,e.neb);
}

/* Emit an end-of-block run of 1..4095 blocks. */
static void emit_eob_run(bitw *bw,int huff_group,int run){
  if(run<=3){
    bw_huff(bw,huff_group,T_EOB1+(run-1));
  }
  else if(run<=7){
    bw_huff(bw,huff_group,T_REP_RUN0);
    bw_put(bw,(unsigned)(run-4),2);
  }
  else if(run<=15){
    bw_huff(bw,huff_group,T_REP_RUN1);
    bw_put(bw,(unsigned)(run-8),3);
  }
  else if(run<=31){
    bw_huff(bw,huff_group,T_REP_RUN2);
    bw_put(bw,(unsigned)(run-16),4);
  }
  else{
    bw_huff(bw,huff_group,T_REP_RUN3);
    bw_put(bw,(unsigned)run,12);
  }
}

/* Emit a pure within-block zero run of 1..63 zero positions.  Empirically the
  landed coefficient sits at (start_stack + extra_bits + 1), so a run of N
  zeros ending just before the value encodes N-1 extra bits. */
static void emit_zero_run(bitw *bw,int huff_group,int len){
  if(len<=8){
    bw_huff(bw,huff_group,T_SHORT_ZRL);
    bw_put(bw,(unsigned)(len-1),3);
  }
  else{
    /* ZRL shares the same landing rule; clamp to its 6-bit field. */
    bw_huff(bw,huff_group,T_ZRL);
    bw_put(bw,(unsigned)(len-1),6);
  }
}

/* ====================================================================== */
/* RLE coders for super-block and fragment coded flags and QI runs.       */
/* ====================================================================== */

static void emit_sb_run(bitw *bw,int run){
  if(run==1){bw_put(bw,0,1);}
  else if(run<=3){bw_put(bw,0b10,2);bw_put(bw,run-2,1);}
  else if(run<=5){bw_put(bw,0b110,3);bw_put(bw,run-4,1);}
  else if(run<=9){bw_put(bw,0b1110,4);bw_put(bw,run-6,2);}
  else if(run<=17){bw_put(bw,0b11110,5);bw_put(bw,run-10,3);}
  else if(run<=33){bw_put(bw,0b111110,6);bw_put(bw,run-18,4);}
  else{bw_put(bw,0b111111,6);bw_put(bw,run-34,12);}
}

static void emit_block_run(bitw *bw,int run){
  if(run<=2){bw_put(bw,0,1);bw_put(bw,run-1,1);}
  else if(run<=4){bw_put(bw,0b10,2);bw_put(bw,run-3,1);}
  else if(run<=6){bw_put(bw,0b110,3);bw_put(bw,run-5,1);}
  else if(run<=10){bw_put(bw,0b1110,4);bw_put(bw,run-7,2);}
  else if(run<=14){bw_put(bw,0b11110,5);bw_put(bw,run-11,2);}
  else{bw_put(bw,0b11111,5);bw_put(bw,run-15,4);}
}

static void emit_flag_runs(bitw *bw,const unsigned char *flags,int n,
 void (*run_writer)(bitw *,int)){
  int i=0;
  bw_put(bw,flags[0],1);
  while(i<n){
    int j=i+1;
    while(j<n&&flags[j]==flags[i])j++;
    run_writer(bw,j-i);
    i=j;
  }
}

/* ====================================================================== */
/* Frame geometry and independent fragment bookkeeping.                   */
/* ====================================================================== */

typedef struct{
  int w,h,pf;
  int nh[3],nv[3],nf[3],froff[3];
  int total_frags;
}geom;

static void geom_init(geom *g,int w,int h,int pf){
  int hdec=!(pf&1),vdec=!(pf&2);
  g->w=w;g->h=h;g->pf=pf;
  g->nh[0]=w>>3;g->nv[0]=h>>3;
  g->nh[1]=g->nh[2]=g->nh[0]>>hdec;
  g->nv[1]=g->nv[2]=g->nv[0]>>vdec;
  g->froff[0]=0;
  g->nf[0]=g->nh[0]*g->nv[0];
  g->nf[1]=g->nf[2]=g->nh[1]*g->nv[1];
  g->froff[1]=g->nf[0];
  g->froff[2]=g->nf[0]+g->nf[1];
  g->total_frags=g->nf[0]+g->nf[1]+g->nf[2];
}

/* Independent Hilbert traversal inside a 4x4 super block, matching the order
  in which the bitstream coded-flag/token walks visit fragments.  The (dx,dy)
  sequence is the standard 4x4 Hilbert curve orientation used by Theora; it
  is hand-derived rather than imported from the production super-block map. */
static const int SB_HILBERT[16][2]={
  {0,0},{1,0},{1,1},{0,1},
  {0,2},{0,3},{1,3},{1,2},
  {2,2},{2,3},{3,3},{3,2},
  {3,1},{2,1},{2,0},{3,0}
};

static int traversal_list(const geom *g,int pli,int *out,int maxn){
  int nh=g->nh[pli],nv=g->nv[pli];
  int nhsb=(nh+3)>>2,nvsb=(nv+3)>>2;
  int n=0,sby,sbx,k;
  for(sby=0;sby<nvsb;sby++){
    for(sbx=0;sbx<nhsb;sbx++){
      for(k=0;k<16;k++){
        int gx=sbx*4+SB_HILBERT[k][0];
        int gy=sby*4+SB_HILBERT[k][1];
        if(gx<nh&&gy<nv&&n<maxn)
          out[n++]=g->froff[pli]+gy*nh+gx;
      }
    }
  }
  return n;
}

/* ====================================================================== */
/* Independent per-frame fragment specification.                          */
/* ====================================================================== */

#define MAX_FRAGS 384

typedef struct{
  int coeff[64]; /* natural-order quantized coefficient matrix  */
  int dc_diff;   /* raw DC differential before prediction        */
  int coded;
  int refi;      /* reference identity for DC/pixel reconstruction */
  int mode;
  int qii;       /* AC quantizer index                            */
}frag_spec;

typedef struct{
  geom      g;
  int       is_key;
  int       qi0;
  int       qi1,qi2;
  int       nqis;
  frag_spec f[3][MAX_FRAGS];
  int       nplane[3];
}frame_spec;

static void spec_init(frame_spec *fs,int w,int h,int pf,int is_key,
 int qi0){
  memset(fs,0,sizeof(*fs));
  geom_init(&fs->g,w,h,pf);
  fs->is_key=is_key;
  fs->qi0=qi0;
  fs->nqis=1;
  fs->qi1=fs->qi2=qi0;
  if(is_key){
    int pli,i;
    for(pli=0;pli<3;pli++){
      fs->nplane[pli]=fs->g.nf[pli];
      for(i=0;i<fs->g.nf[pli];i++){
        fs->f[pli][i].coded=1;
        fs->f[pli][i].refi=REF_SELF;
        fs->f[pli][i].mode=MODE_INTRA;
      }
    }
  }
  else{
    int pli;
    for(pli=0;pli<3;pli++)fs->nplane[pli]=fs->g.nf[pli];
  }
}

/* Assign one reference identity and macro-block mode to every fragment of the
  2x2 macro block whose top-left luma fragment is (mbx,mby).  The bitstream
  codes modes per macro block, so all covered fragments share the reference. */
static void set_mb_ref(frame_spec *fs,int mbx,int mby,int golden){
  int hdec=!(fs->g.pf&1),vdec=!(fs->g.pf&2);
  int refi=golden?REF_GOLD:REF_SELF;
  int mode=golden?MODE_GOLDEN_NOMV:MODE_INTRA;
  int dx,dy,pli;
  for(dy=0;dy<2;dy++)for(dx=0;dx<2;dx++){
    int x=mbx+dx,y=mby+dy;
    if(x<fs->g.nh[0]&&y<fs->g.nv[0]){
      frag_spec *sp=&fs->f[0][y*fs->g.nh[0]+x];
      if(sp->coded){sp->refi=refi;sp->mode=mode;}
    }
  }
  for(pli=1;pli<3;pli++){
    int cbx0=mbx>>hdec,cby0=mby>>vdec;
    int cw=2>>hdec,ch=2>>vdec,ax,ay;
    for(ay=0;ay<ch;ay++)for(ax=0;ax<cw;ax++){
      int cx=cbx0+ax,cy=cby0+ay;
      if(cx<fs->g.nh[pli]&&cy<fs->g.nv[pli]){
        frag_spec *sp=&fs->f[pli][cy*fs->g.nh[pli]+cx];
        if(sp->coded){sp->refi=refi;sp->mode=mode;}
      }
    }
  }
}

/* Macro-block grid taken from the decoder's parsed super-block/macro-block
  map, which is a structural format fact (like the dequantizer tables), not a
  residual expectation.  Reading it guarantees exact mbi slot alignment even
  for truncated 4:4:4/4:2:2 grids.  Invalid mbi slots get (-1,-1). */
static int mb_grid_from_decoder(const th_dec_ctx *dec,
 int out_x[],int out_y[],int maxn){
  int nh=dec->state.fplanes[0].nhfrags;
  int nmbs=(int)dec->state.nmbs;
  int mbi;
  for(mbi=0;mbi<maxn;mbi++){out_x[mbi]=-1;out_y[mbi]=-1;}
  for(mbi=0;mbi<nmbs&&mbi<maxn;mbi++){
    /* Top-left luma fragment is the [0][0] map entry when the macro block is
       valid. */
    ptrdiff_t v=dec->state.mb_maps[mbi][0][0];
    if(v>=0){
      out_x[mbi]=(int)(v%nh);
      out_y[mbi]=(int)(v/nh);
    }
  }
  return nmbs;
}

/* ====================================================================== */
/* Independent DC prediction model.                                       */
/* ====================================================================== */

typedef struct{
  int pred_last[3][4]; /* per plane, per reference identity */
}dc_model;

static void dc_model_reset(dc_model *m){
  memset(m->pred_last,0,sizeof(m->pred_last));
}

/* Data-driven same-reference neighbor blending.  Every non-empty neighbor mask
  below is derived directly from the Theora DC prediction specification as an
  explicit weighted average (the production code instead packs them into a
  switch statement).  All integer divisions truncate toward zero, including on
  negative numerators.  An empty mask falls back to the reference's own causal
  predictor history. */
static int dc_predict(dc_model *m,const frame_spec *fs,
 const unsigned char *coded_map,int pli,int fx,int fy,int refi,
 const int *recon_dc){
  const geom *g=&fs->g;
  int nh=g->nh[pli];
  int l=0,ul=0,u=0,ur=0;
  int mask=0;
  if(fy==0)return m->pred_last[pli][refi];
  if(fx>0&&coded_map[fy*nh+fx-1]&&
   fs->f[pli][fy*nh+fx-1].refi==refi){
    mask|=1;l=recon_dc[fy*nh+fx-1];
  }
  if(fx>0&&coded_map[(fy-1)*nh+fx-1]&&
   fs->f[pli][(fy-1)*nh+fx-1].refi==refi){
    mask|=2;ul=recon_dc[(fy-1)*nh+fx-1];
  }
  if(coded_map[(fy-1)*nh+fx]&&
   fs->f[pli][(fy-1)*nh+fx].refi==refi){
    mask|=4;u=recon_dc[(fy-1)*nh+fx];
  }
  if(fx+1<nh&&coded_map[(fy-1)*nh+fx+1]&&
   fs->f[pli][(fy-1)*nh+fx+1].refi==refi){
    mask|=8;ur=recon_dc[(fy-1)*nh+fx+1];
  }
  if(mask==0)return m->pred_last[pli][refi];
  switch(mask){
    case 1: case 3:
      return l;
    case 2:
      return ul;
    case 4: case 6: case 12:
      return u;
    case 5:
      return (l+u)/2;
    case 8:
      return ur;
    case 9: case 11: case 13:
      return (75*l+53*ur)/128;
    case 10:
      return (ul+ur)/2;
    case 14:
      return (3*(ul+ur)+10*u)/16;
    case 7: case 15:{
      int p=(29*(l+u)-26*ul)/32;
      /* Spatial prediction >128 from any anchor falls back in the fixed
         order upper, left, upper-left. */
      if(my_abs(p-u)>128)return u;
      if(my_abs(p-l)>128)return l;
      if(my_abs(p-ul)>128)return ul;
      return p;
    }
  }
  return m->pred_last[pli][refi];
}

typedef struct{
  ptrdiff_t fragi;
  int       pli,refi,diff,pred,recon;
}dc_expect;

static int model_run_frame(dc_model *m,const frame_spec *fs,
 dc_expect *out,int maxout){
  int pli,n=0;
  for(pli=0;pli<3;pli++){
    int nh=fs->g.nh[pli],nv=fs->g.nv[pli];
    int *recon=(int *)calloc((size_t)nh*nv,sizeof(int));
    unsigned char *coded_map=(unsigned char *)calloc((size_t)nh*nv,1);
    int fy,fx;
    for(fy=0;fy<nv;fy++)for(fx=0;fx<nh;fx++){
      int local=fy*nh+fx;
      const frag_spec *sp=&fs->f[pli][local];
      coded_map[local]=sp->coded;
      if(!sp->coded)continue;
      {
        int pred=dc_predict(m,fs,coded_map,pli,fx,fy,sp->refi,recon);
        int rc=sp->dc_diff+pred;
        recon[local]=rc;
        m->pred_last[pli][sp->refi]=rc;
        if(n<maxout){
          out[n].fragi=fs->g.froff[pli]+local;
          out[n].pli=pli;out[n].refi=sp->refi;
          out[n].diff=sp->dc_diff;out[n].pred=pred;out[n].recon=rc;
          n++;
        }
      }
    }
    free(recon);
    free(coded_map);
  }
  return n;
}

/* ====================================================================== */
/* Independent residual token compiler.                                   */
/*                                                                        */
/* Tokens are grouped by coefficient position.  DC tokens form one stream  */
/* per plane; AC positions 1..63 each carry per-plane events.  EOB runs   */
/* batch consecutive blocks that end at the same position, and zero runs  */
/* stay within a single block.                                            */
/* ====================================================================== */

static void emit_plane_dc(bitw *bw,const frame_spec *fs,int pli,
 const int *trav,int ntrav){
  int pending_eob=0;
  int k;
  /* The production DC walk iterates only over coded fragments (the coded
     order); uncoded fragments occupy no token at all.  An EOB run therefore
     covers successive *coded* fragments whose DC is zero, never uncoded
     fragments.  Test specs keep coded differentials non-zero, so a value
     token is emitted for every coded fragment and EOB runs only terminate a
     trailing group when the stream ends. */
  for(k=0;k<ntrav;k++){
    int local=trav[k]-fs->g.froff[pli];
    const frag_spec *sp=&fs->f[pli][local];
    if(!sp->coded)continue;
    if(sp->dc_diff==0){
      pending_eob++;
      continue;
    }
    if(pending_eob){
      emit_eob_run(bw,0,pending_eob);
      pending_eob=0;
    }
    emit_value(bw,0,sp->dc_diff);
  }
  if(pending_eob)emit_eob_run(bw,0,pending_eob);
}

/* Highest zig-zag position (1..63) holding a non-zero AC coefficient, or 0
  when the block has no coded AC. */
static int block_last_ac(const frag_spec *sp){
  int z,last=0;
  for(z=1;z<64;z++)if(sp->coeff[MY_FZIGZAG[z]]!=0)last=z;
  return last;
}

/* Per-block AC event kinds used by the independent event planner. */
enum{ AC_NONE=0,AC_VALUE=1,AC_ZERO_RUN=2,AC_EOB=3 };

typedef struct{
  int kind;
  int run;   /* zero-run length for AC_ZERO_RUN */
  int value;
}ac_event;

/* Plan the independent AC event sequence of one coded block, indexed by
  zig-zag position 1..63.  The bitstream groups one token per coefficient
  stack: a pure zero-run token is emitted on the stack where the run starts,
  the coefficient it lands on is emitted as an ordinary value token on its
  own stack, and EOB terminates the block on the first stack past the last
  coded coefficient. */
static void plan_block_ac(const frag_spec *sp,ac_event ev[64]){
  int z;
  for(z=0;z<64;z++)ev[z].kind=AC_NONE;
  int last=block_last_ac(sp);
  if(last==0){
    ev[1].kind=AC_EOB;
    return;
  }
  z=1;
  while(z<=last){
    int v=sp->coeff[MY_FZIGZAG[z]];
    if(v==0){
      int run=0,z2=z;
      while(z2<=last&&sp->coeff[MY_FZIGZAG[z2]]==0){run++;z2++;}
      if(z2>last){
        ev[z].kind=AC_EOB;
        return;
      }
      ev[z].kind=AC_ZERO_RUN;
      ev[z].run=run;
      ev[z2].kind=AC_VALUE;
      ev[z2].value=sp->coeff[MY_FZIGZAG[z2]];
      z=z2+1;
    }
    else{
      ev[z].kind=AC_VALUE;
      ev[z].value=v;
      z++;
    }
  }
  if(last+1<=63)ev[last+1].kind=AC_EOB;
}

static void emit_residual(bitw *bw,const frame_spec *fs){
  int trav[3][MAX_FRAGS];
  int ntrav[3];
  int pli,zzi;
  for(pli=0;pli<3;pli++)
    ntrav[pli]=traversal_list(&fs->g,pli,trav[pli],MAX_FRAGS);
  /* DC table selects (Y, chroma): always table index 0. */
  bw_put(bw,0,4);
  bw_put(bw,0,4);
  for(pli=0;pli<3;pli++)emit_plane_dc(bw,fs,pli,trav[pli],ntrav[pli]);
  static const int group_first[5]={1,1,6,15,28};
  static const int group_last[5]={1,5,14,27,63};
  int hg;
  for(hg=1;hg<5;hg++){
    bw_put(bw,0,4);
    bw_put(bw,0,4);
    for(zzi=group_first[hg];zzi<=group_last[hg];zzi++){
      int huff_group=hg*16;
      for(pli=0;pli<3;pli++){
        ac_event (*ev)[64]=(ac_event(*)[64])
         malloc((size_t)ntrav[pli]*sizeof(ac_event)*64);
        unsigned char *ended=(unsigned char *)calloc((size_t)ntrav[pli],1);
        int k,pending_eob=0;
        for(k=0;k<ntrav[pli];k++){
          int local=trav[pli][k]-fs->g.froff[pli];
          if(!fs->f[pli][local].coded){ended[k]=1;continue;}
          plan_block_ac(&fs->f[pli][local],ev[k]);
        }
        for(k=0;k<ntrav[pli];k++){
          ac_event *e=&ev[k][zzi];
          if(ended[k])continue;
          if(e->kind==AC_EOB){
            ended[k]=1;
            pending_eob++;
            continue;
          }
          if(e->kind==AC_NONE)continue;
          if(pending_eob){
            emit_eob_run(bw,huff_group,pending_eob);
            pending_eob=0;
          }
          if(e->kind==AC_ZERO_RUN)
            emit_zero_run(bw,huff_group,e->run);
          else
            emit_value(bw,huff_group,e->value);
        }
        if(pending_eob)emit_eob_run(bw,huff_group,pending_eob);
        free(ev);
        free(ended);
      }
    }
  }
}


/* ====================================================================== */
/* Packet construction.                                                    */
/* ====================================================================== */

static th_dec_ctx *make_decoder(int w,int h,int pf,int zero_loop_filter,
 th_enc_ctx **enc_out,th_info *parsed,th_comment *tc,
 th_setup_info **setup_out){
  th_info ti;ogg_packet op;
  th_enc_ctx *enc;
  th_info_init(&ti);th_info_init(parsed);th_comment_init(tc);
  ti.frame_width=ti.pic_width=w;
  ti.frame_height=ti.pic_height=h;
  ti.fps_numerator=25;ti.fps_denominator=1;
  ti.aspect_numerator=ti.aspect_denominator=1;
  ti.pixel_fmt=pf;ti.quality=32;ti.keyframe_granule_shift=6;
  enc=th_encode_alloc(&ti);
  if(enc==NULL)return NULL;
  if(zero_loop_filter){
    th_quant_info qi=TH_VP31_QUANT_INFO;
    int k;
    for(k=0;k<64;k++)qi.loop_filter_limits[k]=0;
    if(th_encode_ctl(enc,TH_ENCCTL_SET_QUANT_PARAMS,&qi,sizeof(qi))<0){
      th_encode_free(enc);return NULL;
    }
  }
  *setup_out=NULL;
  while(th_encode_flushheader(enc,tc,&op)>0){
    if(th_decode_headerin(parsed,tc,setup_out,&op)<0){
      th_encode_free(enc);return NULL;
    }
  }
  {
    th_dec_ctx *dec=th_decode_alloc(parsed,*setup_out);
    int pp=0;
    if(dec==NULL){th_encode_free(enc);return NULL;}
    th_decode_ctl(dec,TH_DECCTL_SET_PPLEVEL,&pp,sizeof(pp));
    *enc_out=enc;
    th_info_clear(&ti);
    return dec;
  }
}

static size_t build_key_packet(unsigned char *buf,const frame_spec *fs){
  bitw bw;bw_init(&bw,buf,1<<16);
  bw_put(&bw,0,1);
  bw_put(&bw,0,1);
  bw_put(&bw,fs->qi0,6);
  bw_put(&bw,0,1);
  bw_put(&bw,0,3);
  emit_residual(&bw,fs);
  return bw_finish(&bw);
}

/* Emit the two-pass binary RLE of per-fragment AC quantizer indices. */
static void emit_qi_runs(bitw *bw,const frame_spec *fs,
 const int *coded_trav,int ncoded){
  unsigned char *pass1=(unsigned char *)malloc((size_t)ncoded);
  int i,n1=0;
  for(i=0;i<ncoded;i++){
    int fi=coded_trav[i],p=0;
    for(;p<3;p++)if(fi>=fs->g.froff[p]&&fi<fs->g.froff[p]+fs->g.nf[p])
      break;
    pass1[i]=fs->f[p][fi-fs->g.froff[p]].qii>=1;
    if(pass1[i])n1++;
  }
  emit_flag_runs(bw,pass1,ncoded,emit_sb_run);
  if(fs->nqis==3&&n1>0){
    unsigned char *pass2=(unsigned char *)malloc((size_t)n1);
    int j=0;
    for(i=0;i<ncoded;i++){
      int fi=coded_trav[i],p=0;
      for(;p<3;p++)if(fi>=fs->g.froff[p]&&fi<fs->g.froff[p]+fs->g.nf[p])
        break;
      if(fs->f[p][fi-fs->g.froff[p]].qii>=1)
        pass2[j++]=fs->f[p][fi-fs->g.froff[p]].qii==2;
    }
    emit_flag_runs(bw,pass2,n1,emit_sb_run);
    free(pass2);
  }
  free(pass1);
}

static size_t build_inter_packet(unsigned char *buf,frame_spec *fs,
 const th_dec_ctx *dec,const int *coded_trav,int ncoded){
  bitw bw;bw_init(&bw,buf,1<<16);
  const geom *g=&fs->g;
  int i,pli;
  bw_put(&bw,0,1);
  bw_put(&bw,1,1);
  bw_put(&bw,fs->qi0,6);
  if(fs->nqis==1)bw_put(&bw,0,1);
  else{
    bw_put(&bw,1,1);bw_put(&bw,fs->qi1,6);
    if(fs->nqis==2)bw_put(&bw,0,1);
    else{bw_put(&bw,1,1);bw_put(&bw,fs->qi2,6);}
  }
  /* Super-block partial/fully-coded flags across all planes.  A super block
     is fully coded when every (in-frame) fragment it covers is coded, partial
     when only a subset is coded, and absent when it has no coded fragment. */
  {
    int nsbs=0;
    for(pli=0;pli<3;pli++)
      nsbs+=((g->nh[pli]+3)>>2)*((g->nv[pli]+3)>>2);
    {
      unsigned char *partial=(unsigned char *)calloc((size_t)nsbs,1);
      unsigned char *full=(unsigned char *)calloc((size_t)nsbs,1);
      int nnonpartial=0;
      /* Count coded fragments and total in-frame fragments per SB. */
      int *coded_cnt=(int *)calloc((size_t)nsbs,sizeof(int));
      int *total_cnt=(int *)calloc((size_t)nsbs,sizeof(int));
      int pli2;
      for(pli2=0;pli2<3;pli2++){
        int nhp=g->nh[pli2],nvp=g->nv[pli2];
        int nhsbp=(nhp+3)>>2,nvsbp=(nvp+3)>>2;
        int base2=0,q2;
        for(q2=0;q2<pli2;q2++)
          base2+=((g->nh[q2]+3)>>2)*((g->nv[q2]+3)>>2);
        int sby2,sbx2;
        for(sby2=0;sby2<nvsbp;sby2++)for(sbx2=0;sbx2<nhsbp;sbx2++){
          int sbi2=sby2*nhsbp+sbx2;
          int dy2,dx2;
          for(dy2=0;dy2<4;dy2++)for(dx2=0;dx2<4;dx2++){
            int fx2=sbx2*4+dx2,fy2=sby2*4+dy2;
            if(fx2>=nhp||fy2>=nvp)continue;
            total_cnt[base2+sbi2]++;
            if(fs->f[pli2][fy2*nhp+fx2].coded)
              coded_cnt[base2+sbi2]++;
          }
        }
      }
      for(i=0;i<nsbs;i++){
        if(coded_cnt[i]==0)continue;
        if(coded_cnt[i]==total_cnt[i])full[i]=1;
        else partial[i]=1;
      }
      /* Partial flag series (absent SBs read as zero). */
      emit_flag_runs(&bw,partial,nsbs,emit_sb_run);
      for(i=0;i<nsbs;i++)if(!partial[i])nnonpartial++;
      /* Fully-coded series spans only the non-partial SBs, in SB order. */
      if(nnonpartial>0){
        unsigned char *fz=(unsigned char *)malloc((size_t)nnonpartial);
        int j=0;
        for(i=0;i<nsbs;i++)if(!partial[i])fz[j++]=full[i];
        emit_flag_runs(&bw,fz,nnonpartial,emit_sb_run);
        free(fz);
      }
      free(partial);
      free(full);
      free(coded_cnt);
      free(total_cnt);
    }
  }
  /* Per-fragment coded flags in traversal order. */
  {
    int *trav=(int *)malloc(sizeof(int)*g->total_frags);
    int ntrav=0;
    for(pli=0;pli<3;pli++)
      ntrav+=traversal_list(g,pli,trav+ntrav,g->total_frags-ntrav);
    {
      /* Only fragments belonging to partial (not fully-coded) super blocks
         consume block-run symbols; fully-coded SB fragments are implicitly
         coded.  Build the series in global traversal order. */
      unsigned char *block_flags=(unsigned char *)malloc((size_t)ntrav);
      int nflags=0;
      for(i=0;i<ntrav;i++){
        int fi=trav[i],p=0;
        int local3,fx3,fy3;
        for(;p<3;p++)if(fi>=g->froff[p]&&fi<g->froff[p]+g->nf[p])break;
        local3=fi-g->froff[p];
        fx3=local3%g->nh[p];fy3=local3/g->nh[p];
        {
          int nhp3=g->nh[p],nvp3=g->nv[p];
          int sbi_x=fx3>>2,sbi_y=fy3>>2;
          int total3=0,coded3=0,dy3,dx3;
          for(dy3=0;dy3<4;dy3++)for(dx3=0;dx3<4;dx3++){
            int ax=sbi_x*4+dx3,ay=sbi_y*4+dy3;
            if(ax>=nhp3||ay>=nvp3)continue;
            total3++;
            if(fs->f[p][ay*nhp3+ax].coded)coded3++;
          }
          if(coded3!=total3)
            block_flags[nflags++]=fs->f[p][local3].coded;
        }
      }
      if(nflags>0)emit_flag_runs(&bw,block_flags,nflags,emit_block_run);
      free(block_flags);
    }
    free(trav);
  }
  /* Macro block modes through the scheme-7 CLC tree; only macro blocks with
     at least one coded luma fragment carry a mode symbol, emitted in the
     production macro-block (Hilbert) order. */
  bw_put(&bw,7,3);
  {
    int mbx[256],mby[256];
    int nmbs=mb_grid_from_decoder(dec,mbx,mby,256);
    int mbi;
    for(mbi=0;mbi<nmbs;mbi++){
      int quad_coded=0,dx,dy;
      if(mbx[mbi]<0)continue;
      for(dy=0;dy<2;dy++)for(dx=0;dx<2;dx++){
        int x=mbx[mbi]+dx,y=mby[mbi]+dy;
        if(x<g->nh[0]&&y<g->nv[0]&&fs->f[0][y*g->nh[0]+x].coded)
          quad_coded=1;
      }
      if(quad_coded)
        bw_put(&bw,fs->f[0][mby[mbi]*g->nh[0]+mbx[mbi]].mode,3);
    }
  }
  /* Motion-vector tree select (VLC); NOMV/INTRA modes consume no vectors. */
  bw_put(&bw,0,1);
  if(fs->nqis>1)emit_qi_runs(&bw,fs,coded_trav,ncoded);
  emit_residual(&bw,fs);
  return bw_finish(&bw);
}

/* ====================================================================== */
/* Independent dequantization and DC-only pixel reconstruction.           */
/* ====================================================================== */

static int clamp255_indep(int x){
  if(x<0)return 0;
  if(x>255)return 255;
  return x;
}

static ogg_uint16_t dc_quant(const th_dec_ctx *dec,int qi,int pli,int qti){
  return dec->state.dequant_tables[qi][pli][qti][0];
}

static ogg_uint16_t ac_quant_zz(const th_dec_ctx *dec,int qi,int pli,
 int qti,int zzi){
  return dec->state.dequant_tables[qi][pli][qti][zzi];
}

static void expected_dc_only_pixels(const th_dec_ctx *dec,
 const frame_spec *fs,int pli,int local,int recon_dc,unsigned char out[64]){
  const frag_spec *sp=&fs->f[pli][local];
  int qti=sp->mode==MODE_INTRA?0:1;
  int p=((int)recon_dc*(int)dc_quant(dec,fs->qi0,pli,qti)+15)>>5;
  int row,col;
  for(row=0;row<8;row++)for(col=0;col<8;col++){
    int v=sp->refi==REF_SELF?p+128:p;
    out[row*8+col]=(unsigned char)clamp255_indep(v);
  }
}

/* ====================================================================== */
/* Test cases.                                                             */
/* ====================================================================== */

static void stripe_cb(void *ctx,th_ycbcr_buffer buf,int fragy0,int fragy1){
  int *counter=(int *)ctx;
  (void)buf;(void)fragy0;(void)fragy1;
  (*counter)++;
}

static void end_decoder(th_dec_ctx *dec,th_enc_ctx *enc,th_info *parsed,
 th_comment *tc,th_setup_info *setup){
  obs_detach();
  if(dec)th_decode_free(dec);
  if(enc)th_encode_free(enc);
  th_info_clear(parsed);
  th_comment_clear(tc);
  if(setup)th_setup_free(setup);
}

/* Helper: decode one packet and require a clean public entry-point status. */
static int decode_packet(th_dec_ctx *dec,const unsigned char *buf,size_t n){
  ogg_packet op;
  memset(&op,0,sizeof(op));
  op.packet=(unsigned char *)buf;op.bytes=n;
  return th_decode_packetin(dec,&op,NULL);
}

static void collect_coded_trav(const frame_spec *fs,int *out,int *nc){
  int pli;
  *nc=0;
  for(pli=0;pli<3;pli++){
    int trav[MAX_FRAGS];
    int nt=traversal_list(&fs->g,pli,trav,MAX_FRAGS),k;
    for(k=0;k<nt;k++){
      int local=trav[k]-fs->g.froff[pli];
      if(fs->f[pli][local].coded)out[(*nc)++]=trav[k];
    }
  }
}

/* ---- dc_neighbors ----------------------------------------------------- */

static int case_dc_first_row_history(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,32,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec fs;spec_init(&fs,32,32,TH_PF_420,1,32);
  int i;
  for(i=0;i<fs.g.nf[0];i++)fs.f[0][i].dc_diff=(i%3==0)?-7:(i%3==1?5:-1);
  for(i=0;i<fs.g.nf[1];i++)fs.f[1][i].dc_diff=4-i;
  for(i=0;i<fs.g.nf[2];i++)fs.f[2][i].dc_diff=-4;
  dc_model m;dc_model_reset(&m);
  static dc_expect ex[MAX_FRAGS*3];
  int nex=model_run_frame(&m,&fs,ex,MAX_FRAGS*3);
  unsigned char buf[1<<16];
  size_t n=build_key_packet(buf,&fs);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,buf,n)==0);
  TASK_CHECK(OBS.ndc==nex);
  TASK_CHECK(nex==fs.g.total_frags);
  for(i=0;i<nex;i++){
    dc_rec *got=obs_find_dc(ex[i].fragi);
    TASK_CHECK(got!=NULL);
    TASK_CHECK(got->diff==ex[i].diff);
    TASK_CHECK(got->pred==ex[i].pred);
    TASK_CHECK(got->recon_dc==ex[i].recon);
    TASK_CHECK(got->refi==ex[i].refi);
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

static int case_dc_mixed_refs_grid(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,32,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec key;spec_init(&key,32,32,TH_PF_420,1,32);
  unsigned char kbuf[1<<16];size_t kn=build_key_packet(kbuf,&key);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,kbuf,kn)==0);
  obs_detach();
  frame_spec fs;spec_init(&fs,32,32,TH_PF_420,0,32);
  int pli,local;
  for(pli=0;pli<3;pli++)for(local=0;local<fs.g.nf[pli];local++){
    fs.f[pli][local].coded=1;
    fs.f[pli][local].dc_diff=(local%4)+1+pli;
  }
  /* Reference identities are macro-block properties: alternate GOLDEN and
     SELF across macro blocks in production macro-block order so adjacent
     fragments carry different references, exercising same-ref neighbor
     selection and the history fallback. */
  {
    int mbx[256],mby[256],nmbs=mb_grid_from_decoder(dec,mbx,mby,256),m;
    for(m=0;m<nmbs;m++)if(mbx[m]>=0)
      set_mb_ref(&fs,mbx[m],mby[m],(m&1)!=0);
  }
  int coded_trav[MAX_FRAGS*3],nc;
  collect_coded_trav(&fs,coded_trav,&nc);
  dc_model m;dc_model_reset(&m);
  static dc_expect ex[MAX_FRAGS*3];
  int nex=model_run_frame(&m,&fs,ex,MAX_FRAGS*3);
  unsigned char ibuf[1<<16];
  size_t in=build_inter_packet(ibuf,&fs,dec,coded_trav,nc);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,ibuf,in)==0);
  TASK_CHECK(OBS.ndc==nex);
  {
    int i;
    for(i=0;i<nex;i++){
      dc_rec *got=obs_find_dc(ex[i].fragi);
      TASK_CHECK(got!=NULL);
      TASK_CHECK(got->diff==ex[i].diff);
      TASK_CHECK(got->pred==ex[i].pred);
      TASK_CHECK(got->recon_dc==ex[i].recon);
      TASK_CHECK(got->refi==ex[i].refi);
    }
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

static int case_dc_nonsquare_444(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(16,48,TH_PF_444,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec key;spec_init(&key,16,48,TH_PF_444,1,32);
  unsigned char kbuf[1<<16];size_t kn=build_key_packet(kbuf,&key);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,kbuf,kn)==0);
  obs_detach();
  frame_spec fs;spec_init(&fs,16,48,TH_PF_444,0,32);
  int pli,local;
  for(pli=0;pli<3;pli++)for(local=0;local<fs.g.nf[pli];local++){
    fs.f[pli][local].coded=(local%3!=0);
    if(fs.f[pli][local].coded){
      /* Large differentials drive spatial predictions apart by more than
         128, exercising the documented fallback ordering. */
      fs.f[pli][local].dc_diff=(local%5==1)?200:((local%5==2)?-200:
       (local%7)+1);
    }
  }
  /* Assign references at macro-block granularity in production macro-block
     order (each MB spans the full two-fragment width in this 4:4:4 grid). */
  {
    int mbx[256],mby[256],nmbs=mb_grid_from_decoder(dec,mbx,mby,256),m;
    for(m=0;m<nmbs;m++)if(mbx[m]>=0)
      set_mb_ref(&fs,mbx[m],mby[m],(m&1)!=0);
  }
  int coded_trav[MAX_FRAGS*3],nc;
  collect_coded_trav(&fs,coded_trav,&nc);
  dc_model m;dc_model_reset(&m);
  static dc_expect ex[MAX_FRAGS*3];
  int nex=model_run_frame(&m,&fs,ex,MAX_FRAGS*3);
  TASK_CHECK(nex>0);
  unsigned char ibuf[1<<16];
  size_t in=build_inter_packet(ibuf,&fs,dec,coded_trav,nc);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,ibuf,in)==0);
  TASK_CHECK(OBS.ndc==nex);
  {
    int i;
    for(i=0;i<nex;i++){
      dc_rec *got=obs_find_dc(ex[i].fragi);
      TASK_CHECK(got!=NULL);
      TASK_CHECK(got->diff==ex[i].diff);
      TASK_CHECK(got->pred==ex[i].pred);
      TASK_CHECK(got->recon_dc==ex[i].recon);
    }
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

/* ---- stripe_state ----------------------------------------------------- */

static int case_stripe_multi_mcu_reset(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,80,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  int stripes=0;
  th_stripe_callback scb;memset(&scb,0,sizeof(scb));
  scb.ctx=&stripes;scb.stripe_decoded=stripe_cb;
  TASK_CHECK(th_decode_ctl(dec,TH_DECCTL_SET_STRIPE_CB,&scb,
   sizeof(scb))==0);
  frame_spec f1;spec_init(&f1,32,80,TH_PF_420,1,32);
  int i;
  for(i=0;i<f1.g.nf[0];i++)f1.f[0][i].dc_diff=(i%6)+1;
  for(i=0;i<f1.g.nf[1];i++)f1.f[1][i].dc_diff=(i%3)+1;
  for(i=0;i<f1.g.nf[2];i++)f1.f[2][i].dc_diff=3-(i%4);
  dc_model m;dc_model_reset(&m);
  static dc_expect ex1[MAX_FRAGS*3];
  int nex1=model_run_frame(&m,&f1,ex1,MAX_FRAGS*3);
  unsigned char b1[1<<16];size_t n1=build_key_packet(b1,&f1);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,b1,n1)==0);
  TASK_CHECK(stripes>=2);
  TASK_CHECK(OBS.ndc==nex1);
  for(i=0;i<nex1;i++){
    dc_rec *got=obs_find_dc(ex1[i].fragi);
    TASK_CHECK(got!=NULL);
    TASK_CHECK(got->pred==ex1[i].pred);
    TASK_CHECK(got->recon_dc==ex1[i].recon);
  }
  obs_detach();
  frame_spec f2;spec_init(&f2,32,80,TH_PF_420,1,40);
  for(i=0;i<f2.g.nf[0];i++)f2.f[0][i].dc_diff=6;
  for(i=0;i<f2.g.nf[1];i++)f2.f[1][i].dc_diff=-2;
  for(i=0;i<f2.g.nf[2];i++)f2.f[2][i].dc_diff=1;
  dc_model m2;dc_model_reset(&m2);
  static dc_expect ex2[MAX_FRAGS*3];
  int nex2=model_run_frame(&m2,&f2,ex2,MAX_FRAGS*3);
  unsigned char b2[1<<16];size_t n2=build_key_packet(b2,&f2);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,b2,n2)==0);
  TASK_CHECK(OBS.ndc==nex2);
  for(i=0;i<nex2;i++){
    dc_rec *got=obs_find_dc(ex2[i].fragi);
    TASK_CHECK(got!=NULL);
    TASK_CHECK(got->pred==ex2[i].pred);
    TASK_CHECK(got->recon_dc==ex2[i].recon);
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

/* Across two MCU calls, a coded fragment in the second MCU whose same-
  reference neighbors are all uncoded must fall back to the predictor history
  accumulated in the first MCU.  Clearing pred_last on every MCU pipeline init
  must therefore change this prediction and fail the independent check. */
static int case_stripe_mcu_history_carries(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,80,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  int stripes=0;
  th_stripe_callback scb;memset(&scb,0,sizeof(scb));
  scb.ctx=&stripes;scb.stripe_decoded=stripe_cb;
  TASK_CHECK(th_decode_ctl(dec,TH_DECCTL_SET_STRIPE_CB,&scb,
   sizeof(scb))==0);
  /* Keyframe establishes real reference buffers. */
  frame_spec key;spec_init(&key,32,80,TH_PF_420,1,32);
  {
    int i;
    for(i=0;i<key.g.total_frags;i++){
      int pli=i<key.g.nf[0]?0:(i<key.g.nf[0]+key.g.nf[1]?1:2);
      key.f[pli][i-key.g.froff[pli]].dc_diff=2;
    }
  }
  unsigned char kb[1<<16];size_t kn=build_key_packet(kb,&key);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,kb,kn)==0);
  obs_detach();
  /* Inter frame, two MCUs.  The last luma fragment row of the first MCU
     (fy=7) is left uncoded; the first row of the second MCU (fy=8) is coded
     with GOLDEN reference and has no same-reference upper neighbor, forcing a
     predictor-history fallback.  Leaving fy=7 uncoded also keeps every
     super-block's coded-flag run short enough for the block-run tree. */
  frame_spec fs;spec_init(&fs,32,80,TH_PF_420,0,32);
  {
    int x,y;
    for(y=0;y<fs.g.nv[0];y++)for(x=0;x<fs.g.nh[0];x++){
      int coded=(y!=7);
      fs.f[0][y*fs.g.nh[0]+x].coded=coded;
      if(coded){
        fs.f[0][y*fs.g.nh[0]+x].refi=REF_GOLD;
        fs.f[0][y*fs.g.nh[0]+x].mode=MODE_GOLDEN_NOMV;
        fs.f[0][y*fs.g.nh[0]+x].dc_diff=(y==8)?4:1;
      }
    }
    {
      int pli,local;
      for(pli=1;pli<3;pli++)for(local=0;local<fs.g.nf[pli];local++){
        fs.f[pli][local].coded=1;
        fs.f[pli][local].refi=REF_GOLD;
        fs.f[pli][local].mode=MODE_GOLDEN_NOMV;
        fs.f[pli][local].dc_diff=1;
      }
    }
  }
  int coded_trav[MAX_FRAGS*3],nc;
  collect_coded_trav(&fs,coded_trav,&nc);
  dc_model m;dc_model_reset(&m);
  {
    static dc_expect key_ex[MAX_FRAGS*3];
    model_run_frame(&m,&key,key_ex,MAX_FRAGS*3);
  }
  static dc_expect ex[MAX_FRAGS*3];
  int nex=model_run_frame(&m,&fs,ex,MAX_FRAGS*3);
  unsigned char ib[1<<16];
  size_t in=build_inter_packet(ib,&fs,dec,coded_trav,nc);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,ib,in)==0);
  TASK_CHECK(stripes>=2);
  TASK_CHECK(OBS.ndc==nex);
  {
    int i;
    for(i=0;i<nex;i++){
      dc_rec *got=obs_find_dc(ex[i].fragi);
      TASK_CHECK(got!=NULL);
      TASK_CHECK(got->pred==ex[i].pred);
      TASK_CHECK(got->recon_dc==ex[i].recon);
    }
    /* The fy=8 row specifically relies on carried GOLDEN history. */
    for(i=0;i<fs.g.nh[0];i++){
      ptrdiff_t fi=8*fs.g.nh[0]+i;
      dc_rec *got=obs_find_dc(fi);
      TASK_CHECK(got!=NULL&&got->refi==REF_GOLD);
    }
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

static int case_stripe_empty_keeps_history(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,32,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec key;spec_init(&key,32,32,TH_PF_420,1,32);
  int i;
  for(i=0;i<key.g.nf[0];i++)key.f[0][i].dc_diff=4;
  for(i=0;i<key.g.nf[1];i++)key.f[1][i].dc_diff=5;
  for(i=0;i<key.g.nf[2];i++)key.f[2][i].dc_diff=6;
  unsigned char kb[1<<16];size_t kn=build_key_packet(kb,&key);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,kb,kn)==0);
  obs_detach();
  ogg_packet empty;memset(&empty,0,sizeof(empty));
  TASK_CHECK(th_decode_packetin(dec,&empty,NULL)==TH_DUPFRAME);
  frame_spec fs;spec_init(&fs,32,32,TH_PF_420,0,32);
  int pli,local;
  for(pli=0;pli<3;pli++)for(local=0;local<fs.g.nf[pli];local++){
    fs.f[pli][local].coded=1;
    fs.f[pli][local].refi=REF_GOLD;
    fs.f[pli][local].mode=MODE_GOLDEN_NOMV;
    fs.f[pli][local].dc_diff=2;
  }
  int trav[MAX_FRAGS*3],nt=0;
  for(pli=0;pli<3;pli++)
    nt+=traversal_list(&fs.g,pli,trav+nt,MAX_FRAGS*3-nt);
  /* Seed the independent history by running the independent DC model over the
     keyframe itself (spatial prediction means the ending history is not a
     plain sum of differentials).  Keyframe fragments use reference SELF, so
     only pred_last[*][SELF] is updated; the GOLDEN history stays at its zero
     initial value.  An empty inter frame must leave all of this untouched. */
  dc_model m;dc_model_reset(&m);
  {
    static dc_expect key_ex[MAX_FRAGS*3];
    model_run_frame(&m,&key,key_ex,MAX_FRAGS*3);
  }
  static dc_expect ex[MAX_FRAGS*3];
  int nex=model_run_frame(&m,&fs,ex,MAX_FRAGS*3);
  unsigned char ib[1<<16];
  size_t in=build_inter_packet(ib,&fs,dec,trav,nt);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,ib,in)==0);
  TASK_CHECK(OBS.ndc==nex);
  for(i=0;i<nex;i++){
    dc_rec *got=obs_find_dc(ex[i].fragi);
    TASK_CHECK(got!=NULL);
    TASK_CHECK(got->pred==ex[i].pred);
    TASK_CHECK(got->recon_dc==ex[i].recon);
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

/* ---- token_runs ------------------------------------------------------- */

static int case_token_eob_and_zero_runs(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,32,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec fs;spec_init(&fs,32,32,TH_PF_420,1,32);
  int i;
  for(i=0;i<fs.g.nf[0];i++)fs.f[0][i].dc_diff=1;
  for(i=0;i<fs.g.nf[1];i++)fs.f[1][i].dc_diff=1;
  for(i=0;i<fs.g.nf[2];i++)fs.f[2][i].dc_diff=1;
  /* Adjacent non-zero AC coefficients (no within-block zero run) in the first
     blocks, including negative values; all remaining blocks are pure EOB. */
  fs.f[0][0].coeff[MY_FZIGZAG[1]]=3;
  fs.f[0][0].coeff[MY_FZIGZAG[2]]=-5;
  fs.f[0][1].coeff[MY_FZIGZAG[1]]=7;
  fs.f[0][1].coeff[MY_FZIGZAG[2]]=-9;
  fs.f[0][2].coeff[MY_FZIGZAG[1]]=-11;
  /* A short within-block zero run (one zero) followed by a +/-1 coefficient,
     which uses the bundled RUN_CAT1A token path. */
  fs.f[0][3].coeff[MY_FZIGZAG[2]]=1;
  unsigned char buf[1<<16];
  size_t n=build_key_packet(buf,&fs);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,buf,n)==0);
  {
    static const int watch[4]={0,1,2,3};
    for(i=0;i<4;i++){
      int local=watch[i];
      coeff_rec *r=obs_find_coeff(local);
      TASK_CHECK(r!=NULL);
      {
        int k;
        for(k=1;k<64;k++){
          int raw=fs.f[0][local].coeff[k];
          int zz=my_izigzag(k);
          ogg_uint16_t q=ac_quant_zz(dec,32,0,0,zz);
          TASK_CHECK(r->coeffs[k]==(ogg_int16_t)(raw*(int)q));
        }
      }
    }
    /* Every other coded block must contain only the DC slot. */
    for(i=4;i<fs.g.nf[0];i++){
      coeff_rec *r=obs_find_coeff(i);
      int k,allzero=1;
      TASK_CHECK(r!=NULL);
      for(k=1;k<64;k++)if(r->coeffs[k]!=0)allzero=0;
      TASK_CHECK(allzero);
    }
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

static int case_token_no_workspace_residue(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(16,16,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec fs;spec_init(&fs,16,16,TH_PF_420,1,32);
  int i;
  for(i=0;i<fs.g.total_frags;i++){
    int pli=i<fs.g.nf[0]?0:(i<fs.g.nf[0]+fs.g.nf[1]?1:2);
    int local=i-fs.g.froff[pli];
    fs.f[pli][local].dc_diff=(i%2?2:-2);
  }
  fs.f[0][0].coeff[MY_FZIGZAG[1]]=1;
  fs.f[1][0].coeff[MY_FZIGZAG[1]]=-2;
  fs.f[2][0].coeff[MY_FZIGZAG[1]]=3;
  unsigned char buf[1<<16];
  size_t n=build_key_packet(buf,&fs);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,buf,n)==0);
  TASK_CHECK(OBS.ncoeff==fs.g.total_frags);
  for(i=0;i<OBS.ncoeff;i++){
    coeff_rec *r=&OBS.coeff[i];
    int pli=r->pli,local=(int)(r->fragi-fs.g.froff[pli]);
    int k;
    for(k=1;k<64;k++){
      int raw=fs.f[pli][local].coeff[k];
      int zz=my_izigzag(k);
      ogg_uint16_t q=ac_quant_zz(dec,32,pli,0,zz);
      TASK_CHECK(r->coeffs[k]==(ogg_int16_t)(raw*(int)q));
    }
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

/* ---- coefficients ----------------------------------------------------- */

static int case_coeff_multi_qi(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,32,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec key;spec_init(&key,32,32,TH_PF_420,1,24);
  unsigned char kb[1<<16];size_t kn=build_key_packet(kb,&key);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,kb,kn)==0);
  obs_detach();
  frame_spec fs;spec_init(&fs,32,32,TH_PF_420,0,24);
  fs.nqis=3;fs.qi0=24;fs.qi1=40;fs.qi2=48;
  int pli,local,i;
  for(pli=0;pli<3;pli++)for(local=0;local<fs.g.nf[pli];local++){
    fs.f[pli][local].coded=1;
    fs.f[pli][local].refi=REF_GOLD;
    fs.f[pli][local].mode=MODE_GOLDEN_NOMV;
    fs.f[pli][local].dc_diff=1;
    fs.f[pli][local].qii=local%3;
  }
  fs.f[0][0].coeff[MY_FZIGZAG[1]]=2;
  fs.f[0][1].coeff[MY_FZIGZAG[1]]=-3;
  fs.f[0][1].coeff[MY_FZIGZAG[2]]=5;
  fs.f[1][0].coeff[MY_FZIGZAG[1]]=4;
  fs.f[1][0].coeff[MY_FZIGZAG[2]]=-6;
  int coded_trav[MAX_FRAGS*3],nc;
  collect_coded_trav(&fs,coded_trav,&nc);
  unsigned char ibuf[1<<16];
  size_t in=build_inter_packet(ibuf,&fs,dec,coded_trav,nc);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,ibuf,in)==0);
  {
    static const struct{int pli;int local;}watch[]={{0,0},{0,1},{1,0}};
    for(i=0;i<3;i++){
      ptrdiff_t fi=fs.g.froff[watch[i].pli]+watch[i].local;
      coeff_rec *r=obs_find_coeff(fi);
      int qii=fs.f[watch[i].pli][watch[i].local].qii;
      int qi=qii==0?fs.qi0:(qii==1?fs.qi1:fs.qi2);
      int k;
      TASK_CHECK(r!=NULL);
      for(k=1;k<64;k++){
        int raw=fs.f[watch[i].pli][watch[i].local].coeff[k];
        int zz=my_izigzag(k);
        ogg_uint16_t q=ac_quant_zz(dec,qi,watch[i].pli,1,zz);
        TASK_CHECK(r->coeffs[k]==(ogg_int16_t)(raw*(int)q));
      }
    }
    /* All three QI values must really occur among the decoded fragments. */
    {
      unsigned char seen[3]={0,0,0};
      for(i=0;i<OBS.ncoeff;i++){
        int p=OBS.coeff[i].pli;
        int loc=(int)(OBS.coeff[i].fragi-fs.g.froff[p]);
        seen[fs.f[p][loc].qii]=1;
      }
      TASK_CHECK(seen[0]&&seen[1]&&seen[2]);
    }
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

static int case_coeff_dc_only_pixels(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,32,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  frame_spec fs;spec_init(&fs,32,32,TH_PF_420,1,32);
  int i;
  for(i=0;i<fs.g.nf[0];i++)fs.f[0][i].dc_diff=(i%4)+1;
  for(i=0;i<fs.g.nf[1];i++)fs.f[1][i].dc_diff=(i%3)+1;
  for(i=0;i<fs.g.nf[2];i++)fs.f[2][i].dc_diff=(i%2)?3:-3;
  dc_model m;dc_model_reset(&m);
  static dc_expect ex[MAX_FRAGS*3];
  int nex=model_run_frame(&m,&fs,ex,MAX_FRAGS*3);
  unsigned char buf[1<<16];
  size_t n=build_key_packet(buf,&fs);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,buf,n)==0);
  TASK_CHECK(nex==OBS.ndc);
  TASK_CHECK(OBS.npixel==fs.g.total_frags);
  for(i=0;i<nex;i++){
    pixel_rec *pr=obs_find_pixel(ex[i].fragi);
    unsigned char want[64];
    int k;
    TASK_CHECK(pr!=NULL);
    expected_dc_only_pixels(dec,&fs,ex[i].pli,
     (int)(ex[i].fragi-fs.g.froff[ex[i].pli]),ex[i].recon,want);
    for(k=0;k<64;k++)TASK_CHECK(pr->px[k]==want[k]);
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

/* ---- packet_pipeline -------------------------------------------------- */

static int case_pipeline_formats_and_refs(void){
  static const struct{int w,h,pf;}cfgs[2]={
    {32,32,TH_PF_420},{16,16,TH_PF_444}
  };
  int cfg;
  for(cfg=0;cfg<2;cfg++){
    th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
    th_dec_ctx *dec=make_decoder(cfgs[cfg].w,cfgs[cfg].h,cfgs[cfg].pf,1,
     &enc,&parsed,&tc,&setup);
    TASK_CHECK(dec!=NULL);
    frame_spec key;spec_init(&key,cfgs[cfg].w,cfgs[cfg].h,cfgs[cfg].pf,1,32);
    int i;
    for(i=0;i<key.g.total_frags;i++){
      int p=i<key.g.nf[0]?0:(i<key.g.nf[0]+key.g.nf[1]?1:2);
      key.f[p][i-key.g.froff[p]].dc_diff=2;
    }
    unsigned char kb[1<<16];size_t kn=build_key_packet(kb,&key);
    obs_reset();obs_attach();
    TASK_CHECK(decode_packet(dec,kb,kn)==0);
    TASK_CHECK(OBS.ndc==key.g.total_frags);
    obs_detach();
    frame_spec fs;spec_init(&fs,cfgs[cfg].w,cfgs[cfg].h,cfgs[cfg].pf,0,32);
    {
      int pli2,loc2;
      for(pli2=0;pli2<3;pli2++)for(loc2=0;loc2<fs.g.nf[pli2];loc2++){
        fs.f[pli2][loc2].coded=1;
        fs.f[pli2][loc2].refi=REF_GOLD;
        fs.f[pli2][loc2].mode=MODE_GOLDEN_NOMV;
        fs.f[pli2][loc2].dc_diff=(loc2==0)?3:1;
      }
    }
    int coded_trav[MAX_FRAGS*3],nc=0;
    collect_coded_trav(&fs,coded_trav,&nc);
    /* The keyframe only updates SELF predictor history; GOLDEN history stays
       at its zero initial value until a GOLDEN-referenced fragment is coded. */
    dc_model m;dc_model_reset(&m);
    {
      static dc_expect key_ex[MAX_FRAGS*3];
      model_run_frame(&m,&key,key_ex,MAX_FRAGS*3);
    }
    static dc_expect ex[MAX_FRAGS*3];
    int nex=model_run_frame(&m,&fs,ex,MAX_FRAGS*3);
    TASK_CHECK(nex==fs.g.total_frags);
    unsigned char ib[1<<16];
    size_t in=build_inter_packet(ib,&fs,dec,coded_trav,nc);
    obs_reset();obs_attach();
    TASK_CHECK(decode_packet(dec,ib,in)==0);
    TASK_CHECK(OBS.ndc==nex);
    for(i=0;i<nex;i++){
      dc_rec *got=obs_find_dc(ex[i].fragi);
      TASK_CHECK(got!=NULL);
      TASK_CHECK(got->recon_dc==ex[i].recon);
      TASK_CHECK(got->refi==REF_GOLD);
    }
    end_decoder(dec,enc,&parsed,&tc,setup);
  }
  return 0;
}

static int case_pipeline_token_consumption(void){
  th_enc_ctx *enc;th_info parsed;th_comment tc;th_setup_info *setup;
  th_dec_ctx *dec=make_decoder(32,80,TH_PF_420,1,&enc,&parsed,&tc,&setup);
  TASK_CHECK(dec!=NULL);
  int stripes=0;
  th_stripe_callback scb;memset(&scb,0,sizeof(scb));
  scb.ctx=&stripes;scb.stripe_decoded=stripe_cb;
  TASK_CHECK(th_decode_ctl(dec,TH_DECCTL_SET_STRIPE_CB,&scb,
   sizeof(scb))==0);
  frame_spec fs;spec_init(&fs,32,80,TH_PF_420,1,32);
  int i;
  for(i=0;i<fs.g.total_frags;i++){
    int pli=i<fs.g.nf[0]?0:(i<fs.g.nf[0]+fs.g.nf[1]?1:2);
    int local=i-fs.g.froff[pli];
    fs.f[pli][local].dc_diff=(local%3==0)?-4:2;
  }
  /* Sparse AC straddling the Y/Cb plane boundary and the MCU boundary. */
  fs.f[0][fs.g.nf[0]-1].coeff[MY_FZIGZAG[1]]=5;
  fs.f[1][0].coeff[MY_FZIGZAG[2]]=-6;
  fs.f[2][1].coeff[MY_FZIGZAG[3]]=7;
  unsigned char buf[1<<16];
  size_t n=build_key_packet(buf,&fs);
  obs_reset();obs_attach();
  TASK_CHECK(decode_packet(dec,buf,n)==0);
  TASK_CHECK(stripes>=2);
  TASK_CHECK(OBS.ncoeff==fs.g.total_frags);
  {
    coeff_rec *a=obs_find_coeff(fs.g.froff[0]+fs.g.nf[0]-1);
    coeff_rec *b=obs_find_coeff(fs.g.froff[1]);
    coeff_rec *c=obs_find_coeff(fs.g.froff[2]+1);
    TASK_CHECK(a!=NULL&&b!=NULL&&c!=NULL);
    TASK_CHECK(a->coeffs[MY_FZIGZAG[1]]==
     (ogg_int16_t)(5*(int)ac_quant_zz(dec,32,0,0,1)));
    TASK_CHECK(b->coeffs[MY_FZIGZAG[2]]==
     (ogg_int16_t)(-6*(int)ac_quant_zz(dec,32,1,0,2)));
    TASK_CHECK(c->coeffs[MY_FZIGZAG[3]]==
     (ogg_int16_t)(7*(int)ac_quant_zz(dec,32,2,0,3)));
  }
  end_decoder(dec,enc,&parsed,&tc,setup);
  return 0;
}

/* ====================================================================== */
/* Registration.                                                           */
/* ====================================================================== */

const struct task_case task_cases[]={
  {"dc_neighbors","first_row_history",case_dc_first_row_history},
  {"dc_neighbors","mixed_refs_grid",case_dc_mixed_refs_grid},
  {"dc_neighbors","nonsquare_444_fallbacks",case_dc_nonsquare_444},
  {"stripe_state","multi_mcu_and_reset",case_stripe_multi_mcu_reset},
  {"stripe_state","mcu_history_carries",case_stripe_mcu_history_carries},
  {"stripe_state","empty_frame_keeps_history",case_stripe_empty_keeps_history},
  {"token_runs","eob_and_zero_runs",case_token_eob_and_zero_runs},
  {"token_runs","no_workspace_residue",case_token_no_workspace_residue},
  {"coefficients","multi_qi_quantizers",case_coeff_multi_qi},
  {"coefficients","dc_only_pixel_exact",case_coeff_dc_only_pixels},
  {"packet_pipeline","formats_and_real_refs",case_pipeline_formats_and_refs},
  {"packet_pipeline","token_consumption",case_pipeline_token_consumption}
};

const size_t task_case_count=
 sizeof(task_cases)/sizeof(task_cases[0]);
