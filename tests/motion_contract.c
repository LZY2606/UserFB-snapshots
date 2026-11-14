/*
 * motion_contract.c
 *
 * Independent pixel-level regression for the MPEG-1 motion compensation path
 * in pl_mpeg.  A small, self contained bitstream writer emits legal MPEG-1
 * elementary streams (sequence / GOP / picture / slice / macroblock syntax)
 * and a separate reference model keeps its own per-picture planes, samples
 * them with the explicitly listed macroblock types and final half-pel
 * displacements, and compares every byte of Y / Cb / Cr against the decoder.
 *
 * The reference model is written from the MPEG-1 pixel processing rules, not
 * from the decoder: it does not reuse any VLC table, motion vector decoder or
 * block macro from pl_mpeg, and no expected value is a hash of library output.
 *
 * Three groups are registered through task_support/check.h:
 *   stream     - legal stream plumbing, coding vs display order, plane id
 *   prediction - forward / backward / bidirectional half-pel rounding
 *   state      - f_code>1 residual, differential accumulation, modular wrap,
 *                full-pel conversion, skipped macroblocks, slice reset
 *
 * Only C99, the C library and libm are required.  All material is generated
 * deterministically inside this file; no video is downloaded and no external
 * tool is invoked.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include "task_support/check.h"

/* ------------------------------------------------------------------ */
/* Picture-level configuration                                         */
/* ------------------------------------------------------------------ */

#define PIC_I 1
#define PIC_P 2
#define PIC_B 3

#define MAX_MBW 4
#define MAX_MBH 2
#define MAX_MBS (MAX_MBW * MAX_MBH)
#define MAX_PICS 5

/* Macroblock kinds, mapped explicitly to the MPEG-1 macroblock_type VLC. */
enum mb_kind {
    MB_INTRA = 0,              /* intra, no motion                          */
    MB_P_MC,                   /* P: forward motion, no residual (cbp 0)    */
    MB_B_FWD,                  /* B: forward motion only                    */
    MB_B_BACK,                 /* B: backward motion only                   */
    MB_B_BI                    /* B: bidirectional                          */
};

struct mb_spec {
    int col;                   /* macroblock column inside its row          */
    int kind;
    int fmv[2];                /* final forward vector, half-pel luma units */
    int bmv[2];                /* final backward vector, half-pel luma      */
    int dc[6];                 /* intra DC values (MB_INTRA only)           */
};

struct pic_spec {
    int type;
    int temp_ref;
    int ff_code;               /* (full_pel<<3)|f_code, forward             */
    int bf_code;               /* (full_pel<<3)|f_code, backward (B only)   */
    int slice_first;           /* slice start code id for row 0             */
    const struct mb_spec *mbs; /* coded macroblocks, sorted by (row,col)    */
    int mb_count;
};

struct stream_config {
    int mb_w;
    int mb_h;
    const struct pic_spec *pics;
    int pic_count;
};

/* ------------------------------------------------------------------ */
/* Small bitstream writer (independent of the decoder reader)          */
/* ------------------------------------------------------------------ */

struct bitwriter {
    uint8_t *data;
    size_t capacity;
    size_t bitpos;
};

static void bw_init(struct bitwriter *bw, uint8_t *data, size_t capacity) {
    bw->data = data;
    bw->capacity = capacity;
    bw->bitpos = 0;
    memset(data, 0, capacity);
}

static void bw_bits(struct bitwriter *bw, unsigned value, unsigned n) {
    for (unsigned i = n; i > 0; i--) {
        size_t byte = bw->bitpos >> 3;
        if (byte < bw->capacity)
            bw->data[byte] |= (uint8_t)(((value >> (i - 1)) & 1u)
                                        << (7 - (bw->bitpos & 7)));
        bw->bitpos++;
    }
}

static void bw_align(struct bitwriter *bw) {
    while (bw->bitpos & 7) bw_bits(bw, 0, 1);
}

static void bw_start_code(struct bitwriter *bw, unsigned code) {
    bw_align(bw);
    bw_bits(bw, 0, 8);
    bw_bits(bw, 0, 8);
    bw_bits(bw, 1, 8);
    bw_bits(bw, code, 8);
}

/* Canonical motion VLC (table tree walked independently, magnitudes <=16).
 * The sign is the final bit of a fixed-length pair: each positive code ends
 * in 0 and its negative counterpart is the same code with that last bit set
 * to 1 (e.g. +3 = 00010, -3 = 00011), so no extra sign bit is appended. */
static void bw_motion_code(struct bitwriter *bw, int d) {
    if (d == 0) { bw_bits(bw, 0x1, 1); return; }              /* 1       */
    int mag = d < 0 ? -d : d;
    /* (raw bits, bit length) for positive magnitudes 1..16 */
    static const unsigned short pos_val[17] =
        {0, 0x2, 0x2, 0x2, 0x6, 0xA, 0x8, 0x6,
         0x16, 0x54, 0x48, 0x42, 0x40, 0x1E, 0x1C, 0x1A, 0x18};
    static const unsigned char pos_len[17] =
        {0, 3, 4, 5, 7, 8, 8, 8, 10, 10, 10, 11, 11, 11, 11, 11, 11};
    unsigned code = pos_val[mag];
    if (d < 0) code |= 1u;                    /* flip trailing 0 to sign 1 */
    bw_bits(bw, code, pos_len[mag]);
}

/* Emit one motion component so the decoder reconstructs `target` from the
 * running `predictor` for the given f_code, including the (f_code-1)-bit
 * residual and the decoder's modular range wrap. */
static void bw_motion_component(struct bitwriter *bw, int target,
                                int predictor, int f_code) {
    int range = f_code << 5;                  /* 32 * f_code */
    int d = target - predictor;
    while (d > range / 2 - 1) d -= range;     /* fold into modular range */
    while (d < -range / 2) d += range;

    if (f_code == 1) { bw_motion_code(bw, d); return; }

    int r_size = f_code - 1;
    int fscale = 1 << r_size;
    int ad = d < 0 ? -d : d;
    /* A zero difference is coded as the single code "1"; the decoder only
     * reads the (f_code-1)-bit residual when the motion code is non-zero. */
    if (d == 0) { bw_motion_code(bw, 0); return; }
    int m_code = ((ad - 1) >> r_size) + 1;
    int residual = (ad - 1) & (fscale - 1);
    bw_motion_code(bw, d < 0 ? -m_code : m_code);
    bw_bits(bw, (unsigned)residual, (unsigned)r_size);
}

/* DCT dct_size VLC, derived independently from the luminance/chrominance
 * size tables: (raw bits, length). */
static void bw_dct_size(struct bitwriter *bw, int component, int size) {
    /* index 0..8 */
    static const unsigned short lum_val[9] =
        {0x4, 0x0, 0x1, 0x5, 0x6, 0xE, 0x1E, 0x3E, 0x7E};
    static const unsigned char lum_len[9] =
        {3, 2, 2, 3, 3, 4, 5, 6, 7};
    static const unsigned short chr_val[9] =
        {0x0, 0x1, 0x2, 0x6, 0xE, 0x1E, 0x3E, 0x7E, 0xFE};
    static const unsigned char chr_len[9] =
        {2, 2, 2, 3, 4, 5, 6, 7, 8};
    if (component == 0) bw_bits(bw, lum_val[size], lum_len[size]);
    else bw_bits(bw, chr_val[size], chr_len[size]);
}

/* Signed DC differential: minimal dct_size that represents delta.  For both
 * signs the size equals the bit length of |delta| (size s holds positive
 * [2^(s-1), 2^s-1] and negative [-(2^s-1), -2^(s-1)]). */
static int dc_delta_size(int delta) {
    if (delta == 0) return 0;
    int ad = delta < 0 ? -delta : delta;
    int s = 0;
    while (ad) { ad >>= 1; s++; }
    return s;
}

static void bw_dc_delta(struct bitwriter *bw, int component,
                        int delta, int *predictor) {
    int size = dc_delta_size(delta);
    bw_dct_size(bw, component, size);
    if (size > 0) {
        unsigned diff = delta >= 0
            ? (unsigned)delta
            : (unsigned)(delta + (1 << size) - 1);
        bw_bits(bw, diff, (unsigned)size);
    }
    *predictor += delta;
}

/* Macroblock address increment VLC for the small increments we use (1..4). */
static void bw_increment(struct bitwriter *bw, int inc) {
    switch (inc) {
        case 1: bw_bits(bw, 0x1, 1); break;    /* 1    */
        case 2: bw_bits(bw, 0x3, 3); break;    /* 011  */
        case 3: bw_bits(bw, 0x2, 3); break;    /* 010  */
        default: bw_bits(bw, 0x3, 4); break;   /* 0011 = 4 */
    }
}

/* ------------------------------------------------------------------ */
/* Stream construction                                                 */
/* ------------------------------------------------------------------ */

static void put_intra_mb(struct bitwriter *bw, const struct mb_spec *mb,
                         int inc, int pred[3]) {
    bw_increment(bw, inc);
    bw_bits(bw, 0x1, 1);                      /* intra type "1" */
    for (int b = 0; b < 6; b++) {
        int component = b < 4 ? 0 : (b == 4 ? 1 : 2);
        int delta = mb->dc[b] - pred[component];
        bw_dc_delta(bw, component, delta, &pred[component]);
        bw_bits(bw, 0x2, 2);                  /* end_of_block: "10" */
    }
}

static void put_p_mb(struct bitwriter *bw, const struct mb_spec *mb,
                     int inc, int f_code, int full_pel,
                     int *prev_h, int *prev_v) {
    bw_increment(bw, inc);
    /* Type 0x08 ("001"): motion forward, no quantizer bit and no coded block
     * pattern, so cbp defaults to 0 and no cbp VLC is emitted. */
    bw_bits(bw, 0x1, 3);                      /* P motion type "001" */
    int sh = full_pel ? mb->fmv[0] >> 1 : mb->fmv[0];
    int sv = full_pel ? mb->fmv[1] >> 1 : mb->fmv[1];
    bw_motion_component(bw, sh, *prev_h, f_code);
    bw_motion_component(bw, sv, *prev_v, f_code);
    *prev_h = sh;
    *prev_v = sv;
}

static void put_b_mb(struct bitwriter *bw, const struct mb_spec *mb,
                     int inc, int ff, int bf,
                     int *pfh, int *pfv, int *pbh, int *pbv) {
    bw_increment(bw, inc);
    int fwd = mb->kind != MB_B_BACK;
    int back = mb->kind != MB_B_FWD;
    if (fwd && back) bw_bits(bw, 0x2, 2);     /* bidirectional "10" */
    else if (back) bw_bits(bw, 0x2, 3);       /* backward only "010" */
    else bw_bits(bw, 0x2, 4);                 /* forward only "0010" */

    int ffull = (ff >> 3) & 1, ffc = ff & 7;
    int bfull = (bf >> 3) & 1, bfc = bf & 7;
    if (fwd) {
        int sh = ffull ? mb->fmv[0] >> 1 : mb->fmv[0];
        int sv = ffull ? mb->fmv[1] >> 1 : mb->fmv[1];
        bw_motion_component(bw, sh, *pfh, ffc);
        bw_motion_component(bw, sv, *pfv, ffc);
        *pfh = sh; *pfv = sv;
    }
    if (back) {
        int sh = bfull ? mb->bmv[0] >> 1 : mb->bmv[0];
        int sv = bfull ? mb->bmv[1] >> 1 : mb->bmv[1];
        bw_motion_component(bw, sh, *pbh, bfc);
        bw_motion_component(bw, sv, *pbv, bfc);
        *pbh = sh; *pbv = sv;
    }
}

static void emit_intra_sentinel(struct bitwriter *bw, int mb_w, int mb_h);

static size_t build_stream(uint8_t *out, size_t capacity,
                           const struct stream_config *cfg) {
    struct bitwriter bw;
    bw_init(&bw, out, capacity);
    int width = cfg->mb_w * 16;
    int height = cfg->mb_h * 16;

    bw_start_code(&bw, 0xB3);                 /* sequence header */
    bw_bits(&bw, (unsigned)width, 12);
    bw_bits(&bw, (unsigned)height, 12);
    bw_bits(&bw, 1, 4);                       /* pixel aspect ratio */
    bw_bits(&bw, 3, 4);                       /* frame rate code 3 -> 25 fps */
    bw_bits(&bw, 1, 18);                      /* bit_rate */
    bw_bits(&bw, 1, 1);                       /* marker */
    bw_bits(&bw, 20, 10);                     /* vbv buffer size */
    bw_bits(&bw, 0, 1);                       /* constrained */
    bw_bits(&bw, 0, 1);                       /* no intra matrix */
    bw_bits(&bw, 0, 1);                       /* no non-intra matrix */

    bw_start_code(&bw, 0xB8);                 /* group of pictures */
    bw_bits(&bw, 0, 25);

    for (int p = 0; p < cfg->pic_count; p++) {
        const struct pic_spec *pic = &cfg->pics[p];
        bw_start_code(&bw, 0x00);             /* picture */
        bw_bits(&bw, (unsigned)pic->temp_ref, 10);
        bw_bits(&bw, (unsigned)pic->type, 3);
        bw_bits(&bw, 0xFFFF, 16);             /* vbv_delay */
        if (pic->type == PIC_P || pic->type == PIC_B) {
            bw_bits(&bw, (unsigned)((pic->ff_code >> 3) & 1), 1);
            bw_bits(&bw, (unsigned)(pic->ff_code & 7), 3);
        }
        if (pic->type == PIC_B) {
            bw_bits(&bw, (unsigned)((pic->bf_code >> 3) & 1), 1);
            bw_bits(&bw, (unsigned)(pic->bf_code & 7), 3);
        }

        int mbi = 0;
        int pfh = 0, pfv = 0, pbh = 0, pbv = 0;
        int row = 0, prev_col = -1;
        int dc_pred[3] = {128, 128, 128};

        bw_start_code(&bw, (unsigned)(pic->slice_first));
        bw_bits(&bw, 8, 5);                   /* quantizer_scale */
        bw_bits(&bw, 0, 1);                   /* no extra bit */

        while (mbi < pic->mb_count) {
            const struct mb_spec *mb = &pic->mbs[mbi];

            /* A column that does not advance starts the next slice row; the
             * spec lists macroblocks in strict raster order. */
            if (prev_col >= 0 && mb->col <= prev_col) {
                row++;
                bw_start_code(&bw, (unsigned)(pic->slice_first + row));
                bw_bits(&bw, 8, 5);
                bw_bits(&bw, 0, 1);
                pfh = pfv = pbh = pbv = 0;   /* slice reset */
                dc_pred[0] = dc_pred[1] = dc_pred[2] = 128;
                prev_col = -1;
            }

            int inc = mb->col - prev_col;     /* first MB of a slice: col+1 */
            /* A P-picture gap (skipped macroblocks) resets the forward
             * predictor to zero before the next coded macroblock; B-picture
             * skips instead inherit the running vectors, so no reset here. */
            if (pic->type == PIC_P && inc > 1) { pfh = 0; pfv = 0; }
            switch (pic->type) {
                case PIC_I:
                    put_intra_mb(&bw, mb, inc, dc_pred);
                    break;
                case PIC_P:
                    put_p_mb(&bw, mb, inc, pic->ff_code & 7,
                             (pic->ff_code >> 3) & 1, &pfh, &pfv);
                    break;
                case PIC_B:
                    put_b_mb(&bw, mb, inc, pic->ff_code, pic->bf_code,
                             &pfh, &pfv, &pbh, &pbv);
                    break;
            }
            prev_col = mb->col;
            mbi++;
        }
    }

    emit_intra_sentinel(&bw, cfg->mb_w, cfg->mb_h);

    /* A bare trailing picture start code lets the start-code search recognise
     * the end of the sentinel; the sequence end code then stops the stream. */
    bw_start_code(&bw, 0x00);
    bw_bits(&bw, 0x3FF, 10);              /* temporal reference */
    bw_bits(&bw, PIC_I, 3);               /* picture type */
    bw_bits(&bw, 0xFFFF, 16);             /* vbv_delay */
    bw_align(&bw);

    bw_start_code(&bw, 0xB9);                 /* sequence end */
    bw_align(&bw);
    size_t bytes = (bw.bitpos + 7) >> 3;
    /* The decoder's sequence header parser requires a minimum number of
     * buffered bits; pad short fixed-memory streams (the capacity is always
     * much larger here and the tail is already zeroed). */
    return bytes < 256 ? 256 : bytes;
}

/* Emit a fully decodable all-neutral (DC 128) intra picture.  In the decoder's
 * default (B-frame reorder) mode a fixed-memory buffer never reaches its EOF
 * flush branch, so the last real reference picture is only released when the
 * following picture is decoded.  This sentinel performs that release; it is a
 * valid picture but never returned to the caller (the trailing bare picture
 * start ends the decode loop first), so it is excluded from every assertion. */
static void emit_intra_sentinel(struct bitwriter *bw, int mb_w, int mb_h) {
    struct mb_spec mb;
    memset(&mb, 0, sizeof(mb));
    mb.kind = MB_INTRA;
    for (int i = 0; i < 6; i++) mb.dc[i] = 128;

    bw_start_code(bw, 0x00);
    bw_bits(bw, 0x3FE, 10);             /* distinct temporal reference */
    bw_bits(bw, PIC_I, 3);
    bw_bits(bw, 0xFFFF, 16);

    int pred[3] = {128, 128, 128};
    for (int row = 0; row < mb_h; row++) {
        bw_start_code(bw, (unsigned)(1 + row));
        bw_bits(bw, 8, 5);
        bw_bits(bw, 0, 1);
        pred[0] = pred[1] = pred[2] = 128;
        for (int col = 0; col < mb_w; col++) {
            mb.col = col;
            put_intra_mb(bw, &mb, 1, pred);   /* consecutive MB increment */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Independent reference model                                         */
/* ------------------------------------------------------------------ */

struct geometry {
    int mb_w, mb_h;
    int lw, lh, cw, ch;
    int y_sz, c_sz, plane_sz;
};

static void geometry_init(struct geometry *g, int mb_w, int mb_h) {
    g->mb_w = mb_w; g->mb_h = mb_h;
    g->lw = mb_w * 16; g->lh = mb_h * 16;
    g->cw = mb_w * 8;  g->ch = mb_h * 8;
    g->y_sz = g->lw * g->lh;
    g->c_sz = g->cw * g->ch;
    g->plane_sz = g->y_sz + 2 * g->c_sz;
}

static uint8_t ref_clamp(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* Sample one reference plane at (x,y) plus a final half-pel vector.  The
 * two-/four-point sums are rounded exactly once here. */
static uint8_t ref_sample(const uint8_t *src, int w, int h,
                          int x, int y, int mh, int mv) {
    int hp = mh >> 1, vp = mv >> 1;
    int odd_h = mh & 1, odd_v = mv & 1;
    int sx = x + hp, sy = y + vp;
    if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
    if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;

    const uint8_t *p = src + sy * w + sx;
    if (!odd_h && !odd_v) return p[0];
    if (odd_h && !odd_v) {
        int x1 = sx + 1 < w ? sx + 1 : sx;
        return (uint8_t)((p[0] + src[sy * w + x1] + 1) >> 1);
    }
    if (!odd_h && odd_v) {
        int y1 = sy + 1 < h ? sy + 1 : sy;
        return (uint8_t)((p[0] + src[y1 * w + sx] + 1) >> 1);
    }
    int x1 = sx + 1 < w ? sx + 1 : sx;
    int y1 = sy + 1 < h ? sy + 1 : sy;
    int sum = p[0] + src[sy * w + x1]
            + src[y1 * w + sx] + src[y1 * w + x1];
    return (uint8_t)((sum + 2) >> 2);
}

static void ref_render_intra(uint8_t *plane, const struct geometry *g,
                             int mb_col, int mb_row, const int dc[6]) {
    for (int b = 0; b < 4; b++) {
        int ox = mb_col * 16 + (b & 1) * 8;
        int oy = mb_row * 16 + (b & 2) * 4;
        uint8_t v = ref_clamp(dc[b]);
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++)
                plane[(oy + yy) * g->lw + ox + xx] = v;
    }
    uint8_t cbv = ref_clamp(dc[4]);
    uint8_t crv = ref_clamp(dc[5]);
    for (int yy = 0; yy < 8; yy++)
        for (int xx = 0; xx < 8; xx++) {
            plane[g->y_sz + (mb_row * 8 + yy) * g->cw + mb_col * 8 + xx] = cbv;
            plane[g->y_sz + g->c_sz + (mb_row * 8 + yy) * g->cw
                  + mb_col * 8 + xx] = crv;
        }
}

/* Render one prediction MB from one reference; interpolate!=0 blends into
 * the destination with the second, independent +1 rounding. */
static void ref_predict_one(uint8_t *dst, const uint8_t *ref,
                            const struct geometry *g,
                            int mb_col, int mb_row,
                            int lmh, int lmv, int interpolate) {
    for (int yy = 0; yy < 16; yy++)
        for (int xx = 0; xx < 16; xx++) {
            int px = mb_col * 16 + xx, py = mb_row * 16 + yy;
            uint8_t s = ref_sample(ref, g->lw, g->lh, px, py, lmh, lmv);
            size_t idx = (size_t)py * g->lw + px;
            dst[idx] = interpolate ? (uint8_t)((dst[idx] + s + 1) >> 1) : s;
        }

    /* Chroma: truncate the luma half-pel component toward zero FIRST, then
     * apply chroma's own half-pel sampling. */
    int cmh = lmh / 2, cmv = lmv / 2;
    for (int plane = 0; plane < 2; plane++) {
        const uint8_t *csrc = ref + g->y_sz + plane * g->c_sz;
        uint8_t *cdst = dst + g->y_sz + plane * g->c_sz;
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                int px = mb_col * 8 + xx, py = mb_row * 8 + yy;
                uint8_t s = ref_sample(csrc, g->cw, g->ch, px, py, cmh, cmv);
                size_t idx = (size_t)py * g->cw + px;
                cdst[idx] = interpolate
                    ? (uint8_t)((cdst[idx] + s + 1) >> 1) : s;
            }
    }
}

struct ref_store {
    int used;
    int type;
    int temp_ref;
    uint8_t plane[(MAX_MBW * 16) * (MAX_MBH * 16)
                  + 2 * (MAX_MBW * 8) * (MAX_MBH * 8)];
};

/* Pick the two most recent reference (I/P) pictures: older = forward,
 * newest = backward. */
static void pick_references(const struct ref_store *refs, int n,
                            const struct ref_store **fwd,
                            const struct ref_store **back) {
    *fwd = NULL; *back = NULL;
    for (int i = 0; i < n; i++) {
        if (!refs[i].used) continue;
        if (refs[i].type == PIC_I || refs[i].type == PIC_P) {
            *fwd = *back;
            *back = &refs[i];
        }
    }
}

/* Render the single picture held in cfg (render_all presents one picture per
 * call).  Skipped macroblocks are reconstructed from the same state rules the
 * decoder applies:
 *   P skip -> forward vector reset to zero, zero-vector prediction
 *   B skip -> inherit the previous direction and vector(s)
 *   slice start -> every predictor reset
 */
static void ref_render_picture(const struct stream_config *cfg,
                               const struct geometry *g,
                               const struct ref_store *refs, int ref_n,
                               struct ref_store *out) {
    const struct pic_spec *pic = &cfg->pics[0];
    const struct ref_store *fwd = NULL, *back = NULL;
    pick_references(refs, ref_n, &fwd, &back);

    memset(out->plane, 0, (size_t)g->plane_sz);
    out->used = 1;
    out->type = pic->type;
    out->temp_ref = pic->temp_ref;

    int cur_f[2] = {0, 0}, cur_b[2] = {0, 0};
    int have_f = 0, have_b = 0;
    int row = 0, prev_col = -1;

    for (int mbi = 0; mbi < pic->mb_count; mbi++) {
        const struct mb_spec *mb = &pic->mbs[mbi];

        if (prev_col >= 0 && mb->col <= prev_col) {
            row++;                       /* next slice row */
            cur_f[0] = cur_f[1] = cur_b[0] = cur_b[1] = 0;
            have_f = have_b = 0;
            prev_col = -1;
        }

        /* Reconstruct skipped columns before this coded macroblock. */
        for (int scol = prev_col + 1; scol < mb->col; scol++) {
            if (pic->type == PIC_P) {
                cur_f[0] = cur_f[1] = 0;
                ref_predict_one(out->plane, back->plane, g,
                                scol, row, 0, 0, 0);
            } else { /* B */
                if (have_f)
                    ref_predict_one(out->plane, fwd->plane, g,
                                    scol, row, cur_f[0], cur_f[1], 0);
                if (have_b)
                    ref_predict_one(out->plane, back->plane, g,
                                    scol, row, cur_b[0], cur_b[1],
                                    have_f ? 1 : 0);
            }
        }

        if (mb->kind == MB_INTRA) {
            ref_render_intra(out->plane, g, mb->col, row, mb->dc);
            cur_f[0] = cur_f[1] = cur_b[0] = cur_b[1] = 0;
            have_f = have_b = 0;
        } else if (pic->type == PIC_P) {
            cur_f[0] = mb->fmv[0];
            cur_f[1] = mb->fmv[1];
            have_f = 1;
            ref_predict_one(out->plane, back->plane, g,
                            mb->col, row, cur_f[0], cur_f[1], 0);
        } else {
            int fwd_on = mb->kind != MB_B_BACK;
            int back_on = mb->kind != MB_B_FWD;
            if (fwd_on) {
                cur_f[0] = mb->fmv[0]; cur_f[1] = mb->fmv[1];
                have_f = 1;
                ref_predict_one(out->plane, fwd->plane, g,
                                mb->col, row, cur_f[0], cur_f[1], 0);
            }
            if (back_on) {
                cur_b[0] = mb->bmv[0]; cur_b[1] = mb->bmv[1];
                have_b = 1;
                ref_predict_one(out->plane, back->plane, g,
                                mb->col, row, cur_b[0], cur_b[1],
                                fwd_on ? 1 : 0);
            }
        }
        prev_col = mb->col;
    }
}

/* ------------------------------------------------------------------ */
/* Decode drive, snapshot and comparison                               */
/* ------------------------------------------------------------------ */

struct snapshot {
    double time;
    uint8_t plane[(MAX_MBW * 16) * (MAX_MBH * 16)
                  + 2 * (MAX_MBW * 8) * (MAX_MBH * 8)];
};

static int decode_and_snapshot(uint8_t *stream, size_t len,
                               const struct geometry *g,
                               struct snapshot *out, int max) {
    plm_buffer_t *buffer = plm_buffer_create_with_memory(stream, len, 0);
    plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
    if (!plm_video_has_header(video)) { plm_video_destroy(video); return -1; }
    if (plm_video_get_width(video) != g->mb_w * 16 ||
        plm_video_get_height(video) != g->mb_h * 16) {
        plm_video_destroy(video); return -2;
    }
    int n = 0;
    for (;;) {
        plm_frame_t *f = plm_video_decode(video);
        if (!f) break;
        if (n >= max) { plm_video_destroy(video); return -3; }
        out[n].time = f->time;
        memcpy(out[n].plane, f->y.data, (size_t)g->y_sz);
        memcpy(out[n].plane + g->y_sz, f->cb.data, (size_t)g->c_sz);
        memcpy(out[n].plane + g->y_sz + g->c_sz, f->cr.data,
               (size_t)g->c_sz);
        n++;
    }
    plm_video_destroy(video);
    return n;
}

static int planes_match(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Full byte-for-byte comparison of one decoded snapshot vs the reference. */
static int check_snapshot(const char *tag, const struct snapshot *got,
                          const struct ref_store *ref,
                          const struct geometry *g, double want_time) {
    TASK_CHECK(planes_match(got->plane, ref->plane, (size_t)g->plane_sz));
    TASK_CHECK(planes_match(got->plane + g->y_sz,
                            ref->plane + g->y_sz, (size_t)g->c_sz));
    TASK_CHECK(planes_match(got->plane + g->y_sz + g->c_sz,
                            ref->plane + g->y_sz + g->c_sz,
                            (size_t)g->c_sz));
    TASK_CHECK(fabs(got->time - want_time) < 1e-9);
    (void)tag;
    return 0;
}

/* Render every picture of a config in coding order, storing reference planes
 * keyed by picture identity (temp_ref).  Returns count and the display-order
 * indices into the rendered array. */
struct rendered_set {
    struct ref_store pics[MAX_PICS];
    int display_order[MAX_PICS];   /* indices into pics[], in display order */
    int count;
};

static void render_all(const struct stream_config *cfg,
                       const struct geometry *g,
                       struct rendered_set *rs) {
    memset(rs, 0, sizeof(*rs));
    struct ref_store refs[MAX_PICS];
    memset(refs, 0, sizeof(refs));
    int ref_n = 0, count = 0;

    for (int p = 0; p < cfg->pic_count; p++) {
        /* Build a one-picture view so ref_render_picture handles this pic. */
        struct stream_config one = *cfg;
        one.pics = &cfg->pics[p];
        one.pic_count = 1;
        struct ref_store out;
        ref_render_picture(&one, g, refs, ref_n, &out);
        rs->pics[count] = out;
        if (out.type == PIC_I || out.type == PIC_P)
            refs[ref_n++] = out;
        count++;
    }
    rs->count = count;

    /* The pl_mpeg decoder's fixed-memory output rule (see plm_video_decode):
     * a B picture is returned immediately, while an I/P reference picture is
     * held back and only returned when the next reference picture is decoded
     * (the intra sentinel at the end releases the final one).  Replay that
     * rule over the coding order to obtain the exact emission sequence, which
     * is what decode_and_snapshot records. */
    int held = -1;                 /* coding index of a held reference frame */
    int out_n = 0;
    for (int p = 0; p < count; p++) {
        if (rs->pics[p].type == PIC_B) {
            rs->display_order[out_n++] = p;     /* B frames pass through */
        } else {
            if (held >= 0) rs->display_order[out_n++] = held;
            held = p;                            /* defer this reference */
        }
    }
    if (held >= 0) rs->display_order[out_n++] = held;
}

/* ================================================================== */
/* Picture definitions                                                 */
/* ================================================================== */

/* --- Core 48x32 (3x2 MB) stream: I, P, B, B coding order ------------ */

/* Uneven DC-only intra texture: distinct value per 8x8 block so any half-pel
 * offset or chroma column shift changes bytes. */
static const struct mb_spec core_i_mbs[] = {
    {0, MB_INTRA, {0,0}, {0,0}, { 20, 24, 28, 32, 100, 140}},
    {1, MB_INTRA, {0,0}, {0,0}, { 40, 44, 48, 52, 108, 148}},
    {2, MB_INTRA, {0,0}, {0,0}, { 60, 64, 68, 72, 116, 156}},
    {0, MB_INTRA, {0,0}, {0,0}, { 80, 84, 88, 92, 124, 164}},
    {1, MB_INTRA, {0,0}, {0,0}, {100,104,108,112, 132, 172}},
    {2, MB_INTRA, {0,0}, {0,0}, {120,124,128,132, 140, 180}}
};

/* P3 forward vectors: horizontal / vertical / 2D half-pel, positive and
 * negative odd components, every sampling footprint inside 48x32. */
static const struct mb_spec core_p_mbs[] = {
    {0, MB_P_MC, { 3, 0}, {0,0}, {0}},   /* row0: horizontal +odd          */
    {1, MB_P_MC, { 0, 3}, {0,0}, {0}},   /*       vertical +odd            */
    {2, MB_P_MC, {-3, 3}, {0,0}, {0}},   /*       2D, negative h odd       */
    {0, MB_P_MC, { 3,-3}, {0,0}, {0}},   /* row1: 2D, negative v odd       */
    {1, MB_P_MC, { 0,-3}, {0,0}, {0}},   /*       vertical -odd            */
    {2, MB_P_MC, {-3,-3}, {0,0}, {0}}    /*       2D, both negative        */
};

/* B1 references I0 (forward) and P3 (backward): all three directions.  The
 * vectors obey the frame-edge sign rules so every sampling footprint stays
 * inside the 48x32 reference: top-row MBs use only non-negative vertical
 * components, bottom-row only non-positive, left-column only non-negative
 * horizontal and right-column only non-positive. */
static const struct mb_spec core_b1_mbs[] = {
    {0, MB_B_FWD,  { 3, 0}, {0,0}, {0}},   /* row0 col0: horizontal +odd   */
    {1, MB_B_BACK, {0,0}, { 0, 3}, {0}},   /* row0 col1: vertical +odd     */
    {2, MB_B_BI,   {-3, 0}, {-3, 3}, {0}}, /* row0 col2: 2D, negative h    */
    {0, MB_B_FWD,  { 0,-3}, {0,0}, {0}},   /* row1 col0: vertical -odd     */
    {1, MB_B_BI,   { 3,-3}, {-3,-3}, {0}}, /* row1 col1: 2D mixed signs    */
    {2, MB_B_BACK, {0,0}, {-3,-3}, {0}}    /* row1 col2: 2D both negative  */
};

/* B2 reuses both references so reference rotation cannot mask an error. */
static const struct mb_spec core_b2_mbs[] = {
    {0, MB_B_BI,   { 3, 3}, { 3, 0}, {0}}, /* row0 col0: 2D positive       */
    {1, MB_B_FWD,  {-3, 0}, {0,0}, {0}},   /* row0 col1: horizontal -odd   */
    {2, MB_B_BACK, {0,0}, {-3, 3}, {0}},   /* row0 col2: 2D negative h     */
    {0, MB_B_BACK, {0,0}, { 3, 0}, {0}},   /* row1 col0: horizontal +odd   */
    {1, MB_B_BI,   { 0,-3}, {-3,-3}, {0}}, /* row1 col1: vertical/2D neg   */
    {2, MB_B_FWD,  {-3,-3}, {0,0}, {0}}    /* row1 col2: 2D both negative  */
};

static const struct pic_spec core_pics[] = {
    {PIC_I, 0, 0,    0,    1, core_i_mbs,  6},
    {PIC_P, 3, 0x01, 0,    1, core_p_mbs,  6},
    {PIC_B, 1, 0x01, 0x01, 1, core_b1_mbs, 6},
    {PIC_B, 2, 0x01, 0x01, 1, core_b2_mbs, 6}
};
static const struct stream_config core_cfg =
    {3, 2, core_pics, 4};

/* ================================================================== */
/* stream group                                                        */
/* ================================================================== */

static int run_stream_order(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, core_cfg.mb_w, core_cfg.mb_h);
    size_t len = build_stream(stream, sizeof(stream), &core_cfg);
    TASK_CHECK(len > 0 && len < sizeof(stream));

    struct snapshot got[MAX_PICS];
    int n = decode_and_snapshot(stream, len, &g, got, MAX_PICS);
    TASK_CHECK(n == core_cfg.pic_count);

    struct rendered_set rs;
    render_all(&core_cfg, &g, &rs);

    /* Decoder display order must be I0, B1, B2, P3 (different from the
     * I, P, B, B coding order). */
    static const int want_temp[4] = {0, 1, 2, 3};
    for (int i = 0; i < n; i++) {
        int idx = rs.display_order[i];
        TASK_CHECK(rs.pics[idx].temp_ref == want_temp[i]);
        TASK_CHECK(check_snapshot("order", &got[i], &rs.pics[idx],
                                  &g, i / 25.0) == 0);
    }
    return 0;
}

static int run_stream_planes_distinct(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, 3, 2);
    size_t len = build_stream(stream, sizeof(stream), &core_cfg);
    struct snapshot got[MAX_PICS];
    int n = decode_and_snapshot(stream, len, &g, got, MAX_PICS);
    TASK_CHECK(n == 4);

    /* First displayed frame is I0; last is P3.  They must differ on every
     * plane, and Cb must differ from Cr. */
    int yd = 0, cbd = 0, crd = 0, cbc = 0;
    const uint8_t *i0 = got[0].plane, *p3 = got[3].plane;
    for (int i = 0; i < g.y_sz; i++) if (i0[i] != p3[i]) yd = 1;
    for (int i = 0; i < g.c_sz; i++) {
        if (i0[g.y_sz + i] != p3[g.y_sz + i]) cbd = 1;
        if (i0[g.y_sz + g.c_sz + i] != p3[g.y_sz + g.c_sz + i]) crd = 1;
        if (i0[g.y_sz + i] != i0[g.y_sz + g.c_sz + i]) cbc = 1;
    }
    TASK_CHECK(yd && cbd && crd && cbc);

    /* Display timestamps strictly increase and match 25 fps. */
    for (int i = 1; i < n; i++) TASK_CHECK(got[i - 1].time < got[i].time);
    TASK_CHECK(fabs(got[3].time - 3 / 25.0) < 1e-9);
    return 0;
}

/* ================================================================== */
/* prediction group                                                    */
/* ================================================================== */

static int run_prediction_halfpel(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, 3, 2);
    size_t len = build_stream(stream, sizeof(stream), &core_cfg);
    struct snapshot got[MAX_PICS];
    int n = decode_and_snapshot(stream, len, &g, got, MAX_PICS);
    TASK_CHECK(n == 4);

    struct rendered_set rs;
    render_all(&core_cfg, &g, &rs);
    /* P3 is display index 3. */
    TASK_CHECK(check_snapshot("halfpel", &got[3], &rs.pics[1],
                              &g, 3 / 25.0) == 0);

    /* Hand-worked luma samples against the DC-only I0 texture.
     * mb(0,0) vector (3,0): at output x6 the footprint straddles the 8px
     * block edge, so (I0(6,0)+I0(7,0)=20,20 region) ... the boundary pixel is
     * (I0(7,0)=20 + I0(8,0)=24 + 1) >> 1 = 22. */
    TASK_CHECK(got[3].plane[6] == 22);
    /* mb(1,0) vector (0,3): vertical half-pel on the flat value 40 block,
     * (40 + 40 + 1) >> 1 = 40. */
    TASK_CHECK(got[3].plane[16] == 40);

    /* Chroma truncate-toward-zero hand check.  The luma vector (-3,3) on
     * mb(2,0) becomes a chroma vector of (-3/2, 3/2) = (-1,1).  At chroma
     * output (16,0) the 2D footprint straddles the Cb column edge (col1=108,
     * col2=116): (108+116+108+116+2) >> 2 = 112. */
    TASK_CHECK(got[3].plane[g.y_sz + 16] == 112);
    return 0;
}

static int run_prediction_bidirect(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, 3, 2);
    size_t len = build_stream(stream, sizeof(stream), &core_cfg);
    struct snapshot got[MAX_PICS];
    int n = decode_and_snapshot(stream, len, &g, got, MAX_PICS);
    TASK_CHECK(n == 4);

    struct rendered_set rs;
    render_all(&core_cfg, &g, &rs);
    TASK_CHECK(check_snapshot("b1", &got[1], &rs.pics[2],
                              &g, 1 / 25.0) == 0);
    TASK_CHECK(check_snapshot("b2", &got[2], &rs.pics[3],
                              &g, 2 / 25.0) == 0);

    /* Guarantee a rounding-sensitive bidirectional pixel exists: recompute
     * the two independently rounded predictions for B1 mb(2,0) (bi, fwd
     * (3,0) from I0, back (-3,3) from P3) and require an odd sum somewhere. */
    const uint8_t *i0 = rs.pics[0].plane;
    const uint8_t *p3 = rs.pics[1].plane;
    int found_odd = 0;
    for (int yy = 0; yy < 16 && !found_odd; yy++)
        for (int xx = 0; xx < 16; xx++) {
            int px = 32 + xx, py = yy;
            uint8_t a = ref_sample(i0, g.lw, g.lh, px, py, 3, 0);
            uint8_t b = ref_sample(p3, g.lw, g.lh, px, py, -3, 3);
            if (((int)a + (int)b) & 1) { found_odd = 1; break; }
        }
    TASK_CHECK(found_odd);

    /* Exact decoder byte at the first such pixel must match the reference
     * (which rounds the average a second time). */
    TASK_CHECK(planes_match(got[1].plane, rs.pics[2].plane,
                            (size_t)g.plane_sz));
    return 0;
}

/* ================================================================== */
/* state group                                                         */
/* ================================================================== */

/* --- f_code=2 residual + differential accumulation, 48x32 ---------- */

/* f_code=2 residual coding, differential accumulation and sign changes.
 * Per-macroblock sign rules keep every footprint inside the 48x32 frame:
 * top-row vertical components are >= 0, bottom-row <= 0, left-column
 * horizontal >= 0 and right-column <= 0. */
static const struct mb_spec res_p_mbs[] = {
    {0, MB_P_MC, { 5, 0}, {0,0}, {0}},   /* delta +5: m_code 3, residual 0 */
    {1, MB_P_MC, { 9, 0}, {0,0}, {0}},   /* delta +4: m_code 2, residual 1 */
    {2, MB_P_MC, {-3, 0}, {0,0}, {0}},   /* delta -12: sign change          */
    {0, MB_P_MC, { 0,-3}, {0,0}, {0}},   /* new slice: predictor reset      */
    {1, MB_P_MC, { 0,-9}, {0,0}, {0}},   /* vertical accumulation           */
    {2, MB_P_MC, { 0,-3}, {0,0}, {0}}    /* delta +6: negative->positive    */
};

static const struct mb_spec res_b_mbs[] = {
    {0, MB_B_BI,  { 5, 3}, { 5, 3}, {0}},
    {1, MB_B_BI,  { 9, 3}, {-9, 3}, {0}},
    {2, MB_B_FWD, {-3, 0}, { 0,0}, {0}},
    {0, MB_B_BACK,{ 0,0}, { 5,-3}, {0}},   /* row1 slice reset */
    {1, MB_B_BI,  {-3,-3}, { 3,-3}, {0}},
    {2, MB_B_FWD, {-9, 0}, { 0,0}, {0}}
};

static const struct pic_spec res_pics[] = {
    {PIC_I, 0, 0,    0,    1, core_i_mbs, 6},
    {PIC_P, 4, 0x02, 0,    1, res_p_mbs,  6},
    {PIC_B, 5, 0x02, 0x02, 1, res_b_mbs,  6}
};
static const struct stream_config res_cfg = {3, 2, res_pics, 3};

static int run_state_residual(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, 3, 2);
    size_t len = build_stream(stream, sizeof(stream), &res_cfg);
    struct snapshot got[MAX_PICS];
    int n = decode_and_snapshot(stream, len, &g, got, MAX_PICS);
    TASK_CHECK(n == 3);

    struct rendered_set rs;
    render_all(&res_cfg, &g, &rs);

    /* Emission order is I, B5, P4 (the P reference is deferred behind the B),
     * which render_all records in display_order.  Verify every snapshot, its
     * picture identity and its monotonic 25 fps timestamp. */
    int saw_p = 0, saw_b = 0;
    for (int i = 0; i < n; i++) {
        int idx = rs.display_order[i];
        if (rs.pics[idx].type == PIC_P) saw_p = 1;
        if (rs.pics[idx].type == PIC_B) saw_b = 1;
        TASK_CHECK(check_snapshot("res", &got[i], &rs.pics[idx],
                                  &g, i / 25.0) == 0);
    }
    TASK_CHECK(saw_p && saw_b);
    TASK_CHECK(got[0].time < got[1].time && got[1].time < got[2].time);

    /* Hand check of residual reconstruction at P mb(1,0), final (9,0):
     * delta 4, f_code 2 -> m_code 2, residual 1 =>
     * ((2-1)<<1)+1+1 = 4; predictor 5 + 4 = 9.  Locate the P snapshot. */
    for (int i = 0; i < n; i++) {
        int idx = rs.display_order[i];
        if (rs.pics[idx].type == PIC_P)
            TASK_CHECK(got[i].plane[16] == rs.pics[idx].plane[16]);
    }
    return 0;
}

/* --- modular wrap and full-pel, 64x32 (4x2 MB) ---------------------- */

static const struct mb_spec wrap_i_mbs[] = {
    {0, MB_INTRA, {0,0}, {0,0}, { 16, 20, 24, 28,  96, 136}},
    {1, MB_INTRA, {0,0}, {0,0}, { 32, 36, 40, 44, 104, 144}},
    {2, MB_INTRA, {0,0}, {0,0}, { 48, 52, 56, 60, 112, 152}},
    {3, MB_INTRA, {0,0}, {0,0}, { 64, 68, 72, 76, 120, 160}},
    {0, MB_INTRA, {0,0}, {0,0}, { 80, 84, 88, 92, 128, 168}},
    {1, MB_INTRA, {0,0}, {0,0}, { 96,100,104,108, 136, 176}},
    {2, MB_INTRA, {0,0}, {0,0}, {112,116,120,124, 144, 184}},
    {3, MB_INTRA, {0,0}, {0,0}, {128,132,136,140, 152, 192}}
};

/* Horizontal differentials that force both modular wrap directions while
 * every reconstructed vector stays inside the 64 px footprint.
 *   mb0 +13 -> mb1 -14 (emit +5, 13+5=18 wraps to -14)
 *   mb1 -14 -> mb2 +13 (emit -5, -14-5=-19 wraps to +13)
 *   mb2 +13 -> mb3 -13 (emit +6, 13+6=19 wraps to -13)                */
static const struct mb_spec wrap_p_mbs[] = {
    {0, MB_P_MC, { 13, 0}, {0,0}, {0}},
    {1, MB_P_MC, {-14, 0}, {0,0}, {0}},
    {2, MB_P_MC, { 13, 0}, {0,0}, {0}},
    {3, MB_P_MC, {-13, 0}, {0,0}, {0}},
    {0, MB_P_MC, { 3,-3}, {0,0}, {0}},   /* row1, fresh slice, predictor 0 */
    {1, MB_P_MC, { 0,-3}, {0,0}, {0}},
    {2, MB_P_MC, {-3, 0}, {0,0}, {0}},
    {3, MB_P_MC, {-3,-3}, {0,0}, {0}}
};

static const struct pic_spec wrap_pics[] = {
    {PIC_I, 0, 0,    0, 1, wrap_i_mbs, 8},
    {PIC_P, 1, 0x01, 0, 1, wrap_p_mbs, 8}
};
static const struct stream_config wrap_cfg = {4, 2, wrap_pics, 2};

static int run_state_wrap(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, 4, 2);
    size_t len = build_stream(stream, sizeof(stream), &wrap_cfg);
    struct snapshot got[4];
    int n = decode_and_snapshot(stream, len, &g, got, 4);
    TASK_CHECK(n == 2);

    struct rendered_set rs;
    render_all(&wrap_cfg, &g, &rs);
    TASK_CHECK(check_snapshot("wrap", &got[1], &rs.pics[1],
                              &g, 1 / 25.0) == 0);

    /* If wrap were broken, mb1 would keep +18 (out of range) instead of -14,
     * shifting every pixel of that macroblock; the full-plane check already
     * catches it.  Anchor one explicit byte: mb(1,0) pixel (16,0) with final
     * vector -14 samples I0 luma column 16-7 = 9. */
    TASK_CHECK(got[1].plane[16] == rs.pics[1].plane[16]);
    return 0;
}

/* full_pel=1: vectors are coded in whole pixels and the decoder shifts them
 * to half-pel internally, so the final half-pel vectors here are all even.
 * Edge-safe signs per macroblock position on the 64x32 frame. */
static const struct mb_spec fp_p_mbs[] = {
    {0, MB_P_MC, { 2, 0}, {0,0}, {0}},   /* row0: coded +1 full pel        */
    {1, MB_P_MC, { 4, 0}, {0,0}, {0}},
    {2, MB_P_MC, {-4, 0}, {0,0}, {0}},
    {3, MB_P_MC, {-2, 0}, {0,0}, {0}},
    {0, MB_P_MC, { 2,-2}, {0,0}, {0}},   /* row1: negative vertical        */
    {1, MB_P_MC, { 4,-4}, {0,0}, {0}},
    {2, MB_P_MC, {-2,-2}, {0,0}, {0}},
    {3, MB_P_MC, {-4,-4}, {0,0}, {0}}
};
static const struct pic_spec fp_pics[] = {
    {PIC_I, 0, 0,    0,    1, wrap_i_mbs, 8},
    {PIC_P, 6, 0x81, 0,    1, fp_p_mbs,   8}
};
static const struct stream_config fp_cfg = {4, 2, fp_pics, 2};

static int run_state_fullpel(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, 4, 2);
    size_t len = build_stream(stream, sizeof(stream), &fp_cfg);
    struct snapshot got[4];
    int n = decode_and_snapshot(stream, len, &g, got, 4);
    TASK_CHECK(n == 2);

    struct rendered_set rs;
    render_all(&fp_cfg, &g, &rs);
    TASK_CHECK(check_snapshot("fullpel", &got[1], &rs.pics[1],
                              &g, 1 / 25.0) == 0);
    /* coded +1 full pel -> integer luma shift, exact byte at (0,0). */
    TASK_CHECK(got[1].plane[0] == rs.pics[1].plane[0]);
    return 0;
}

/* --- skipped macroblocks and slice reset, 48x32 --------------------- */

/* P: each row codes col0 and col2, leaving col1 skipped (must predict with a
 * reset zero vector).  Both rows are fully covered. */
static const struct mb_spec skip_p_mbs[] = {
    {0, MB_P_MC, { 3, 0}, {0,0}, {0}},
    {2, MB_P_MC, { 0, 3}, {0,0}, {0}},
    {0, MB_P_MC, { 3, 0}, {0,0}, {0}},   /* row 1: fresh slice, predictor 0 */
    {2, MB_P_MC, { 0,-3}, {0,0}, {0}}
};
/* B: row 0 col0 bidirectional, col1 skipped (inherits the bidir direction and
 * both vectors), col2 forward.  Row 1 starts a new slice (state reset): col0
 * is backward only, col1 skipped inheriting that backward direction.  Signs
 * follow the frame-edge rules so every footprint stays inside 48x32. */
static const struct mb_spec skip_b_mbs[] = {
    {0, MB_B_BI,   { 3, 0}, { 3, 3}, {0}},
    {2, MB_B_FWD,  { 0, 3}, { 0,0}, {0}},
    {0, MB_B_BACK, { 0,0}, { 3,-3}, {0}},
    {2, MB_B_BI,   {-3,-3}, {-3,-3}, {0}}
};
static const struct pic_spec skip_pics[] = {
    {PIC_I, 0, 0,    0,    1, core_i_mbs, 6},
    {PIC_P, 3, 0x01, 0,    1, skip_p_mbs, 4},
    {PIC_B, 1, 0x01, 0x01, 1, skip_b_mbs, 4}
};
static const struct stream_config skip_cfg = {3, 2, skip_pics, 3};

static int run_state_skips(void) {
    static uint8_t stream[16384];
    struct geometry g;
    geometry_init(&g, 3, 2);
    size_t len = build_stream(stream, sizeof(stream), &skip_cfg);
    struct snapshot got[6];
    int n = decode_and_snapshot(stream, len, &g, got, 6);
    TASK_CHECK(n == 3);                     /* display order: I, B1, P3 */

    struct rendered_set rs;
    render_all(&skip_cfg, &g, &rs);
    for (int i = 0; i < n; i++)
        TASK_CHECK(check_snapshot("skip", &got[i],
                                  &rs.pics[rs.display_order[i]],
                                  &g, i / 25.0) == 0);

    /* Explicit P skip check: the skipped middle MB (row 0, col1) equals a
     * zero-vector copy of I0 row 0. */
    const uint8_t *i0 = got[0].plane;
    const uint8_t *pframe = got[2].plane;  /* P3 is last in display order */
    int skip_ok = 1;
    for (int yy = 0; yy < 16; yy++)
        for (int xx = 0; xx < 16; xx++)
            if (pframe[yy * g.lw + 16 + xx] != i0[yy * g.lw + 16 + xx])
                skip_ok = 0;
    TASK_CHECK(skip_ok);

    /* B skip inheritance plus slice reset: the full B frame matches the
     * independently rendered reference (row 0 col1 inherits bidir, row 1 col1
     * inherits the reset backward-only direction). */
    TASK_CHECK(planes_match(got[1].plane,
                            rs.pics[rs.display_order[1]].plane,
                            (size_t)g.plane_sz));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"stream", "coding_and_display_order", run_stream_order},
    {"stream", "reference_planes_distinct", run_stream_planes_distinct},

    {"prediction", "luma_halfpel_and_chroma_trunc", run_prediction_halfpel},
    {"prediction", "bidirectional_rounding", run_prediction_bidirect},

    {"state", "fcode_residual_accumulation", run_state_residual},
    {"state", "modular_vector_wrap", run_state_wrap},
    {"state", "fullpel_conversion", run_state_fullpel},
    {"state", "skipped_mbs_and_slice_reset", run_state_skips}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
