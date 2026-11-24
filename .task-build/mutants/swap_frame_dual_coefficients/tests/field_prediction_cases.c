/*
 * Independent field-motion regression cases for libmpeg2.
 *
 * This file deliberately generates tiny, legal 4:2:0 MPEG-2 elementary
 * streams itself.  Expected pixels are produced by a separate field model
 * addressed by (picture identity, field parity, row inside that field,
 * final displacement).  It does not reuse libmpeg2's motion macros or its
 * compressed vector parser.  Decode expectations are checked only after
 * feeding the complete ES through the public mpeg2_buffer/mpeg2_parse API.
 */

#include "task_support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "config.h"
#include "attributes.h"
#include "mpeg2.h"
#include "mpeg2_internal.h"

/* ----------------------------------------------------------------------- */
/* Generic bit writer and elementary-stream builder                         */
/* ----------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len, cap;
    unsigned bit_acc;
    int bit_count;
} BitWriter;

static void bw_reserve(BitWriter *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    b->cap = b->cap ? b->cap * 2 : 512;
    while (b->cap < b->len + extra) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
    if (!b->data) { fprintf(stderr, "out of memory\n"); abort(); }
}

static void bw_flush(BitWriter *b) {
    while (b->bit_count >= 8) {
        b->bit_count -= 8;
        bw_reserve(b, 1);
        b->data[b->len++] = (uint8_t)((b->bit_acc >> b->bit_count) & 0xffu);
        if (b->bit_count < 32)
            b->bit_acc &= (1u << b->bit_count) - 1u;
    }
}

static void bw_bits(BitWriter *b, uint32_t value, int n) {
    if (n == 0) return;
    b->bit_acc = (b->bit_acc << n) | (value & ((n >= 32) ? 0xffffffffu :
                                               ((1u << n) - 1)));
    b->bit_count += n;
    if (b->bit_count >= 16) bw_flush(b);
}

static void bw_align(BitWriter *b) {
    int pad = (8 - (b->bit_count & 7)) & 7;
    if (pad) bw_bits(b, 0, pad);
    bw_flush(b);
}

static void bw_bytes(BitWriter *b, const void *p, size_t n) {
    bw_align(b);
    bw_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void bw_start_code(BitWriter *b, uint8_t code) {
    static const uint8_t sc[4] = {0, 0, 1, 0};
    uint8_t x[4];
    memcpy(x, sc, 4); x[3] = code;
    bw_bytes(b, x, 4);
}

static void bw_free(BitWriter *b) { free(b->data); memset(b, 0, sizeof(*b)); }

/* MPEG-2 macroblock_address_increment for 1 is VLC "1". */
static void bw_mba1(BitWriter *b) { bw_bits(b, 1, 1); }

/* MPEG-2 macroblock_address_increment VLCs for the small counts we use. */
static void bw_mba_inc(BitWriter *b, int n) {
    if (n == 1) bw_bits(b, 1, 1);
    else if (n == 2) bw_bits(b, 3, 3);    /* 011 */
    else if (n == 3) bw_bits(b, 2, 3);    /* 010 */
    else if (n == 4) bw_bits(b, 3, 4);    /* 0011 */
    else { fprintf(stderr, "MBA increment %d not implemented\n", n); abort(); }
}

/* I macroblock type I = "1". */
static void bw_mb_intra(BitWriter *b) { bw_bits(b, 1, 1); }

/*
 * P macroblock types used here.  The five-bit index is the raw lookahead used
 * by MB_P.  We use:
 *   index 8..15  : MC, VLC "01" (table entries 4..7? see below)
 *   index 16..31 : MC|CODED, VLC "1"
 * Explicit codes are supplied by callers through p_mb_mc()/p_mb_mc_coded().
 */
static void bw_p_mb_mc(BitWriter *b) { bw_bits(b, 1, 3); }       /* "001": P MC */
static void bw_p_mb_mc_coded(BitWriter *b) { bw_bits(b, 1, 1); } /* "1" */

/* motion_code zero is "1"; dmvector 0 is "00", +1 "01", -1 "11". */
static void bw_mv_zero(BitWriter *b) { bw_bits(b, 1, 1); }
static void bw_dmv(BitWriter *b, int d) {
    if (d > 0) bw_bits(b, 2, 2);  /* "10" -> +1 */
    else if (d < 0) bw_bits(b, 3, 2); /* "11" -> -1 */
    else bw_bits(b, 0, 1);        /* "0" -> 0 */
}

/* Encode one half-pel-unit motion delta from a zero predictor, f_code=1. */
static void bw_mv_delta(BitWriter *b, int delta) {
    int mag = abs(delta), sign = delta < 0;
    /*
     * MPEG-2 motion_code VLC, followed by sign and one f_code residual bit.
     * The residual makes each code pair cover two adjacent magnitudes.
     */
    if (mag >= 1 && mag <= 2) {
        bw_bits(b, 0x1, 2);    /* "01", motion_code 1 */
        bw_bits(b, sign, 1);
        bw_bits(b, mag - 1, 1);
    } else if (mag >= 3 && mag <= 4) {
        bw_bits(b, 0x1, 3);    /* "001", motion_code 2 */
        bw_bits(b, sign, 1);
        bw_bits(b, mag - 3, 1);
    } else if (mag >= 5 && mag <= 6) {
        bw_bits(b, 0x1, 4);    /* "0001", motion_code 3? table delta 2 */
        bw_bits(b, sign, 1);
        bw_bits(b, mag - 5, 1);
    } else {
        fprintf(stderr, "unsupported test MV magnitude %d\n", mag);
        abort();
    }
}

/* CBP=0 is a nine-bit VLC: "010000000" per CBP_9 first entry. */
static void bw_cbp_zero(BitWriter *b) { bw_bits(b, 0x80, 9); }

/* DC coefficient size/luma tables.  Values chosen are small signed DC diffs. */
static void bw_luma_dc(BitWriter *b, int diff) {
    int n = 0, v;
    uint32_t code;
    int len;
    if (diff == 0) { bw_bits(b, 4, 3); return; } /* size 0 "100" */
    v = abs(diff);
    while ((1 << n) <= v) n++;
    if (n == 1) { code = 0; len = 2; }       /* "00" */
    else if (n == 2) { code = 1; len = 2; }  /* "01" */
    else if (n == 3) { code = 5; len = 3; } /* "101" */
    else if (n == 4) { code = 6; len = 3; }  /* "110" */
    else { code = (1u << (n - 1)) - 2; len = n - 1; }
    bw_bits(b, code, len);
    if (diff > 0) bw_bits(b, diff, n);
    else bw_bits(b, (1u << n) - 1 - v, n);
}

static void bw_chroma_dc(BitWriter *b, int diff) {
    int n = 0, v;
    uint32_t code;
    int len;
    if (diff == 0) { bw_bits(b, 0, 2); return; } /* size 0 "00" */
    v = abs(diff);
    while ((1 << n) <= v) n++;
    if (n == 1) { code = 1; len = 2; } /* "01" */
    else if (n == 2) { code = 2; len = 2; } /* "10" */
    else { code = (1u << n) - 2; len = n; } /* 110, 1110, ... */
    bw_bits(b, code, len);
    if (diff > 0) bw_bits(b, diff, n);
    else bw_bits(b, (1u << n) - 1 - v, n);
}

/* An intra 8x8 block with only DC and end_of_block ("10"). */
static void bw_intra_dc_block(BitWriter *b, int dc, int luma) {
    if (luma) bw_luma_dc(b, dc);
    else bw_chroma_dc(b, dc);
    bw_bits(b, 2, 2); /* EOB */
}

/* ----------------------------------------------------------------------- */
/* Stream plans                                                            */
/* ----------------------------------------------------------------------- */

enum { PLANE_Y = 0, PLANE_U = 1, PLANE_V = 2 };
enum { PIC_I = 1, PIC_P = 2 };
enum { STRUCT_TOP = 1, STRUCT_BOTTOM = 2, STRUCT_FRAME = 3 };
enum {
    MB_SKIP = 0,
    MB_INTRA_DC = 1,
    MB_FIELD = 2,
    MB_FRAME = 3,
    MB_16X8 = 4,
    MB_DUALPRIME = 5
};

#define W_MB 4
#define H_MB 4
#define PW 64
#define PH 64
#define CW 32
#define CH 32
#define NMB (W_MB * H_MB)
#define FIELD_MB_ROWS (H_MB / 2)
#define FIELD_NMB (W_MB * FIELD_MB_ROWS)

typedef struct {
    int kind;
    int ref_id;          /* model picture identity selected by field_select 0/1 mapping */
    int ref_id2;         /* second half / second dual-prime route if needed */
    int fx[2], fy[2];    /* final motion vectors in half-luma samples, per half/predictor */
    int dm_x, dm_y;      /* dmvector integer samples */
    int field_sel[2];    /* raw field_select bits */
    int dct_dc[6];       /* intra DC values if MB_INTRA_DC */
} MBPlan;

typedef struct {
    int id;
    int coding;
    int structure;
    int top_field_first;
    int frame_pred_frame_dct;
    int progressive_sequence;
    int low_delay;
    MBPlan mb[NMB];
} PicPlan;

typedef struct {
    int npics;
    PicPlan pics[12];
} StreamPlan;

/* ----------------------------------------------------------------------- */
/* Independent reference world and expected raster                         */
/* ----------------------------------------------------------------------- */

typedef struct {
    int id;
    uint8_t y[PW * PH], u[CW * CH], v[CW * CH];
} RefPic;

typedef struct {
    RefPic refs[12];
    int nref;
    /* Separate storage for the first decoded field of the current picture. */
    uint8_t first_y[PW * PH], first_u[CW * CH], first_v[CW * CH];
    int first_valid, first_parity;
} LegacyWorld;

static uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

static RefPic *world_ref(LegacyWorld *w, int id) {
    int i;
    for (i = 0; i < w->nref; ++i) if (w->refs[i].id == id) return &w->refs[i];
    fprintf(stderr, "model missing ref id %d\n", id);
    abort();
}

/*
 * Reference texture is intentionally DC-on-8x8 blocks.  The formula puts
 * different ramps in every block and makes parity obvious: even frame rows
 * and odd frame rows differ by a fixed signature.
 */
static void make_ref_texture(RefPic *r, int seed) {
    int y, x, by, bx;
    r->id = seed;
    for (y = 0; y < PH; ++y) for (x = 0; x < PW; ++x) {
        bx = x >> 3; by = y >> 3;
        r->y[y * PW + x] = clamp8(28 + 22 * ((seed + 3 * by + bx) & 7) +
                                  (y & 1) * 9 + ((x & 7) == 0) * 5);
    }
    for (y = 0; y < CH; ++y) for (x = 0; x < CW; ++x) {
        bx = x >> 3; by = y >> 3;
        r->u[y * CW + x] = clamp8(90 + 18 * ((seed + 2 * by + bx + 1) & 7) +
                                  (y & 1) * 7);
        r->v[y * CW + x] = clamp8(140 + 14 * ((seed + by + 2 * bx + 2) & 7) +
                                  (y & 1) * 5);
    }
}

/* Half-sample interpolation matching MPEG-2's averaging/rounding rules. */
static uint8_t half_sample(const uint8_t *p, int stride, int x, int y,
                           int hx, int hy) {
    int a = p[y * stride + x];
    int b = hx ? p[y * stride + x + 1] : a;
    int c = hy ? p[(y + 1) * stride + x] : a;
    int d = (hx && hy) ? p[(y + 1) * stride + x + 1] : c;
    if (!hx && !hy) return (uint8_t)a;
    if (hx && !hy) return (uint8_t)((a + b + 1) >> 1);
    if (!hx && hy) return (uint8_t)((a + c + 1) >> 1);
    return (uint8_t)((a + b + c + d + 2) >> 2);
}

static uint8_t avg2(uint8_t a, uint8_t b) { return (uint8_t)((a + b + 1) >> 1); }

/*
 * Address a field as parity (0=top/even frame rows, 1=bottom/odd), row within
 * that field, and column.  The stored frame has ordinary PW stride; production
 * field pointers use doubled stride plus parity offset.
 */
static const uint8_t *field_plane(const RefPic *r, int plane, int parity,
                                 int *stride, int *chroma) {
    if (plane == PLANE_Y) { *stride = PW * 2; *chroma = 0; return r->y + parity * PW; }
    if (plane == PLANE_U) { *stride = CW * 2; *chroma = 1; return r->u + parity * CW; }
    *stride = CW * 2; *chroma = 1; return r->v + parity * CW;
}

static uint8_t sample_ref_field(const RefPic *r, int plane, int parity,
                                int col, int field_row, int mvx, int mvy) {
    int dstride, chroma;
    const uint8_t *base = field_plane(r, plane, parity, &dstride, &chroma);
    int pos_x = 2 * col + mvx;
    int pos_y = field_row + mvy; /* field MV vertical unit is half field row? see callers */
    int sx = pos_x >> 1;
    int sy = pos_y >> 1;
    int hx = pos_x & 1;
    int hy = pos_y & 1;
    int fstride = chroma ? CW : PW;
    /* base already has parity offset; interpolation rows are doubled. */
    return half_sample(base, fstride * 2, sx, sy, hx, hy);
}

/* ----------------------------------------------------------------------- */
/* ES encoding                                                             */
/* ----------------------------------------------------------------------- */

static void write_headers(BitWriter *bw, const PicPlan *p) {
    /* sequence header: 32x32, pel aspect 1, bitrate marker, vbv, constrained. */
    bw_start_code(bw, 0xb3);
    bw_bits(bw, PW, 12);       /* coded width */
    bw_bits(bw, PH, 12);       /* coded height */
    bw_bits(bw, 1, 4);       /* square pel */
    bw_bits(bw, 3, 4);       /* frame_rate code */
    bw_bits(bw, 0x3ffff, 18);/* bitrate */
    bw_bits(bw, 1, 1);       /* marker */
    bw_bits(bw, 0x3ff, 10);  /* vbv */
    bw_bits(bw, 0, 2);       /* no matrices */
    bw_align(bw);

    /* sequence extension, 4:2:0, interlaced according to first picture. */
    bw_start_code(bw, 0xb5);
    bw_bits(bw, 0x1, 4);       /* extension id sequence */
    bw_bits(bw, 0x48, 8);      /* profile/level SP@ML-ish */
    bw_bits(bw, p->progressive_sequence ? 1 : 0, 1); /* progressive_sequence */
    bw_bits(bw, 1, 2);         /* chroma 4:2:0 */
    bw_bits(bw, 0, 2);         /* horizontal_size_extension */
    bw_bits(bw, 0, 2);         /* vertical_size_extension */
    bw_bits(bw, 0, 12);        /* bit_rate_extension */
    bw_bits(bw, 1, 1);         /* marker */
    bw_bits(bw, 0, 8);         /* vbv ext */
    bw_bits(bw, p->low_delay ? 1 : 0, 1);
    bw_bits(bw, 0, 2);         /* frame_rate_extension_n */
    bw_bits(bw, 0, 2);         /* frame_rate_extension_d */
    bw_align(bw);
}

static void write_picture_header(BitWriter *bw, const PicPlan *p, int temporal) {
    bw_start_code(bw, 0x00);
    bw_bits(bw, temporal & 0x3ff, 10);
    bw_bits(bw, p->coding, 3);
    bw_bits(bw, 0xffff, 16); /* vbv delay */
    if (p->coding == PIC_P) {
        bw_bits(bw, 1, 1); /* full_pel_forward */
        bw_bits(bw, 1, 3); /* forward_f_code */
    }
    bw_align(bw);

    bw_start_code(bw, 0xb5);
    bw_bits(bw, 0x8, 4); /* picture coding extension */
    /* Syntax f_code=2 makes libmpeg2's exponent f_code = value - 1 = 1. */
    bw_bits(bw, 2, 4);   /* f_code[0][0] */
    bw_bits(bw, 2, 4);   /* f_code[0][1] */
    bw_bits(bw, 2, 4);   /* f_code[1][0] */
    bw_bits(bw, 2, 4);   /* f_code[1][1] */
    bw_bits(bw, 0, 2);   /* intra DC precision 8 */
    bw_bits(bw, p->structure, 2);
    bw_bits(bw, p->top_field_first, 1);
    bw_bits(bw, p->frame_pred_frame_dct, 1);
    bw_bits(bw, 0, 1); /* concealment */
    bw_bits(bw, 0, 1); /* qscale type */
    bw_bits(bw, 0, 1); /* intra vlc */
    bw_bits(bw, 0, 1); /* alternate scan */
    bw_bits(bw, 0, 1); /* repeat_first_field */
    bw_bits(bw, 1, 1); /* chroma_420_type */
    bw_bits(bw, p->structure == STRUCT_FRAME ? 1 : 0, 1); /* progressive_frame */
    bw_bits(bw, 0, 1); /* composite display flag */
    bw_align(bw);
}

static void write_intra_mb(BitWriter *bw, const int diff[6]) {
    int i;
    bw_mb_intra(bw);
    /* No DCT type bit when frame_pred_frame_dct=1 or field picture. */
    for (i = 0; i < 6; ++i) bw_intra_dc_block(bw, diff[i], i < 4);
}

static void write_motion_vector(BitWriter *bw, int v) {
    if (v == 0) bw_mv_zero(bw);
    else bw_mv_delta(bw, v);
}

static void write_predicted_mb(BitWriter *bw, const PicPlan *p,
                              const MBPlan *m, int pmv[2][2]) {
    (void)p;
    if (m->kind == MB_DUALPRIME) {
        int dx = m->fx[0] - pmv[0][0];
        int dy = m->fy[0] - pmv[0][1];
        bw_p_mb_mc(bw); /* P MB type MC, motion_type follows in two bits */
        bw_bits(bw, 3, 2); /* motion_type dualprime */
        write_motion_vector(bw, dx);
        bw_dmv(bw, m->dm_x);
        write_motion_vector(bw, dy);
        bw_dmv(bw, m->dm_y);
        pmv[0][0] = pmv[1][0] = m->fx[0];
        pmv[0][1] = pmv[1][1] = m->fy[0];
        return;
    }
    if (m->kind == MB_FIELD) {
        int dx = m->fx[0] - pmv[0][0];
        int dy = m->fy[0] - pmv[0][1];
        bw_p_mb_mc(bw);
        bw_bits(bw, 1, 2); /* motion_type field */
        bw_bits(bw, m->field_sel[0], 1);
        write_motion_vector(bw, dx);
        write_motion_vector(bw, dy);
        pmv[0][0] = pmv[1][0] = m->fx[0];
        pmv[0][1] = pmv[1][1] = m->fy[0];
        return;
    }
    if (m->kind == MB_16X8) {
        int h;
        bw_p_mb_mc(bw);
        bw_bits(bw, 2, 2); /* motion_type 16x8 */
        for (h = 0; h < 2; ++h) {
            int dx = m->fx[h] - pmv[h][0];
            int dy = m->fy[h] - pmv[h][1];
            bw_bits(bw, m->field_sel[h], 1);
            write_motion_vector(bw, dx);
            write_motion_vector(bw, dy);
            pmv[h][0] = m->fx[h];
            pmv[h][1] = m->fy[h];
        }
        return;
    }
    if (m->kind == MB_FRAME) {
        bw_p_mb_mc(bw);
        if (!p->frame_pred_frame_dct) bw_bits(bw, 2, 2); /* frame motion_type */
        write_motion_vector(bw, m->fx[0]);
        write_motion_vector(bw, m->fy[0]);
        pmv[0][0] = pmv[1][0] = m->fx[0];
        pmv[0][1] = pmv[1][1] = m->fy[0];
        return;
    }
    abort();
}

static void write_slice(BitWriter *bw, const PicPlan *p, int mb_start,
                        int count, int slice_code) {
    int i, k;
    int pred_y = 0, pred_u = 0, pred_v = 0;
    int pmv[2][2] = {{0, 0}, {0, 0}};
    bw_start_code(bw, (uint8_t)slice_code);
    bw_bits(bw, 8, 5); /* quantizer_scale_code */
    bw_bits(bw, 0, 1); /* extra_bit_slice terminator */
    bw_mba1(bw);
    for (k = 0; k < count; ++k) {
        i = mb_start + k;
        if (p->mb[i].kind == MB_SKIP) {
            /* End slice immediately before this MB; P skips are implicit. */
            break;
        }
        if (p->coding == PIC_I || p->mb[i].kind == MB_INTRA_DC) {
            int diff[6], j;
            /* DC predictors continue through intra blocks in a slice. */
            for (j = 0; j < 4; ++j) {
                diff[j] = p->mb[i].dct_dc[j] - pred_y;
                pred_y = p->mb[i].dct_dc[j];
            }
            diff[4] = p->mb[i].dct_dc[4] - pred_u;
            pred_u = p->mb[i].dct_dc[4];
            diff[5] = p->mb[i].dct_dc[5] - pred_v;
            pred_v = p->mb[i].dct_dc[5];
            pmv[0][0] = pmv[0][1] = pmv[1][0] = pmv[1][1] = 0;
            write_intra_mb(bw, diff);
        } else {
            /* A predicted MB resets intra DC predictors in production. */
            pred_y = pred_u = pred_v = 0;
            write_predicted_mb(bw, p, &p->mb[i], pmv);
        }
        if (k + 1 < count) {
            if (p->mb[i + 1].kind == MB_SKIP) {
                /* Address to end-of-row; production fills skipped MBs. */
                bw_mba_inc(bw, count - k);
                break;
            }
            bw_mba1(bw);
        }
    }
    bw_align(bw);
}

static void build_es(const StreamPlan *sp, uint8_t **out, size_t *out_len) {
    BitWriter bw = {0};
    int i, temporal = 0;
    for (i = 0; i < sp->npics; ++i) {
        const PicPlan *p = &sp->pics[i];
        if (i == 0) write_headers(&bw, p);
        write_picture_header(&bw, p, temporal++);
        if (p->structure == STRUCT_FRAME) {
            int row;
            for (row = 0; row < H_MB; ++row)
                write_slice(&bw, p, row * W_MB, W_MB, row + 1);
        } else {
            int row;
            /* Field slice rows are 16 field lines apart. */
            for (row = 0; row < FIELD_MB_ROWS; ++row)
                write_slice(&bw, p, row * W_MB, W_MB, row + 1);
        }
    }
    bw_start_code(&bw, 0xb7);
    bw_align(&bw);
    *out = bw.data; *out_len = bw.len;
}

/* ----------------------------------------------------------------------- */
/* Public decode harness and content comparison                            */
/* ----------------------------------------------------------------------- */

typedef struct {
    uint8_t y[PW * PH], u[CW * CH], v[CW * CH];
    int valid;
    int parity;
    int second_field;
    unsigned pic_flags;
    unsigned nb_fields;
} DecodedPic;

typedef struct {
    DecodedPic pics[16];
    int count;
    DecodedPic first[16];
    int first_count;
    int states_seen[128];
    int nstates;
    char arrival[64];
    char display_order[64];
    int arrival_n;
    int display_n;
} DecodeResult;

static void append_char(char *s, int *n, int cap, char c) {
    if (*n < cap - 1) { s[(*n)++] = c; s[*n] = 0; }
}

static int parity_char(mpeg2dec_t *d) {
    if (d->decoder.picture_structure == TOP_FIELD) return 'T';
    if (d->decoder.picture_structure == BOTTOM_FIELD) return 'B';
    return 'F';
}

static void copy_pic(DecodedPic *q, const mpeg2_fbuf_t *f, mpeg2dec_t *d) {
    memset(q, 0, sizeof(*q));
    memcpy(q->y, f->buf[0], PW * PH);
    memcpy(q->u, f->buf[1], CW * CH);
    memcpy(q->v, f->buf[2], CW * CH);
    q->valid = 1;
    q->parity = d->decoder.picture_structure;
    q->second_field = d->decoder.second_field;
    if (d->info.current_picture) {
        q->pic_flags = d->info.current_picture->flags;
        q->nb_fields = d->info.current_picture->nb_fields;
    }
}

static int decode_es_chunks(const uint8_t *es, size_t len, int chunk_mode,
                            DecodeResult *r) {
    mpeg2_accel(0);
    mpeg2dec_t *d = mpeg2_init();
    const mpeg2_info_t *info;
    size_t pos = 0;
    if (!d) return 1;
    info = mpeg2_info(d);
    memset(r, 0, sizeof(*r));

    while (1) {
        mpeg2_state_t st = mpeg2_parse(d);
        if (st >= 0 && st < 128 && r->nstates < 128)
            r->states_seen[r->nstates++] = st;
        if (st == STATE_BUFFER) {
            size_t n;
            if (pos >= len) {
                mpeg2_buffer(d, (uint8_t *)es + len, (uint8_t *)es + len);
                continue;
            }
            if (chunk_mode == 0) {
                n = len - pos;
            } else if (chunk_mode == 1) {
                n = 1;
            } else {
                static const size_t pat[] = {1, 3, 5, 2, 8, 1, 13, 4, 6, 2};
                n = pat[(pos / 5) % (sizeof(pat)/sizeof(pat[0]))];
                if (n > len - pos) n = len - pos;
            }
            mpeg2_buffer(d, (uint8_t *)es + pos, (uint8_t *)es + pos + n);
            pos += n;
            continue;
        }
        if (st == STATE_SLICE_1ST && info->current_fbuf && r->first_count < 16) {
            DecodedPic *q = &r->first[r->first_count++];
            copy_pic(q, info->current_fbuf, d);
            append_char(r->arrival, &r->arrival_n, sizeof r->arrival,
                        parity_char(d));
        }
        if (st == STATE_SLICE) {
            const mpeg2_fbuf_t *f = info->current_fbuf ?
                                    info->current_fbuf : info->display_fbuf;
            if (f && r->count < 16) {
                DecodedPic *q = &r->pics[r->count++];
                copy_pic(q, f, d);
                if (d->decoder.picture_structure == FRAME_PICTURE) {
                    append_char(r->display_order, &r->display_n,
                                sizeof r->display_order, 'F');
                    append_char(r->arrival, &r->arrival_n, sizeof r->arrival,
                                'F');
                } else {
                    /* One completed field pair is one display frame; record
                     * its two-field presentation order separately. */
                    if (d->decoder.top_field_first) {
                        append_char(r->display_order, &r->display_n,
                                    sizeof r->display_order, 'T');
                        append_char(r->display_order, &r->display_n,
                                    sizeof r->display_order, 'B');
                    } else {
                        append_char(r->display_order, &r->display_n,
                                    sizeof r->display_order, 'B');
                        append_char(r->display_order, &r->display_n,
                                    sizeof r->display_order, 'T');
                    }
                    append_char(r->arrival, &r->arrival_n, sizeof r->arrival,
                                parity_char(d));
                }
            }
        }
        if (st == STATE_END || st == STATE_INVALID || st == STATE_INVALID_END)
            break;
    }
    mpeg2_close(d);
    return 0;
}

static int first_diff_plane(const char *who, const uint8_t *got,
                            const uint8_t *want, int w, int h,
                            char *msg, size_t msglen) {
    int y, x;
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        int i = y * w + x;
        if (got[i] != want[i]) {
            snprintf(msg, msglen,
                     "%s first pixel diff at x=%d y=%d field=%s got=%u want=%u",
                     who, x, y, (y & 1) ? "bottom" : "top", got[i], want[i]);
            return i + 1;
        }
    }
    return 0;
}

static int check_pic_bytes(const char *identity, const DecodedPic *got,
                           const uint8_t *ey, const uint8_t *eu,
                           const uint8_t *ev) {
    char msg[256];
    int bad;
    if (!got->valid) { fprintf(stderr, "%s: picture not decoded\n", identity); return 0; }
    bad = first_diff_plane(identity, got->y, ey, PW, PH, msg, sizeof msg);
    if (bad) { fprintf(stderr, "PIXEL_DIFF picture=%s plane=Y %s\n", identity, msg); return 0; }
    bad = first_diff_plane(identity, got->u, eu, CW, CH, msg, sizeof msg);
    if (bad) { fprintf(stderr, "PIXEL_DIFF picture=%s plane=Cb %s\n", identity, msg); return 0; }
    bad = first_diff_plane(identity, got->v, ev, CW, CH, msg, sizeof msg);
    if (bad) { fprintf(stderr, "PIXEL_DIFF picture=%s plane=Cr %s\n", identity, msg); return 0; }
    return 1;
}

/* The callbacks are the case bodies below; each returns 0 on success. */
static int run_frame_dual_cases(void);
static int run_field_pair_cases(void);
static int run_predictor_cases(void);
static int run_mutation_cases(void);

const struct task_case task_cases[] = {
    {"frame_dual", "derived-cross-field-vectors", run_frame_dual_cases},
    {"field_pair", "arrival-order-and-current-first-field", run_field_pair_cases},
    {"predictors", "field-vs-16x8-continuity-slice-reset", run_predictor_cases},
    {"mutations", "isolated-source-degeneracies-fail-on-pixels", run_mutation_cases},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

/* Remaining model/case implementation is appended below. */

/* ----------------------------------------------------------------------- */
/* Deterministic DC texture and independent field-address model             */
/* ----------------------------------------------------------------------- */

typedef DecodedPic ModelFrame;

static int luma_code(int seed, int parity, int mb, int block) {
    int base = (parity == TOP_FIELD ? -12 : 4) + 3 * mb + seed;
    return base + 9 * block;
}
static int chroma_code(int seed, int plane, int parity) {
    static const int tab[2][2] = { { -15, 14 }, { 15, -14 } };
    return tab[plane == PLANE_V][parity == BOTTOM_FIELD] + seed;
}

static void set_reference_mb(MBPlan *m, int seed, int parity, int mb) {
    int c0 = luma_code(seed, parity, mb, 0);
    int c1 = luma_code(seed, parity, mb, 1);
    int c2 = luma_code(seed, parity, mb, 2);
    int c3 = luma_code(seed, parity, mb, 3);
    memset(m, 0, sizeof(*m));
    m->kind = MB_INTRA_DC;
    m->dct_dc[0] = c0;
    m->dct_dc[1] = c1;
    m->dct_dc[2] = c2;
    m->dct_dc[3] = c3;
    m->dct_dc[4] = chroma_code(seed, PLANE_U, parity);
    m->dct_dc[5] = chroma_code(seed, PLANE_V, parity);
}

static void reference_field_plan(PicPlan *p, int parity, int seed) {
    int mb;
    memset(p, 0, sizeof(*p));
    p->id = 100 + seed * 4 + parity;
    p->coding = PIC_I;
    p->structure = parity;
    p->low_delay = 1;
    p->progressive_sequence = 0;
    p->top_field_first = (parity == TOP_FIELD);
    for (mb = 0; mb < FIELD_NMB; ++mb)
        set_reference_mb(&p->mb[mb], seed, parity, mb);
}

static void make_reference_model(ModelFrame *f, int seed) {
    int parity, mb, bx, by, kx, kv, b, x, field_row, frame_y;
    memset(f, 0, sizeof(*f));
    f->valid = 1;
    for (parity = TOP_FIELD; parity <= BOTTOM_FIELD; ++parity) {
        int p = parity - 1; /* TOP=1 -> 0, BOTTOM=2 -> 1 frame-row parity */
        for (mb = 0; mb < FIELD_NMB; ++mb) {
            int mbx = mb % W_MB, mby = mb / W_MB;
            for (kv = 0; kv < 2; ++kv) for (kx = 0; kx < 2; ++kx) {
                int block = kv * 2 + kx; /* encoder order: TL TR BL BR */
                int code = luma_code(seed, parity, mb, block);
                uint8_t val = clamp8(128 + code);
                bx = 8 * (2 * mbx + kx);
                by = 2 * mby + kv; /* field 8-row block index */
                for (b = 0; b < 8; ++b) {
                    int fr = 8 * by + b;
                    frame_y = p + 2 * fr;
                    for (x = 0; x < 8; ++x)
                        f->y[frame_y * PW + bx + x] = val;
                }
            }
            for (b = 0; b < 8; ++b) {
                int cfr = 8 * mby + b;
                int cy = p + 2 * cfr;
                int cx0 = 8 * mbx;
                uint8_t u = clamp8(128 + chroma_code(seed, PLANE_U, parity));
                uint8_t v = clamp8(128 + chroma_code(seed, PLANE_V, parity));
                for (x = 0; x < 8; ++x) {
                    f->u[cy * CW + cx0 + x] = u;
                    f->v[cy * CW + cx0 + x] = v;
                }
            }
        }
    }
}

static uint8_t sample_frame_rows(const uint8_t *plane, int frame_stride,
                                 int start_row, int col, int field_step,
                                 int k, int hx, int hy) {
    const uint8_t *base = plane + start_row * frame_stride;
    return half_sample(base, frame_stride * 2, col, k * field_step, hx, hy);
}

/* Explicit floor(z/2), including negative odd z. */
static int floor2(int z) { return z >= 0 ? z / 2 : -((-z + 1) / 2); }

static uint8_t sample_main_route(const uint8_t *plane, int stride,
                                 int mbx, int mby, int mx, int my, int p,
                                 int i, int k) {
    int pos_x = 32 * mbx + 2 * i + 2 * 0 + mx; /* i added through column below */
    int pos_y = 16 * mby + my;
    int sx = (pos_x >> 1);
    int hx = pos_x & 1;
    int start = (pos_y & ~1) + p;
    int hy = pos_y & 1;
    (void)pos_x;
    /* The MC routine advances one source column per destination i. */
    return sample_frame_rows(plane, stride, start, (2 * (16 * mbx) + mx) / 2 + i
                             + ((2 * (16 * mbx) + mx) < 0 && (mx & 1) ? 0 : 0),
                             1, k, hx, hy);
}

static void render_frame_dualprime_mb(ModelFrame *dst, const ModelFrame *ref,
                                      int mb_index, const MBPlan *m,
                                      int top_field_first) {
    int mbx = mb_index % W_MB, mby = mb_index / W_MB;
    int mx = m->fx[0], my = m->fy[0], dmx = m->dm_x, dmy = m->dm_y;
    int i, j;
    /* Start from the ordinary zero-MV frame copy for this MB. */
    for (j = 0; j < 16; ++j) {
        int y = 16 * mby + j;
        for (i = 0; i < 16; ++i) {
            int x = 16 * mbx + i;
            int p = y & 1, k = j >> 1;
            int main_s, coef, ox, oy, posx, posy, sx, start, hy;
            uint8_t a, b;
            int base_x = 2 * (16 * mbx) + mx;
            main_s = sample_frame_rows(ref->y, PW,
                                       ((16 * mby + my) & ~1) + p,
                                       (base_x >> 1) + i, 1, k,
                                       base_x & 1, (16 * mby + my) & 1);
            coef = (p == 0) ? (top_field_first ? 1 : 3)
                            : (top_field_first ? 3 : 1);
            ox = floor2(coef * mx + (mx > 0)) + dmx;
            oy = floor2(coef * my + (my > 0)) + dmy + (p == 0 ? -1 : 1);
            posx = 2 * (16 * mbx) + ox;
            posy = 16 * mby + oy;
            sx = (posx >> 1) + i;
            if (p == 0) start = posy | 1;
            else start = posy & ~1;
            hy = posy & 1;
            b = sample_frame_rows(ref->y, PW, start, sx, 1, k,
                                  posx & 1, hy);
            a = avg2((uint8_t)main_s, b);
            dst->y[y * PW + x] = a;
        }
    }
    /* 4:2:0 chroma is uniform per reference field, so both cross-field
     * routes reduce to the same average.  Keep the address split explicit. */
    for (int plane = 1; plane <= 2; ++plane) {
        const uint8_t *src = plane == 1 ? ref->u : ref->v;
        uint8_t top = src[0], bottom = src[PW < CW ? 0 : CW]; /* replaced below */
        uint8_t avg;
        (void)top; (void)bottom;
        top = plane == 1 ? clamp8(128 + chroma_code(0, PLANE_U, TOP_FIELD))
                         : clamp8(128 + chroma_code(0, PLANE_V, TOP_FIELD));
        bottom = plane == 1 ? clamp8(128 + chroma_code(0, PLANE_U, BOTTOM_FIELD))
                            : clamp8(128 + chroma_code(0, PLANE_V, BOTTOM_FIELD));
        avg = avg2(top, bottom);
        for (j = 0; j < 8; ++j)
            for (i = 0; i < 8; ++i) {
                int x = 8 * mbx + i, y = 8 * mby + j;
                if (plane == 1) dst->u[y * CW + x] = avg;
                else dst->v[y * CW + x] = avg;
            }
    }
}

static void copy_model_frame(ModelFrame *d, const ModelFrame *s) { *d = *s; }

static void render_frame_dual_case(ModelFrame *out, const ModelFrame *ref,
                                   const MBPlan *target, int target_mb,
                                   int tff) {
    int i;
    copy_model_frame(out, ref);
    /* Non-target MBs are explicit zero frame MVs, hence exact copies. */
    render_frame_dualprime_mb(out, ref, target_mb, target, tff);
    (void)i;
}

static void frame_dual_stream(StreamPlan *sp, int tff, const MBPlan *target,
                              int target_mb) {
    memset(sp, 0, sizeof(*sp));
    sp->npics = 3;
    reference_field_plan(&sp->pics[0], TOP_FIELD, 0);
    reference_field_plan(&sp->pics[1], BOTTOM_FIELD, 0);
    PicPlan *p = &sp->pics[2];
    int mb;
    memset(p, 0, sizeof(*p));
    p->id = 77; p->coding = PIC_P; p->structure = STRUCT_FRAME;
    p->low_delay = 1; p->progressive_sequence = 0;
    p->top_field_first = tff; p->frame_pred_frame_dct = 0;
    for (mb = 0; mb < NMB; ++mb) {
        p->mb[mb].kind = MB_FRAME;
        p->mb[mb].fx[0] = 0; p->mb[mb].fy[0] = 0;
    }
    p->mb[target_mb] = *target;
    p->mb[target_mb].kind = MB_DUALPRIME;
    for (mb = target_mb + 1; mb < (target_mb / W_MB + 1) * W_MB; ++mb)
        p->mb[mb].kind = MB_SKIP;
}

/* ----------------------------------------------------------------------- */
/* Field-picture motion model                                              */
/* ----------------------------------------------------------------------- */

typedef struct {
    ModelFrame prev;
    ModelFrame first;
    int first_parity;
    int have_first;
} FieldWorld;

static uint8_t sample_field_luma(const ModelFrame *src, int sel_parity,
                                int mbx, int mby, int mvx, int mvy,
                                int half8, int col, int row_in_call) {
    int parity01 = sel_parity == BOTTOM_FIELD ? 1 : 0;
    int pos_x = 32 * mbx + mvx;
    int pos_y = 32 * mby + mvy + 16 * half8;
    int sx = pos_x >> 1;
    int sy = pos_y >> 1;
    int hx = pos_x & 1;
    int hy = pos_y & 1;
    const uint8_t *base = src->y + parity01 * PW;
    /* col is destination column 0..15; row_in_call is MC row 0..15/7. */
    return half_sample(base, 2 * PW, sx + col, sy + row_in_call, hx, hy);
}

static uint8_t field_chroma_const(const FieldWorld *w, int source,
                                  int plane, int sel_parity,
                                  int mbx, int chroma_field_row) {
    const ModelFrame *f = source == 2 ? &w->first : &w->prev;
    int parity01 = sel_parity == BOTTOM_FIELD ? 1 : 0;
    int frame_row = parity01 + 2 * chroma_field_row;
    const uint8_t *p = plane == PLANE_U ? f->u : f->v;
    return p[frame_row * CW + 8 * mbx];
}

static const ModelFrame *route_source(const FieldWorld *w, int source) {
    return source == 2 ? &w->first : &w->prev;
}

/*
 * Render one field MB.  current_parity is TOP_FIELD/BOTTOM_FIELD.  For a
 * second non-B P field, the opposite-parity route has source=2 (current
 * reconstructed first field); same parity always has source=1 (last frame).
 */
static void render_field_mb(ModelFrame *out, const FieldWorld *w,
                           int current_parity, int second_field,
                           int mb_index, const MBPlan *m) {
    int mbx = mb_index % W_MB, mby = mb_index / W_MB;
    int c01 = current_parity == BOTTOM_FIELD;
    int i, j, h;
    (void)second_field;

    if (m->kind == MB_FIELD || m->kind == MB_DUALPRIME) {
        int sel = m->field_sel[0];
        int sel_parity = sel ? BOTTOM_FIELD : TOP_FIELD;
        int source = m->ref_id ? m->ref_id : 1;
        if (m->kind == MB_DUALPRIME) {
            /* Main route always uses same-parity previous-field storage. */
            sel_parity = current_parity;
            source = 1;
        }
        for (j = 0; j < 16; ++j) for (i = 0; i < 16; ++i) {
            uint8_t val = sample_field_luma(route_source(w, source),
                                            sel_parity, mbx, mby,
                                            m->fx[0], m->fy[0], 0, i, j);
            if (m->kind == MB_DUALPRIME) {
                int opp_parity = current_parity == TOP_FIELD ?
                                 BOTTOM_FIELD : TOP_FIELD;
                int opp_source = second_field ? 2 : 1;
                int ox = floor2(m->fx[0] + (m->fx[0] > 0)) + m->dm_x;
                int oy = floor2(m->fy[0] + (m->fy[0] > 0)) + m->dm_y +
                         (current_parity == BOTTOM_FIELD ? 1 : -1);
                uint8_t d = sample_field_luma(route_source(w, opp_source),
                                              opp_parity, mbx, mby,
                                              ox, oy, 0, i, j);
                val = avg2(val, d);
            }
            int frame_y = c01 + 2 * (16 * mby + j);
            int frame_x = 16 * mbx + i;
            out->y[frame_y * PW + frame_x] = val;
        }
        /* Uniform chroma fields: average the two dual-prime routes. */
        for (int plane = 1; plane <= 2; ++plane) {
            uint8_t val = field_chroma_const(w, source, plane, sel_parity,
                                             mbx, 8 * mby);
            if (m->kind == MB_DUALPRIME) {
                int opp = current_parity == TOP_FIELD ? BOTTOM_FIELD : TOP_FIELD;
                int opp_source = second_field ? 2 : 1;
                uint8_t d = field_chroma_const(w, opp_source, plane, opp,
                                               mbx, 8 * mby);
                val = avg2(val, d);
            }
            for (j = 0; j < 8; ++j) for (i = 0; i < 8; ++i) {
                int cy = c01 + 2 * (8 * mby + j);
                int cx = 8 * mbx + i;
                if (plane == 1) out->u[cy * CW + cx] = val;
                else out->v[cy * CW + cx] = val;
            }
        }
        return;
    }

    if (m->kind == MB_16X8) {
        for (h = 0; h < 2; ++h) {
            int sel_parity = m->field_sel[h] ? BOTTOM_FIELD : TOP_FIELD;
            int source = h == 0 ? (m->ref_id ? m->ref_id : 1)
                                : (m->ref_id2 ? m->ref_id2 : 1);
            for (j = 0; j < 8; ++j) for (i = 0; i < 16; ++i) {
                uint8_t val = sample_field_luma(route_source(w, source),
                                                sel_parity, mbx, mby,
                                                m->fx[h], m->fy[h], h, i, j);
                int field_row = 16 * mby + 8 * h + j;
                int frame_y = c01 + 2 * field_row;
                out->y[frame_y * PW + 16 * mbx + i] = val;
            }
            /* Four chroma rows belong to this 8-luma-row half. */
            uint8_t cv = field_chroma_const(w, source, PLANE_U, sel_parity,
                                            mbx, 8 * mby + 4 * h);
            uint8_t cr = field_chroma_const(w, source, PLANE_V, sel_parity,
                                            mbx, 8 * mby + 4 * h);
            for (j = 0; j < 4; ++j) for (i = 0; i < 8; ++i) {
                int cfield_row = 8 * mby + 4 * h + j;
                int cy = c01 + 2 * cfield_row;
                int cx = 8 * mbx + i;
                out->u[cy * CW + cx] = cv;
                out->v[cy * CW + cx] = cr;
            }
        }
        return;
    }
    abort();
}

static void render_field_picture(ModelFrame *out, const FieldWorld *w,
                                 int parity, int second_field,
                                 const PicPlan *p) {
    int mb;
    /* Caller initializes out to the assembled pair/first-field storage. */
    for (mb = 0; mb < FIELD_NMB; ++mb)
        render_field_mb(out, w, parity, second_field, mb, &p->mb[mb]);
}

static void field_pair_stream(StreamPlan *sp, int first_parity,
                              const PicPlan *first_p,
                              const PicPlan *second_p) {
    memset(sp, 0, sizeof(*sp));
    sp->npics = 4;
    reference_field_plan(&sp->pics[0], first_parity, 0);
    reference_field_plan(&sp->pics[1],
                         first_parity == TOP_FIELD ? BOTTOM_FIELD : TOP_FIELD, 0);
    sp->pics[2] = *first_p;
    sp->pics[2].structure = first_parity;
    sp->pics[2].coding = PIC_P;
    sp->pics[2].low_delay = 1;
    sp->pics[2].progressive_sequence = 0;
    sp->pics[2].top_field_first = (first_parity == TOP_FIELD);
    sp->pics[3] = *second_p;
    sp->pics[3].structure = first_parity == TOP_FIELD ? BOTTOM_FIELD : TOP_FIELD;
    sp->pics[3].coding = PIC_P;
    sp->pics[3].low_delay = 1;
    sp->pics[3].progressive_sequence = 0;
    sp->pics[3].top_field_first = (first_parity == TOP_FIELD);
}

static int run_frame_dual_cases(void) {
    struct frame_case {
        int tff, mx, my, dmx, dmy;
        const char *name;
    } cases[] = {
        {1,  1,  1,  0,  0, "tff-positive-odd-zero-dmv"},
        {0, -1,  1,  0,  0, "bff-negative-x-zero-dmv"},
        {1,  1, -1,  0,  0, "tff-negative-y-zero-dmv"},
        {0, -1, -1,  0,  0, "bff-negative-both-zero-dmv"},
        {1,  3,  1,  1,  1, "positive-odd3-dmv-plus"},
        {0, -3,  1, -1,  1, "negative-odd3-dmv-minus"},
        {1,  1,  3,  0, -1, "vertical-odd3-dmv-minus"},
        {0,  1, -3,  1,  0, "negative-vertical-odd3"},
        {1, -1,  3, -1,  1, "mixed-odd-dmv-values"},
    };
    size_t ci;
    for (ci = 0; ci < sizeof(cases)/sizeof(cases[0]); ++ci) {
        MBPlan m;
        StreamPlan sp;
        ModelFrame ref, want;
        uint8_t *es = NULL;
        size_t n = 0;
        int target_mb, mode;
        char identity[192];

        memset(&m, 0, sizeof m);
        m.fx[0] = cases[ci].mx;
        m.fy[0] = cases[ci].my;
        m.dm_x = cases[ci].dmx;
        m.dm_y = cases[ci].dmy;
        target_mb = (cases[ci].my < 0 ? W_MB : 0) +
                    (cases[ci].mx < 0 ? 2 : 1);
        frame_dual_stream(&sp, cases[ci].tff, &m, target_mb);
        /* The reference field pair is top-first for this frame group. */
        sp.pics[0].top_field_first = sp.pics[1].top_field_first = 1;
        build_es(&sp, &es, &n);
        make_reference_model(&ref, 0);
        render_frame_dual_case(&want, &ref, &m, target_mb, cases[ci].tff);
        snprintf(identity, sizeof identity,
                 "frame_dual/%s/tff=%d/mv=(%d,%d)/dm=(%d,%d)/mb=%d",
                 cases[ci].name, cases[ci].tff, m.fx[0], m.fy[0],
                 m.dm_x, m.dm_y, target_mb);

        for (mode = 0; mode < 3; ++mode) {
            DecodeResult dr;
            TASK_CHECK(decode_es_chunks(es, n, mode, &dr) == 0);
            TASK_CHECK(dr.count == 2);
            TASK_CHECK(dr.first_count == 1);
            TASK_CHECK(strcmp(dr.arrival, "TBF") == 0);
            snprintf(identity + strlen(identity),
                     sizeof identity - strlen(identity), "/chunk=%d", mode);
            TASK_CHECK(check_pic_bytes(identity, &dr.pics[0],
                                       ref.y, ref.u, ref.v) == 1);
            TASK_CHECK(check_pic_bytes(identity, &dr.pics[1],
                                       want.y, want.u, want.v) == 1);
        }
        free(es);
    }
    return 0;
}

static int check_field_bytes(const char *identity, const DecodedPic *got,
                             const ModelFrame *want, int parity) {
    char msg[256];
    int p01 = parity == BOTTOM_FIELD ? 1 : 0;
    int r, x;
    if (!got->valid) {
        fprintf(stderr, "%s: field not decoded\n", identity);
        return 0;
    }
    for (r = 0; r < PH / 2; ++r) {
        int y = p01 + 2 * r;
        for (x = 0; x < PW; ++x) {
            int i = y * PW + x;
            if (got->y[i] != want->y[i]) {
                snprintf(msg, sizeof msg,
                         "%s field=%s first Y diff at field_row=%d x=%d frame_y=%d got=%u want=%u",
                         identity, p01 ? "bottom" : "top", r, x, y,
                         got->y[i], want->y[i]);
                fprintf(stderr, "PIXEL_DIFF picture=%s plane=Y %s\n",
                        identity, msg);
                return 0;
            }
        }
    }
    for (r = 0; r < CH / 2; ++r) {
        int y = p01 + 2 * r;
        for (x = 0; x < CW; ++x) {
            int i = y * CW + x;
            if (got->u[i] != want->u[i]) {
                fprintf(stderr,
                        "PIXEL_DIFF picture=%s field=%s plane=Cb field_row=%d x=%d got=%u want=%u\n",
                        identity, p01 ? "bottom" : "top", r, x, got->u[i], want->u[i]);
                return 0;
            }
            if (got->v[i] != want->v[i]) {
                fprintf(stderr,
                        "PIXEL_DIFF picture=%s field=%s plane=Cr field_row=%d x=%d got=%u want=%u\n",
                        identity, p01 ? "bottom" : "top", r, x, got->v[i], want->v[i]);
                return 0;
            }
        }
    }
    return 1;
}

static void dual_all_field_plan(PicPlan *p) {
    int i;
    memset(p, 0, sizeof(*p));
    p->coding = PIC_P;
    for (i = 0; i < FIELD_NMB; ++i) {
        int mbx = i % W_MB;
        p->mb[i].kind = MB_DUALPRIME;
        p->mb[i].ref_id = 1;
        p->mb[i].fx[0] = (mbx == W_MB - 1) ? 0 : 1;
        p->mb[i].fy[0] = (i < W_MB) ? 1 : -1;
        p->mb[i].dm_y = 0;
    }
}

static void opposite_copy_plan(PicPlan *p, int current_parity) {
    int i;
    int c01 = current_parity == BOTTOM_FIELD;
    memset(p, 0, sizeof(*p));
    p->coding = PIC_P;
    for (i = 0; i < FIELD_NMB; ++i) {
        p->mb[i].kind = MB_FIELD;
        p->mb[i].field_sel[0] = c01 ? 0 : 1; /* opposite raw field parity */
        p->mb[i].ref_id = 2;                 /* just completed first field */
    }
}

static void same_copy_plan(PicPlan *p, int parity) {
    int i;
    memset(p, 0, sizeof(*p));
    p->coding = PIC_P;
    for (i = 0; i < FIELD_NMB; ++i) {
        p->mb[i].kind = MB_FIELD;
        p->mb[i].field_sel[0] = parity == BOTTOM_FIELD ? 1 : 0;
        p->mb[i].ref_id = 1; /* committed previous frame */
    }
}

static void field_chain_stream(StreamPlan *sp, int first_parity,
                               const PicPlan *p1, const PicPlan *p2) {
    PicPlan zero_first, zero_second;
    int second_parity = first_parity == TOP_FIELD ? BOTTOM_FIELD : TOP_FIELD;
    memset(sp, 0, sizeof(*sp));
    sp->npics = 6;
    reference_field_plan(&sp->pics[0], first_parity, 0);
    reference_field_plan(&sp->pics[1], second_parity, 0);
    sp->pics[2] = *p1;
    sp->pics[3] = *p2;
    same_copy_plan(&zero_first, first_parity);
    same_copy_plan(&zero_second, second_parity);
    sp->pics[4] = zero_first;
    sp->pics[5] = zero_second;
    for (int i = 0; i < 6; ++i) {
        PicPlan *q = &sp->pics[i];
        q->low_delay = 1;
        q->progressive_sequence = 0;
        q->top_field_first = (first_parity == TOP_FIELD);
        if (i >= 2) q->coding = PIC_P;
        q->structure = (i == 0 || i == 2 || i == 4) ? first_parity : second_parity;
    }
}

static int run_field_pair_cases(void) {
    int order;
    for (order = 0; order < 2; ++order) {
        int first_parity = order ? BOTTOM_FIELD : TOP_FIELD;
        int second_parity = order ? TOP_FIELD : BOTTOM_FIELD;
        PicPlan p1, p2;
        StreamPlan sp;
        ModelFrame ref, first_want, pair_want, next_want;
        FieldWorld w;
        uint8_t *es;
        size_t n;
        int mode;
        char identity[160];

        dual_all_field_plan(&p1);
        dual_all_field_plan(&p2);
        field_chain_stream(&sp, first_parity, &p1, &p2);
        build_es(&sp, &es, &n);
        make_reference_model(&ref, 0);

        first_want = ref;
        memset(&w, 0, sizeof w);
        w.prev = ref;
        render_field_picture(&first_want, &w, first_parity, 0, &p1);
        pair_want = first_want;
        w.first = first_want;
        w.first_parity = first_parity;
        w.have_first = 1;
        render_field_picture(&pair_want, &w, second_parity, 1, &p2);
        next_want = pair_want; /* zero same-parity MVs reconstruct it */

        snprintf(identity, sizeof identity, "field_pair/%s-first",
                 order ? "bottom" : "top");
        for (mode = 0; mode < 3; ++mode) {
            DecodeResult dr;
            TASK_CHECK(decode_es_chunks(es, n, mode, &dr) == 0);
            TASK_CHECK(dr.count == 3);
            TASK_CHECK(dr.first_count == 3);
            TASK_CHECK(strlen(dr.arrival) == 6);
            if (strcmp(dr.arrival, order ? "BTBTBT" : "TBTBTB") != 0 ||
                strcmp(dr.display_order, order ? "BTBTBT" : "TBTBTB") != 0)
                fprintf(stderr, "ORDER_DIFF %s arrival=%s display=%s\n",
                        identity, dr.arrival, dr.display_order);
            TASK_CHECK(strcmp(dr.arrival, order ? "BTBTBT" : "TBTBTB") == 0);
            TASK_CHECK(strcmp(dr.display_order,
                              order ? "BTBTBT" : "TBTBTB") == 0);
            TASK_CHECK(check_pic_bytes(identity, &dr.pics[0],
                                       ref.y, ref.u, ref.v) == 1);
            TASK_CHECK(check_field_bytes(identity, &dr.first[1],
                                         &first_want, first_parity) == 1);
            TASK_CHECK(check_pic_bytes(identity, &dr.pics[1],
                                       pair_want.y, pair_want.u, pair_want.v) == 1);
            TASK_CHECK(check_pic_bytes(identity, &dr.pics[2],
                                       next_want.y, next_want.u, next_want.v) == 1);
        }
        free(es);
    }
    return 0;
}

static void predictor_16x8_plan(PicPlan *c) {
    int i;
    memset(c, 0, sizeof(*c));
    c->coding = PIC_P;
    for (i = 0; i < FIELD_NMB; ++i) {
        c->mb[i].kind = MB_16X8;
        c->mb[i].field_sel[0] = 0;
        c->mb[i].field_sel[1] = 1;
        c->mb[i].ref_id = 2;
        c->mb[i].ref_id2 = 1;
    }
    /* MB1 establishes P0=(3,1), P1=(-3,-1); MB2 must inherit each. */
    for (i = 1; i <= 2; ++i) {
        c->mb[i].fx[0] = 3;
        c->mb[i].fy[0] = 1;
        c->mb[i].fx[1] = -3;
        c->mb[i].fy[1] = -1;
    }
    c->mb[2].field_sel[0] = 1;
    c->mb[2].field_sel[1] = 0;
}

static void field_continuity_plan(PicPlan *f) {
    int i;
    memset(f, 0, sizeof(*f));
    f->coding = PIC_P;
    for (i = 0; i < FIELD_NMB; ++i) {
        f->mb[i].kind = MB_FIELD;
        f->mb[i].field_sel[0] = 0;
        f->mb[i].ref_id = 2;
    }
    f->mb[1].fx[0] = 3;
    f->mb[1].fy[0] = 1;
    /* Same final vector: the writer emits a zero delta after continuation. */
    f->mb[2].fx[0] = 3;
    f->mb[2].fy[0] = 1;
}

static int region_matches_16x8_top(const DecodeResult *field_dr,
                                   const DecodeResult *sixteen_dr) {
    int j, x;
    if (field_dr->count < 2 || sixteen_dr->count < 2) return 0;
    for (j = 0; j < 8; ++j) {
        int y = 1 + 2 * j; /* second field is bottom parity */
        for (x = 16; x < 32; ++x) {
            int idx = y * PW + x;
            if (field_dr->pics[1].y[idx] != sixteen_dr->pics[1].y[idx])
                return 0;
        }
    }
    return 1;
}

static int run_predictor_cases(void) {
    PicPlan zero_first, p16, pfield;
    StreamPlan sp16, spf;
    ModelFrame ref, want16, wantf;
    FieldWorld w;
    uint8_t *es16, *esf;
    size_t n16, nf;
    int mode;

    same_copy_plan(&zero_first, TOP_FIELD);
    predictor_16x8_plan(&p16);
    field_continuity_plan(&pfield);
    field_pair_stream(&sp16, TOP_FIELD, &zero_first, &p16);
    field_pair_stream(&spf, TOP_FIELD, &zero_first, &pfield);
    build_es(&sp16, &es16, &n16);
    build_es(&spf, &esf, &nf);
    make_reference_model(&ref, 0);

    memset(&w, 0, sizeof w);
    w.prev = ref;
    /* The first field is the zero top field; it equals reference top. */
    w.first = ref;
    want16 = ref;
    render_field_picture(&want16, &w, BOTTOM_FIELD, 1, &p16);
    wantf = ref;
    render_field_picture(&wantf, &w, BOTTOM_FIELD, 1, &pfield);

    for (mode = 0; mode < 3; ++mode) {
        DecodeResult dr16, drf;
        char id[96];
        snprintf(id, sizeof id, "predictors/16x8-separate/chunk=%d", mode);
        TASK_CHECK(decode_es_chunks(es16, n16, mode, &dr16) == 0);
        TASK_CHECK(dr16.count == 2);
        TASK_CHECK(check_pic_bytes(id, &dr16.pics[1],
                                   want16.y, want16.u, want16.v) == 1);
        TASK_CHECK(decode_es_chunks(esf, nf, mode, &drf) == 0);
        TASK_CHECK(drf.count == 2);
        snprintf(id, sizeof id, "predictors/field-continuity/chunk=%d", mode);
        TASK_CHECK(check_pic_bytes(id, &drf.pics[1],
                                   wantf.y, wantf.u, wantf.v) == 1);
        if (mode == 0)
            TASK_CHECK(region_matches_16x8_top(&drf, &dr16) == 1);
    }
    free(es16);
    free(esf);
    return 0;
}

static int run_mutation_cases(void) {
    int status;
    if (getenv("FIELD_PRED_MUTATION_CHILD")) {
        TASK_CHECK(1);
        return 0;
    }
    /*
     * The shell helper makes an isolated copy, applies each source mutation,
     * compiles the same public-API regression binary there, and requires a
     * pixel-level failure rather than a build/crash/zero-test outcome.
     */
    status = system("bash task_support/field_mutations.sh");
    TASK_CHECK(status != -1);
    TASK_CHECK(WIFEXITED(status));
    TASK_CHECK(WEXITSTATUS(status) == 0);
    return 0;
}
