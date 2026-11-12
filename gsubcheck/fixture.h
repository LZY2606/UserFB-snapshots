/* Original geometric test material, MIT (same license as the task additions).
   This builds bytes, not a GSUB interpreter. No installed fonts are used. */
#ifndef GSUBCHECK_FIXTURE_H
#define GSUBCHECK_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char b[32768]; int n; } gc_bytes;
static void gc_u8(gc_bytes *b, unsigned x) { if(b->n >= 32768) abort(); b->b[b->n++]=(unsigned char)x; }
static void gc_u16(gc_bytes *b, unsigned x) { gc_u8(b,x>>8); gc_u8(b,x); }
static void gc_u32(gc_bytes *b, unsigned x) { gc_u16(b,x>>16); gc_u16(b,x); }
static void gc_patch16(gc_bytes *b,int p,unsigned x) { b->b[p]=(unsigned char)(x>>8); b->b[p+1]=(unsigned char)x; }
static void gc_patch32(gc_bytes *b,int p,unsigned x) { gc_patch16(b,p,x>>16); gc_patch16(b,p+2,x); }
static void gc_zero(gc_bytes *b,int n) { while(n--) gc_u8(b,0); }
static unsigned gc_get32(const unsigned char *p) { return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static unsigned gc_sum(const gc_bytes *b) {
  unsigned sum=0; int i;
  for(i=0;i<b->n;i+=4) {
    unsigned v=0; int j;
    for(j=0;j<4;j++) v=(v<<8)|(i+j<b->n ? b->b[i+j]:0);
    sum+=v;
  }
  return sum;
}
static void gc_base_gsub(gc_bytes *b) {
  int script,feature,look;
  b->n=0; gc_u32(b,0x10000); gc_zero(b,6);
  script=b->n; gc_patch16(b,4,(unsigned)script);
  gc_u16(b,1); gc_u32(b,0x44464c54); gc_u16(b,8); /* DFLT */
  gc_u16(b,4); gc_u16(b,0); /* default LangSys, no named languages */
  gc_u16(b,0); gc_u16(b,0xffff); gc_u16(b,1); gc_u16(b,0);
  feature=b->n; gc_patch16(b,6,(unsigned)feature);
  gc_u16(b,1); gc_u32(b,0x6c696761); gc_u16(b,8); /* liga */
  gc_u16(b,0); gc_u16(b,1); gc_u16(b,0);
  look=b->n; gc_patch16(b,8,(unsigned)look);
  gc_u16(b,1); gc_u16(b,4);
  gc_u16(b,4); gc_u16(b,8); gc_u16(b,1); gc_u16(b,8);
  gc_u16(b,1); gc_u16(b,8); gc_u16(b,1); gc_u16(b,14);
  gc_u16(b,1); gc_u16(b,1); gc_u16(b,1); /* coverage: f */
  gc_u16(b,1); gc_u16(b,4); /* ligature set */
  gc_u16(b,3); gc_u16(b,2); gc_u16(b,2); /* f i -> fi */
}
static void gc_base_gdef(gc_bytes *b) {
  static const unsigned cls[8]={0,1,1,2,3,1,1,1}; int i;
  b->n=0; gc_u32(b,0x10000); gc_u16(b,12); gc_zero(b,6);
  gc_u16(b,1); gc_u16(b,0); gc_u16(b,8);
  for(i=0;i<8;i++) gc_u16(b,cls[i]);
}
/* Replace GSUB/GDEF independently to build new public test inputs. */
static void gc_font(gc_bytes *out,const gc_bytes *gsub,const gc_bytes *gdef) {
  static const char tags[11][5]={"GDEF","GSUB","cmap","glyf","head","hhea","hmtx","loca","maxp","name","post"};
  gc_bytes *t=(gc_bytes*)calloc(11,sizeof(gc_bytes));
  int i,j,head_at=0; static const int widths[8]={200,200,100,450,80,250,150,300};
  if(!t) abort();
  t[0]=*gdef; t[1]=*gsub;
  gc_u16(&t[2],0);gc_u16(&t[2],1);gc_u16(&t[2],3);gc_u16(&t[2],1);gc_u32(&t[2],12);
  gc_u16(&t[2],4);gc_u16(&t[2],480);gc_u16(&t[2],0);
  gc_u16(&t[2],4);gc_u16(&t[2],4);gc_u16(&t[2],1);gc_u16(&t[2],0);
  gc_u16(&t[2],255);gc_u16(&t[2],0xffff);gc_u16(&t[2],0);
  gc_u16(&t[2],32);gc_u16(&t[2],0xffff);gc_u16(&t[2],0);gc_u16(&t[2],1);
  gc_u16(&t[2],4);gc_u16(&t[2],0);
  for(i=32;i<256;i++)gc_u16(&t[2],i=='i'?2:1);
  for(i=0;i<8;i++) {
    int w=widths[i]; gc_u32(&t[7],(unsigned)t[3].n);
    gc_u16(&t[3],1); gc_u16(&t[3],0);gc_u16(&t[3],0);gc_u16(&t[3],w);gc_u16(&t[3],600);
    gc_u16(&t[3],3); gc_u16(&t[3],0);
    for(j=0;j<4;j++)gc_u8(&t[3],1);
    gc_u16(&t[3],0);gc_u16(&t[3],w);gc_u16(&t[3],0);gc_u16(&t[3],(unsigned)-w);
    gc_u16(&t[3],0);gc_u16(&t[3],0);gc_u16(&t[3],600);gc_u16(&t[3],0);
    gc_u16(&t[6],w+50);gc_u16(&t[6],0);
  }
  gc_u32(&t[7],(unsigned)t[3].n);
  gc_zero(&t[4],54);gc_patch32(&t[4],0,0x10000);gc_patch32(&t[4],12,0x5f0f3cf5);
  gc_patch16(&t[4],18,1000);gc_patch16(&t[4],40,450);gc_patch16(&t[4],42,600);gc_patch16(&t[4],50,1);
  gc_zero(&t[5],36);gc_patch32(&t[5],0,0x10000);gc_patch16(&t[5],4,800);
  gc_patch16(&t[5],6,(unsigned)-200);gc_patch16(&t[5],34,8);
  gc_zero(&t[8],32);gc_patch32(&t[8],0,0x10000);gc_patch16(&t[8],4,8);
  gc_u16(&t[9],0);gc_u16(&t[9],1);gc_u16(&t[9],18);
  gc_u16(&t[9],3);gc_u16(&t[9],1);gc_u16(&t[9],0x409);gc_u16(&t[9],4);gc_u16(&t[9],8);gc_u16(&t[9],0);
  gc_u16(&t[9],'D');gc_u16(&t[9],'e');gc_u16(&t[9],'m');gc_u16(&t[9],'o');
  gc_u32(&t[10],0x30000);gc_zero(&t[10],28);
  out->n=0;gc_u32(out,0x10000);gc_u16(out,11);gc_u16(out,128);gc_u16(out,3);gc_u16(out,48);
  gc_zero(out,16*11);
  for(i=0;i<11;i++) {
    int r=12+16*i;
    for(j=0;j<4;j++)out->b[r+j]=(unsigned char)tags[i][j];
    gc_patch32(out,r+4,gc_sum(&t[i]));gc_patch32(out,r+8,(unsigned)out->n);gc_patch32(out,r+12,(unsigned)t[i].n);
    if(i==4)head_at=out->n;
    for(j=0;j<t[i].n;j++)gc_u8(out,t[i].b[j]);
    while(out->n%4)gc_u8(out,0);
  }
  gc_patch32(out,head_at+8,0xb1b0afba-gc_sum(out));free(t);
}
static void gc_default_font(gc_bytes *out) {
  gc_bytes gsub,gdef;gc_base_gsub(&gsub);gc_base_gdef(&gdef);gc_font(out,&gsub,&gdef);
}

/* ---- GSUB/GDEF assembler for the stbtt_ApplyGSUB test groups ---- */

#define GC_TAG(a,b,c,d) (((unsigned)(a)<<24)|((unsigned)(b)<<16)|((unsigned)(c)<<8)|(unsigned)(d))

/* Coverage format 1: explicit glyph list (caller keeps it sorted/unique). */
static void gc_cov_list(gc_bytes *w,const unsigned short *g,int n) {
  int i; gc_u16(w,1); gc_u16(w,(unsigned)n);
  for(i=0;i<n;i++) gc_u16(w,g[i]);
}
/* Coverage format 2: ranges as start/end/startIndex triples. */
static void gc_cov_ranges(gc_bytes *w,const unsigned short *triples,int nr) {
  int i; gc_u16(w,2); gc_u16(w,(unsigned)nr);
  for(i=0;i<nr*3;i++) gc_u16(w,triples[i]);
}
static int gc_place16(gc_bytes *w) { int p=w->n; gc_u16(w,0); return p; }

/* Each subtable builder appends one self-contained blob (all internal
   offsets are relative to the blob start) and returns the blob offset. */
static int gc_sub_single1(gc_bytes *w,const unsigned short *covg,int ncov,int delta) {
  int start=w->n, covp;
  gc_u16(w,1); covp=gc_place16(w); gc_u16(w,(unsigned)delta);
  gc_patch16(w,covp,(unsigned)(w->n-start));
  gc_cov_list(w,covg,ncov);
  return start;
}
/* SingleSubst format 1 backed by a format-2 (range) coverage. */
static int gc_sub_single1_range(gc_bytes *w,const unsigned short *triples,int nr,int delta) {
  int start=w->n, covp;
  gc_u16(w,1); covp=gc_place16(w); gc_u16(w,(unsigned)delta);
  gc_patch16(w,covp,(unsigned)(w->n-start));
  gc_cov_ranges(w,triples,nr);
  return start;
}
/* SingleSubst format 2 backed by a format-2 (range) coverage. The output
   array length must match the number of glyphs spanned by the ranges. */
static int gc_sub_single2_range(gc_bytes *w,const unsigned short *triples,int nr,
                                const unsigned short *subst,int n) {
  int start=w->n, covp, i;
  gc_u16(w,2); covp=gc_place16(w); gc_u16(w,(unsigned)n);
  for(i=0;i<n;i++) gc_u16(w,subst[i]);
  gc_patch16(w,covp,(unsigned)(w->n-start));
  gc_cov_ranges(w,triples,nr);
  return start;
}
static int gc_sub_single2(gc_bytes *w,const unsigned short *covg,
                          const unsigned short *subst,int n) {
  int start=w->n, covp, i;
  gc_u16(w,2); covp=gc_place16(w); gc_u16(w,(unsigned)n);
  for(i=0;i<n;i++) gc_u16(w,subst[i]);
  gc_patch16(w,covp,(unsigned)(w->n-start));
  gc_cov_list(w,covg,n);
  return start;
}
static int gc_sub_multiple(gc_bytes *w,const unsigned short *covg,int ncov,
                           const unsigned short **seqs,const int *seqlens,int nseq) {
  int start=w->n, covp, i, j;
  int *seqpos=(int*)calloc((size_t)(nseq?nseq:1),sizeof(int));
  gc_u16(w,1); covp=gc_place16(w); gc_u16(w,(unsigned)nseq);
  for(i=0;i<nseq;i++) seqpos[i]=gc_place16(w);
  gc_patch16(w,covp,(unsigned)(w->n-start));
  gc_cov_list(w,covg,ncov);
  for(i=0;i<nseq;i++) {
    gc_patch16(w,seqpos[i],(unsigned)(w->n-start));
    gc_u16(w,(unsigned)seqlens[i]);
    for(j=0;j<seqlens[i];j++) gc_u16(w,seqs[i][j]);
  }
  free(seqpos);
  return start;
}
typedef struct { unsigned short first; const unsigned short *rest; int nrest; unsigned short lig; } gc_liga_spec;
static int gc_sub_liga(gc_bytes *w,const unsigned short *covg,int ncov,
                       const gc_liga_spec *specs,int nspec) {
  int start=w->n, covp, i, j;
  int *setpos=(int*)calloc((size_t)(nspec?nspec:1),sizeof(int));
  gc_u16(w,1); covp=gc_place16(w); gc_u16(w,(unsigned)nspec);
  for(i=0;i<nspec;i++) setpos[i]=gc_place16(w);
  gc_patch16(w,covp,(unsigned)(w->n-start));
  gc_cov_list(w,covg,ncov);
  for(i=0;i<nspec;i++) {
    int setstart, ligp;
    gc_patch16(w,setpos[i],(unsigned)(w->n-start));
    setstart=w->n;
    gc_u16(w,1); ligp=gc_place16(w);
    gc_patch16(w,ligp,(unsigned)(w->n-setstart));
    gc_u16(w,specs[i].lig);
    /* compCount counts the coverage glyph plus the following components; the
       component id array stores only the following components, matching the
       convention used by the original f/i fixture material. */
    gc_u16(w,(unsigned)(specs[i].nrest+1));
    for(j=0;j<specs[i].nrest;j++) gc_u16(w,specs[i].rest[j]);
  }
  free(setpos);
  return start;
}
/* ChainContext format 3. Coverages are built inline from glyph lists. */
typedef struct { unsigned short si, li; } gc_record;
static int gc_sub_chain3(gc_bytes *w,
                         const unsigned short *back,int nback,
                         const unsigned short *input,int ninput,
                         const unsigned short *ahead,int nahead,
                         const gc_record *recs,int nrec) {
  int start=w->n, i;
  int *bp=(int*)calloc((size_t)(nback?nback:1),sizeof(int));
  int *ip=(int*)calloc((size_t)(ninput?ninput:1),sizeof(int));
  int *ap=(int*)calloc((size_t)(nahead?nahead:1),sizeof(int));
  gc_u16(w,3);
  gc_u16(w,(unsigned)nback);
  for(i=0;i<nback;i++) bp[i]=gc_place16(w);
  gc_u16(w,(unsigned)ninput);
  for(i=0;i<ninput;i++) ip[i]=gc_place16(w);
  gc_u16(w,(unsigned)nahead);
  for(i=0;i<nahead;i++) ap[i]=gc_place16(w);
  gc_u16(w,(unsigned)nrec);
  for(i=0;i<nrec;i++) { gc_u16(w,recs[i].si); gc_u16(w,recs[i].li); }
  for(i=0;i<nback;i++) { gc_patch16(w,bp[i],(unsigned)(w->n-start)); gc_cov_list(w,&back[i],1); }
  for(i=0;i<ninput;i++){ gc_patch16(w,ip[i],(unsigned)(w->n-start)); gc_cov_list(w,&input[i],1);}
  for(i=0;i<nahead;i++){ gc_patch16(w,ap[i],(unsigned)(w->n-start)); gc_cov_list(w,&ahead[i],1);}
  free(bp); free(ip); free(ap);
  return start;
}
/* Extension format 1 wrapping an already-built concrete subtable blob. */
static int gc_wrap_extension(gc_bytes *w,int ext_type,const gc_bytes *inner) {
  int start=w->n, offp, k;
  gc_u16(w,1); gc_u16(w,(unsigned)ext_type); offp=w->n; gc_u32(w,0);
  for(k=0;k<inner->n;k++) gc_u8(w,inner->b[k]);
  /* the concrete table starts immediately after the 8-byte ext header, and
     the offset is measured from the extension subtable itself */
  gc_patch32(w,offp,8u);
  return start;
}

/* A lookup is a type, its flags and up to four subtable blobs. */
typedef struct { int type; int flags; int nsub; gc_bytes sub[4]; } gc_lookup_b;
typedef struct { unsigned tag; int nlook; unsigned short look[8]; } gc_feat_spec;
typedef struct { unsigned tag; int required; int nfeat; unsigned short feat[10]; } gc_lang_spec; /* tag 0 => default */
typedef struct {
  unsigned tag; int has_default; gc_lang_spec def;
  int nlang; gc_lang_spec lang[8];
} gc_script_spec;

static void gc_build_gsub(gc_bytes *out,
                          const gc_script_spec *scripts,int nscript,
                          const gc_feat_spec *feats,int nfeat,
                          const gc_lookup_b *looks,int nlook) {
  gc_bytes b; int i,j;
  int p_sl,p_fl,p_ll, sl_base, fl_base, ll_base;
  int *script_rec=(int*)calloc((size_t)(nscript?nscript:1),sizeof(int));
  int *feat_rec=(int*)calloc((size_t)(nfeat?nfeat:1),sizeof(int));
  int *look_rec=(int*)calloc((size_t)(nlook?nlook:1),sizeof(int));
  b.n=0;
  gc_u32(&b,0x10000); p_sl=gc_place16(&b); p_fl=gc_place16(&b); p_ll=gc_place16(&b);
  /* ScriptList: record offsets are relative to the ScriptList base. */
  sl_base=b.n;
  gc_patch16(&b,p_sl,(unsigned)sl_base);
  gc_u16(&b,(unsigned)nscript);
  for(i=0;i<nscript;i++){ gc_u32(&b,scripts[i].tag); script_rec[i]=gc_place16(&b); }
  for(i=0;i<nscript;i++) {
    const gc_script_spec *s=&scripts[i]; int st_start=b.n, defp=0;
    gc_patch16(&b,script_rec[i],(unsigned)(st_start-sl_base));
    if(s->has_default) defp=gc_place16(&b); else gc_u16(&b,0);
    gc_u16(&b,(unsigned)s->nlang);
    { int *lang_rec=(int*)calloc((size_t)(s->nlang?s->nlang:1),sizeof(int));
      for(j=0;j<s->nlang;j++){ gc_u32(&b,s->lang[j].tag); lang_rec[j]=gc_place16(&b); }
      if(s->has_default) {
        const gc_lang_spec *L=&s->def; int k;
        gc_patch16(&b,defp,(unsigned)(b.n-st_start));
        gc_u16(&b,0); gc_u16(&b,(unsigned)L->required); gc_u16(&b,(unsigned)L->nfeat);
        for(k=0;k<L->nfeat;k++) gc_u16(&b,L->feat[k]);
      }
      for(j=0;j<s->nlang;j++) {
        const gc_lang_spec *L=&s->lang[j]; int k;
        gc_patch16(&b,lang_rec[j],(unsigned)(b.n-st_start));
        gc_u16(&b,0); gc_u16(&b,(unsigned)L->required); gc_u16(&b,(unsigned)L->nfeat);
        for(k=0;k<L->nfeat;k++) gc_u16(&b,L->feat[k]);
      }
      free(lang_rec);
    }
  }
  /* FeatureList: record offsets are relative to the FeatureList base. */
  fl_base=b.n;
  gc_patch16(&b,p_fl,(unsigned)fl_base);
  gc_u16(&b,(unsigned)nfeat);
  for(i=0;i<nfeat;i++){ gc_u32(&b,feats[i].tag); feat_rec[i]=gc_place16(&b); }
  for(i=0;i<nfeat;i++) {
    gc_patch16(&b,feat_rec[i],(unsigned)(b.n-fl_base));
    gc_u16(&b,0); gc_u16(&b,(unsigned)feats[i].nlook);
    for(j=0;j<feats[i].nlook;j++) gc_u16(&b,feats[i].look[j]);
  }
  /* LookupList: record offsets are relative to the LookupList base. */
  ll_base=b.n;
  gc_patch16(&b,p_ll,(unsigned)ll_base);
  gc_u16(&b,(unsigned)nlook);
  for(i=0;i<nlook;i++) look_rec[i]=gc_place16(&b);
  for(i=0;i<nlook;i++) {
    int lk_start=b.n, k;
    int offpos[4];
    gc_patch16(&b,look_rec[i],(unsigned)(lk_start-ll_base));
    gc_u16(&b,(unsigned)looks[i].type);
    gc_u16(&b,(unsigned)looks[i].flags);
    gc_u16(&b,(unsigned)looks[i].nsub);
    for(k=0;k<looks[i].nsub;k++) offpos[k]=gc_place16(&b);
    for(k=0;k<looks[i].nsub;k++) {
      gc_patch16(&b,offpos[k],(unsigned)(b.n-lk_start));
      { int m; for(m=0;m<looks[i].sub[k].n;m++) gc_u8(&b,looks[i].sub[k].b[m]); }
    }
  }
  free(script_rec); free(feat_rec); free(look_rec);
  *out=b;
}

/* GDEF 1.0 with a format-1 GlyphClassDef over the 8 fixture glyphs. */
static void gc_gdef_fmt1(gc_bytes *out,const unsigned short cls[8]) {
  int i;
  out->n=0; gc_u32(out,0x10000); gc_u16(out,12); gc_zero(out,6);
  gc_u16(out,1); gc_u16(out,0); gc_u16(out,8);
  for(i=0;i<8;i++) gc_u16(out,cls[i]);
}
/* GDEF 1.0 with a format-2 GlyphClassDef: triples start/end/class. */
static void gc_gdef_fmt2(gc_bytes *out,const unsigned short *triples,int nr) {
  int i;
  out->n=0; gc_u32(out,0x10000); gc_u16(out,12); gc_zero(out,6);
  gc_u16(out,2); gc_u16(out,(unsigned)nr);
  for(i=0;i<nr*3;i++) gc_u16(out,triples[i]);
}
/* GDEF 1.0 with no GlyphClassDef (offset 0): every glyph falls in class 0. */
static void gc_gdef_noclass(gc_bytes *out) {
  out->n=0; gc_u32(out,0x10000); gc_zero(out,8);
}
/* A GDEF carrying an unsupported version: the entry must tolerate it. */
static void gc_gdef_v2(gc_bytes *out) {
  out->n=0; gc_u32(out,0x00020000); gc_zero(out,8);
}

/* Font builder that allows GSUB and/or GDEF to be omitted (NULL pointer). */
static void gc_font_opt(gc_bytes *out,const gc_bytes *gsub,const gc_bytes *gdef) {
  static const char tags[11][5]={"GDEF","GSUB","cmap","glyf","head","hhea","hmtx","loca","maxp","name","post"};
  gc_bytes *t=(gc_bytes*)calloc(11,sizeof(gc_bytes));
  gc_bytes empty;
  int i,j,head_at=0,ntab=0; static const int widths[8]={200,200,100,450,80,250,150,300};
  if(!t) abort();
  empty.n=0;
  if(gdef) t[0]=*gdef;
  t[1]= gsub ? *gsub : empty;
  gc_u16(&t[2],0);gc_u16(&t[2],1);gc_u16(&t[2],3);gc_u16(&t[2],1);gc_u32(&t[2],12);
  gc_u16(&t[2],4);gc_u16(&t[2],480);gc_u16(&t[2],0);
  gc_u16(&t[2],4);gc_u16(&t[2],4);gc_u16(&t[2],1);gc_u16(&t[2],0);
  gc_u16(&t[2],255);gc_u16(&t[2],0xffff);gc_u16(&t[2],0);
  gc_u16(&t[2],32);gc_u16(&t[2],0xffff);gc_u16(&t[2],0);gc_u16(&t[2],1);
  gc_u16(&t[2],4);gc_u16(&t[2],0);
  for(i=32;i<256;i++)gc_u16(&t[2],i=='i'?2:1);
  for(i=0;i<8;i++) {
    int w=widths[i]; gc_u32(&t[7],(unsigned)t[3].n);
    gc_u16(&t[3],1); gc_u16(&t[3],0);gc_u16(&t[3],0);gc_u16(&t[3],w);gc_u16(&t[3],600);
    gc_u16(&t[3],3); gc_u16(&t[3],0);
    for(j=0;j<4;j++)gc_u8(&t[3],1);
    gc_u16(&t[3],0);gc_u16(&t[3],w);gc_u16(&t[3],0);gc_u16(&t[3],(unsigned)-w);
    gc_u16(&t[3],0);gc_u16(&t[3],0);gc_u16(&t[3],600);gc_u16(&t[3],0);
    gc_u16(&t[6],w+50);gc_u16(&t[6],0);
  }
  gc_u32(&t[7],(unsigned)t[3].n);
  gc_zero(&t[4],54);gc_patch32(&t[4],0,0x10000);gc_patch32(&t[4],12,0x5f0f3cf5);
  gc_patch16(&t[4],18,1000);gc_patch16(&t[4],40,450);gc_patch16(&t[4],42,600);gc_patch16(&t[4],50,1);
  gc_zero(&t[5],36);gc_patch32(&t[5],0,0x10000);gc_patch16(&t[5],4,800);
  gc_patch16(&t[5],6,(unsigned)-200);gc_patch16(&t[5],34,8);
  gc_zero(&t[8],32);gc_patch32(&t[8],0,0x10000);gc_patch16(&t[8],4,8);
  gc_u16(&t[9],0);gc_u16(&t[9],1);gc_u16(&t[9],18);
  gc_u16(&t[9],3);gc_u16(&t[9],1);gc_u16(&t[9],0x409);gc_u16(&t[9],4);gc_u16(&t[9],8);gc_u16(&t[9],0);
  gc_u16(&t[9],'D');gc_u16(&t[9],'e');gc_u16(&t[9],'m');gc_u16(&t[9],'o');
  gc_u32(&t[10],0x30000);gc_zero(&t[10],28);
  for(i=0;i<11;i++) if(!(i==0&&!gdef)&&!(i==1&&!gsub)) ntab++;
  out->n=0;gc_u32(out,0x10000);gc_u16(out,(unsigned)ntab);gc_u16(out,128);gc_u16(out,3);gc_u16(out,48);
  gc_zero(out,16*11);
  for(i=0;i<11;i++) {
    int r=12+16*i;
    if((i==0&&!gdef)||(i==1&&!gsub)) continue;
    for(j=0;j<4;j++)out->b[r+j]=(unsigned char)tags[i][j];
    gc_patch32(out,r+4,gc_sum(&t[i]));gc_patch32(out,r+8,(unsigned)out->n);gc_patch32(out,r+12,(unsigned)t[i].n);
    if(i==4)head_at=out->n;
    for(j=0;j<t[i].n;j++)gc_u8(out,t[i].b[j]);
    while(out->n%4)gc_u8(out,0);
  }
  gc_patch32(out,head_at+8,0xb1b0afba-gc_sum(out));free(t);
}
#endif
