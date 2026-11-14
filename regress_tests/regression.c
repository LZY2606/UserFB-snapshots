/* Regression tests for Mat_VarReadSparseSlab.
 *
 * All sparse material is generated in C: matio itself writes the native
 * files, a small built-in MAT5 writer produces the reverse-endian and the
 * corrupt files.  Expected CSC results are computed independently from the
 * source slot lists as a coordinate table.  Allocation and file-read
 * accounting prove that the slab reader never pulls the whole matrix into
 * memory.
 */
#include "check.h"
#include "matio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <dlfcn.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* Allocation and read-path accounting                                 */
/*                                                                     */
/* libmatio.a is linked statically into this binary, so the malloc /   */
/* calloc / realloc / free and fread references of the library bind to */
/* the definitions below.  Each one forwards to the real libc routine  */
/* (located with dlsym(RTLD_NEXT, ...)) and only adds accounting on    */
/* top, so pointers produced inside libc (strdup, vasprintf, ...) are  */
/* still handled correctly: unknown pointers are simply forwarded to   */
/* the real free.                                                      */
/* ------------------------------------------------------------------ */

static void *(*p_malloc)(size_t);
static void *(*p_calloc)(size_t, size_t);
static void *(*p_realloc)(void *, size_t);
static void (*p_free)(void *);
static size_t (*p_fread)(void *, size_t, size_t, FILE *);
static int (*p_fseek)(FILE *, long, int);
static int (*p_fseeko)(FILE *, off_t, int);

static int trk_ready;
static int trk_in_dlsym;

/* Bootstrap arena used only while dlsym (possibly allocating) runs. */
static unsigned char trk_arena[1 << 20];
static size_t trk_arena_used;

static void *
trk_arena_alloc(size_t n)
{
    void *p;
    n = (n + 15) & ~(size_t)15;
    if ( trk_arena_used + n > sizeof(trk_arena) )
        return NULL;
    p = trk_arena + trk_arena_used;
    trk_arena_used += n;
    return p;
}

static int
trk_is_arena(const void *p)
{
    return p >= (const void *)trk_arena && p < (const void *)(trk_arena + sizeof(trk_arena));
}

static void
trk_resolve(void)
{
    if ( trk_ready || trk_in_dlsym )
        return;
    trk_in_dlsym = 1;
    p_malloc = dlsym(RTLD_NEXT, "malloc");
    p_calloc = dlsym(RTLD_NEXT, "calloc");
    p_realloc = dlsym(RTLD_NEXT, "realloc");
    p_free = dlsym(RTLD_NEXT, "free");
    p_fread = dlsym(RTLD_NEXT, "fread");
    p_fseek = dlsym(RTLD_NEXT, "fseek");
    p_fseeko = dlsym(RTLD_NEXT, "fseeko");
    if ( p_malloc && p_calloc && p_realloc && p_free && p_fread && p_fseek )
        trk_ready = 1;
    trk_in_dlsym = 0;
}

/* pointer -> size table (open addressing, static storage) */
#define TRK_SLOTS (1u << 16)
#define TRK_TOMB ((void *)1)
static void *trk_ptr[TRK_SLOTS];
static size_t trk_sz[TRK_SLOTS];

static size_t
trk_hash(const void *p)
{
    uintptr_t x = (uintptr_t)p;
    x >>= 4;
    x *= (uintptr_t)0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 16) & (TRK_SLOTS - 1);
}

static void
trk_put(void *p, size_t n)
{
    size_t s = trk_hash(p), i;
    for ( i = 0; i < TRK_SLOTS; i++ ) {
        size_t h = (s + i) & (TRK_SLOTS - 1);
        if ( trk_ptr[h] == p ) {
            trk_sz[h] = n;
            return;
        }
        if ( trk_ptr[h] == NULL || trk_ptr[h] == TRK_TOMB ) {
            trk_ptr[h] = p;
            trk_sz[h] = n;
            return;
        }
    }
}

static int
trk_del(void *p, size_t *n)
{
    size_t s = trk_hash(p), i;
    for ( i = 0; i < TRK_SLOTS; i++ ) {
        size_t h = (s + i) & (TRK_SLOTS - 1);
        if ( trk_ptr[h] == NULL )
            return 0;
        if ( trk_ptr[h] == p ) {
            *n = trk_sz[h];
            trk_ptr[h] = TRK_TOMB;
            return 1;
        }
    }
    return 0;
}

static int trk_watch;
static long trk_live, trk_peak;
static unsigned long trk_fread_bytes, trk_fread_calls, trk_seek_calls;

static void
trk_alloc_note(void *p, size_t n)
{
    if ( NULL != p && trk_watch ) {
        trk_put(p, n);
        trk_live += (long)n;
        if ( trk_live > trk_peak )
            trk_peak = trk_live;
    }
}

void *
malloc(size_t n)
{
    void *p;
    if ( !trk_ready )
        trk_resolve();
    if ( NULL == p_malloc )
        return trk_arena_alloc(n);
    p = p_malloc(n);
    trk_alloc_note(p, n);
    return p;
}

void *
calloc(size_t a, size_t b)
{
    void *p;
    if ( !trk_ready )
        trk_resolve();
    if ( NULL == p_calloc )
        return trk_arena_alloc(a * b); /* arena storage is zero-initialised */
    p = p_calloc(a, b);
    trk_alloc_note(p, a * b);
    return p;
}

void
free(void *p)
{
    size_t n;
    if ( NULL == p )
        return;
    if ( trk_is_arena(p) )
        return; /* bootstrap allocations are leaked on purpose */
    if ( !trk_ready )
        trk_resolve();
    if ( NULL == p_free )
        return;
    if ( trk_watch && trk_del(p, &n) )
        trk_live -= (long)n;
    p_free(p);
}

void *
realloc(void *p, size_t n)
{
    void *q;
    size_t old = 0;
    int known;
    if ( !trk_ready )
        trk_resolve();
    if ( NULL == p )
        return malloc(n);
    if ( trk_is_arena(p) ) {
        q = malloc(n);
        if ( NULL != q ) {
            size_t avail = (trk_arena + sizeof(trk_arena)) - (unsigned char *)p;
            memcpy(q, p, n < avail ? n : avail);
        }
        return q;
    }
    if ( NULL == p_realloc )
        return NULL;
    known = trk_watch && trk_del(p, &old);
    q = p_realloc(p, n);
    if ( NULL != q ) {
        if ( trk_watch ) {
            if ( known )
                trk_live -= (long)old;
            trk_put(q, n);
            trk_live += (long)n;
            if ( trk_live > trk_peak )
                trk_peak = trk_live;
        }
    } else if ( known ) {
        trk_put(p, old); /* realloc failed, old pointer still valid */
    }
    return q;
}

size_t
fread(void *ptr, size_t size, size_t count, FILE *stream)
{
    size_t r;
    if ( !trk_ready )
        trk_resolve();
    r = p_fread(ptr, size, count, stream);
    if ( trk_watch ) {
        trk_fread_calls++;
        trk_fread_bytes += (unsigned long)(r * size);
    }
    return r;
}

int
fseek(FILE *stream, long offset, int whence)
{
    if ( !trk_ready )
        trk_resolve();
    if ( trk_watch )
        trk_seek_calls++;
    return p_fseek(stream, offset, whence);
}

int
fseeko(FILE *stream, off_t offset, int whence)
{
    if ( !trk_ready )
        trk_resolve();
    if ( trk_watch )
        trk_seek_calls++;
    if ( NULL != p_fseeko )
        return p_fseeko(stream, offset, whence);
    return p_fseek(stream, (long)offset, whence);
}

static void
trk_reset(void)
{
    trk_live = 0;
    trk_peak = 0;
    trk_fread_bytes = 0;
    trk_fread_calls = 0;
    trk_seek_calls = 0;
    trk_watch = 1;
}

static void
trk_stop(void)
{
    trk_watch = 0;
}

/* ------------------------------------------------------------------ */
/* Coordinate-table model                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint32_t r, c;
    double re, im;
} ent_t;

typedef struct
{
    size_t dims[2];
    uint32_t *ir, *jc;
    double *re, *im;
    uint32_t nir, njc;
    int iscomplex;
} csc_t;

static void
csc_free(csc_t *c)
{
    free(c->ir);
    free(c->jc);
    free(c->re);
    free(c->im);
    memset(c, 0, sizeof(*c));
}

/* Independent slab projection: for every output row (ascending) the
 * matching source slots are summed in source slot order. */
static int
expect_slab(const ent_t *en, const uint32_t *jc_src, const size_t start[2],
            const size_t stride[2], const size_t edge[2], int iscomplex, csc_t *out)
{
    size_t j, n = 0, cap = 16;
    memset(out, 0, sizeof(*out));
    out->dims[0] = edge[0];
    out->dims[1] = edge[1];
    out->iscomplex = iscomplex;
    out->njc = (uint32_t)(edge[1] + 1);
    out->jc = (uint32_t *)calloc(edge[1] + 1, sizeof(uint32_t));
    out->ir = (uint32_t *)malloc(cap * sizeof(uint32_t));
    out->re = (double *)malloc(cap * sizeof(double));
    out->im = (double *)malloc(cap * sizeof(double));
    if ( NULL == out->jc || NULL == out->ir || NULL == out->re || NULL == out->im )
        return -1;
    for ( j = 0; j < edge[1]; j++ ) {
        const size_t c = start[1] + j * stride[1];
        size_t k;
        out->jc[j] = (uint32_t)n;
        /* walk output rows in ascending order; for each one sum every
         * source slot projecting to it in source slot order */
        for ( k = 0; k < edge[0]; k++ ) {
            uint32_t s;
            double sre = 0.0, sim = 0.0;
            for ( s = jc_src[c]; s < jc_src[c + 1]; s++ ) {
                if ( (size_t)en[s].r >= start[0] && (en[s].r - start[0]) % stride[0] == 0 &&
                     (size_t)(en[s].r - start[0]) / stride[0] == k ) {
                    sre += en[s].re;
                    sim += en[s].im;
                }
            }
            if ( sre != 0.0 || sim != 0.0 ) {
                if ( n == cap ) {
                    uint32_t *ti;
                    double *td;
                    cap *= 2;
                    ti = (uint32_t *)realloc(out->ir, cap * sizeof(uint32_t));
                    if ( NULL == ti )
                        return -1;
                    out->ir = ti;
                    td = (double *)realloc(out->re, cap * sizeof(double));
                    if ( NULL == td )
                        return -1;
                    out->re = td;
                    td = (double *)realloc(out->im, cap * sizeof(double));
                    if ( NULL == td )
                        return -1;
                    out->im = td;
                }
                out->ir[n] = (uint32_t)k;
                out->re[n] = sre;
                out->im[n] = sim;
                n++;
            }
        }
    }
    out->jc[edge[1]] = (uint32_t)n;
    out->nir = (uint32_t)n;
    return 0;
}

static int
dbl_eq(double a, double b)
{
    return a == b || (isnan(a) && isnan(b));
}

/* Compare a matio sparse variable against the expected CSC model. */
static int
check_csc(const matvar_t *mv, const csc_t *e)
{
    const mat_sparse_t *sp;
    const double *re, *im = NULL;
    uint32_t j;
    UNIT_CHECK(NULL != mv);
    UNIT_CHECK(mv->class_type == MAT_C_SPARSE);
    UNIT_CHECK(mv->rank == 2);
    UNIT_CHECK(mv->dims[0] == e->dims[0] && mv->dims[1] == e->dims[1]);
    UNIT_CHECK((mv->isComplex != 0) == (e->iscomplex != 0));
    sp = (const mat_sparse_t *)mv->data;
    UNIT_CHECK(NULL != sp);
    UNIT_CHECK(sp->njc == e->njc);
    UNIT_CHECK(sp->nir == e->nir);
    UNIT_CHECK(sp->ndata == e->nir);
    for ( j = 0; j < e->njc; j++ )
        UNIT_CHECK(sp->jc[j] == e->jc[j]);
    for ( j = 0; j + 1 < e->njc; j++ ) {
        uint32_t k;
        for ( k = sp->jc[j]; k < sp->jc[j + 1]; k++ ) {
            UNIT_CHECK(sp->ir[k] == e->ir[k]);
            if ( k > sp->jc[j] )
                UNIT_CHECK(sp->ir[k] > sp->ir[k - 1]); /* strictly increasing */
        }
    }
    if ( e->iscomplex ) {
        const mat_complex_split_t *cs = (const mat_complex_split_t *)sp->data;
        UNIT_CHECK(NULL != cs);
        re = (const double *)cs->Re;
        im = (const double *)cs->Im;
    } else {
        re = (const double *)sp->data;
    }
    if ( e->nir > 0 ) {
        uint32_t k;
        UNIT_CHECK(NULL != re);
        if ( e->iscomplex )
            UNIT_CHECK(NULL != im);
        for ( k = 0; k < e->nir; k++ ) {
            UNIT_CHECK(dbl_eq(re[k], e->re[k]));
            if ( e->iscomplex )
                UNIT_CHECK(dbl_eq(im[k], e->im[k]));
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Source construction helpers                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint32_t *ir, *jc;
    double *re, *im;
} src_arrays_t;

static void
src_arrays_free(src_arrays_t *a)
{
    free(a->ir);
    free(a->jc);
    free(a->re);
    free(a->im);
    memset(a, 0, sizeof(*a));
}

/* Build CSC slot arrays from a coordinate table.  Entries with the same
 * column keep their table (slot) order; duplicates are preserved. */
static int
build_arrays(const ent_t *en, size_t n, uint32_t ncols, src_arrays_t *out)
{
    size_t i;
    uint32_t *cur;
    memset(out, 0, sizeof(*out));
    out->jc = (uint32_t *)calloc((size_t)ncols + 1, sizeof(uint32_t));
    out->ir = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    out->re = (double *)malloc((n ? n : 1) * sizeof(double));
    out->im = (double *)malloc((n ? n : 1) * sizeof(double));
    if ( NULL == out->jc || NULL == out->ir || NULL == out->re || NULL == out->im )
        return -1;
    for ( i = 0; i < n; i++ )
        out->jc[en[i].c + 1]++;
    for ( i = 0; i < ncols; i++ )
        out->jc[i + 1] += out->jc[i];
    cur = (uint32_t *)malloc(((size_t)ncols + 1) * sizeof(uint32_t));
    if ( NULL == cur )
        return -1;
    memcpy(cur, out->jc, ((size_t)ncols + 1) * sizeof(uint32_t));
    for ( i = 0; i < n; i++ ) {
        const uint32_t s = cur[en[i].c]++;
        out->ir[s] = en[i].r;
        out->re[s] = en[i].re;
        out->im[s] = en[i].im;
    }
    free(cur);
    return 0;
}

static matvar_t *
make_sparse_var(const char *name, uint32_t nrows, uint32_t ncols, const src_arrays_t *a,
                uint32_t ndata, int iscomplex)
{
    size_t dims[2];
    mat_sparse_t sp;
    mat_complex_split_t cs;
    dims[0] = nrows;
    dims[1] = ncols;
    sp.nzmax = ndata;
    sp.ir = a->ir;
    sp.nir = ndata;
    sp.jc = a->jc;
    sp.njc = ncols + 1;
    sp.ndata = ndata;
    cs.Re = a->re;
    cs.Im = a->im;
    sp.data = iscomplex ? (void *)&cs : (void *)a->re;
    return Mat_VarCreate(name, MAT_C_SPARSE, MAT_T_DOUBLE, 2, dims, &sp,
                         iscomplex ? MAT_F_COMPLEX : 0);
}

static int
write_after_var(mat_t *m)
{
    size_t d2[2] = {2, 2};
    double a[4] = {10.0, 20.0, 30.0, 40.0};
    matvar_t *av = Mat_VarCreate("after", MAT_C_DOUBLE, MAT_T_DOUBLE, 2, d2, a, 0);
    int rc;
    if ( NULL == av )
        return -1;
    rc = Mat_VarWrite(m, av, MAT_COMPRESSION_NONE);
    Mat_VarFree(av);
    return rc;
}

/* Write a native MAT5 file holding the sparse variable plus a trailing
 * dense variable "after". */
static int
write_native_file(const char *path, matvar_t *v)
{
    mat_t *m = Mat_CreateVer(path, NULL, MAT_FT_MAT5);
    int rc;
    if ( NULL == m )
        return -1;
    rc = Mat_VarWrite(m, v, MAT_COMPRESSION_NONE);
    if ( 0 == rc )
        rc = write_after_var(m);
    if ( 0 != Mat_Close(m) )
        rc = -1;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Minimal MAT5 writer (endianness control, corrupt framing)           */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *buf;
    size_t len, cap;
    int be;
} bw_t;

static void
bw_need(bw_t *w, size_t n)
{
    if ( w->len + n > w->cap ) {
        w->cap = 2 * (w->len + n) + 64;
        w->buf = (unsigned char *)realloc(w->buf, w->cap);
    }
}

static void
bw_u32(bw_t *w, uint32_t v)
{
    bw_need(w, 4);
    if ( w->be ) {
        w->buf[w->len++] = (unsigned char)(v >> 24);
        w->buf[w->len++] = (unsigned char)(v >> 16);
        w->buf[w->len++] = (unsigned char)(v >> 8);
        w->buf[w->len++] = (unsigned char)v;
    } else {
        w->buf[w->len++] = (unsigned char)v;
        w->buf[w->len++] = (unsigned char)(v >> 8);
        w->buf[w->len++] = (unsigned char)(v >> 16);
        w->buf[w->len++] = (unsigned char)(v >> 24);
    }
}

static void
bw_f64(bw_t *w, double d)
{
    uint64_t u;
    int i;
    memcpy(&u, &d, 8);
    bw_need(w, 8);
    for ( i = 0; i < 8; i++ )
        w->buf[w->len++] = (unsigned char)(u >> (w->be ? (56 - 8 * i) : (8 * i)));
}

static void
bw_raw(bw_t *w, const void *p, size_t n)
{
    bw_need(w, n);
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void
bw_zero(bw_t *w, size_t n)
{
    bw_need(w, n);
    memset(w->buf + w->len, 0, n);
    w->len += n;
}

static size_t
bw_pad8_len(size_t n)
{
    return (n % 8) ? 8 - (n % 8) : 0;
}

static void
bw_header(bw_t *w, int be)
{
    const char *txt = "MATLAB 5.0 MAT-file, Platform: GLNXA64, Created by matio regression";
    w->be = be;
    bw_raw(w, txt, strlen(txt));
    bw_zero(w, 116 - strlen(txt));
    bw_zero(w, 8);           /* subsystem offset */
    if ( be ) {
        bw_raw(w, "\x01\x00", 2);
        bw_raw(w, "MI", 2);  /* big-endian indicator */
    } else {
        bw_raw(w, "\x00\x01", 2);
        bw_raw(w, "IM", 2);  /* little-endian indicator */
    }
}

typedef struct
{
    const char *name;
    int iscomplex;
    uint32_t nrows, ncols;
    uint32_t ir_type, jc_type, pr_type, pi_type; /* 0 -> miUINT32/miUINT32/miDOUBLE/miDOUBLE */
    const uint32_t *ir;
    uint32_t nir;
    const uint32_t *jc;
    uint32_t njc;
    const double *re;
    uint32_t ndre;
    const double *im;
    uint32_t ndim;
    uint32_t ir_decl, jc_decl, pr_decl; /* UINT32_MAX -> element count * size */
    int small_ir, small_jc;             /* use small data element format when it fits */
    int truncate_after_pr;              /* stop the file right after the pr payload */
} sp_spec_t;

static void
bw_name(bw_t *w, const char *name)
{
    const size_t nlen = strlen(name);
    if ( nlen <= 4 ) {
        unsigned char b[4] = {0, 0, 0, 0};
        bw_u32(w, 1u | ((uint32_t)nlen << 16));
        memcpy(b, name, nlen);
        bw_raw(w, b, 4);
    } else {
        bw_u32(w, 1);
        bw_u32(w, (uint32_t)nlen);
        bw_raw(w, name, nlen);
        bw_zero(w, bw_pad8_len(nlen));
    }
}

static size_t
bw_name_len(const char *name)
{
    const size_t nlen = strlen(name);
    return nlen <= 4 ? 8 : 8 + nlen + bw_pad8_len(nlen);
}

static void
bw_sparse(bw_t *w, const sp_spec_t *s)
{
    const uint32_t ir_type = s->ir_type ? s->ir_type : 6; /* miUINT32 */
    const uint32_t jc_type = s->jc_type ? s->jc_type : 6;
    const uint32_t pr_type = s->pr_type ? s->pr_type : 9; /* miDOUBLE */
    const uint32_t pi_type = s->pi_type ? s->pi_type : 9;
    const uint32_t ir_decl = s->ir_decl == UINT32_MAX ? s->nir * 4 : s->ir_decl;
    const uint32_t jc_decl = s->jc_decl == UINT32_MAX ? s->njc * 4 : s->jc_decl;
    const uint32_t pr_decl = s->pr_decl == UINT32_MAX ? s->ndre * 8 : s->pr_decl;
    const uint32_t pi_decl = s->ndim * 8;
    const int small_ir = s->small_ir && ir_decl <= 4;
    const int small_jc = s->small_jc && jc_decl <= 4;
    uint32_t total, i;
    size_t e_ir = small_ir ? 8 : 8 + ir_decl + bw_pad8_len(ir_decl);
    size_t e_jc = small_jc ? 8 : 8 + jc_decl + bw_pad8_len(jc_decl);
    size_t e_pr = 8 + pr_decl + bw_pad8_len(pr_decl);
    size_t e_pi = s->iscomplex ? 8 + pi_decl + bw_pad8_len(pi_decl) : 0;

    total = (uint32_t)(16 + 16 + bw_name_len(s->name) + e_ir + e_jc + e_pr + e_pi);
    bw_u32(w, 14); /* miMATRIX */
    bw_u32(w, total);
    /* array flags */
    bw_u32(w, 6);
    bw_u32(w, 8);
    bw_u32(w, 5u | (s->iscomplex ? 0x0800u : 0u)); /* mxSPARSE */
    bw_u32(w, s->ndre > s->ndim ? s->ndre : s->ndim); /* nzmax */
    /* dimensions */
    bw_u32(w, 5);
    bw_u32(w, 8);
    bw_u32(w, s->nrows);
    bw_u32(w, s->ncols);
    bw_name(w, s->name);
    /* ir */
    if ( small_ir ) {
        bw_u32(w, ir_type | (ir_decl << 16)); /* data packed into the tag */
        bw_u32(w, s->nir > 0 ? s->ir[0] : 0);
    } else {
        bw_u32(w, ir_type);
        bw_u32(w, ir_decl);
        for ( i = 0; i < s->nir; i++ )
            bw_u32(w, s->ir[i]);
        if ( ir_decl > s->nir * 4 )
            bw_zero(w, ir_decl - s->nir * 4);
        bw_zero(w, bw_pad8_len(ir_decl));
    }
    /* jc */
    if ( small_jc ) {
        bw_u32(w, jc_type | (jc_decl << 16)); /* data packed into the tag */
        bw_u32(w, s->njc > 0 ? s->jc[0] : 0);
    } else {
        bw_u32(w, jc_type);
        bw_u32(w, jc_decl);
        for ( i = 0; i < s->njc; i++ )
            bw_u32(w, s->jc[i]);
        if ( jc_decl > s->njc * 4 )
            bw_zero(w, jc_decl - s->njc * 4);
        bw_zero(w, bw_pad8_len(jc_decl));
    }
    /* pr */
    bw_u32(w, pr_type);
    bw_u32(w, pr_decl);
    for ( i = 0; i < s->ndre; i++ )
        bw_f64(w, s->re[i]);
    if ( s->truncate_after_pr )
        return; /* declared length intentionally not backed by bytes */
    if ( pr_decl > s->ndre * 8 )
        bw_zero(w, pr_decl - s->ndre * 8);
    bw_zero(w, bw_pad8_len(pr_decl));
    /* pi */
    if ( s->iscomplex ) {
        bw_u32(w, pi_type);
        bw_u32(w, pi_decl);
        for ( i = 0; i < s->ndim; i++ )
            bw_f64(w, s->im[i]);
        bw_zero(w, bw_pad8_len(pi_decl));
    }
}

static void
bw_double_matrix(bw_t *w, const char *name, uint32_t r, uint32_t c, const double *data)
{
    const uint32_t n = r * c;
    uint32_t i;
    const uint32_t total = (uint32_t)(16 + 16 + bw_name_len(name) + 8 + (size_t)n * 8);
    bw_u32(w, 14);
    bw_u32(w, total);
    bw_u32(w, 6);
    bw_u32(w, 8);
    bw_u32(w, 6); /* mxDOUBLE */
    bw_u32(w, 0);
    bw_u32(w, 5);
    bw_u32(w, 8);
    bw_u32(w, r);
    bw_u32(w, c);
    bw_name(w, name);
    bw_u32(w, 9);
    bw_u32(w, n * 8);
    for ( i = 0; i < n; i++ )
        bw_f64(w, data[i]);
}

static int
bw_write_file(const char *path, const bw_t *w)
{
    FILE *f = fopen(path, "wb");
    if ( NULL == f )
        return -1;
    if ( 1 != fwrite(w->buf, w->len, 1, f) ) {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

/* ------------------------------------------------------------------ */
/* Shared slab scenario driver                                         */
/* ------------------------------------------------------------------ */

static int
run_slab_case(const char *path, const char *outpath, const char *var, const ent_t *en,
              const uint32_t *jc_src, const size_t start[2], const size_t stride[2],
              const size_t edge[2], int iscomplex, const char *next_name)
{
    mat_t *m = NULL, *m2 = NULL;
    matvar_t *src = NULL, *out = NULL, *nv = NULL, *rv = NULL;
    csc_t exp;
    size_t d0 = 0, d1 = 0, nb = 0;
    enum matio_types dt = MAT_T_UNKNOWN;
    int rc = 1;

    memset(&exp, 0, sizeof(exp));
    UNIT_CHECK(0 == expect_slab(en, jc_src, start, stride, edge, iscomplex, &exp));

    m = Mat_Open(path, MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, var);
    UNIT_CHECK(NULL != src);
    UNIT_CHECK(src->class_type == MAT_C_SPARSE);
    UNIT_CHECK(NULL == src->data);
    d0 = src->dims[0];
    d1 = src->dims[1];
    nb = src->nbytes;
    dt = src->data_type;

    /* Mat_VarReadInfo restores the position it found, so the stream is
     * back at the first variable; step over it once.  The slab call below
     * must then leave the position exactly here. */
    if ( NULL != next_name ) {
        nv = Mat_VarReadNextInfo(m);
        UNIT_CHECK(NULL != nv);
        UNIT_CHECK(NULL != nv->name && 0 == strcmp(nv->name, var));
        Mat_VarFree(nv);
        nv = NULL;
    }

    UNIT_CHECK(0 == Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL != out);

    /* source metadata and data pointer must be untouched */
    UNIT_CHECK(NULL == src->data);
    UNIT_CHECK(src->dims[0] == d0 && src->dims[1] == d1);
    UNIT_CHECK(src->nbytes == nb && src->data_type == dt);
    UNIT_CHECK(src->class_type == MAT_C_SPARSE);

    UNIT_CHECK(0 == check_csc(out, &exp));

    /* the file position must be restored: sequential reading continues */
    if ( NULL != next_name ) {
        nv = Mat_VarReadNextInfo(m);
        UNIT_CHECK(NULL != nv);
        UNIT_CHECK(NULL != nv->name && 0 == strcmp(nv->name, next_name));
        if ( 0 == strcmp(next_name, "after") ) {
            double *ad;
            UNIT_CHECK(0 == Mat_VarReadDataAll(m, nv));
            ad = (double *)nv->data;
            UNIT_CHECK(NULL != ad && ad[0] == 10.0 && ad[3] == 40.0);
        }
        Mat_VarFree(nv);
        nv = NULL;
    }

    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;

    /* with the source file closed the result must still be writable */
    m2 = Mat_CreateVer(outpath, NULL, MAT_FT_MAT5);
    UNIT_CHECK(NULL != m2);
    UNIT_CHECK(0 == Mat_VarWrite(m2, out, MAT_COMPRESSION_NONE));
    UNIT_CHECK(0 == Mat_Close(m2));
    m2 = NULL;
    Mat_VarFree(out);
    out = NULL;

    m2 = Mat_Open(outpath, MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m2);
    rv = Mat_VarReadInfo(m2, var);
    UNIT_CHECK(NULL != rv);
    UNIT_CHECK(0 == Mat_VarReadDataAll(m2, rv));
    UNIT_CHECK(0 == check_csc(rv, &exp));
    Mat_VarFree(rv);
    rv = NULL;
    UNIT_CHECK(0 == Mat_Close(m2));
    m2 = NULL;

    rc = 0;
    csc_free(&exp);
    Mat_VarFree(src);
    Mat_VarFree(out);
    Mat_VarFree(nv);
    Mat_VarFree(rv);
    if ( NULL != m )
        (void)Mat_Close(m);
    if ( NULL != m2 )
        (void)Mat_Close(m2);
    return rc;
}

/* ------------------------------------------------------------------ */
/* normal group                                                        */
/* ------------------------------------------------------------------ */

static int
case_real_strided(void)
{
    /* 7x6 real sparse: unsorted rows, duplicate slots, empty columns */
    static const ent_t en[] = {{5, 0, 1.0, 0}, {2, 0, 2.0, 0},  {5, 0, 0.5, 0},
                               {0, 2, 3.0, 0}, {6, 2, 4.0, 0},  {3, 2, 5.0, 0},
                               {3, 2, -5.0, 0}, {1, 2, 2.5, 0}, {4, 3, 1.5, 0},
                               {2, 4, 7.0, 0}, {2, 4, 8.0, 0}};
    static const size_t start[2] = {1, 0}, stride[2] = {2, 2}, edge[2] = {3, 3};
    src_arrays_t a;
    matvar_t *v;
    int rc;
    UNIT_CHECK(0 == build_arrays(en, sizeof(en) / sizeof(en[0]), 6, &a));
    v = make_sparse_var("s", 7, 6, &a, (uint32_t)(sizeof(en) / sizeof(en[0])), 0);
    UNIT_CHECK(NULL != v);
    UNIT_CHECK(0 == write_native_file("rg_real.mat", v));
    Mat_VarFree(v);
    rc = run_slab_case("rg_real.mat", "rg_real_out.mat", "s", en, a.jc, start, stride, edge, 0,
                       "after");
    src_arrays_free(&a);
    return rc;
}

static int
case_complex_split(void)
{
    /* 8x5 split-complex sparse, non-unit strides in both dimensions */
    static const ent_t en[] = {{2, 1, 1.0, 2.0},  {5, 1, 3.0, -1.0}, {2, 1, 0.5, 0.25},
                               {7, 1, 9.0, 9.0},  {2, 3, 1.0, 1.0},  {5, 3, 2.0, 2.0},
                               {2, 3, -1.0, 4.0}, {0, 3, 8.0, 8.0},  {4, 4, 6.0, 6.0}};
    static const size_t start[2] = {2, 1}, stride[2] = {3, 2}, edge[2] = {2, 2};
    src_arrays_t a;
    matvar_t *v;
    int rc;
    UNIT_CHECK(0 == build_arrays(en, sizeof(en) / sizeof(en[0]), 5, &a));
    v = make_sparse_var("s", 8, 5, &a, (uint32_t)(sizeof(en) / sizeof(en[0])), 1);
    UNIT_CHECK(NULL != v);
    UNIT_CHECK(0 == write_native_file("rg_cplx.mat", v));
    Mat_VarFree(v);
    rc = run_slab_case("rg_cplx.mat", "rg_cplx_out.mat", "s", en, a.jc, start, stride, edge, 1,
                       "after");
    src_arrays_free(&a);
    return rc;
}

static int
case_cancel_nan(void)
{
    /* duplicate rows cancelling to zero are dropped, NaN results survive */
    static const ent_t en[] = {
        {1, 0, 1.0, 0}, {1, 0, -1.0, 0},           /* cancels exactly */
        {2, 0, 0.0, 0},                            /* NaN filled below */
        {0, 1, 0.0, 0},                            /* +Inf, filled below */
        {0, 1, 0.0, 0},                            /* -Inf, filled below */
        {3, 1, -0.0, 0}, {3, 1, 0.0, 0},           /* sums to +0.0: dropped */
        {2, 2, 5.0, 0},
    };
    static const size_t start[2] = {0, 0}, stride[2] = {1, 1}, edge[2] = {4, 3};
    ent_t ent[8];
    src_arrays_t a;
    matvar_t *v;
    int rc;
    memcpy(ent, en, sizeof(ent));
    ent[2].re = nan("");
    ent[3].re = INFINITY;
    ent[4].re = -INFINITY;
    UNIT_CHECK(0 == build_arrays(ent, 8, 3, &a));
    v = make_sparse_var("s", 4, 3, &a, 8, 0);
    UNIT_CHECK(NULL != v);
    UNIT_CHECK(0 == write_native_file("rg_nan.mat", v));
    Mat_VarFree(v);
    rc = run_slab_case("rg_nan.mat", "rg_nan_out.mat", "s", ent, a.jc, start, stride, edge, 0,
                       "after");
    src_arrays_free(&a);
    return rc;
}

/* ------------------------------------------------------------------ */
/* boundary group                                                      */
/* ------------------------------------------------------------------ */

static int
case_empty_and_errors(void)
{
    static const ent_t en[] = {{0, 0, 1.0, 0}, {2, 1, 2.0, 0}, {3, 2, 3.0, 0}};
    static const uint32_t jc_src[4] = {0, 1, 2, 3};
    src_arrays_t a;
    matvar_t *v = NULL, *src = NULL, *out = NULL, *nv = NULL;
    mat_t *m = NULL;
    csc_t exp;
    size_t start[2], stride[2], edge[2];
    int rc = 1;

    memset(&exp, 0, sizeof(exp));
    UNIT_CHECK(0 == build_arrays(en, 3, 3, &a));
    v = make_sparse_var("s", 4, 3, &a, 3, 0);
    UNIT_CHECK(NULL != v);
    UNIT_CHECK(0 == write_native_file("rg_empty.mat", v));
    Mat_VarFree(v);
    v = NULL;

    m = Mat_Open("rg_empty.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "s");
    UNIT_CHECK(NULL != src);
    /* step over the first variable so the trailing one is next */
    nv = Mat_VarReadNextInfo(m);
    UNIT_CHECK(NULL != nv);
    Mat_VarFree(nv);
    nv = NULL;

    /* empty row selection: legal, yields an empty but writable CSC */
    start[0] = 0; start[1] = 1;
    stride[0] = 1; stride[1] = 1;
    edge[0] = 0; edge[1] = 2;
    UNIT_CHECK(0 == Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL != out);
    UNIT_CHECK(0 == expect_slab(en, jc_src, start, stride, edge, 0, &exp));
    UNIT_CHECK(0 == check_csc(out, &exp));
    {
        mat_t *m2 = Mat_CreateVer("rg_empty_out.mat", NULL, MAT_FT_MAT5);
        matvar_t *rv;
        UNIT_CHECK(NULL != m2);
        UNIT_CHECK(0 == Mat_VarWrite(m2, out, MAT_COMPRESSION_NONE));
        UNIT_CHECK(0 == Mat_Close(m2));
        m2 = Mat_Open("rg_empty_out.mat", MAT_ACC_RDONLY);
        UNIT_CHECK(NULL != m2);
        rv = Mat_VarReadInfo(m2, "s");
        UNIT_CHECK(NULL != rv);
        UNIT_CHECK(0 == Mat_VarReadDataAll(m2, rv));
        UNIT_CHECK(0 == check_csc(rv, &exp));
        Mat_VarFree(rv);
        UNIT_CHECK(0 == Mat_Close(m2));
    }
    Mat_VarFree(out);
    out = NULL;
    csc_free(&exp);
    memset(&exp, 0, sizeof(exp));

    /* empty column selection */
    edge[0] = 2; edge[1] = 0;
    UNIT_CHECK(0 == Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL != out);
    UNIT_CHECK(0 == expect_slab(en, jc_src, start, stride, edge, 0, &exp));
    UNIT_CHECK(0 == check_csc(out, &exp));
    Mat_VarFree(out);
    out = NULL;
    csc_free(&exp);
    memset(&exp, 0, sizeof(exp));

    /* stride zero is rejected */
    stride[0] = 0; stride[1] = 1;
    edge[0] = 1; edge[1] = 1;
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    stride[0] = 1; stride[1] = 0;
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    stride[1] = 1;

    /* out of bounds selections are rejected */
    start[0] = 4; start[1] = 0;
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    start[0] = 0;
    edge[0] = 5; /* last row 4 >= dims[0] */
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    edge[0] = 1;
    start[1] = 2; edge[1] = 2; /* last column 3 >= dims[1] */
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    start[1] = 0; edge[1] = 1;

    /* index arithmetic overflow is rejected */
    start[0] = SIZE_MAX - 1;
    stride[0] = 2;
    edge[0] = 2;
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    start[0] = 0;
    stride[0] = 1;
    edge[0] = 1;

    /* NULL arguments are rejected */
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, NULL, stride, edge, &out));
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, NULL, edge, &out));
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, NULL, &out));
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, src, start, stride, edge, NULL));
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, NULL, start, stride, edge, &out));
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(NULL, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);

    /* a variable that never came from a file is rejected */
    v = make_sparse_var("mem", 4, 3, &a, 3, 0);
    UNIT_CHECK(NULL != v);
    UNIT_CHECK(0 != Mat_VarReadSparseSlab(m, v, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    Mat_VarFree(v);
    v = NULL;

    /* after every failure the sequential read position is intact */
    nv = Mat_VarReadNextInfo(m);
    UNIT_CHECK(NULL != nv);
    UNIT_CHECK(NULL != nv->name && 0 == strcmp(nv->name, "after"));
    Mat_VarFree(nv);
    nv = NULL;

    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;
    rc = 0;

    src_arrays_free(&a);
    Mat_VarFree(v);
    Mat_VarFree(src);
    Mat_VarFree(out);
    Mat_VarFree(nv);
    if ( NULL != m )
        (void)Mat_Close(m);
    csc_free(&exp);
    return rc;
}

static int
case_unsupported(void)
{
    size_t d2[2] = {2, 2};
    double ad[4] = {1, 2, 3, 4};
    size_t d3[3] = {2, 2, 1};
    size_t start[2] = {0, 0}, stride[2] = {1, 1}, edge[2] = {1, 1};
    mat_t *m = NULL;
    matvar_t *v = NULL, *src = NULL, *out = NULL;
    int rc = 1;

    /* dense double variable in a MAT5 file */
    v = Mat_VarCreate("d", MAT_C_DOUBLE, MAT_T_DOUBLE, 2, d2, ad, 0);
    UNIT_CHECK(NULL != v);
    m = Mat_CreateVer("rg_uns1.mat", NULL, MAT_FT_MAT5);
    UNIT_CHECK(NULL != m);
    UNIT_CHECK(0 == Mat_VarWrite(m, v, MAT_COMPRESSION_NONE));
    Mat_VarFree(v);
    v = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = Mat_Open("rg_uns1.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "d");
    UNIT_CHECK(NULL != src);
    UNIT_CHECK(MATIO_E_OPERATION_NOT_SUPPORTED ==
               Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;

    /* sparse variable whose on-disk data type is not double */
    {
        uint32_t ir[2] = {0, 1}, jc[3] = {0, 1, 2};
        int32_t idata[2] = {7, 8};
        mat_sparse_t sp;
        sp.nzmax = 2;
        sp.ir = ir;
        sp.nir = 2;
        sp.jc = jc;
        sp.njc = 3;
        sp.ndata = 2;
        sp.data = idata;
        v = Mat_VarCreate("si", MAT_C_SPARSE, MAT_T_INT32, 2, d2, &sp, 0);
        UNIT_CHECK(NULL != v);
    }
    m = Mat_CreateVer("rg_uns2.mat", NULL, MAT_FT_MAT5);
    UNIT_CHECK(NULL != m);
    UNIT_CHECK(0 == Mat_VarWrite(m, v, MAT_COMPRESSION_NONE));
    Mat_VarFree(v);
    v = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = Mat_Open("rg_uns2.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "si");
    UNIT_CHECK(NULL != src);
    UNIT_CHECK(src->class_type == MAT_C_SPARSE);
    UNIT_CHECK(MATIO_E_OPERATION_NOT_SUPPORTED ==
               Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;

    /* rank-3 sparse variable */
    {
        uint32_t ir[1] = {0}, jc[3] = {0, 1, 1};
        double dd[1] = {1.0};
        mat_sparse_t sp;
        sp.nzmax = 1;
        sp.ir = ir;
        sp.nir = 1;
        sp.jc = jc;
        sp.njc = 3;
        sp.ndata = 1;
        sp.data = dd;
        v = Mat_VarCreate("s3", MAT_C_SPARSE, MAT_T_DOUBLE, 3, d3, &sp, 0);
        UNIT_CHECK(NULL != v);
    }
    m = Mat_CreateVer("rg_uns3.mat", NULL, MAT_FT_MAT5);
    UNIT_CHECK(NULL != m);
    UNIT_CHECK(0 == Mat_VarWrite(m, v, MAT_COMPRESSION_NONE));
    Mat_VarFree(v);
    v = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = Mat_Open("rg_uns3.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "s3");
    UNIT_CHECK(NULL != src);
    UNIT_CHECK(MATIO_E_OPERATION_NOT_SUPPORTED ==
               Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;

    /* MAT v4 file */
    v = Mat_VarCreate("d4", MAT_C_DOUBLE, MAT_T_DOUBLE, 2, d2, ad, 0);
    UNIT_CHECK(NULL != v);
    m = Mat_CreateVer("rg_uns4.mat", NULL, MAT_FT_MAT4);
    UNIT_CHECK(NULL != m);
    UNIT_CHECK(0 == Mat_VarWrite(m, v, MAT_COMPRESSION_NONE));
    Mat_VarFree(v);
    v = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = Mat_Open("rg_uns4.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "d4");
    UNIT_CHECK(NULL != src);
    UNIT_CHECK(MATIO_E_OPERATION_NOT_SUPPORTED ==
               Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    UNIT_CHECK(NULL == out);
    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;

    rc = 0;
    Mat_VarFree(v);
    Mat_VarFree(src);
    Mat_VarFree(out);
    if ( NULL != m )
        (void)Mat_Close(m);
    return rc;
}

/* One corrupt-file scenario: info must be readable, the slab read must
 * fail with *out == NULL, and the framing must stay intact so that the
 * trailing variable is still found afterwards. */
static int
check_corrupt_file(const char *path, const sp_spec_t *s, int expect_rc, int has_after)
{
    bw_t w;
    mat_t *m = NULL;
    matvar_t *src = NULL, *out = NULL, *nv = NULL;
    double ad[4] = {10, 20, 30, 40};
    int rc = 1, err;

    memset(&w, 0, sizeof(w));
    bw_header(&w, 0);
    bw_sparse(&w, s);
    if ( has_after )
        bw_double_matrix(&w, "after", 2, 2, ad);
    UNIT_CHECK(0 == bw_write_file(path, &w));
    free(w.buf);
    w.buf = NULL;

    m = Mat_Open(path, MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "s");
    UNIT_CHECK(NULL != src);
    UNIT_CHECK(src->class_type == MAT_C_SPARSE);
    if ( has_after ) {
        /* step over the first variable; the failed slab read must not
         * disturb the sequential position */
        nv = Mat_VarReadNextInfo(m);
        UNIT_CHECK(NULL != nv);
        Mat_VarFree(nv);
        nv = NULL;
    }
    err = Mat_VarReadSparseSlab(m, src, (const size_t[]){0, 0}, (const size_t[]){1, 1},
                                (const size_t[]){s->nrows, s->ncols}, &out);
    UNIT_CHECK(0 != err);
    if ( expect_rc )
        UNIT_CHECK(expect_rc == err);
    UNIT_CHECK(NULL == out);
    if ( has_after ) {
        nv = Mat_VarReadNextInfo(m);
        UNIT_CHECK(NULL != nv);
        UNIT_CHECK(NULL != nv->name && 0 == strcmp(nv->name, "after"));
        Mat_VarFree(nv);
        nv = NULL;
    }
    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;
    rc = 0;

    free(w.buf);
    Mat_VarFree(src);
    Mat_VarFree(out);
    Mat_VarFree(nv);
    if ( NULL != m )
        (void)Mat_Close(m);
    return rc;
}

static int
case_corrupt(void)
{
    uint32_t ir_ok[2] = {0, 2};
    uint32_t jc_ok[4] = {0, 1, 2, 2};
    double re_ok[2] = {1.0, 2.0};
    sp_spec_t s;
    int rc = 1;

    memset(&s, 0, sizeof(s));
    s.name = "s";
    s.nrows = 4;
    s.ncols = 3;
    s.ir = ir_ok;
    s.nir = 2;
    s.jc = jc_ok;
    s.njc = 4;
    s.re = re_ok;
    s.ndre = 2;
    s.ir_decl = UINT32_MAX;
    s.jc_decl = UINT32_MAX;
    s.pr_decl = UINT32_MAX;

    /* jc does not start at zero */
    {
        uint32_t jc_bad[4] = {1, 1, 2, 2};
        sp_spec_t t = s;
        t.jc = jc_bad;
        UNIT_CHECK(0 == check_corrupt_file("rg_cor1.mat", &t, 0, 1));
    }
    /* jc not monotone */
    {
        uint32_t jc_bad[4] = {0, 2, 1, 2};
        sp_spec_t t = s;
        t.jc = jc_bad;
        UNIT_CHECK(0 == check_corrupt_file("rg_cor2.mat", &t, 0, 1));
    }
    /* jc final value beyond the ir/data element counts */
    {
        uint32_t jc_bad[4] = {0, 1, 2, 9};
        sp_spec_t t = s;
        t.jc = jc_bad;
        UNIT_CHECK(0 == check_corrupt_file("rg_cor3.mat", &t, 0, 1));
    }
    /* out-of-range row index in a selected column */
    {
        uint32_t ir_bad[2] = {0, 99};
        sp_spec_t t = s;
        t.ir = ir_bad;
        UNIT_CHECK(0 == check_corrupt_file("rg_cor4.mat", &t, 0, 1));
    }
    /* ir element with a non-integer tag type */
    {
        sp_spec_t t = s;
        t.ir_type = 9; /* miDOUBLE */
        UNIT_CHECK(0 == check_corrupt_file("rg_cor5.mat", &t, MATIO_E_OPERATION_NOT_SUPPORTED, 1));
    }
    /* jc element byte length not a multiple of four */
    {
        sp_spec_t t = s;
        t.njc = 1;    /* one payload word plus two fill bytes ... */
        t.jc_decl = 6; /* ... keeps framing intact, length is invalid */
        UNIT_CHECK(0 == check_corrupt_file("rg_cor6.mat", &t, 0, 1));
    }
    /* jc element count does not match ncols + 1 */
    {
        uint32_t jc_short[3] = {0, 1, 2};
        sp_spec_t t = s;
        t.jc = jc_short;
        t.njc = 3;
        UNIT_CHECK(0 == check_corrupt_file("rg_cor7.mat", &t, 0, 1));
    }
    /* declared pr length reaches past the end of the file */
    {
        sp_spec_t t = s;
        t.pr_decl = 2 * 8 + 8;
        t.truncate_after_pr = 1;
        UNIT_CHECK(0 == check_corrupt_file("rg_cor8.mat", &t, 0, 0));
    }

    rc = 0;
    return rc;
}

static int
case_small_element(void)
{
    /* ir/jc elements stored in the small data element format */
    static const ent_t en[] = {{2, 1, 3.0, 0}};
    static const uint32_t jc_src[3] = {0, 0, 1};
    static const size_t start[2] = {0, 0}, stride[2] = {1, 1}, edge[2] = {4, 2};
    uint32_t ir[1] = {2};
    uint32_t jc[3] = {0, 0, 1};
    double re[1] = {3.0};
    double ad[4] = {10, 20, 30, 40};
    bw_t w;
    sp_spec_t s;
    mat_t *m = NULL;
    matvar_t *src = NULL, *out = NULL;
    csc_t exp;
    int rc = 1;

    memset(&exp, 0, sizeof(exp));
    memset(&w, 0, sizeof(w));
    bw_header(&w, 0);
    memset(&s, 0, sizeof(s));
    s.name = "s";
    s.nrows = 4;
    s.ncols = 2;
    s.ir = ir;
    s.nir = 1;
    s.jc = jc;
    s.njc = 3;
    s.re = re;
    s.ndre = 1;
    s.ir_decl = UINT32_MAX;
    s.jc_decl = UINT32_MAX;
    s.pr_decl = UINT32_MAX;
    s.small_ir = 1; /* 4-byte ir payload fits into the tag */
    bw_sparse(&w, &s);
    bw_double_matrix(&w, "after", 2, 2, ad);
    UNIT_CHECK(0 == bw_write_file("rg_small.mat", &w));
    free(w.buf);
    w.buf = NULL;

    rc = run_slab_case("rg_small.mat", "rg_small_out.mat", "s", en, jc_src, start, stride,
                       edge, 0, "after");
    UNIT_CHECK(0 == rc);

    /* 4x0 sparse with a small-format jc element and no data at all */
    memset(&w, 0, sizeof(w));
    bw_header(&w, 0);
    memset(&s, 0, sizeof(s));
    s.name = "s";
    s.nrows = 4;
    s.ncols = 0;
    s.ir = NULL;
    s.nir = 0;
    s.jc = jc; /* jc[0] = 0 */
    s.njc = 1;
    s.re = NULL;
    s.ndre = 0;
    s.ir_decl = UINT32_MAX;
    s.jc_decl = UINT32_MAX;
    s.pr_decl = UINT32_MAX;
    s.small_jc = 1;
    bw_sparse(&w, &s);
    bw_double_matrix(&w, "after", 2, 2, ad);
    UNIT_CHECK(0 == bw_write_file("rg_small2.mat", &w));
    free(w.buf);
    w.buf = NULL;

    m = Mat_Open("rg_small2.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "s");
    UNIT_CHECK(NULL != src);
    {
        size_t st2[2] = {0, 0}, str2[2] = {1, 1}, ed2[2] = {2, 0};
        UNIT_CHECK(0 == Mat_VarReadSparseSlab(m, src, st2, str2, ed2, &out));
        UNIT_CHECK(0 == expect_slab(en, jc_src, st2, str2, ed2, 0, &exp));
    }
    UNIT_CHECK(NULL != out);
    UNIT_CHECK(0 == check_csc(out, &exp));
    Mat_VarFree(out);
    out = NULL;
    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;
    rc = 0;

    free(w.buf);
    csc_free(&exp);
    Mat_VarFree(src);
    Mat_VarFree(out);
    if ( NULL != m )
        (void)Mat_Close(m);
    return rc;
}

/* ------------------------------------------------------------------ */
/* integration group                                                   */
/* ------------------------------------------------------------------ */
static int
case_be_writeback(void)
{
    /* big-endian MAT5 file with a split-complex sparse matrix */
    static const ent_t en[] = {{1, 1, 1.0, 0.5},  {4, 1, 2.0, -2.0}, {1, 1, 3.0, 1.5},
                               {7, 1, 5.0, 0.0},  {4, 3, 1.0, 1.0},  {4, 3, -1.0, 3.0},
                               {7, 3, 2.5, -2.5}, {0, 3, 9.0, 9.0},  {1, 4, 4.0, 4.0}};
    static const size_t start[2] = {1, 1}, stride[2] = {3, 2}, edge[2] = {3, 2};
    src_arrays_t a;
    bw_t w;
    double ad[4] = {10, 20, 30, 40};
    sp_spec_t s;
    int rc;

    UNIT_CHECK(0 == build_arrays(en, sizeof(en) / sizeof(en[0]), 6, &a));
    memset(&w, 0, sizeof(w));
    bw_header(&w, 1); /* reverse endianness */
    memset(&s, 0, sizeof(s));
    s.name = "s";
    s.iscomplex = 1;
    s.nrows = 9;
    s.ncols = 6;
    s.ir = a.ir;
    s.nir = 9;
    s.jc = a.jc;
    s.njc = 7;
    s.re = a.re;
    s.ndre = 9;
    s.im = a.im;
    s.ndim = 9;
    s.ir_decl = UINT32_MAX;
    s.jc_decl = UINT32_MAX;
    s.pr_decl = UINT32_MAX;
    bw_sparse(&w, &s);
    bw_double_matrix(&w, "after", 2, 2, ad);
    UNIT_CHECK(0 == bw_write_file("rg_be.mat", &w));
    free(w.buf);

    rc = run_slab_case("rg_be.mat", "rg_be_out.mat", "s", en, a.jc, start, stride, edge, 1,
                       "after");
    src_arrays_free(&a);
    return rc;
}

static int
case_large_bounded(void)
{
    enum { NROWS = 48, NCOLS = 16384, PERNZ = 32 };
    static const size_t start[2] = {0, 7}, stride[2] = {1, 2048}, edge[2] = {NROWS, 8};
    ent_t *en = NULL;
    src_arrays_t a;
    matvar_t *v = NULL, *src = NULL, *out = NULL, *nv = NULL;
    mat_t *m = NULL;
    csc_t exp;
    size_t nent = (size_t)NCOLS * PERNZ, i;
    long file_bytes = 0;
    int rc = 1;

    memset(&a, 0, sizeof(a));
    memset(&exp, 0, sizeof(exp));

    en = (ent_t *)calloc(nent, sizeof(ent_t));
    UNIT_CHECK(NULL != en);
    for ( i = 0; i < nent; i++ ) {
        const uint32_t c = (uint32_t)(i / PERNZ), k = (uint32_t)(i % PERNZ);
        en[i].c = c;
        en[i].r = (c * 3 + k * 5) % NROWS;
        en[i].re = (double)((c * 31 + k * 17) % 13) - 6.0;
        en[i].im = 0.0;
    }
    UNIT_CHECK(0 == build_arrays(en, nent, NCOLS, &a));
    v = make_sparse_var("big", NROWS, NCOLS, &a, (uint32_t)nent, 0);
    UNIT_CHECK(NULL != v);
    UNIT_CHECK(0 == write_native_file("rg_big.mat", v));
    Mat_VarFree(v);
    v = NULL;

    {
        FILE *f = fopen("rg_big.mat", "rb");
        UNIT_CHECK(NULL != f);
        UNIT_CHECK(0 == fseek(f, 0, SEEK_END));
        file_bytes = ftell(f);
        fclose(f);
        UNIT_CHECK(file_bytes > 4 * 1024 * 1024); /* unselected data dominates */
    }

    m = Mat_Open("rg_big.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    src = Mat_VarReadInfo(m, "big");
    UNIT_CHECK(NULL != src);
    /* step over the first variable so the trailing one is next */
    nv = Mat_VarReadNextInfo(m);
    UNIT_CHECK(NULL != nv);
    Mat_VarFree(nv);
    nv = NULL;

    trk_reset();
    UNIT_CHECK(0 == Mat_VarReadSparseSlab(m, src, start, stride, edge, &out));
    trk_stop();
    UNIT_CHECK(NULL != out);

    /* recorded allocations and read path: a whole-matrix read would need
     * several MB of both, the slab must stay far below that */
    printf("large_bounded: file=%ldB peak_alloc=%ldB fread=%luB calls=%lu seeks=%lu\n",
           file_bytes, trk_peak, trk_fread_bytes, trk_fread_calls, trk_seek_calls);
    UNIT_CHECK(trk_peak < 512 * 1024);
    UNIT_CHECK(trk_fread_bytes < 1024 * 1024);
    UNIT_CHECK(trk_fread_calls < 2000);

    UNIT_CHECK(0 == expect_slab(en, a.jc, start, stride, edge, 0, &exp));
    UNIT_CHECK(0 == check_csc(out, &exp));

    nv = Mat_VarReadNextInfo(m);
    UNIT_CHECK(NULL != nv);
    UNIT_CHECK(NULL != nv->name && 0 == strcmp(nv->name, "after"));
    Mat_VarFree(nv);
    nv = NULL;

    Mat_VarFree(src);
    src = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;

    /* write the small result back and re-read it */
    m = Mat_CreateVer("rg_big_out.mat", NULL, MAT_FT_MAT5);
    UNIT_CHECK(NULL != m);
    UNIT_CHECK(0 == Mat_VarWrite(m, out, MAT_COMPRESSION_NONE));
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;
    Mat_VarFree(out);
    out = NULL;
    m = Mat_Open("rg_big_out.mat", MAT_ACC_RDONLY);
    UNIT_CHECK(NULL != m);
    nv = Mat_VarReadInfo(m, "big");
    UNIT_CHECK(NULL != nv);
    UNIT_CHECK(0 == Mat_VarReadDataAll(m, nv));
    UNIT_CHECK(0 == check_csc(nv, &exp));
    Mat_VarFree(nv);
    nv = NULL;
    UNIT_CHECK(0 == Mat_Close(m));
    m = NULL;

    rc = 0;
    free(en);
    src_arrays_free(&a);
    csc_free(&exp);
    Mat_VarFree(v);
    Mat_VarFree(src);
    Mat_VarFree(out);
    Mat_VarFree(nv);
    if ( NULL != m )
        (void)Mat_Close(m);
    return rc;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

static const unit_case cases[] = {
    {"real_strided", "normal", case_real_strided},
    {"complex_split", "normal", case_complex_split},
    {"cancel_nan", "normal", case_cancel_nan},
    {"empty_and_errors", "boundary", case_empty_and_errors},
    {"unsupported", "boundary", case_unsupported},
    {"corrupt", "boundary", case_corrupt},
    {"small_element", "boundary", case_small_element},
    {"be_writeback", "integration", case_be_writeback},
    {"large_bounded", "integration", case_large_bounded},
};

const unit_case *
unit_cases(size_t *count)
{
    *count = sizeof(cases) / sizeof(cases[0]);
    return cases;
}
