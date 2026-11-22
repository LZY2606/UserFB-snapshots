//--------------------------------------------//
// SFOTool - SF2 subset/repack                //
// License: Public Domain (www.unlicense.org) //
//--------------------------------------------//
//
// Core logic for "sfotool --subset in.sf2 out.sf2 bank:preset[,bank:preset...]".
//
// The file is processed on the original hydra records (phdr/pbag/pgen/pmod,
// inst/ibag/igen/imod, shdr), not on any flattened playback representation:
//   - selected presets keep all of their global and local zones, in the
//     original phdr order; duplicate selections are merged,
//   - the instrument/sample closure is taken along instrument (genOper 41)
//     and sampleID (genOper 53) references; unreachable instruments and
//     samples are dropped,
//   - a kept left/right sample drags its sampleLink partner along even if no
//     zone references that partner directly, and sampleLink is remapped,
//   - every kept sample contributes its [start,end) window plus the 46 guard
//     sample points right after end; intervals that overlap or touch in
//     source coordinates are merged so shared PCM is copied only once, and
//     start/end/startLoop/endLoop are translated with the interval they fall
//     into (relative meaning of loop/address generators is preserved),
//   - bag boundaries, terminal records (EOP/EOI/EOS and the trailing
//     bag/mod/gen records) are closed again and reference ids are rewritten
//     through the old->new maps; all other generator/modulator bytes are
//     copied verbatim,
//   - INFO and any other chunks that are neither hydra nor smpl are copied
//     verbatim (payload and pad), RIFF/LIST sizes and odd pads are
//     recomputed.
//
// Only plain 16-bit PCM SF2 files are supported: smpo (Ogg), sm24 and ROM
// samples are rejected. Invalid references, out-of-order bag indices, broken
// sample links or samples reaching past the smpl chunk are rejected as well;
// in any error case the output file is not created or modified.

#include "sf2subset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define SFO_PHDR_SIZE 38
#define SFO_PBAG_SIZE 4
#define SFO_PMOD_SIZE 10
#define SFO_PGEN_SIZE 4
#define SFO_INST_SIZE 22
#define SFO_IBAG_SIZE 4
#define SFO_IMOD_SIZE 10
#define SFO_IGEN_SIZE 4
#define SFO_SHDR_SIZE 46

#define SFO_GUARD 46 /* guard sample points copied after each kept sample */

#define SFO_GEN_INSTRUMENT 41
#define SFO_GEN_SAMPLEID   53

static u16 sfo_rd16(const u8* p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 sfo_rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void sfo_wr16(u8* p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void sfo_wr32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static int sfo_id_eq(const u8* p, const char* id) { return p[0] == (u8)id[0] && p[1] == (u8)id[1] && p[2] == (u8)id[2] && p[3] == (u8)id[3]; }

/* ------------------------------------------------------------------ */
/* growable output buffer                                             */
/* ------------------------------------------------------------------ */

struct sfo_obuf { u8* data; size_t size, cap; };

static int sfo_obuf_put(struct sfo_obuf* o, const void* p, size_t n)
{
    if (o->size + n < o->size) return 0;
    if (o->size + n > o->cap)
    {
        size_t cap = o->cap ? o->cap : 4096;
        u8* nd;
        while (cap < o->size + n)
        {
            if (cap > ((size_t)-1) / 2) { cap = o->size + n; break; }
            cap *= 2;
        }
        nd = (u8*)realloc(o->data, cap);
        if (!nd) return 0;
        o->data = nd;
        o->cap = cap;
    }
    memcpy(o->data + o->size, p, n);
    o->size += n;
    return 1;
}

static int sfo_obuf_u8(struct sfo_obuf* o, u8 v) { return sfo_obuf_put(o, &v, 1); }

static int sfo_obuf_u32(struct sfo_obuf* o, u32 v) { u8 b[4]; sfo_wr32(b, v); return sfo_obuf_put(o, b, 4); }

/* begin a RIFF chunk: writes id and a placeholder size, returns the size field offset */
static int sfo_obuf_chunk_begin(struct sfo_obuf* o, const char* id, size_t* size_pos)
{
    if (!sfo_obuf_put(o, id, 4)) return 0;
    *size_pos = o->size;
    return sfo_obuf_u32(o, 0);
}

/* patch the chunk size and append the odd pad byte */
static int sfo_obuf_chunk_end(struct sfo_obuf* o, size_t size_pos)
{
    size_t sz = o->size - size_pos - 4;
    if (sz > 0xFFFFFFFFu) return 0;
    sfo_wr32(o->data + size_pos, (u32)sz);
    if (sz & 1) return sfo_obuf_u8(o, 0);
    return 1;
}

/* ------------------------------------------------------------------ */
/* preset selection list                                              */
/* ------------------------------------------------------------------ */

struct sfo_sel { u16 bank, preset; };

static int sfo_parse_selection(const char* s, struct sfo_sel** out, int* outn, char* err, size_t errsz)
{
    struct sfo_sel* list = NULL;
    int n = 0, cap = 0;
    const char* p = s;
    if (!s || !*s)
    {
        snprintf(err, errsz, "preset list is empty");
        return -1;
    }
    for (;;)
    {
        unsigned long bank, preset;
        struct sfo_sel* nl;
        if (*p < '0' || *p > '9') { snprintf(err, errsz, "expected bank number in preset list"); goto fail; }
        bank = 0;
        while (*p >= '0' && *p <= '9')
        {
            bank = bank * 10 + (unsigned long)(*p - '0');
            if (bank > 65535) { snprintf(err, errsz, "bank number out of range in preset list"); goto fail; }
            p++;
        }
        if (*p != ':') { snprintf(err, errsz, "expected ':' in preset list"); goto fail; }
        p++;
        if (*p < '0' || *p > '9') { snprintf(err, errsz, "expected preset number in preset list"); goto fail; }
        preset = 0;
        while (*p >= '0' && *p <= '9')
        {
            preset = preset * 10 + (unsigned long)(*p - '0');
            if (preset > 65535) { snprintf(err, errsz, "preset number out of range in preset list"); goto fail; }
            p++;
        }
        if (n == cap)
        {
            cap = cap ? cap * 2 : 8;
            nl = (struct sfo_sel*)realloc(list, (size_t)cap * sizeof(struct sfo_sel));
            if (!nl) { snprintf(err, errsz, "out of memory"); goto fail; }
            list = nl;
        }
        list[n].bank = (u16)bank;
        list[n].preset = (u16)preset;
        n++;
        if (!*p) break;
        if (*p != ',') { snprintf(err, errsz, "expected ',' in preset list"); goto fail; }
        p++;
    }
    *out = list;
    *outn = n;
    return 0;
fail:
    free(list);
    return -1;
}

/* ------------------------------------------------------------------ */
/* chunk walking                                                      */
/* ------------------------------------------------------------------ */

struct sfo_chunk { size_t off; u32 size; }; /* off = offset of chunk header, payload at off+8 */

/* returns 1 if a chunk was read, 0 when the parent range is exhausted, -1 on malformed data */
static int sfo_chunk_next(const u8* src, size_t end, size_t* poff, struct sfo_chunk* c)
{
    size_t off = *poff;
    if (off == end) return 0;
    if (end - off < 8) return -1;
    c->off = off;
    c->size = sfo_rd32(src + off + 4);
    if ((size_t)c->size > end - off - 8) return -1;
    off += 8 + (size_t)c->size + (c->size & 1);
    if (off > end) return -1;
    *poff = off;
    return 1;
}

static size_t sfo_chunk_total(const struct sfo_chunk* c) { return 8 + (size_t)c->size + (c->size & 1); }

/* an item in the emission sequence of a chunk list */
struct sfo_item { int kind; size_t off, len; }; /* kind >= 0: rebuilt table, -1: verbatim copy from src */

/* hydra table ids */
enum { SFO_T_PHDR, SFO_T_PBAG, SFO_T_PMOD, SFO_T_PGEN, SFO_T_INST, SFO_T_IBAG, SFO_T_IMOD, SFO_T_IGEN, SFO_T_SHDR, SFO_T_COUNT };
static const char* const sfo_tab_names[SFO_T_COUNT] = { "phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr" };
static const u32 sfo_tab_recsizes[SFO_T_COUNT] = { SFO_PHDR_SIZE, SFO_PBAG_SIZE, SFO_PMOD_SIZE, SFO_PGEN_SIZE, SFO_INST_SIZE, SFO_IBAG_SIZE, SFO_IMOD_SIZE, SFO_IGEN_SIZE, SFO_SHDR_SIZE };

/* record field accessors on raw hydra records */
#define SFO_PHDR_PRESET(r) sfo_rd16((r) + 20)
#define SFO_PHDR_BANK(r)   sfo_rd16((r) + 22)
#define SFO_PHDR_BAG(r)    sfo_rd16((r) + 24)
#define SFO_BAG_GEN(r)     sfo_rd16((r) + 0)
#define SFO_BAG_MOD(r)     sfo_rd16((r) + 2)
#define SFO_GEN_OPER(r)    sfo_rd16((r) + 0)
#define SFO_GEN_AMOUNT(r)  sfo_rd16((r) + 2)
#define SFO_INST_BAG(r)    sfo_rd16((r) + 20)
#define SFO_SHDR_START(r)     sfo_rd32((r) + 20)
#define SFO_SHDR_END(r)       sfo_rd32((r) + 24)
#define SFO_SHDR_LOOPSTART(r) sfo_rd32((r) + 28)
#define SFO_SHDR_LOOPEND(r)   sfo_rd32((r) + 32)
#define SFO_SHDR_LINK(r)      sfo_rd16((r) + 42)
#define SFO_SHDR_TYPE(r)      sfo_rd16((r) + 44)

struct sfo_interval { u32 src, end, dst; };

static int sfo_interval_cmp(const void* a, const void* b)
{
    u32 x = ((const struct sfo_interval*)a)->src, y = ((const struct sfo_interval*)b)->src;
    return (x > y) - (x < y);
}

/* map a source sample offset through the merged intervals to its new offset */
static int sfo_map_point(const struct sfo_interval* iv, int n, u32 p, u32* out)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (p < iv[mid].src) hi = mid - 1;
        else if (p >= iv[mid].end) lo = mid + 1;
        else { *out = iv[mid].dst + (p - iv[mid].src); return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main entry                                                         */
/* ------------------------------------------------------------------ */

int sfo_subset_file(const char* in_path, const char* out_path, const char* preset_list, char* errmsg, size_t errmsg_size)
{
    char errbuf[256];
    char* err = (errmsg && errmsg_size) ? errmsg : errbuf;
    size_t errsz = (errmsg && errmsg_size) ? errmsg_size : sizeof(errbuf);
    u8* src = NULL;
    size_t srcsize = 0;
    struct sfo_sel* sel = NULL;
    int seln = 0;
    struct sfo_item* top_items = NULL; int ntop = 0, captop = 0;
    struct sfo_item* sdta_items = NULL; int nsdta = 0, capsdta = 0;
    struct sfo_item* pdta_items = NULL; int npdta = 0, cappdta = 0;
    const u8** rec[SFO_T_COUNT];
    int nrec[SFO_T_COUNT];
    u8* keep[SFO_T_COUNT];
    int* map_inst = NULL;
    int* map_shdr = NULL;
    struct sfo_interval* iv = NULL;
    struct sfo_obuf tables[SFO_T_COUNT];
    struct sfo_obuf pool = { NULL, 0, 0 };
    struct sfo_obuf out = { NULL, 0, 0 };
    size_t smpl_payload = 0;
    u32 smpl_size = 0, smpl_count, pool_size = 0;
    int have_sdta = 0, have_pdta = 0, have_smpl = 0, niv = 0;
    size_t sdta_off = 0, pdta_off = 0;
    u32 sdta_size = 0, pdta_size = 0;
    int i, j, g, m, t, r, result = 1;
    u32 run;

#define SFO_ERR(...) do { snprintf(err, errsz, __VA_ARGS__); goto fail; } while (0)
#define SFO_ALLOC(p, n) do { (p) = calloc((size_t)(n) ? (size_t)(n) : 1, 1); if (!(p)) SFO_ERR("out of memory"); } while (0)
#define SFO_OBUF(o, ...) do { if (!(__VA_ARGS__)) SFO_ERR("out of memory"); } while (0)

    for (t = 0; t < SFO_T_COUNT; t++) { rec[t] = NULL; nrec[t] = 0; keep[t] = NULL; tables[t].data = NULL; tables[t].size = tables[t].cap = 0; }

    /* ---- read the whole input file ---- */
    {
        FILE* f = fopen(in_path, "rb");
        long sz;
        if (!f) { snprintf(err, errsz, "unable to open input file '%s'", in_path); return 1; }
        if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
        {
            fclose(f);
            snprintf(err, errsz, "unable to read input file '%s'", in_path);
            return 1;
        }
        srcsize = (size_t)sz;
        src = (u8*)malloc(srcsize ? srcsize : 1);
        if (!src) { fclose(f); snprintf(err, errsz, "out of memory"); return 1; }
        if (srcsize && fread(src, 1, srcsize, f) != srcsize)
        {
            fclose(f);
            snprintf(err, errsz, "unable to read input file '%s'", in_path);
            goto fail;
        }
        fclose(f);
    }

    /* ---- parse the preset selection ---- */
    if (sfo_parse_selection(preset_list, &sel, &seln, err, errsz)) goto fail;

    /* ---- walk the RIFF/sfbk container ---- */
    {
        size_t top_end, off;
        struct sfo_chunk c;
        if (srcsize < 12 || !sfo_id_eq(src, "RIFF") || !sfo_id_eq(src + 8, "sfbk"))
            SFO_ERR("input file '%s' is not a RIFF/sfbk soundfont", in_path);
        {
            u32 riff_size = sfo_rd32(src + 4);
            if (riff_size < 4 || (size_t)riff_size + 8 > srcsize)
                SFO_ERR("input file '%s' has a truncated RIFF chunk", in_path);
            top_end = 8 + (size_t)riff_size;
        }
        off = 12;
        while ((r = sfo_chunk_next(src, top_end, &off, &c)) == 1)
        {
            const u8* id = src + c.off;
            int kind = -1;
            if (sfo_id_eq(id, "LIST") && c.size >= 4 && sfo_id_eq(src + c.off + 8, "sdta"))
            {
                if (have_sdta) SFO_ERR("duplicate sdta list in '%s'", in_path);
                have_sdta = 1; sdta_off = c.off; sdta_size = c.size; kind = 0;
            }
            else if (sfo_id_eq(id, "LIST") && c.size >= 4 && sfo_id_eq(src + c.off + 8, "pdta"))
            {
                if (have_pdta) SFO_ERR("duplicate pdta list in '%s'", in_path);
                have_pdta = 1; pdta_off = c.off; pdta_size = c.size; kind = 1;
            }
            if (ntop == captop)
            {
                struct sfo_item* ni;
                captop = captop ? captop * 2 : 8;
                ni = (struct sfo_item*)realloc(top_items, (size_t)captop * sizeof(struct sfo_item));
                if (!ni) SFO_ERR("out of memory");
                top_items = ni;
            }
            top_items[ntop].kind = kind;
            top_items[ntop].off = c.off;
            top_items[ntop].len = sfo_chunk_total(&c);
            ntop++;
        }
        if (r < 0) SFO_ERR("malformed chunk structure in '%s'", in_path);
        if (!have_sdta || !have_pdta) SFO_ERR("input file '%s' has no sdta or pdta list", in_path);
    }

    /* ---- walk sdta ---- */
    {
        size_t off = sdta_off + 12, end = sdta_off + 8 + (size_t)sdta_size;
        struct sfo_chunk c;
        while ((r = sfo_chunk_next(src, end, &off, &c)) == 1)
        {
            const u8* id = src + c.off;
            int kind = -1;
            if (sfo_id_eq(id, "smpl"))
            {
                if (have_smpl) SFO_ERR("duplicate smpl chunk in '%s'", in_path);
                have_smpl = 1; smpl_payload = c.off + 8; smpl_size = c.size; kind = 0;
            }
            else if (sfo_id_eq(id, "smpo")) SFO_ERR("'%s' contains a compressed (smpo) sample stream, only plain 16-bit PCM SF2 is supported", in_path);
            else if (sfo_id_eq(id, "sm24")) SFO_ERR("'%s' contains 24-bit (sm24) sample data, only plain 16-bit PCM SF2 is supported", in_path);
            if (nsdta == capsdta)
            {
                struct sfo_item* ni;
                capsdta = capsdta ? capsdta * 2 : 4;
                ni = (struct sfo_item*)realloc(sdta_items, (size_t)capsdta * sizeof(struct sfo_item));
                if (!ni) SFO_ERR("out of memory");
                sdta_items = ni;
            }
            sdta_items[nsdta].kind = kind;
            sdta_items[nsdta].off = c.off;
            sdta_items[nsdta].len = sfo_chunk_total(&c);
            nsdta++;
        }
        if (r < 0) SFO_ERR("malformed sdta list in '%s'", in_path);
        if (!have_smpl) SFO_ERR("input file '%s' has no smpl chunk", in_path);
        if (smpl_size & 1) SFO_ERR("smpl chunk in '%s' has an odd size, not 16-bit PCM", in_path);
        smpl_count = smpl_size / 2;
    }

    /* ---- walk pdta and parse the hydra tables ---- */
    {
        size_t off = pdta_off + 12, end = pdta_off + 8 + (size_t)pdta_size;
        struct sfo_chunk c;
        int seen[SFO_T_COUNT];
        for (t = 0; t < SFO_T_COUNT; t++) seen[t] = 0;
        while ((r = sfo_chunk_next(src, end, &off, &c)) == 1)
        {
            const u8* id = src + c.off;
            int kind = -1;
            for (t = 0; t < SFO_T_COUNT; t++)
            {
                if (!sfo_id_eq(id, sfo_tab_names[t])) continue;
                if (seen[t]) SFO_ERR("duplicate %.4s chunk in '%s'", sfo_tab_names[t], in_path);
                if (c.size % sfo_tab_recsizes[t]) SFO_ERR("%.4s chunk in '%s' has a broken record size", sfo_tab_names[t], in_path);
                seen[t] = 1;
                nrec[t] = (int)(c.size / sfo_tab_recsizes[t]);
                kind = t;
                break;
            }
            if (npdta == cappdta)
            {
                struct sfo_item* ni;
                cappdta = cappdta ? cappdta * 2 : 16;
                ni = (struct sfo_item*)realloc(pdta_items, (size_t)cappdta * sizeof(struct sfo_item));
                if (!ni) SFO_ERR("out of memory");
                pdta_items = ni;
            }
            pdta_items[npdta].kind = kind;
            pdta_items[npdta].off = c.off;
            pdta_items[npdta].len = sfo_chunk_total(&c);
            npdta++;
            if (kind >= 0)
            {
                int k;
                SFO_ALLOC(rec[kind], (size_t)nrec[kind] * sizeof(const u8*));
                for (k = 0; k < nrec[kind]; k++)
                    rec[kind][k] = src + c.off + 8 + (size_t)k * sfo_tab_recsizes[kind];
            }
        }
        if (r < 0) SFO_ERR("malformed pdta list in '%s'", in_path);
        for (t = 0; t < SFO_T_COUNT; t++)
        {
            if (!seen[t]) SFO_ERR("pdta list in '%s' is missing the %.4s chunk", in_path, sfo_tab_names[t]);
            if (!nrec[t]) SFO_ERR("%.4s chunk in '%s' is empty", sfo_tab_names[t], in_path);
        }
    }

    /* ---- validate bag index structure ---- */
    {
        int n_phdr = nrec[SFO_T_PHDR], n_pbag = nrec[SFO_T_PBAG], n_pmod = nrec[SFO_T_PMOD], n_pgen = nrec[SFO_T_PGEN];
        int n_inst = nrec[SFO_T_INST], n_ibag = nrec[SFO_T_IBAG], n_imod = nrec[SFO_T_IMOD], n_igen = nrec[SFO_T_IGEN];
        u32 prev;
        prev = 0;
        for (i = 0; i < n_phdr; i++)
        {
            u32 b = SFO_PHDR_BAG(rec[SFO_T_PHDR][i]);
            if (b >= (u32)n_pbag || (i && b < prev)) SFO_ERR("phdr bag indices in '%s' are out of order", in_path);
            prev = b;
        }
        prev = 0;
        for (j = 0; j < n_pbag; j++)
        {
            u32 gg = SFO_BAG_GEN(rec[SFO_T_PBAG][j]);
            if (gg >= (u32)n_pgen || (j && gg < prev)) SFO_ERR("pbag generator indices in '%s' are out of order", in_path);
            prev = gg;
        }
        prev = 0;
        for (j = 0; j < n_pbag; j++)
        {
            u32 mm = SFO_BAG_MOD(rec[SFO_T_PBAG][j]);
            if (mm >= (u32)n_pmod || (j && mm < prev)) SFO_ERR("pbag modulator indices in '%s' are out of order", in_path);
            prev = mm;
        }
        prev = 0;
        for (i = 0; i < n_inst; i++)
        {
            u32 b = SFO_INST_BAG(rec[SFO_T_INST][i]);
            if (b >= (u32)n_ibag || (i && b < prev)) SFO_ERR("inst bag indices in '%s' are out of order", in_path);
            prev = b;
        }
        prev = 0;
        for (j = 0; j < n_ibag; j++)
        {
            u32 gg = SFO_BAG_GEN(rec[SFO_T_IBAG][j]);
            if (gg >= (u32)n_igen || (j && gg < prev)) SFO_ERR("ibag generator indices in '%s' are out of order", in_path);
            prev = gg;
        }
        prev = 0;
        for (j = 0; j < n_ibag; j++)
        {
            u32 mm = SFO_BAG_MOD(rec[SFO_T_IBAG][j]);
            if (mm >= (u32)n_imod || (j && mm < prev)) SFO_ERR("ibag modulator indices in '%s' are out of order", in_path);
            prev = mm;
        }
    }

    /* ---- validate instrument/sample references ---- */
    for (g = 0; g < nrec[SFO_T_PGEN] - 1; g++)
    {
        const u8* p = rec[SFO_T_PGEN][g];
        if (SFO_GEN_OPER(p) == SFO_GEN_INSTRUMENT && SFO_GEN_AMOUNT(p) >= (u32)nrec[SFO_T_INST] - 1)
            SFO_ERR("pgen record %d in '%s' references an invalid instrument", g, in_path);
    }
    for (g = 0; g < nrec[SFO_T_IGEN] - 1; g++)
    {
        const u8* p = rec[SFO_T_IGEN][g];
        if (SFO_GEN_OPER(p) == SFO_GEN_SAMPLEID && SFO_GEN_AMOUNT(p) >= (u32)nrec[SFO_T_SHDR] - 1)
            SFO_ERR("igen record %d in '%s' references an invalid sample", g, in_path);
    }

    /* ---- validate sample headers ---- */
    for (i = 0; i < nrec[SFO_T_SHDR]; i++)
    {
        const u8* p = rec[SFO_T_SHDR][i];
        u32 start = SFO_SHDR_START(p), end = SFO_SHDR_END(p);
        u32 ls = SFO_SHDR_LOOPSTART(p), le = SFO_SHDR_LOOPEND(p);
        u16 type = SFO_SHDR_TYPE(p), link = SFO_SHDR_LINK(p);
        int terminal = (i == nrec[SFO_T_SHDR] - 1);
        if (start > ls || ls > le || le > end || end > smpl_count)
            SFO_ERR("sample '%.20s' in '%s' reaches past the smpl chunk", p, in_path);
        if (terminal) continue;
        if (type & 0x8000)
            SFO_ERR("sample '%.20s' in '%s' is a ROM sample, which is not supported", p, in_path);
        if (type != 1 && type != 2 && type != 4)
            SFO_ERR("sample '%.20s' in '%s' has an unsupported sample type %u", p, in_path, (unsigned)type);
        if (type == 1)
        {
            if (link) SFO_ERR("mono sample '%.20s' in '%s' has an invalid sample link", p, in_path);
        }
        else
        {
            const u8* q;
            u16 want = (type == 4 ? 2 : 4);
            if (link >= (u32)nrec[SFO_T_SHDR] - 1)
                SFO_ERR("sample '%.20s' in '%s' has an invalid sample link", p, in_path);
            q = rec[SFO_T_SHDR][link];
            if (SFO_SHDR_TYPE(q) != want || SFO_SHDR_LINK(q) != (u16)i)
                SFO_ERR("sample '%.20s' in '%s' has a broken sample link", p, in_path);
        }
    }

    /* ---- closure over the hydra graph ---- */
    for (t = 0; t < SFO_T_COUNT; t++)
        SFO_ALLOC(keep[t], (size_t)nrec[t]);

    /* select presets by identity (bank/preset), keeping phdr order */
    for (i = 0; i < seln; i++)
    {
        int matched = 0;
        for (j = 0; j < nrec[SFO_T_PHDR] - 1; j++)
        {
            const u8* p = rec[SFO_T_PHDR][j];
            if (SFO_PHDR_BANK(p) == sel[i].bank && SFO_PHDR_PRESET(p) == sel[i].preset)
            {
                keep[SFO_T_PHDR][j] = 1;
                matched = 1;
            }
        }
        if (!matched) SFO_ERR("preset %u:%u not found in '%s'", (unsigned)sel[i].bank, (unsigned)sel[i].preset, in_path);
    }

    /* preset -> bags -> gens/mods -> instruments */
    for (i = 0; i < nrec[SFO_T_PHDR] - 1; i++)
    {
        if (!keep[SFO_T_PHDR][i]) continue;
        for (j = SFO_PHDR_BAG(rec[SFO_T_PHDR][i]); j < (int)SFO_PHDR_BAG(rec[SFO_T_PHDR][i + 1]); j++)
            keep[SFO_T_PBAG][j] = 1;
    }
    for (j = 0; j < nrec[SFO_T_PBAG] - 1; j++)
    {
        if (!keep[SFO_T_PBAG][j]) continue;
        for (g = SFO_BAG_GEN(rec[SFO_T_PBAG][j]); g < (int)SFO_BAG_GEN(rec[SFO_T_PBAG][j + 1]); g++)
            keep[SFO_T_PGEN][g] = 1;
        for (m = SFO_BAG_MOD(rec[SFO_T_PBAG][j]); m < (int)SFO_BAG_MOD(rec[SFO_T_PBAG][j + 1]); m++)
            keep[SFO_T_PMOD][m] = 1;
    }
    for (g = 0; g < nrec[SFO_T_PGEN] - 1; g++)
    {
        const u8* p = rec[SFO_T_PGEN][g];
        if (keep[SFO_T_PGEN][g] && SFO_GEN_OPER(p) == SFO_GEN_INSTRUMENT)
            keep[SFO_T_INST][SFO_GEN_AMOUNT(p)] = 1;
    }

    /* instruments -> bags -> gens/mods -> samples */
    for (i = 0; i < nrec[SFO_T_INST] - 1; i++)
    {
        if (!keep[SFO_T_INST][i]) continue;
        for (j = SFO_INST_BAG(rec[SFO_T_INST][i]); j < (int)SFO_INST_BAG(rec[SFO_T_INST][i + 1]); j++)
            keep[SFO_T_IBAG][j] = 1;
    }
    for (j = 0; j < nrec[SFO_T_IBAG] - 1; j++)
    {
        if (!keep[SFO_T_IBAG][j]) continue;
        for (g = SFO_BAG_GEN(rec[SFO_T_IBAG][j]); g < (int)SFO_BAG_GEN(rec[SFO_T_IBAG][j + 1]); g++)
            keep[SFO_T_IGEN][g] = 1;
        for (m = SFO_BAG_MOD(rec[SFO_T_IBAG][j]); m < (int)SFO_BAG_MOD(rec[SFO_T_IBAG][j + 1]); m++)
            keep[SFO_T_IMOD][m] = 1;
    }
    for (g = 0; g < nrec[SFO_T_IGEN] - 1; g++)
    {
        const u8* p = rec[SFO_T_IGEN][g];
        if (keep[SFO_T_IGEN][g] && SFO_GEN_OPER(p) == SFO_GEN_SAMPLEID)
            keep[SFO_T_SHDR][SFO_GEN_AMOUNT(p)] = 1;
    }

    /* drag linked sample partners along */
    {
        int changed;
        do
        {
            changed = 0;
            for (i = 0; i < nrec[SFO_T_SHDR] - 1; i++)
            {
                const u8* p = rec[SFO_T_SHDR][i];
                u16 type = SFO_SHDR_TYPE(p);
                if (!keep[SFO_T_SHDR][i] || (type != 2 && type != 4)) continue;
                if (!keep[SFO_T_SHDR][SFO_SHDR_LINK(p)])
                {
                    keep[SFO_T_SHDR][SFO_SHDR_LINK(p)] = 1;
                    changed = 1;
                }
            }
        } while (changed);
    }

    /* old -> new index maps */
    SFO_ALLOC(map_inst, (size_t)nrec[SFO_T_INST] * sizeof(int));
    SFO_ALLOC(map_shdr, (size_t)nrec[SFO_T_SHDR] * sizeof(int));
    run = 0;
    for (i = 0; i < nrec[SFO_T_INST]; i++)
        map_inst[i] = (keep[SFO_T_INST][i] ? (int)run++ : -1);
    run = 0;
    for (i = 0; i < nrec[SFO_T_SHDR]; i++)
        map_shdr[i] = (keep[SFO_T_SHDR][i] ? (int)run++ : -1);

    /* ---- build the merged sample intervals ---- */
    for (i = 0; i < nrec[SFO_T_SHDR] - 1; i++)
    {
        const u8* p;
        u32 end;
        if (!keep[SFO_T_SHDR][i]) continue;
        p = rec[SFO_T_SHDR][i];
        end = SFO_SHDR_END(p);
        if (end + SFO_GUARD > smpl_count)
            SFO_ERR("sample '%.20s' in '%s' has no room for its 46 guard sample points", p, in_path);
        {
            struct sfo_interval* niv_p = (struct sfo_interval*)realloc(iv, (size_t)(niv + 1) * sizeof(struct sfo_interval));
            if (!niv_p) SFO_ERR("out of memory");
            iv = niv_p;
            iv[niv].src = SFO_SHDR_START(p);
            iv[niv].end = end + SFO_GUARD;
            iv[niv].dst = 0;
            niv++;
        }
    }
    if (!niv) SFO_ERR("preset selection in '%s' does not reference any samples", in_path);
    qsort(iv, (size_t)niv, sizeof(struct sfo_interval), sfo_interval_cmp);
    {
        int k, w = 0;
        for (k = 0; k < niv; k++)
        {
            if (w && iv[k].src <= iv[w - 1].end)
            {
                if (iv[k].end > iv[w - 1].end) iv[w - 1].end = iv[k].end;
            }
            else iv[w++] = iv[k];
        }
        niv = w;
    }
    run = 0;
    for (i = 0; i < niv; i++) { iv[i].dst = run; run += iv[i].end - iv[i].src; }
    pool_size = run;

    /* ---- copy the merged PCM intervals into the new pool ---- */
    for (i = 0; i < niv; i++)
        SFO_OBUF(pool, sfo_obuf_put(&pool, src + smpl_payload + (size_t)iv[i].src * 2, (size_t)(iv[i].end - iv[i].src) * 2));

    /* ---- rebuild the hydra tables ---- */

    /* phdr: kept presets in original order + terminal record */
    run = 0;
    for (i = 0; i < nrec[SFO_T_PHDR]; i++)
    {
        const u8* p = rec[SFO_T_PHDR][i];
        u8 tmp[SFO_PHDR_SIZE];
        int terminal = (i == nrec[SFO_T_PHDR] - 1);
        if (!terminal && !keep[SFO_T_PHDR][i]) continue;
        if (run > 0xFFFF) SFO_ERR("too many preset bags in subset");
        memcpy(tmp, p, SFO_PHDR_SIZE);
        sfo_wr16(tmp + 24, (u16)run);
        SFO_OBUF(tables[SFO_T_PHDR], sfo_obuf_put(&tables[SFO_T_PHDR], tmp, SFO_PHDR_SIZE));
        if (!terminal) run += (u32)SFO_PHDR_BAG(rec[SFO_T_PHDR][i + 1]) - SFO_PHDR_BAG(p);
    }

    /* pbag/pgen/pmod: bags of kept presets, then terminal records */
    {
        u32 run_gen = 0, run_mod = 0;
        for (j = 0; j < nrec[SFO_T_PBAG]; j++)
        {
            const u8* p = rec[SFO_T_PBAG][j];
            u8 tmp[SFO_PBAG_SIZE];
            int terminal = (j == nrec[SFO_T_PBAG] - 1);
            if (!terminal && !keep[SFO_T_PBAG][j]) continue;
            if (run_gen > 0xFFFF || run_mod > 0xFFFF) SFO_ERR("too many preset generators/modulators in subset");
            sfo_wr16(tmp + 0, (u16)run_gen);
            sfo_wr16(tmp + 2, (u16)run_mod);
            SFO_OBUF(tables[SFO_T_PBAG], sfo_obuf_put(&tables[SFO_T_PBAG], tmp, SFO_PBAG_SIZE));
            if (terminal) break;
            for (g = SFO_BAG_GEN(p); g < (int)SFO_BAG_GEN(rec[SFO_T_PBAG][j + 1]); g++)
            {
                const u8* q = rec[SFO_T_PGEN][g];
                u8 gtmp[SFO_PGEN_SIZE];
                memcpy(gtmp, q, SFO_PGEN_SIZE);
                if (SFO_GEN_OPER(q) == SFO_GEN_INSTRUMENT)
                    sfo_wr16(gtmp + 2, (u16)map_inst[SFO_GEN_AMOUNT(q)]);
                SFO_OBUF(tables[SFO_T_PGEN], sfo_obuf_put(&tables[SFO_T_PGEN], gtmp, SFO_PGEN_SIZE));
                run_gen++;
            }
            for (m = SFO_BAG_MOD(p); m < (int)SFO_BAG_MOD(rec[SFO_T_PBAG][j + 1]); m++)
            {
                SFO_OBUF(tables[SFO_T_PMOD], sfo_obuf_put(&tables[SFO_T_PMOD], rec[SFO_T_PMOD][m], SFO_PMOD_SIZE));
                run_mod++;
            }
        }
        /* terminal pgen/pmod records */
        SFO_OBUF(tables[SFO_T_PGEN], sfo_obuf_put(&tables[SFO_T_PGEN], rec[SFO_T_PGEN][nrec[SFO_T_PGEN] - 1], SFO_PGEN_SIZE));
        SFO_OBUF(tables[SFO_T_PMOD], sfo_obuf_put(&tables[SFO_T_PMOD], rec[SFO_T_PMOD][nrec[SFO_T_PMOD] - 1], SFO_PMOD_SIZE));
    }

    /* inst: kept instruments + terminal record */
    run = 0;
    for (i = 0; i < nrec[SFO_T_INST]; i++)
    {
        const u8* p = rec[SFO_T_INST][i];
        u8 tmp[SFO_INST_SIZE];
        int terminal = (i == nrec[SFO_T_INST] - 1);
        if (!terminal && !keep[SFO_T_INST][i]) continue;
        if (run > 0xFFFF) SFO_ERR("too many instrument bags in subset");
        memcpy(tmp, p, SFO_INST_SIZE);
        sfo_wr16(tmp + 20, (u16)run);
        SFO_OBUF(tables[SFO_T_INST], sfo_obuf_put(&tables[SFO_T_INST], tmp, SFO_INST_SIZE));
        if (!terminal) run += (u32)SFO_INST_BAG(rec[SFO_T_INST][i + 1]) - SFO_INST_BAG(p);
    }

    /* ibag/igen/imod: bags of kept instruments, then terminal records */
    {
        u32 run_gen = 0, run_mod = 0;
        for (j = 0; j < nrec[SFO_T_IBAG]; j++)
        {
            const u8* p = rec[SFO_T_IBAG][j];
            u8 tmp[SFO_IBAG_SIZE];
            int terminal = (j == nrec[SFO_T_IBAG] - 1);
            if (!terminal && !keep[SFO_T_IBAG][j]) continue;
            if (run_gen > 0xFFFF || run_mod > 0xFFFF) SFO_ERR("too many instrument generators/modulators in subset");
            sfo_wr16(tmp + 0, (u16)run_gen);
            sfo_wr16(tmp + 2, (u16)run_mod);
            SFO_OBUF(tables[SFO_T_IBAG], sfo_obuf_put(&tables[SFO_T_IBAG], tmp, SFO_IBAG_SIZE));
            if (terminal) break;
            for (g = SFO_BAG_GEN(p); g < (int)SFO_BAG_GEN(rec[SFO_T_IBAG][j + 1]); g++)
            {
                const u8* q = rec[SFO_T_IGEN][g];
                u8 gtmp[SFO_IGEN_SIZE];
                memcpy(gtmp, q, SFO_IGEN_SIZE);
                if (SFO_GEN_OPER(q) == SFO_GEN_SAMPLEID)
                    sfo_wr16(gtmp + 2, (u16)map_shdr[SFO_GEN_AMOUNT(q)]);
                SFO_OBUF(tables[SFO_T_IGEN], sfo_obuf_put(&tables[SFO_T_IGEN], gtmp, SFO_IGEN_SIZE));
                run_gen++;
            }
            for (m = SFO_BAG_MOD(p); m < (int)SFO_BAG_MOD(rec[SFO_T_IBAG][j + 1]); m++)
            {
                SFO_OBUF(tables[SFO_T_IMOD], sfo_obuf_put(&tables[SFO_T_IMOD], rec[SFO_T_IMOD][m], SFO_IMOD_SIZE));
                run_mod++;
            }
        }
        /* terminal igen/imod records */
        SFO_OBUF(tables[SFO_T_IGEN], sfo_obuf_put(&tables[SFO_T_IGEN], rec[SFO_T_IGEN][nrec[SFO_T_IGEN] - 1], SFO_IGEN_SIZE));
        SFO_OBUF(tables[SFO_T_IMOD], sfo_obuf_put(&tables[SFO_T_IMOD], rec[SFO_T_IMOD][nrec[SFO_T_IMOD] - 1], SFO_IMOD_SIZE));
    }

    /* shdr: kept samples with translated addresses + EOS at the new pool end */
    for (i = 0; i < nrec[SFO_T_SHDR]; i++)
    {
        const u8* p = rec[SFO_T_SHDR][i];
        u8 tmp[SFO_SHDR_SIZE];
        int terminal = (i == nrec[SFO_T_SHDR] - 1);
        u32 v;
        if (!terminal && !keep[SFO_T_SHDR][i]) continue;
        memcpy(tmp, p, SFO_SHDR_SIZE);
        if (terminal)
        {
            sfo_wr32(tmp + 20, pool_size);
            sfo_wr32(tmp + 24, pool_size);
            sfo_wr32(tmp + 28, pool_size);
            sfo_wr32(tmp + 32, pool_size);
        }
        else
        {
            if (!sfo_map_point(iv, niv, SFO_SHDR_START(p), &v)) SFO_ERR("internal error mapping sample start");
            sfo_wr32(tmp + 20, v);
            if (!sfo_map_point(iv, niv, SFO_SHDR_END(p), &v)) SFO_ERR("internal error mapping sample end");
            sfo_wr32(tmp + 24, v);
            if (!sfo_map_point(iv, niv, SFO_SHDR_LOOPSTART(p), &v)) SFO_ERR("internal error mapping loop start");
            sfo_wr32(tmp + 28, v);
            if (!sfo_map_point(iv, niv, SFO_SHDR_LOOPEND(p), &v)) SFO_ERR("internal error mapping loop end");
            sfo_wr32(tmp + 32, v);
            if (SFO_SHDR_TYPE(p) == 2 || SFO_SHDR_TYPE(p) == 4)
                sfo_wr16(tmp + 42, (u16)map_shdr[SFO_SHDR_LINK(p)]);
        }
        SFO_OBUF(tables[SFO_T_SHDR], sfo_obuf_put(&tables[SFO_T_SHDR], tmp, SFO_SHDR_SIZE));
    }

    /* ---- assemble the output file ---- */
    {
        size_t sp;
        SFO_OBUF(out, sfo_obuf_chunk_begin(&out, "RIFF", &sp));
        SFO_OBUF(out, sfo_obuf_put(&out, "sfbk", 4));
        for (i = 0; i < ntop; i++)
        {
            if (top_items[i].kind < 0)
            {
                /* INFO and other foreign chunks are copied verbatim, pad included */
                SFO_OBUF(out, sfo_obuf_put(&out, src + top_items[i].off, top_items[i].len));
            }
            else if (top_items[i].kind == 0) /* sdta */
            {
                size_t lsp;
                SFO_OBUF(out, sfo_obuf_chunk_begin(&out, "LIST", &lsp));
                SFO_OBUF(out, sfo_obuf_put(&out, "sdta", 4));
                for (j = 0; j < nsdta; j++)
                {
                    if (sdta_items[j].kind < 0)
                    {
                        SFO_OBUF(out, sfo_obuf_put(&out, src + sdta_items[j].off, sdta_items[j].len));
                    }
                    else
                    {
                        size_t csp;
                        SFO_OBUF(out, sfo_obuf_chunk_begin(&out, "smpl", &csp));
                        SFO_OBUF(out, sfo_obuf_put(&out, pool.data, pool.size));
                        SFO_OBUF(out, sfo_obuf_chunk_end(&out, csp));
                    }
                }
                SFO_OBUF(out, sfo_obuf_chunk_end(&out, lsp));
            }
            else /* pdta */
            {
                size_t lsp;
                SFO_OBUF(out, sfo_obuf_chunk_begin(&out, "LIST", &lsp));
                SFO_OBUF(out, sfo_obuf_put(&out, "pdta", 4));
                for (j = 0; j < npdta; j++)
                {
                    if (pdta_items[j].kind < 0)
                    {
                        SFO_OBUF(out, sfo_obuf_put(&out, src + pdta_items[j].off, pdta_items[j].len));
                    }
                    else
                    {
                        size_t csp;
                        int k = pdta_items[j].kind;
                        SFO_OBUF(out, sfo_obuf_chunk_begin(&out, sfo_tab_names[k], &csp));
                        SFO_OBUF(out, sfo_obuf_put(&out, tables[k].data, tables[k].size));
                        SFO_OBUF(out, sfo_obuf_chunk_end(&out, csp));
                    }
                }
                SFO_OBUF(out, sfo_obuf_chunk_end(&out, lsp));
            }
        }
        SFO_OBUF(out, sfo_obuf_chunk_end(&out, sp));
    }

    /* ---- everything built: only now touch the output file ---- */
    {
        FILE* f = fopen(out_path, "wb");
        if (!f) SFO_ERR("unable to open output file '%s'", out_path);
        if (out.size && fwrite(out.data, 1, out.size, f) != out.size)
        {
            fclose(f);
            remove(out_path);
            SFO_ERR("unable to write output file '%s'", out_path);
        }
        if (fclose(f)) { remove(out_path); SFO_ERR("unable to write output file '%s'", out_path); }
    }
    result = 0;

fail:
    free(src);
    free(sel);
    free(top_items);
    free(sdta_items);
    free(pdta_items);
    for (t = 0; t < SFO_T_COUNT; t++) { free(rec[t]); free(keep[t]); free(tables[t].data); }
    free(map_inst);
    free(map_shdr);
    free(iv);
    free(pool.data);
    free(out.data);
    return result;

#undef SFO_ERR
#undef SFO_ALLOC
#undef SFO_OBUF
}
