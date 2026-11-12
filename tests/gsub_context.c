/* GSUB 1.0 subset tests for stbtt_ApplyGSUB().
 *
 * The fonts are synthesized at runtime through gsubcheck/fixture.h; no system
 * fonts are used. Content assertions go through GC_CHECK so they survive
 * NDEBUG. Substituted sequences are rendered with the existing stb_truetype
 * bitmap and SDF entry points and compared against explicit target glyphs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Counting, fail-injecting allocator: every stb allocation is accounted for so
 * the error paths can prove there are no leaks and no partial publication. */
static long gc_live;
static long gc_total_malloc;
static long gc_total_free;
static long gc_fail_at;            /* 0 = never fail, else 1-based call index */
static long gc_alloc_serial;

static void *gc_test_malloc(size_t z, void *u)
{
   void *p;
   (void)u;
   ++gc_alloc_serial;
   if (gc_fail_at && gc_alloc_serial == gc_fail_at) return NULL;
   p = malloc(z ? z : 1);
   if (p) { ++gc_live; ++gc_total_malloc; }
   return p;
}
static void gc_test_free(void *p, void *u)
{
   (void)u;
   if (p) { --gc_live; ++gc_total_free; free(p); }
}
#define STBTT_malloc gc_test_malloc
#define STBTT_free   gc_test_free

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "fixture.h"
#include "suite.h"

#define GC_DFLT GC_TAG('D','F','L','T')
#define GC_LATN GC_TAG('l','a','t','n')
#define GC_LIGA GC_TAG('l','i','g','a')
#define GC_CALT GC_TAG('c','a','l','t')
#define GC_RLIG GC_TAG('r','l','i','g')
#define GC_DUMY GC_TAG('d','u','m','y')
#define GC_ENG  GC_TAG('E','N','G',' ')
#define GC_ZZZ  GC_TAG('z','z','z','z')

static void gc_open(const gc_bytes *b, stbtt_fontinfo *f)
{
   memset(f, 0, sizeof(*f));
   GC_CHECK(stbtt_InitFont(f, b->b, 0) != 0);
   GC_CHECK(f->numGlyphs == 8);
}

static int gc_apply(const stbtt_fontinfo *f, int sz, unsigned script, unsigned lang,
                    const unsigned *feats, int nfeat,
                    unsigned short *g, int n, int cap)
{
   int cnt = n;
   int r = stbtt_ApplyGSUB(f, sz, script, lang, feats, nfeat, g, &cnt, cap);
   if (!r) return -1;
   return cnt;
}

static int gc_seq_same(const unsigned short *a, const unsigned short *b, int n)
{
   int i;
   for (i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
   return 1;
}

static void gc_expect(const char *name, const unsigned short *got, int n,
                      const unsigned short *want, int wn)
{
   int i;
   GC_CHECK(n == wn);
   if (n != wn) { fprintf(stderr, "seq %s: count %d != %d\n", name, n, wn); return; }
   for (i = 0; i < n; ++i) {
      GC_CHECK(got[i] == want[i]);
      if (got[i] != want[i])
         fprintf(stderr, "seq %s: idx %d glyph %u != %u\n", name, i, got[i], want[i]);
   }
}

/* Bitmap of g1 must be byte-for-byte the bitmap of explicit target g2. */
static int gc_bitmaps_equal(const stbtt_fontinfo *f, int g1, int g2)
{
   int w1,h1,x1,y1,w2,h2,x2,y2,i,ok;
   unsigned char *a = stbtt_GetGlyphBitmap(f, 0.04f, 0.04f, g1, &w1,&h1,&x1,&y1);
   unsigned char *b = stbtt_GetGlyphBitmap(f, 0.04f, 0.04f, g2, &w2,&h2,&x2,&y2);
   GC_CHECK(a != NULL && b != NULL);
   ok = a && b && w1==w2 && h1==h2 && x1==x2 && y1==y2;
   if (ok) for (i=0; i<w1*h1; ++i) if (a[i] != b[i]) { ok = 0; break; }
   GC_CHECK(ok);
   stbtt_FreeBitmap(a, NULL);
   stbtt_FreeBitmap(b, NULL);
   return ok;
}

static int gc_sdf_equal(const stbtt_fontinfo *f, int g1, int g2)
{
   int w1,h1,x1,y1,w2,h2,x2,y2,i,ok;
   unsigned char *a = stbtt_GetGlyphSDF(f, 0.04f, g1, 2, 128, 8.0f, &w1,&h1,&x1,&y1);
   unsigned char *b = stbtt_GetGlyphSDF(f, 0.04f, g2, 2, 128, 8.0f, &w2,&h2,&x2,&y2);
   GC_CHECK(a != NULL && b != NULL);
   ok = a && b && w1==w2 && h1==h2 && x1==x2 && y1==y2;
   if (ok) for (i=0; i<w1*h1; ++i) if (a[i] != b[i]) { ok = 0; break; }
   GC_CHECK(ok);
   stbtt_FreeSDF(a, NULL);
   stbtt_FreeSDF(b, NULL);
   return ok;
}

/* ------------------------------------------------------------------ */
/* Group 1: script/language/feature selection and lookup list ordering */
/* ------------------------------------------------------------------ */

/* L0 1->3, L1 2->4, L2 5->6, L3 1->7 (orphan), L4 unsupported type 3 and
 * reachable only through the unselected 'dummy' feature. */
static void gc_plan_font(gc_bytes *out)
{
   gc_bytes gsub, gdef;
   gc_lookup_b L[5];
   gc_feat_spec feat[4];
   gc_script_spec scr[2];
   static const unsigned short c1[]={1}, c2[]={2}, c5[]={5};
   static const unsigned short o3[]={3}, o4[]={4}, o6[]={6}, o7[]={7};
   static const unsigned short cls[8]={0,1,1,1,3,1,1,1};
   int i;
   memset(L, 0, sizeof(L));
   L[0].type=1; L[0].flags=0; L[0].nsub=1; gc_sub_single2(&L[0].sub[0], c1, o3, 1);
   L[1].type=1; L[1].flags=0; L[1].nsub=1; gc_sub_single2(&L[1].sub[0], c2, o4, 1);
   L[2].type=1; L[2].flags=0; L[2].nsub=1; gc_sub_single2(&L[2].sub[0], c5, o6, 1);
   L[3].type=1; L[3].flags=0; L[3].nsub=1; gc_sub_single2(&L[3].sub[0], c1, o7, 1);
   L[4].type=3; L[4].flags=0; L[4].nsub=1; /* Alternating: intentionally unsupported */
   L[4].sub[0].n=0; gc_u16(&L[4].sub[0],1); gc_zero(&L[4].sub[0],6);

   memset(feat,0,sizeof(feat));
   feat[0].tag=GC_LIGA; feat[0].nlook=1; feat[0].look[0]=0;
   feat[1].tag=GC_CALT; feat[1].nlook=1; feat[1].look[0]=1;
   feat[2].tag=GC_RLIG; feat[2].nlook=2; feat[2].look[0]=2; feat[2].look[1]=0;
   feat[3].tag=GC_DUMY; feat[3].nlook=1; feat[3].look[0]=4;

   memset(scr,0,sizeof(scr));
   scr[0].tag=GC_LATN;
   scr[0].has_default=1;
   scr[0].def.required=0xffff; scr[0].def.nfeat=1; scr[0].def.feat[0]=0; /* liga */
   scr[0].nlang=1;
   scr[0].lang[0].tag=GC_ENG;
   scr[0].lang[0].required=2;                 /* rlig always on */
   scr[0].lang[0].nfeat=3;
   scr[0].lang[0].feat[0]=0;                  /* liga */
   scr[0].lang[0].feat[1]=1;                  /* calt */
   scr[0].lang[0].feat[2]=3;                  /* dummy (unselected at first) */
   scr[1].tag=GC_DFLT;
   scr[1].has_default=1;
   scr[1].def.required=0xffff; scr[1].def.nfeat=1; scr[1].def.feat[0]=1; /* calt */
   scr[1].nlang=0;

   gc_build_gsub(&gsub, scr, 2, feat, 4, L, 5);
   gc_gdef_fmt1(&gdef, cls);
   gc_font_opt(out, &gsub, &gdef);
   (void)i;
}

static int gc_group_lookup_plan(void)
{
   int group_start = gc_checks;
   gc_bytes b; stbtt_fontinfo f;
   unsigned short g[16];
   static const unsigned short in3[]   = {1,2,5};
   static const unsigned short eng[]   = {3,4,6};   /* L0,L1,L2 in index order */
   static const unsigned short liga[]  = {3,2,5};   /* default lang: liga only */
   static const unsigned short calt[]  = {1,4,5};   /* DFLT default: calt only */
   unsigned fe_fwd[2] = {GC_LIGA, GC_CALT};
   unsigned fe_rev[2] = {GC_CALT, GC_LIGA};
   unsigned fe_dup[3] = {GC_LIGA, GC_CALT, GC_LIGA};
   unsigned fe_dummy[1] = {GC_DUMY};
   unsigned fe_rlig[2] = {GC_LIGA, GC_RLIG};
   unsigned fe_liga_only[1] = {GC_LIGA};
   int n;

   gc_plan_font(&b);
   gc_open(&b, &f);

   /* ENG: required rlig plus requested liga/calt -> L0,L1,L2 index order. */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_LATN, GC_ENG, fe_fwd, 2, g, 3, 16);
   gc_expect("eng-order", g, n, eng, 3);

   /* Feature argument order must not reorder the LookupList sweep. */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_LATN, GC_ENG, fe_rev, 2, g, 3, 16);
   gc_expect("eng-reversed-args", g, n, eng, 3);

   /* Duplicate requested tags still execute the merged plan exactly once. */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_LATN, GC_ENG, fe_dup, 3, g, 3, 16);
   gc_expect("eng-duplicate-tag", g, n, eng, 3);

   /* Default language has no required feature and references only liga. */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_LATN, 0, fe_fwd, 2, g, 3, 16);
   gc_expect("latn-default-lang", g, n, liga, 3);

   /* Unknown language falls back to the script default. */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_LATN, GC_ZZZ, fe_fwd, 2, g, 3, 16);
   gc_expect("unknown-lang-fallback", g, n, liga, 3);

   /* Unknown script falls back to DFLT (calt only). */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_ZZZ, 0, fe_fwd, 2, g, 3, 16);
   gc_expect("script-dflt-fallback", g, n, calt, 3);

   /* The DFLT LangSys references neither requested tag, so nothing runs. */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_DFLT, 0, fe_rlig, 2, g, 3, 16);
   gc_expect("dflt-ignores-unreferenced", g, n, in3, 3);

   /* The unsupported lookup sits behind an unselected feature: font accepted. */
   memcpy(g, in3, sizeof(in3));
   n = gc_apply(&f, b.n, GC_LATN, GC_ENG, fe_fwd, 2, g, 3, 16);
   GC_CHECK(n == 3);

   /* Selecting the feature that owns the unsupported lookup must fail hard. */
   {
      unsigned short backup[16];
      memcpy(g, in3, sizeof(in3));
      memcpy(backup, g, sizeof(in3));
      n = gc_apply(&f, b.n, GC_LATN, GC_ENG, fe_dummy, 1, g, 3, 16);
      GC_CHECK(n == -1);
      GC_CHECK(gc_seq_same(g, backup, 3));
   }

   /* A selected lookup carrying an unsupported flag must fail hard. */
   {
      gc_bytes b2, g2, gs2;
      gc_lookup_b L2[1];
      gc_feat_spec ft2[1];
      gc_script_spec sc2[1];
      static const unsigned short c1b[]={1}, o7b[]={7};
      memset(L2,0,sizeof(L2));
      L2[0].type=1; L2[0].flags=0x10; L2[0].nsub=1; /* UseMarkFilteringSet */
      gc_sub_single2(&L2[0].sub[0], c1b, o7b, 1);
      memset(ft2,0,sizeof(ft2));
      ft2[0].tag=GC_LIGA; ft2[0].nlook=1; ft2[0].look[0]=0;
      memset(sc2,0,sizeof(sc2));
      sc2[0].tag=GC_DFLT; sc2[0].has_default=1;
      sc2[0].def.required=0xffff; sc2[0].def.nfeat=1; sc2[0].def.feat[0]=0;
      gc_build_gsub(&gs2, sc2, 1, ft2, 1, L2, 1);
      gc_gdef_noclass(&g2);
      gc_font_opt(&b2, &gs2, &g2);
      {
         stbtt_fontinfo f2; unsigned short g3[2]={1,2}; unsigned short bk[2]={1,2};
         gc_open(&b2, &f2);
         n = gc_apply(&f2, b2.n, GC_DFLT, 0, fe_liga_only, 1, g3, 2, 2);
         GC_CHECK(n == -1);
         GC_CHECK(gc_seq_same(g3, bk, 2));
      }
   }

   /* ---- table formats: Single fmt1/fmt2, coverage fmt2, Extension fmt1 ---- */
   {
      gc_bytes bf, gsf, gdf;
      gc_lookup_b Lf[2];
      gc_feat_spec ff[2];
      gc_script_spec sf[1];
      gc_bytes inner;
      static const unsigned short range13[3]={1,3,0};          /* coverage fmt2: glyphs 1..3 */
      static const unsigned short range45[3]={4,5,0};          /* coverage fmt2: glyphs 4..5 */
      static const unsigned short s2out[2]={7,6};              /* 4->7, 5->6 */
      static const unsigned short c4[]={4}, o7[]={7};
      static const unsigned short cls_triples[3]={4,4,3};      /* glyph 4 is a mark (class 3) */
      static const unsigned short in[]={1,2,3,5};
      /* L0 (delta +3 over 1..3): {4,5,6,5}; L1 extension then maps 4->7. */
      static const unsigned short want[]={7,5,6,5};
      unsigned fe_two[2]={GC_LIGA, GC_CALT};
      memset(Lf,0,sizeof(Lf));
      /* L0: SingleSubst format 1, range (coverage format 2), delta +3. */
      Lf[0].type=1; Lf[0].flags=0; Lf[0].nsub=1;
      gc_sub_single1_range(&Lf[0].sub[0], range13, 1, 3);
      /* L1: ExtensionSubst format 1 wrapping a concrete SingleSubst. */
      Lf[1].type=7; Lf[1].flags=0; Lf[1].nsub=1;
      inner.n=0; gc_sub_single2(&inner, c4, o7, 1);
      gc_wrap_extension(&Lf[1].sub[0], 1, &inner);
      memset(ff,0,sizeof(ff));
      ff[0].tag=GC_LIGA; ff[0].nlook=1; ff[0].look[0]=0;
      ff[1].tag=GC_CALT; ff[1].nlook=1; ff[1].look[0]=1;
      memset(sf,0,sizeof(sf));
      sf[0].tag=GC_DFLT; sf[0].has_default=1;
      sf[0].def.required=0xffff; sf[0].def.nfeat=2;
      sf[0].def.feat[0]=0; sf[0].def.feat[1]=1;
      gc_build_gsub(&gsf, sf, 1, ff, 2, Lf, 2);
      gc_gdef_noclass(&gdf);
      gc_font_opt(&bf, &gsf, &gdf);
      {
         stbtt_fontinfo ff2; unsigned short gg[8];
         gc_open(&bf, &ff2);
         memcpy(gg, in, sizeof(in));
         n = gc_apply(&ff2, bf.n, GC_DFLT, 0, fe_two, 2, gg, 4, 8);
         gc_expect("single1-range-and-extension", gg, n, want, 4);
      }

      /* SingleSubst format 2 on a range coverage: coverage index continues
         across ranges, mapping glyph 5 (index 1) to 6. */
      {
         gc_bytes b3, gs3, gd3;
         gc_lookup_b L3[1]; gc_feat_spec ft3[1]; gc_script_spec sc3[1];
         static const unsigned short in3[]={4,5};
         static const unsigned short want3[]={7,6};
         memset(L3,0,sizeof(L3));
         L3[0].type=1; L3[0].flags=0; L3[0].nsub=1;
         gc_sub_single2_range(&L3[0].sub[0], range45, 1, s2out, 2);
         memset(ft3,0,sizeof(ft3));
         ft3[0].tag=GC_LIGA; ft3[0].nlook=1; ft3[0].look[0]=0;
         memset(sc3,0,sizeof(sc3));
         sc3[0].tag=GC_DFLT; sc3[0].has_default=1;
         sc3[0].def.required=0xffff; sc3[0].def.nfeat=1; sc3[0].def.feat[0]=0;
         gc_build_gsub(&gs3, sc3, 1, ft3, 1, L3, 1);
         gc_gdef_noclass(&gd3);
         gc_font_opt(&b3, &gs3, &gd3);
         {
            stbtt_fontinfo f3; unsigned short gg[4];
            gc_open(&b3, &f3);
            memcpy(gg, in3, sizeof(in3));
            n = gc_apply(&f3, b3.n, GC_DFLT, 0, fe_liga_only, 1, gg, 2, 4);
            gc_expect("single2-range-coverage", gg, n, want3, 2);
         }
      }

      /* GDEF GlyphClassDef format 2: glyph 4 is a mark; an IgnoreMarks
         lookup leaves it alone while still substituting the base glyphs. */
      {
         gc_bytes b4, gs4, gd4;
         gc_lookup_b L4[1]; gc_feat_spec ft4[1]; gc_script_spec sc4[1];
         static const unsigned short range15[3]={1,5,0};
         static const unsigned short in4[]={1,4,2};
         static const unsigned short want4[]={2,4,3};
         memset(L4,0,sizeof(L4));
         L4[0].type=1; L4[0].flags=0x8; L4[0].nsub=1;      /* IgnoreMarks */
         gc_sub_single1_range(&L4[0].sub[0], range15, 1, 1);
         memset(ft4,0,sizeof(ft4));
         ft4[0].tag=GC_LIGA; ft4[0].nlook=1; ft4[0].look[0]=0;
         memset(sc4,0,sizeof(sc4));
         sc4[0].tag=GC_DFLT; sc4[0].has_default=1;
         sc4[0].def.required=0xffff; sc4[0].def.nfeat=1; sc4[0].def.feat[0]=0;
         gc_build_gsub(&gs4, sc4, 1, ft4, 1, L4, 1);
         gc_gdef_fmt2(&gd4, cls_triples, 1);
         gc_font_opt(&b4, &gs4, &gd4);
         {
            stbtt_fontinfo f4; unsigned short gg[4];
            gc_open(&b4, &f4);
            memcpy(gg, in4, sizeof(in4));
            n = gc_apply(&f4, b4.n, GC_DFLT, 0, fe_liga_only, 1, gg, 3, 4);
            gc_expect("gdef-classdef-format2", gg, n, want4, 3);
         }
      }

      /* SingleSubst format 1 with a format-1 (explicit list) coverage. */
      {
         gc_bytes b5, gs5, gd5;
         gc_lookup_b L5[1]; gc_feat_spec ft5[1]; gc_script_spec sc5[1];
         static const unsigned short list_cov[2]={2,3};
         static const unsigned short in5[]={1,2,3};
         static const unsigned short want5[]={1,5,6};  /* +3 delta on 2 and 3 */
         memset(L5,0,sizeof(L5));
         L5[0].type=1; L5[0].flags=0; L5[0].nsub=1;
         gc_sub_single1(&L5[0].sub[0], list_cov, 2, 3);
         memset(ft5,0,sizeof(ft5));
         ft5[0].tag=GC_LIGA; ft5[0].nlook=1; ft5[0].look[0]=0;
         memset(sc5,0,sizeof(sc5));
         sc5[0].tag=GC_DFLT; sc5[0].has_default=1;
         sc5[0].def.required=0xffff; sc5[0].def.nfeat=1; sc5[0].def.feat[0]=0;
         gc_build_gsub(&gs5, sc5,1, ft5,1, L5,1);
         gc_gdef_noclass(&gd5);
         gc_font_opt(&b5, &gs5, &gd5);
         {
            stbtt_fontinfo f5; unsigned short gg[4];
            gc_open(&b5, &f5);
            memcpy(gg, in5, sizeof(in5));
            n = gc_apply(&f5, b5.n, GC_DFLT, 0, fe_liga_only, 1, gg, 3, 4);
            gc_expect("single1-list-coverage", gg, n, want5, 3);
         }
      }
   }

   GC_GROUP_DONE();
   return gc_checks - group_start;
}

/* ------------------------------------------------------------------ */
/* Group 2: nested positions after expansion and contraction           */
/* ------------------------------------------------------------------ */

/* Glyph roles: A=1 B=2 C=3 M=4(mark) D=5 X=6 Y=7.
 * Outer chain L0 (IgnoreMarks): input A B C, lookahead D.
 * expand variant: si0 Multiple A->X Y (inner IgnoreLigatures), si2 Single B->7.
 * liga variant:   si0 Ligature A B->6, si1 Single C->7. */
static void gc_context_font(gc_bytes *out, int expand)
{
   gc_bytes gsub, gdef;
   gc_lookup_b L[3];
   gc_feat_spec feat[1];
   gc_script_spec scr[1];
   static const unsigned short cA[]={1}, cB[]={2}, cC[]={3}, cD[]={5};
   static const unsigned short xy[]={6,7};
   static const unsigned short *mseqs[]={xy};
   static const int msl[]={2};
   static const unsigned short bto7[]={7};
   static const unsigned short lig_rest[]={2};
   static const gc_liga_spec lig = { 1, lig_rest, 1, 6 };
   static const gc_record rec_expand[] = { {0,1}, {2,2} };
   static const gc_record rec_liga[]   = { {0,1}, {1,2} };
   static const unsigned short cls[8]={0,1,1,1,3,1,2,1};
   static const unsigned short chain_in[3]={1,2,3};

   memset(L,0,sizeof(L));
   if (expand) {
      L[1].type=2; L[1].flags=0x4; L[1].nsub=1;            /* IgnoreLigatures */
      gc_sub_multiple(&L[1].sub[0], cA, 1, mseqs, msl, 1);
   } else {
      /* The nested ligature carries its own IgnoreMarks flag, distinct from
         the expansion branch's IgnoreLigatures; the mark is skipped for the
         match but must remain in the sequence. */
      L[1].type=4; L[1].flags=0x8; L[1].nsub=1;
      gc_sub_liga(&L[1].sub[0], cA, 1, &lig, 1);
   }
   L[2].type=1; L[2].flags=0; L[2].nsub=1;
   if (expand) gc_sub_single2(&L[2].sub[0], cB, bto7, 1);       /* B -> 7 */
   else {
      static const unsigned short cto7[]={7};
      gc_sub_single2(&L[2].sub[0], cC, cto7, 1);                /* C -> 7 */
   }

   L[0].type=6; L[0].flags=0x8; L[0].nsub=1;               /* outer IgnoreMarks */
   gc_sub_chain3(&L[0].sub[0],
                 0, 0,                                      /* no backtrack */
                 chain_in, 3,                               /* input A B C */
                 cD, 1,                                     /* lookahead D */
                 expand ? rec_expand : rec_liga, expand ? 2 : 2);

   memset(feat,0,sizeof(feat));
   feat[0].tag=GC_LIGA; feat[0].nlook=1; feat[0].look[0]=0;
   memset(scr,0,sizeof(scr));
   scr[0].tag=GC_DFLT; scr[0].has_default=1;
   scr[0].def.required=0xffff; scr[0].def.nfeat=1; scr[0].def.feat[0]=0;

   gc_build_gsub(&gsub, scr, 1, feat, 1, L, 3);
   gc_gdef_fmt1(&gdef, cls);
   gc_font_opt(out, &gsub, &gdef);
   (void)cC;
}

static int gc_group_context_edits(void)
{
   int group_start = gc_checks;
   gc_bytes be, bl; stbtt_fontinfo fe, fl;
   unsigned short g[24];
   unsigned feats[1] = {GC_LIGA};
   int n;

   gc_context_font(&be, 1);
   gc_context_font(&bl, 0);
   gc_open(&be, &fe);
   gc_open(&bl, &fl);

   /* Expansion: A B C D -> X Y B C D, then si2 lands on B -> X Y 7 C D. */
   {
      static const unsigned short in[]  = {1,2,3,5};
      static const unsigned short want[]= {6,7,7,3,5};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fe, be.n, GC_DFLT, 0, feats, 1, g, 4, 24);
      gc_expect("expand-no-mark", g, n, want, 5);
   }

   /* A M B C D: the outer-ignored mark survives and si2 still reaches B. */
   {
      static const unsigned short in[]  = {1,4,2,3,5};
      static const unsigned short want[]= {6,7,4,7,3,5};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fe, be.n, GC_DFLT, 0, feats, 1, g, 5, 24);
      gc_expect("expand-with-mark", g, n, want, 6);
   }

   /* Contraction: A B C D -> L C D, then si1 now lands on C -> L 7 D. */
   {
      static const unsigned short in[]  = {1,2,3,5};
      static const unsigned short want[]= {6,7,5};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fl, bl.n, GC_DFLT, 0, feats, 1, g, 4, 24);
      gc_expect("liga-contract", g, n, want, 3);
   }

   /* Lookahead requirement: without D the outer context never matches. */
   {
      static const unsigned short in[]  = {1,2,3};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fl, bl.n, GC_DFLT, 0, feats, 1, g, 3, 24);
      GC_CHECK(n == 3);
      GC_CHECK(g[0]==1 && g[1]==2 && g[2]==3);
   }

   /* Two independent contexts both fire; the scan does not eat the lookahead. */
   {
      static const unsigned short in[]  = {1,2,3,5, 1,2,3,5};
      static const unsigned short want[]= {6,7,5, 6,7,5};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fl, bl.n, GC_DFLT, 0, feats, 1, g, 8, 24);
      gc_expect("liga-two-contexts", g, n, want, 6);
   }

   /* Expansion variant over two contexts keeps every length shift correct. */
   {
      static const unsigned short in[]  = {1,2,3,5, 1,2,3,5};
      static const unsigned short want[]= {6,7,7,3,5, 6,7,7,3,5};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fe, be.n, GC_DFLT, 0, feats, 1, g, 8, 24);
      gc_expect("expand-two-contexts", g, n, want, 10);
   }

   /* Backtrack coverage: B is substituted only when preceded by A. */
   {
      gc_bytes bb, gsb, gdb;
      gc_lookup_b Lb[2]; gc_feat_spec ftb[1]; gc_script_spec scb[1];
      static const unsigned short cA[]={1}, cB[]={2}, bto7[]={7};
      static const unsigned short chain_in[1]={2};
      static const gc_record rec[1]={{0,1}};
      memset(Lb,0,sizeof(Lb));
      Lb[1].type=1; Lb[1].flags=0; Lb[1].nsub=1;
      gc_sub_single2(&Lb[1].sub[0], cB, bto7, 1);
      Lb[0].type=6; Lb[0].flags=0; Lb[0].nsub=1;
      gc_sub_chain3(&Lb[0].sub[0], cA,1, chain_in,1, 0,0, rec,1);
      memset(ftb,0,sizeof(ftb));
      ftb[0].tag=GC_LIGA; ftb[0].nlook=1; ftb[0].look[0]=0;
      memset(scb,0,sizeof(scb));
      scb[0].tag=GC_DFLT; scb[0].has_default=1;
      scb[0].def.required=0xffff; scb[0].def.nfeat=1; scb[0].def.feat[0]=0;
      gc_build_gsub(&gsb, scb,1, ftb,1, Lb,2);
      gc_gdef_noclass(&gdb);
      gc_font_opt(&bb, &gsb, &gdb);
      {
         stbtt_fontinfo fb;
         static const unsigned short yes[]={1,2};
         static const unsigned short yes_want[]={1,7};
         static const unsigned short no[]={2,2};
         gc_open(&bb, &fb);
         memcpy(g, yes, sizeof(yes));
         n = gc_apply(&fb, bb.n, GC_DFLT,0, feats,1, g,2,24);
         gc_expect("backtrack-hit", g, n, yes_want, 2);
         memcpy(g, no, sizeof(no));
         n = gc_apply(&fb, bb.n, GC_DFLT,0, feats,1, g,2,24);
         gc_expect("backtrack-miss", g, n, no, 2);
      }
   }

   /* RightToLeft flag is accepted and does not change the LTR scan direction. */
   {
      gc_bytes br2, gsr, gdr;
      gc_lookup_b Lr[1]; gc_feat_spec ftr[1]; gc_script_spec scr[1];
      static const unsigned short c2[]={2}, o7[]={7};
      memset(Lr,0,sizeof(Lr));
      Lr[0].type=1; Lr[0].flags=0x1; Lr[0].nsub=1;       /* RightToLeft */
      gc_sub_single2(&Lr[0].sub[0], c2, o7, 1);
      memset(ftr,0,sizeof(ftr));
      ftr[0].tag=GC_LIGA; ftr[0].nlook=1; ftr[0].look[0]=0;
      memset(scr,0,sizeof(scr));
      scr[0].tag=GC_DFLT; scr[0].has_default=1;
      scr[0].def.required=0xffff; scr[0].def.nfeat=1; scr[0].def.feat[0]=0;
      gc_build_gsub(&gsr, scr,1, ftr,1, Lr,1);
      gc_gdef_noclass(&gdr);
      gc_font_opt(&br2, &gsr, &gdr);
      {
         stbtt_fontinfo fr;
         static const unsigned short in[]={2,1,2};
         static const unsigned short want[]={7,1,7};
         gc_open(&br2, &fr);
         memcpy(g, in, sizeof(in));
         n = gc_apply(&fr, br2.n, GC_DFLT,0, feats,1, g,3,24);
         gc_expect("rtl-scan-direction", g, n, want, 3);
      }
   }

   /* A mark between the ligature components is never swallowed. */
   {
      static const unsigned short in[]  = {1,4,2,3,5};
      static const unsigned short want[]= {6,4,7,5};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fl, bl.n, GC_DFLT, 0, feats, 1, g, 5, 24);
      gc_expect("liga-keeps-mark", g, n, want, 4);
   }

   GC_GROUP_DONE();
   return gc_checks - group_start;
}

/* ------------------------------------------------------------------ */
/* Group 3: two different filters plus real bitmap/SDF rendering       */
/* ------------------------------------------------------------------ */

/* Inner single lookup flagged IgnoreLigatures: at a class-2 target it must
 * not fire even though the outer context matched. The outer lookup ignores
 * marks (0x8), the nested one ignores ligatures (0x4): distinct filters. */
static void gc_filter_font(gc_bytes *out)
{
   gc_bytes gsub, gdef;
   gc_lookup_b L[2];
   gc_feat_spec feat[1];
   gc_script_spec scr[1];
   static const unsigned short cB[]={2};
   static const unsigned short chain_in[2]={6,2};
   static const gc_record recs[2] = { {0,1}, {1,1} };
   static const unsigned short covboth[2]={2,6};
   static const unsigned short outboth[2]={7,7};
   /* glyph 6 is a ligature (class 2), glyph 2 is a base (class 1). */
   static const unsigned short cls[8]={0,1,1,1,3,1,2,1};

   memset(L,0,sizeof(L));
   L[1].type=1; L[1].flags=0x4; L[1].nsub=1;                 /* IgnoreLigatures */
   gc_sub_single2(&L[1].sub[0], covboth, outboth, 2);
   L[0].type=6; L[0].flags=0x8; L[0].nsub=1;                 /* IgnoreMarks */
   gc_sub_chain3(&L[0].sub[0], 0,0, chain_in,2, 0,0, recs,2);

   memset(feat,0,sizeof(feat));
   feat[0].tag=GC_LIGA; feat[0].nlook=1; feat[0].look[0]=0;
   memset(scr,0,sizeof(scr));
   scr[0].tag=GC_DFLT; scr[0].has_default=1;
   scr[0].def.required=0xffff; scr[0].def.nfeat=1; scr[0].def.feat[0]=0;

   gc_build_gsub(&gsub, scr, 1, feat, 1, L, 2);
   gc_gdef_fmt1(&gdef, cls);
   gc_font_opt(out, &gsub, &gdef);
   (void)cB;
}

static int gc_group_filter_and_render(void)
{
   int group_start = gc_checks;
   gc_bytes bf, bd; stbtt_fontinfo ff, fd;
   unsigned short g[24];
   unsigned feats[1]={GC_LIGA};
   int n;

   gc_filter_font(&bf);
   gc_open(&bf, &ff);

   /* Ligature glyph 6 is filtered by the *inner* IgnoreLigatures lookup, so
    * record si0 is a no-op; the base glyph 2 at si1 still maps to 7. */
   {
      static const unsigned short in[]  = {6,2};
      static const unsigned short want[]= {6,7};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&ff, bf.n, GC_DFLT, 0, feats, 1, g, 2, 24);
      gc_expect("inner-filter-different", g, n, want, 2);
   }

   /* Rendering: the default fixture turns f,i into glyph 3; the substituted
    * glyph sequence renders identically to the explicit target glyph, using
    * both the bitmap and SDF entry points. */
   gc_default_font(&bd);
   gc_open(&bd, &fd);
   {
      static const unsigned short in[]={1,2};
      unsigned short r[4];
      memcpy(r, in, sizeof(in));
      n = gc_apply(&fd, bd.n, GC_DFLT, 0, feats, 1, r, 2, 4);
      GC_CHECK(n == 1 && r[0] == 3);
      GC_CHECK(gc_bitmaps_equal(&fd, r[0], 3));
      GC_CHECK(gc_sdf_equal(&fd, r[0], 3));
      /* unchanged single-glyph render path still works on the raw inputs */
      GC_CHECK(gc_bitmaps_equal(&fd, 1, 1));
      GC_CHECK(gc_sdf_equal(&fd, 2, 2));
   }

   /* The expanded multi-glyph sequence renders each glyph equal to the
    * explicit target glyph bitmap, not merely an equal re-run. */
   {
      gc_bytes be; stbtt_fontinfo fe;
      static const unsigned short in[]={1,2,3,5};
      static const unsigned short want[]={6,7,7,3,5};
      int i;
      gc_context_font(&be, 1);
      gc_open(&be, &fe);
      memcpy(g, in, sizeof(in));
      n = gc_apply(&fe, be.n, GC_DFLT, 0, feats, 1, g, 4, 24);
      gc_expect("render-sequence", g, n, want, 5);
      for (i=0; i<n; ++i) GC_CHECK(gc_bitmaps_equal(&fe, g[i], want[i]));
   }

   GC_GROUP_DONE();
   return gc_checks - group_start;
}

/* ------------------------------------------------------------------ */
/* Group 4: failure preservation, allocation accounting and legacy API */
/* ------------------------------------------------------------------ */

static int gc_group_errors_and_legacy(void)
{
   int group_start = gc_checks;
   gc_bytes b; stbtt_fontinfo f;
   unsigned short g[24], backup[24];
   unsigned feats[1]={GC_LIGA};
   int n, i, saw_failures;

   /* ---- counting allocator: every injection point fails safely ---- */
   gc_default_font(&b);
   gc_open(&b, &f);
   saw_failures = 0;
   for (i = 1; i <= 64; ++i) {
      long m0, fr0;
      static const unsigned short in[]={1,2};
      gc_fail_at = i;
      gc_alloc_serial = 0;
      m0 = gc_total_malloc; fr0 = gc_total_free;
      memcpy(g, in, sizeof(in));
      memcpy(backup, in, sizeof(in));
      n = gc_apply(&f, b.n, GC_DFLT, 0, feats, 1, g, 2, 24);
      if (n == -1) {
         ++saw_failures;
         GC_CHECK(gc_seq_same(g, backup, 2)); /* no partial publication */
      }
      GC_CHECK(gc_live == 0);                 /* nothing leaked overall */
      GC_CHECK((gc_total_malloc - m0) == (gc_total_free - fr0));
      if (n != -1) break;                     /* covered every alloc site */
   }
   gc_fail_at = 0;
   GC_CHECK(saw_failures >= 3);

   /* ---- argument validation ---- */
   {
      int cnt=2; unsigned short gg[2]={1,2};
      GC_CHECK(stbtt_ApplyGSUB(NULL, b.n, GC_DFLT,0, feats,1, gg,&cnt,2) == 0);
      GC_CHECK(cnt == 2 && gg[0]==1 && gg[1]==2);
      cnt=2;
      GC_CHECK(stbtt_ApplyGSUB(&f, -1, GC_DFLT,0, feats,1, gg,&cnt,2) == 0);
      cnt=3;
      GC_CHECK(stbtt_ApplyGSUB(&f, b.n, GC_DFLT,0, feats,1, gg,&cnt,2) == 0);
      cnt=2;
      GC_CHECK(stbtt_ApplyGSUB(&f, b.n, GC_DFLT,0, NULL,1, gg,&cnt,2) == 0);
      cnt=1;
      GC_CHECK(stbtt_ApplyGSUB(&f, b.n, GC_DFLT,0, feats,1, NULL,&cnt,1) == 0);
      GC_CHECK(stbtt_ApplyGSUB(&f, b.n, GC_DFLT,0, feats,-1, gg,&cnt,1) == 0);
      /* empty input with NULL glyphs and zero capacity is a valid no-op */
      cnt=0;
      GC_CHECK(stbtt_ApplyGSUB(&f, b.n, GC_DFLT,0, NULL,0, NULL,&cnt,0) == 1);
      GC_CHECK(cnt == 0);
   }

   /* ---- capacity shortfall on expansion: failure, input untouched ---- */
   {
      gc_bytes be; stbtt_fontinfo fe;
      static const unsigned short in[]={1,2,3,5};
      gc_context_font(&be, 1);
      gc_open(&be, &fe);
      memcpy(g, in, sizeof(in));
      memcpy(backup, in, sizeof(in));
      n = gc_apply(&fe, be.n, GC_DFLT, 0, feats, 1, g, 4, 4); /* grows to 5 */
      GC_CHECK(n == -1);
      GC_CHECK(gc_seq_same(g, backup, 4));
   }

   /* ---- a zero-length Multiple sequence is rejected, not treated as a delete ---- */
   {
      gc_bytes bz, gsz, gdz;
      gc_lookup_b Lz[1]; gc_feat_spec ftz[1]; gc_script_spec scz[1];
      static const unsigned short cA[]={1};
      static const unsigned short *empty_seq[1]={0};
      static const int empty_len[1]={0};
      memset(Lz,0,sizeof(Lz));
      Lz[0].type=2; Lz[0].flags=0; Lz[0].nsub=1;
      gc_sub_multiple(&Lz[0].sub[0], cA, 1, empty_seq, empty_len, 1);
      memset(ftz,0,sizeof(ftz));
      ftz[0].tag=GC_LIGA; ftz[0].nlook=1; ftz[0].look[0]=0;
      memset(scz,0,sizeof(scz));
      scz[0].tag=GC_DFLT; scz[0].has_default=1;
      scz[0].def.required=0xffff; scz[0].def.nfeat=1; scz[0].def.feat[0]=0;
      gc_build_gsub(&gsz, scz,1, ftz,1, Lz,1);
      gc_gdef_noclass(&gdz);
      gc_font_opt(&bz, &gsz, &gdz);
      {
         stbtt_fontinfo fz; unsigned short gg[3]={1,2,3}; unsigned short bk[3]={1,2,3};
         gc_open(&bz, &fz);
         n = gc_apply(&fz, bz.n, GC_DFLT,0, feats,1, gg,3,3);
         GC_CHECK(n == -1);
         GC_CHECK(gc_seq_same(gg, bk, 3));
      }
   }

   /* ---- truncated buffer: a table offset runs past data_size ---- */
   {
      int cnt=2; unsigned short gg[2]={1,2};
      GC_CHECK(stbtt_ApplyGSUB(&f, 40, GC_DFLT,0, feats,1, gg,&cnt,2) == 0);
      GC_CHECK(cnt == 2 && gg[0]==1 && gg[1]==2);
   }

   /* ---- corrupted GSUB lookup offset ---- */
   {
      gc_bytes bad;
      int gsub_rel, cnt=2; unsigned short gg[2]={1,2};
      gc_default_font(&bad);
      gsub_rel = (int)stbtt__find_table(bad.b, 0, "GSUB");
      GC_CHECK(gsub_rel > 0);
      /* LookupList offset (GSUB+8) pointed at offset 44 originally; smash it. */
      bad.b[gsub_rel+8] = 0xff; bad.b[gsub_rel+9] = 0xff;
      {
         stbtt_fontinfo bf;
         gc_open(&bad, &bf);
         GC_CHECK(stbtt_ApplyGSUB(&bf, bad.n, GC_DFLT,0, feats,1, gg,&cnt,2) == 0);
         GC_CHECK(cnt == 2 && gg[0]==1 && gg[1]==2);
      }
   }

   /* ---- recursive chain beyond depth 32 fails ---- */
   {
      gc_bytes br, gs, gd;
      gc_lookup_b L[1]; gc_feat_spec ft[1]; gc_script_spec sc[1];
      static const unsigned short cA[]={1};
      static const gc_record rec[1]={{0,0}}; /* points back at itself */
      memset(L,0,sizeof(L));
      L[0].type=6; L[0].flags=0; L[0].nsub=1;
      gc_sub_chain3(&L[0].sub[0], 0,0, cA,1, 0,0, rec,1);
      memset(ft,0,sizeof(ft));
      ft[0].tag=GC_LIGA; ft[0].nlook=1; ft[0].look[0]=0;
      memset(sc,0,sizeof(sc));
      sc[0].tag=GC_DFLT; sc[0].has_default=1;
      sc[0].def.required=0xffff; sc[0].def.nfeat=1; sc[0].def.feat[0]=0;
      gc_build_gsub(&gs, sc,1, ft,1, L,1);
      gc_gdef_noclass(&gd);
      gc_font_opt(&br, &gs, &gd);
      {
         stbtt_fontinfo fr; unsigned short gg[2]={1,5}; unsigned short bk[2]={1,5};
         gc_open(&br, &fr);
         n = gc_apply(&fr, br.n, GC_DFLT,0, feats,1, gg,2,2);
         GC_CHECK(n == -1);
         GC_CHECK(gc_seq_same(gg, bk, 2));
      }
   }

   /* ---- absent / version-skew tables are tolerated ---- */
   {
      gc_bytes no_gsub, g2, b_gdef2, b_nogdef, b_gsub2;
      stbtt_fontinfo f2;
      int cnt=2; unsigned short gg[2]={1,2};
      gc_gdef_noclass(&g2);
      gc_font_opt(&no_gsub, NULL, &g2);              /* no GSUB at all */
      gc_open(&no_gsub, &f2);
      GC_CHECK(stbtt_ApplyGSUB(&f2, no_gsub.n, GC_DFLT,0, feats,1, gg,&cnt,2) == 1);
      GC_CHECK(cnt==2 && gg[0]==1 && gg[1]==2);

      gc_gdef_v2(&g2);
      gc_default_font(&b_gdef2);                     /* default GSUB, GDEF v2 */
      /* rebuild with the v2 GDEF */
      {
         gc_bytes gs; gc_base_gsub(&gs);
         gc_font_opt(&b_gdef2, &gs, &g2);
      }
      gc_open(&b_gdef2, &f2);
      cnt=2; gg[0]=1; gg[1]=2;
      GC_CHECK(stbtt_ApplyGSUB(&f2, b_gdef2.n, GC_DFLT,0, feats,1, gg,&cnt,2) == 1);
      GC_CHECK(cnt==1 && gg[0]==3);                  /* class-0 fallback still ligates */

      {
         gc_bytes gs; gc_base_gsub(&gs);
         gc_font_opt(&b_nogdef, &gs, NULL);          /* no GDEF at all */
      }
      gc_open(&b_nogdef, &f2);
      cnt=2; gg[0]=1; gg[1]=2;
      GC_CHECK(stbtt_ApplyGSUB(&f2, b_nogdef.n, GC_DFLT,0, feats,1, gg,&cnt,2) == 1);
      GC_CHECK(cnt==1 && gg[0]==3);

      /* GSUB 2.0 is outside the supported subset: succeed and keep input. */
      {
         gc_bytes gs2, gd2;
         gc_base_gsub(&gs2);
         gc_patch32(&gs2, 0, 0x00020000u);
         gc_gdef_noclass(&gd2);
         gc_font_opt(&b_gsub2, &gs2, &gd2);
      }
      gc_open(&b_gsub2, &f2);
      cnt=2; gg[0]=1; gg[1]=2;
      GC_CHECK(stbtt_ApplyGSUB(&f2, b_gsub2.n, GC_DFLT,0, feats,1, gg,&cnt,2) == 1);
      GC_CHECK(cnt==2 && gg[0]==1 && gg[1]==2);
   }

   /* ---- an unsupported coverage format in a selected lookup fails hard ---- */
   {
      gc_bytes bc, gsc, gdc;
      gc_lookup_b Lc[1]; gc_feat_spec ftc[1]; gc_script_spec scc[1];
      static const unsigned short c1c[]={1}, o7c[]={7};
      memset(Lc,0,sizeof(Lc));
      Lc[0].type=1; Lc[0].flags=0; Lc[0].nsub=1;
      gc_sub_single2(&Lc[0].sub[0], c1c, o7c, 1);
      memset(ftc,0,sizeof(ftc));
      ftc[0].tag=GC_LIGA; ftc[0].nlook=1; ftc[0].look[0]=0;
      memset(scc,0,sizeof(scc));
      scc[0].tag=GC_DFLT; scc[0].has_default=1;
      scc[0].def.required=0xffff; scc[0].def.nfeat=1; scc[0].def.feat[0]=0;
      gc_build_gsub(&gsc, scc,1, ftc,1, Lc,1);
      gc_gdef_noclass(&gdc);
      gc_font_opt(&bc, &gsc, &gdc);
      /* Smash the coverage table's format field to an unsupported value. In a
         Single fmt2 subtable the first u16 after the header is the coverage
         offset, and the coverage format is its first u16. */
      {
         int go=-1, q;
         for(q=0;q+16<bc.n;q++) if(bc.b[q]=='G'&&bc.b[q+1]=='S'&&bc.b[q+2]=='U'&&bc.b[q+3]=='B'){
            go=(bc.b[q+8]<<24)|(bc.b[q+9]<<16)|(bc.b[q+10]<<8)|bc.b[q+11];break;}
         GC_CHECK(go >= 0);
         {
            int ll=(bc.b[go+8]<<8)|bc.b[go+9];
            int lo=(bc.b[go+ll+2]<<8)|bc.b[go+ll+3];
            int so=(bc.b[go+ll+lo+6]<<8)|bc.b[go+ll+lo+7];
            int cov=(bc.b[go+ll+lo+so+2]<<8)|bc.b[go+ll+lo+so+3];
            bc.b[go+ll+lo+so+cov+1]=9; /* coverage format 9: unsupported */
         }
      }
      {
         stbtt_fontinfo fc; unsigned short gg[2]={1,2}; unsigned short bk[2]={1,2};
         int cnt2=2;
         gc_open(&bc, &fc);
         GC_CHECK(stbtt_ApplyGSUB(&fc, bc.n, GC_DFLT,0, feats,1, gg,&cnt2,2) == 0);
         GC_CHECK(gc_seq_same(gg, bk, 2));
      }
   }

   /* ---- slots at/after the new count are left exactly as supplied ---- */
   {
      static const unsigned short in[]={1,2, 0x77,0x77,0x77};
      memcpy(g, in, sizeof(in));
      n = gc_apply(&f, b.n, GC_DFLT,0, feats,1, g, 2, 5);
      GC_CHECK(n == 1 && g[0] == 3);
      /* new count is 1, so indices 1..4 keep their original contents */
      GC_CHECK(g[1]==2 && g[2]==0x77 && g[3]==0x77 && g[4]==0x77);
   }

   /* ---- legacy single-glyph and data-structure entry points unchanged ---- */
   {
      int adv, lsb, w,h,x,y;
      unsigned char *bm;
      GC_CHECK(stbtt_FindGlyphIndex(&f, 'f') == 1);
      GC_CHECK(stbtt_FindGlyphIndex(&f, 'i') == 2);
      stbtt_GetGlyphHMetrics(&f, 3, &adv, &lsb);
      GC_CHECK(adv == 500);              /* fixture hmtx advance = width + 50 */
      bm = stbtt_GetGlyphBitmap(&f, 0.04f, 0.04f, 3, &w,&h,&x,&y);
      GC_CHECK(bm && w>0 && h>0);
      stbtt_FreeBitmap(bm, NULL);
      GC_CHECK(stbtt_GetGlyphSDF(&f, 0.04f, 1, 1, 128, 8.0f, &w,&h,&x,&y) != NULL);
   }

   GC_SUITE_DONE();
   GC_GROUP_DONE();
   return gc_checks - group_start;
}

int unit_run_group(const char *name)
{
   if (strcmp(name, "lookup_plan") == 0)       return gc_group_lookup_plan();
   if (strcmp(name, "context_edits") == 0)    return gc_group_context_edits();
   if (strcmp(name, "filter_and_render") == 0) return gc_group_filter_and_render();
   if (strcmp(name, "errors_and_legacy") == 0) return gc_group_errors_and_legacy();
   return 0;
}
