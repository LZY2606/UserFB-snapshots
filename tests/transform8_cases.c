#include "check.h"

#include "h264bsd_decoder.h"
#include "h264bsd_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small C bit writer and Annex-B builder                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t b[40000];
    size_t bits;
} BitWriter;

static void bw_init(BitWriter *w)
{
    memset(w, 0, sizeof(*w));
}

static void bw_bit(BitWriter *w, int v)
{
    size_t i = w->bits++;
    if (i / 8 < sizeof(w->b)) {
        if (v)
            w->b[i / 8] |= (uint8_t)(0x80U >> (i & 7));
        else
            w->b[i / 8] &= (uint8_t)~(0x80U >> (i & 7));
    }
}

static void bw_bits(BitWriter *w, uint32_t v, int n)
{
    int i;
    for (i = n - 1; i >= 0; --i)
        bw_bit(w, (v >> i) & 1U);
}

static void bw_ue(BitWriter *w, uint32_t x)
{
    uint32_t v = x + 1;
    int n = 0;
    while ((v >> (n + 1)) != 0)
        ++n;
    bw_bits(w, 0, n);
    bw_bits(w, v, n + 1);
}

static void bw_se(BitWriter *w, int32_t x)
{
    if (x > 0)
        bw_ue(w, (uint32_t)(2 * x - 1));
    else if (x < 0)
        bw_ue(w, (uint32_t)(-2 * x));
    else
        bw_ue(w, 0);
}

static void bw_align_zero(BitWriter *w)
{
    while (w->bits & 7U)
        bw_bit(w, 0);
}

static void bw_trailing(BitWriter *w)
{
    bw_bit(w, 1);
    while (w->bits & 7U)
        bw_bit(w, 0);
}

static void bw_bytes(BitWriter *w, const uint8_t *p, size_t n)
{
    size_t i;
    bw_align_zero(w);
    for (i = 0; i < n; ++i)
        bw_bits(w, p[i], 8);
}

static size_t bw_size(const BitWriter *w)
{
    return (w->bits + 7U) / 8U;
}

typedef struct {
    uint8_t b[140000];
    size_t n;
} AnnexB;

static void ab_u8(AnnexB *a, uint8_t x)
{
    if (a->n < sizeof(a->b))
        a->b[a->n++] = x;
}

static void ab_nal(AnnexB *a, int type, int ref_idc,
                   const uint8_t *rbsp, size_t len)
{
    size_t i;
    int zeros = 0;
    ab_u8(a, 0); ab_u8(a, 0); ab_u8(a, 0); ab_u8(a, 1);
    ab_u8(a, (uint8_t)((ref_idc << 5) | type));
    for (i = 0; i < len; ++i) {
        uint8_t c = rbsp[i];
        if (zeros >= 2 && c <= 3) {
            ab_u8(a, 3);
            zeros = 0;
        }
        ab_u8(a, c);
        if (c == 0)
            ++zeros;
        else
            zeros = 0;
    }
}

static void ab_nal_bw(AnnexB *a, int type, int ref_idc, BitWriter *w)
{
    ab_nal(a, type, ref_idc, w->b, bw_size(w));
}

/* CAVLC coefficient-token tables from the fixed pure-C material encoder.
 * They are syntax tables only; pixel expectations below use independent
 * scalar transform models and never use production scan/dequant tables. */
extern const uint8_t h264e_g_coeff_token[];
extern const uint8_t h264e_g_total_zeros[];

static void cavlc_token(BitWriter *w, int total_coeff, int trailing_ones,
                        int nc)
{
    int off, val, n, code;
    if (nc > 16) nc = 16;
    if (nc < 0) nc = 0;
    off = h264e_g_coeff_token[nc];
    val = h264e_g_coeff_token[off + trailing_ones +
                              4 * (total_coeff - trailing_ones)];
    n = 6;
    code = val;
    if (off != 230) {
        n = (val & 15) + 1;
        code = val >> 4;
    }
    bw_bits(w, (uint32_t)code, n);
}

/* Invert the production level decoding arithmetic with a small bounded
 * search.  This keeps the writer reviewable while supporting arbitrary small
 * signed levels. */
static void cavlc_level(BitWriter *w, int level, int index, int t1,
                        int total_coeff, int *suffix_io)
{
    int p, suffix, found = 0;
    (void)total_coeff;
    for (p = 0; p <= 15 && !found; ++p) {
        int s0 = *suffix_io;
        int nbits = p < 14 ? s0 : (p == 14 ? (s0 ? s0 : 4) : 12);
        int shift_s = s0;
        if (p >= 15 && shift_s == 0)
            shift_s = 1;
        for (suffix = 0; suffix < (1 << nbits); ++suffix) {
            int code = (p << shift_s) + suffix;
            int decoded, suf;
            if (index == t1 && t1 < 3)
                code += 2;
            decoded = (code + 2) >> 1;
            if (code & 1)
                decoded = -decoded;
            if (decoded != level)
                continue;
            bw_bits(w, 0, p);
            bw_bit(w, 1);
            if (nbits)
                bw_bits(w, (uint32_t)suffix, nbits);
            suf = s0 ? s0 : 1;
            if (abs(level) > (3 << (suf - 1)) && suf < 6)
                ++suf;
            *suffix_io = suf;
            found = 1;
            break;
        }
    }
    /* All levels used by the tests are small enough to be representable. */
    if (!found)
        bw_bit(w, 1);
}

static void run_before_code(int run, int zeros_left, int *code, int *len)
{
    if (zeros_left == 1) {
        *len = 1; *code = run ? 0 : 1;
    } else if (zeros_left == 2) {
        if (run == 2) { *len = 2; *code = 0; }
        else if (run == 1) { *len = 2; *code = 1; }
        else { *len = 1; *code = 1; }
    } else if (zeros_left == 3) {
        *len = 2; *code = 3 - run;
    } else if (zeros_left == 4) {
        if (run == 4) { *len = 3; *code = 0; }
        else if (run == 3) { *len = 3; *code = 1; }
        else if (run == 2) { *len = 2; *code = 1; }
        else if (run == 1) { *len = 2; *code = 2; }
        else { *len = 2; *code = 3; }
    } else if (zeros_left == 5) {
        if (run >= 2) { *len = 3; *code = 5 - run; }
        else { *len = 2; *code = run ? 2 : 3; }
    } else if (zeros_left == 6) {
        static const int code6[7] = {3, 0, 1, 3, 2, 5, 4};
        if (run == 0) { *len = 2; *code = 3; }
        else { *len = 3; *code = code6[run]; }
    } else {
        if (run <= 6) { *len = 3; *code = 7 - run; }
        else {
            *len = run - 3;       /* run-7 zeros then 1: length run-3 */
            *code = 1;
        }
    }
}

/* coeff[0..15] are CAVLC scan-order levels. */
static void cavlc_block(BitWriter *w, const int16_t coeff[16], int nc)
{
    int pos[16], val[16], tc = 0, t1 = 0;
    int i, k, zeros_left, suffix;

    for (k = 0; k < 16; ++k) {
        if (coeff[k]) {
            pos[tc] = k;
            val[tc] = coeff[k];
            ++tc;
        }
    }
    if (!tc) {
        cavlc_token(w, 0, 0, nc);
        return;
    }

    while (t1 < 3 && t1 < tc && abs(val[tc - 1 - t1]) == 1)
        ++t1;

    cavlc_token(w, tc, t1, nc);

    /* trailing-one signs, highest-frequency first */
    for (i = 0; i < t1; ++i)
        bw_bit(w, val[tc - 1 - i] < 0);

    suffix = (tc > 10 && t1 < 3) ? 1 : 0;
    /* other levels: next frequency below the trailing ones, then downward */
    for (i = t1; i < tc; ++i) {
        int spatial = tc - 1 - i;
        cavlc_level(w, val[spatial], i, t1, tc, &suffix);
    }

    if (tc < 16) {
        int max_pos = pos[tc - 1];
        int total_zeros = max_pos + 1 - tc;
        int off = h264e_g_total_zeros[tc - 1];
        int entry = h264e_g_total_zeros[off + total_zeros];
        int n = entry & 15;
        int code = entry >> 4;
        bw_bits(w, (uint32_t)code, n);
        zeros_left = total_zeros;
    } else {
        zeros_left = 0;
    }

    for (i = 0; i < tc - 1; ++i) {
        int gap_index = tc - 2 - i;
        int run = pos[gap_index + 1] - pos[gap_index] - 1;
        if (zeros_left > 0) {
            int code, len;
            run_before_code(run, zeros_left, &code, &len);
            bw_bits(w, (uint32_t)code, len);
            zeros_left -= run;
        }
        /* when zeros_left == 0 the decoder substitutes run=1 with no bits */
    }
}

/* ------------------------------------------------------------------ */
/* Parameter-set and slice construction                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int width_mbs, height_mbs;
    int poc_type;
    int init_qp;
    int transform8;
    int sps_scaling;
    int pps_scaling;
    int chroma_offset2;
    int profile_idc;
    int refs;
} StreamParams;

static void write_sps(BitWriter *w, const StreamParams *p)
{
    int profile = p->profile_idc ? p->profile_idc : 100;
    bw_bits(w, (uint32_t)profile, 8);
    bw_bits(w, 0, 8); /* constraint flags + reserved */
    bw_bits(w, 30, 8); /* level_idc */
    bw_ue(w, 0);      /* sps id */
    if (profile == 100 || profile == 110 || profile == 122 ||
        profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128) {
        bw_ue(w, 1); /* chroma_format_idc = 4:2:0 */
        bw_ue(w, 0); /* bit_depth_luma_minus8 */
        bw_ue(w, 0); /* bit_depth_chroma_minus8 */
        bw_bit(w, 0); /* qpprime_y_zero_transform_bypass_flag */
        bw_bit(w, p->sps_scaling);
        /* Tests that set sps_scaling=1 are expected to be rejected by the
         * parser immediately; do not need a legal scaling list here. */
    }
    bw_ue(w, 0); /* log2_max_frame_num_minus4 */
    bw_ue(w, p->poc_type);
    if (p->poc_type == 0)
        bw_ue(w, 0); /* log2_max_pic_order_cnt_lsb_minus4 */
    bw_ue(w, (uint32_t)(p->refs > 0 ? p->refs : 1)); /* max_num_ref_frames */
    bw_bit(w, 0); /* gaps_in_frame_num_allowed_flag */
    bw_ue(w, (uint32_t)(p->width_mbs - 1));
    bw_ue(w, (uint32_t)(p->height_mbs - 1));
    bw_bit(w, 1); /* frame_mbs_only_flag */
    bw_bit(w, 0); /* direct_8x8_inference_flag */
    bw_bit(w, 0); /* frame_cropping_flag */
    bw_bit(w, 0); /* vui_present_flag */
    bw_trailing(w);
}

static void write_pps(BitWriter *w, const StreamParams *p)
{
    bw_ue(w, 0); /* pps id */
    bw_ue(w, 0); /* sps id */
    bw_bit(w, 0); /* CAVLC */
    bw_bit(w, 0); /* bottom_field_pic_order_in_frame_present_flag */
    bw_ue(w, 0); /* one slice group */
    bw_ue(w, (uint32_t)((p->refs > 1 ? p->refs : 1) - 1)); /* l0 refs minus1 */
    bw_ue(w, 0); /* num_ref_idx_l1_default_active_minus1 */
    bw_bit(w, 0); /* weighted_pred_flag */
    bw_bits(w, 0, 2); /* weighted_bipred_idc */
    bw_se(w, p->init_qp - 26);
    bw_se(w, 0); /* pic_init_qs_minus26 */
    bw_se(w, 0); /* chroma_qp_index_offset */
    bw_bit(w, 1); /* deblocking_filter_control_present_flag */
    bw_bit(w, 0); /* constrained_intra_pred_flag */
    bw_bit(w, 0); /* redundant_pic_cnt_present_flag */
    if (p->transform8 || p->pps_scaling || p->chroma_offset2) {
        bw_bit(w, p->transform8);
        bw_bit(w, p->pps_scaling);
        bw_se(w, p->chroma_offset2);
    }
    bw_trailing(w);
}

static void append_param_sets(AnnexB *a, const StreamParams *p)
{
    BitWriter w;
    bw_init(&w); write_sps(&w, p); ab_nal_bw(a, 7, 3, &w);
    bw_init(&w); write_pps(&w, p); ab_nal_bw(a, 8, 3, &w);
}

static void write_slice_header(BitWriter *w, const StreamParams *p,
                               int slice_type, int first_mb, int frame_num,
                               int poc_lsb, int disable_deblock)
{
    bw_ue(w, (uint32_t)first_mb);
    bw_ue(w, (uint32_t)slice_type); /* 0=P, 2=I */
    bw_ue(w, 0); /* pps id */
    bw_bits(w, (uint32_t)frame_num & 0xF, 4);
    if (slice_type == 2)
        bw_ue(w, 0); /* idr_pic_id */
    if (p->poc_type == 0)
        bw_bits(w, (uint32_t)poc_lsb & 0xF, 4);
    if (slice_type == 0) {
        bw_bit(w, 0); /* num_ref_idx_active_override_flag */
        bw_bit(w, 0); /* ref_pic_list_reordering_flag_l0 */
    }
    /* nal_ref_idc is non-zero for all test slices */
    if (slice_type == 2) {
        bw_bit(w, 0); /* no_output_of_prior_pics_flag */
        bw_bit(w, 0); /* long_term_reference_flag */
    } else {
        bw_bit(w, 0); /* adaptive_ref_pic_marking_mode_flag */
    }
    bw_se(w, 0); /* slice_qp_delta */
    bw_ue(w, (uint32_t)disable_deblock);
    if (disable_deblock != 1) {
        bw_se(w, 0); /* slice_alpha_c0_offset_div2 */
        bw_se(w, 0); /* slice_beta_offset_div2 */
    }
}

typedef void (*MbWriter)(BitWriter *w, void *ctx, int mb);

static AnnexB build_picture(const StreamParams *p, int type, int frame_num,
                            int poc, int disable_deblock,
                            MbWriter writer, void *ctx)
{
    AnnexB a;
    BitWriter h;
    int mbs = p->width_mbs * p->height_mbs;
    memset(&a, 0, sizeof(a));
    append_param_sets(&a, p);
    bw_init(&h);
    write_slice_header(&h, p, type, 0, frame_num, poc, disable_deblock);
    if (writer) {
        int mb;
        for (mb = 0; mb < mbs; ++mb)
            writer(&h, ctx, mb);
    }
    bw_trailing(&h);
    ab_nal_bw(&a, type == 2 ? 5 : 1, 3, &h);
    return a;
}

/* ---------------- I_PCM reference picture ---------------- */

typedef struct {
    const uint8_t *yuv;
    int width, height;
} PcmContext;

static void write_pcm_picture(BitWriter *w, void *ctx_, int mb)
{
    PcmContext *c = ctx_;
    int wm = c->width / 16;
    int mx = mb % wm, my = mb / wm;
    int x0 = mx * 16, y0 = my * 16;
    uint8_t block[384];
    int y, x;
    memset(block, 128, sizeof(block));
    for (y = 0; y < 16; ++y)
        for (x = 0; x < 16; ++x)
            block[y*16+x] = c->yuv[(y0+y)*c->width + x0+x];
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            int cx = x0/2+x, cy = y0/2+y;
            block[256+y*8+x] =
                c->yuv[c->width*c->height + cy*(c->width/2) + cx];
            block[320+y*8+x] =
                c->yuv[c->width*c->height*5/4 + cy*(c->width/2) + cx];
        }
    }
    bw_ue(w, 25); /* I_PCM: parser adds 6 to the exp-golomb value */
    bw_bytes(w, block, sizeof(block));
}

static AnnexB build_idr_pcm(const StreamParams *p, const uint8_t *yuv)
{
    PcmContext c = { yuv, p->width_mbs*16, p->height_mbs*16 };
    return build_picture(p, 2, 0, 0, 1, write_pcm_picture, &c);
}

/* ---------------- public decoder harness ---------------- */

typedef struct {
    int count;
    uint32_t pic_id[8];
    uint32_t err[8];
    int w[8], h[8];
    uint8_t yuv[8][64*64*3/2];
} FrameStore;

typedef struct {
    storage_t dec;
    FrameStore frames;
} Harness;

static void harness_init(Harness *h)
{
    memset(h, 0, sizeof(*h));
    h264bsdInit(&h->dec, 1);
}

static void harness_destroy(Harness *h)
{
    h264bsdShutdown(&h->dec);
}

static void drain_frames(Harness *h)
{
    for (;;) {
        u32 id = 999, idr = 999, errs = 999;
        u8 *p = h264bsdNextOutputPicture(&h->dec, &id, &idr, &errs);
        int idx, w, hh;
        size_t sz;
        if (!p)
            break;
        if (h->frames.count >= 8)
            break;
        idx = h->frames.count++;
        w = (int)h264bsdPicWidth(&h->dec) * 16;
        hh = (int)h264bsdPicHeight(&h->dec) * 16;
        if (w > 64) w = 64;
        if (hh > 64) hh = 64;
        sz = (size_t)w * hh * 3 / 2;
        h->frames.w[idx] = w;
        h->frames.h[idx] = hh;
        h->frames.pic_id[idx] = id;
        h->frames.err[idx] = errs;
        memcpy(h->frames.yuv[idx], p, sz);
    }
}

static uint32_t decode_au(Harness *h, AnnexB *a, uint32_t pic_id)
{
    size_t off = 0;
    int guard = 0;
    while (off < a->n) {
        u32 used = 0;
        u32 r = h264bsdDecode(&h->dec, a->b + off, (u32)(a->n - off),
                              pic_id, &used);
        if (r >= H264BSD_ERROR)
            return r;
        if (used == 0) {
            r = h264bsdDecode(&h->dec, a->b + off, (u32)(a->n - off),
                              pic_id, &used);
            if (r >= H264BSD_ERROR)
                return r;
            if (used == 0)
                return H264BSD_ERROR;
        }
        off += used;
        drain_frames(h);
        if (++guard > 1000)
            return H264BSD_ERROR;
    }
    return H264BSD_RDY;
}

static void harness_flush(Harness *h)
{
    h264bsdFlushBuffer(&h->dec);
    drain_frames(h);
}

/* ---------------- independent scalar 8x8 model ---------------- */

static const int model_scan4[16][2] = {
    {0,0},{1,0},{0,1},{0,2},{1,1},{2,0},{3,0},{2,1},
    {1,2},{0,3},{1,3},{2,2},{3,1},{3,2},{2,3},{3,3}
};
static const int model_scale8[6][6] = {
    {20,18,32,19,25,24},{22,19,35,21,28,26},
    {26,23,42,24,33,31},{28,25,45,26,35,33},
    {32,28,51,30,40,38},{36,32,58,34,46,42}
};
static const int model_cat8[8][8] = {
    {0,1,2,1,0,1,2,1},{1,3,5,3,1,3,5,3},
    {2,5,4,5,2,5,4,5},{1,3,5,3,1,3,5,3},
    {0,1,2,1,0,1,2,1},{1,3,5,3,1,3,5,3},
    {2,5,4,5,2,5,4,5},{1,3,5,3,1,3,5,3}
};

static void model_idct1(int *v, int stride)
{
    int a0=v[0]+v[4*stride], a2=v[0]-v[4*stride];
    int a4=(v[2*stride]>>1)-v[6*stride];
    int a6=v[2*stride]+(v[6*stride]>>1);
    int b0=a0+a6,b2=a2+a4,b4=a2-a4,b6=a0-a6;
    int a1=-v[3*stride]+v[5*stride]-v[7*stride]-(v[7*stride]>>1);
    int a3= v[1*stride]+v[7*stride]-v[3*stride]-(v[3*stride]>>1);
    int a5=-v[1*stride]+v[7*stride]+v[5*stride]+(v[5*stride]>>1);
    int a7= v[1*stride]+v[3*stride]+v[5*stride]+(v[5*stride]>>1);
    int b1=(a7>>2)+a1,b3=a3+(a5>>2),b5=(a3>>2)-a5,b7=a7-(a1>>2);
    int x[8] = {b0+b7,b2+b5,b4+b3,b6+b1,b6-b1,b4-b3,b2-b5,b0-b7};
    int i;
    for (i=0;i<8;i++) v[i*stride]=x[i];
}

/* groups are the four CAVLC arrays for one 8x8 luma block. */
static void model_8x8(const int16_t groups[4][16], int qp, int out[64])
{
    int c[8][8];
    int g,k,x,y;
    memset(c,0,sizeof(c));
    for (g=0;g<4;g++) {
        int sx=(g&1)?4:0, sy=(g&2)?4:0;
        for (k=0;k<16;k++) {
            x=sx+model_scan4[k][0]; y=sy+model_scan4[k][1];
            if (groups[g][k])
                c[y][x] = groups[g][k] *
                    (model_scale8[qp%6][model_cat8[y][x]] << (qp/6));
        }
    }
    for (y=0;y<8;y++) model_idct1(&c[y][0],1);
    for (x=0;x<8;x++) model_idct1(&c[0][x],8);
    for (y=0;y<8;y++)
        for (x=0;x<8;x++)
            out[y*8+x] = (c[y][x]+32)>>6;
}

/* ---------------- generic coded P16x16 plan ---------------- */

typedef struct {
    int transform8;
    int ref_idx;
    int mvd_x, mvd_y;
    int qp_delta;
    int16_t coeff[16][16];
} MbPlan;

typedef struct {
    int width_mbs;
    int refs;
    int transform_mode;
    MbPlan mb[8];
} PlanCtx;

static const int plan_bx[16] =
    {0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3};
static const int plan_by[16] =
    {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3};

static int plan_tc(const MbPlan *p, int idx)
{
    int k, n = 0;
    for (k = 0; k < 16; ++k)
        if (p->coeff[idx][k]) ++n;
    return n;
}

static int plan_idx_at(int bx, int by)
{
    int i;
    for (i = 0; i < 16; ++i)
        if (plan_bx[i] == bx && plan_by[i] == by)
            return i;
    return -1;
}

static int plan_nc(PlanCtx *c, int mb, int idx)
{
    int bx = plan_bx[idx], by = plan_by[idx];
    int a = 0, b = 0, have_a = 0, have_b = 0;
    if (bx > 0) {
        int ni = plan_idx_at(bx-1, by);
        a = plan_tc(&c->mb[mb], ni); have_a = 1;
    } else if ((mb % c->width_mbs) > 0) {
        static const int right_idx[4] = {5,7,13,15};
        a = plan_tc(&c->mb[mb-1], right_idx[by]); have_a = 1;
    }
    if (by > 0) {
        int ni = plan_idx_at(bx, by-1);
        b = plan_tc(&c->mb[mb], ni); have_b = 1;
    }
    if (have_a && have_b) return (a+b+1)>>1;
    if (have_a) return a;
    if (have_b) return b;
    return 0;
}

static void write_plan_mb(BitWriter *w, void *ctx_, int mb)
{
    PlanCtx *c = ctx_;
    MbPlan *p = &c->mb[mb];
    int idx;
    bw_ue(w, 0); /* skip_run */
    bw_ue(w, 0); /* P_L0_16x16 */
    if (c->refs == 2)
        bw_bit(w, p->ref_idx ? 0 : 1); /* truncated: bit=1 -> 0 */
    else if (c->refs > 2)
        bw_ue(w, (uint32_t)p->ref_idx);
    bw_se(w, p->mvd_x);
    bw_se(w, p->mvd_y);
    bw_ue(w, 11); /* inter mapped codeNum 11 -> cbp=15 */
    bw_se(w, p->qp_delta);
    if (c->transform_mode)
        bw_bit(w, p->transform8);
    for (idx = 0; idx < 16; ++idx)
        cavlc_block(w, p->coeff[idx], plan_nc(c, mb, idx));
}

/* Independent scalar 4x4 inverse scaling/transform. */
static const int model_scale4[6][3] = {
    {10,13,16},{11,14,18},{13,16,20},
    {14,18,23},{16,20,25},{18,23,29}
};
static void model_4x4(const int16_t coeff[16], int qp, int out[16])
{
    int a[16], i;
    int qm=qp%6, ds=qp/6;
    static const int cat[16] = {0,1,0,1,1,2,1,1,0,1,0,1,1,1,1,1};
    for (i=0;i<16;i++)
        a[i] = coeff[i] * (model_scale4[qm][cat[i]] << ds);
    /* row transform */
    for (i=0;i<4;i++) {
        int *r=a+4*i;
        int t0=r[0]+r[2], t1=r[0]-r[2];
        int t2=(r[1]>>1)-r[3], t3=r[1]+(r[3]>>1);
        int o0=t0+t3,o1=t1+t2,o2=t1-t2,o3=t0-t3;
        r[0]=o0;r[1]=o1;r[2]=o2;r[3]=o3;
    }
    /* column transform and rounding */
    for (i=0;i<4;i++) {
        int t0=a[i]+a[8+i], t1=a[i]-a[8+i];
        int t2=(a[4+i]>>1)-a[12+i], t3=a[4+i]+(a[12+i]>>1);
        int o0=(t0+t3+32)>>6, o1=(t1+t2+32)>>6;
        int o2=(t1-t2+32)>>6, o3=(t0-t3+32)>>6;
        out[i]=o0; out[4+i]=o1; out[8+i]=o2; out[12+i]=o3;
    }
}

static void plan_expected_luma(const PlanCtx *c, int qp, uint8_t *plane,
                               int width, int height, const uint8_t *base)
{
    int mb, idx, k;
    memcpy(plane, base, (size_t)width*height);
    for (mb=0; mb<c->width_mbs*((height+15)/16); mb++) {
        int mx=mb%c->width_mbs, my=mb/c->width_mbs;
        int x0=mx*16,y0=my*16;
        if (c->mb[mb].transform8) {
            for (k=0;k<4;k++) {
                int16_t groups[4][16];
                int res[64], g, xx, yy, ox=(k&1)?8:0, oy=(k&2)?8:0;
                memset(groups,0,sizeof(groups));
                for (g=0;g<4;g++)
                    memcpy(groups[g], c->mb[mb].coeff[k*4+g],
                           sizeof(groups[g]));
                model_8x8(groups, qp+c->mb[mb].qp_delta, res);
                for(yy=0;yy<8;yy++) for(xx=0;xx<8;xx++) {
                    int X=x0+ox+xx,Y=y0+oy+yy,v;
                    if(X>=width||Y>=height) continue;
                    v=base[Y*width+X]+res[yy*8+xx];
                    plane[Y*width+X]=(uint8_t)(v<0?0:v>255?255:v);
                }
            }
        } else {
            for (idx=0;idx<16;idx++) {
                int res[16], xx, yy;
                int bx=plan_bx[idx]*4, by=plan_by[idx]*4;
                model_4x4(c->mb[mb].coeff[idx],
                          qp+c->mb[mb].qp_delta,res);
                for(yy=0;yy<4;yy++) for(xx=0;xx<4;xx++) {
                    int X=x0+bx+xx,Y=y0+by+yy,v;
                    if(X>=width||Y>=height) continue;
                    v=base[Y*width+X]+res[yy*4+xx];
                    plane[Y*width+X]=(uint8_t)(v<0?0:v>255?255:v);
                }
            }
        }
    }
}

/* Normal BS=2 luma vertical edge filter, independent scalar edge model. */
static void model_vertical_bs2(uint8_t *p, int width, int x, int y0)
{
    int row;
    for (row=0; row<4; row++) {
        int y=y0+row;
        uint8_t *q=p+y*width+x;
        int p3=q[-4],p2=q[-3],p1=q[-2],p0=q[-1];
        int q0=q[0],q1=q[1],q2=q[2],q3=q[3];
        int alpha=63,beta=9,tc=4; /* QP 38, zero offsets, BS=2 */
        int delta, tmp, val;
        (void)p3;(void)q3;
        if (abs(p0-q0)>=alpha || abs(p1-p0)>=beta ||
            abs(q1-q0)>=beta)
            continue;
        tmp=tc;
        if (abs(p2-p0)<beta) {
            val=(p2+((p0+q0+1)>>1)-2*p1)>>1;
            q[-2]=(uint8_t)CLIP3(0,255,p1+CLIP3(-tc,tc,val));
            ++tmp;
        }
        if (abs(q2-q0)<beta) {
            val=(q2+((p0+q0+1)>>1)-2*q1)>>1;
            q[1]=(uint8_t)CLIP3(0,255,q1+CLIP3(-tc,tc,val));
            ++tmp;
        }
        val=(((q0-p0)<<2)+(p1-q1)+4)>>3;
        delta=CLIP3(-tmp,tmp,val);
        q[-1]=(uint8_t)CLIP3(0,255,p0+delta);
        q[0]=(uint8_t)CLIP3(0,255,q0-delta);
    }
}

static void model_filter_32x16(uint8_t *plane)
{
    int band, y;
    /* Production order per 4-row band and MB: x=8, then x=16,20,24,28. */
    for (band=0; band<4; band++) {
        y=band*4;
        model_vertical_bs2(plane,32,8,y);
        model_vertical_bs2(plane,32,16,y);
        model_vertical_bs2(plane,32,20,y);
        model_vertical_bs2(plane,32,24,y);
        model_vertical_bs2(plane,32,28,y);
    }
}

/* ---------------- concrete MB streams ---------------- */

typedef struct {
    int qp;
    int16_t groups[4][16];
    int transform_flag;
    int mb_type_code; /* exp-golomb mb_type value: 0=P16x16 */
} One8Context;

static int cavlc_tc(const int16_t coeff[16])
{
    int k,n=0;
    for(k=0;k<16;k++) if(coeff[k]) n++;
    return n;
}

static void write_one_8_mb(BitWriter *w, void *ctx_, int mb)
{
    One8Context *c = ctx_;
    int pairs = 1, i;
    (void)mb;
    bw_ue(w, 0); /* skip_run = 0 */
    bw_ue(w, (uint32_t)c->mb_type_code);
    if (c->mb_type_code == 1 || c->mb_type_code == 2)
        pairs = 2;
    else if (c->mb_type_code == 3 || c->mb_type_code == 4) {
        for (i = 0; i < 4; ++i)
            bw_ue(w, 0); /* all four P_L0_8x8 sub-partitions */
        pairs = 4;
    }
    for (i = 0; i < pairs; ++i) {
        bw_se(w, 0); bw_se(w, 0);
    }
    bw_ue(w, 2); /* inter cbp codeNum 2 maps to cbp=1 (luma 8x8 area 0) */
    bw_se(w, c->qp - 26);
    bw_bit(w, c->transform_flag);
    cavlc_block(w, c->groups[0], 0);
    cavlc_block(w, c->groups[1], cavlc_tc(c->groups[0]));
    cavlc_block(w, c->groups[2], cavlc_tc(c->groups[0]));
    cavlc_block(w, c->groups[3],
        (cavlc_tc(c->groups[2]) +
         cavlc_tc(c->groups[1]) + 1) >> 1);
}

/* The simplified nC above is only valid for deliberately zero groups 1/2;
 * residual cases use a dedicated writer with explicit nC values. */
typedef struct {
    int qp;
    int16_t groups[4][16];
    int nc[4];
} FourGroupContext;

static void write_four_group_mb(BitWriter *w, void *ctx_, int mb)
{
    FourGroupContext *c = ctx_;
    int g;
    (void)mb;
    bw_ue(w, 0);
    bw_ue(w, 0); /* P_L0_16x16 */
    bw_se(w, 0); bw_se(w, 0);
    bw_ue(w, 2); /* cbp=1 */
    bw_se(w, c->qp - 26);
    bw_bit(w, 1);
    for (g=0;g<4;g++)
        cavlc_block(w, c->groups[g], c->nc[g]);
}

static void flat_yuv(uint8_t *p, int w, int h, uint8_t y)
{
    int i;
    for (i=0;i<w*h;i++) p[i]=y;
    for (i=0;i<w*h/2;i++) p[w*h+i]=128;
}

static int decode_one_idr_and_p(const StreamParams *sp,
                                const uint8_t *pcm,
                                AnnexB *p_au,
                                Harness *h)
{
    AnnexB idr = build_idr_pcm(sp, pcm);
    uint32_t r;
    harness_init(h);
    r = decode_au(h, &idr, 0);
    if (r != H264BSD_RDY && r != H264BSD_PIC_RDY && r != H264BSD_HDRS_RDY)
        return (int)r;
    r = decode_au(h, p_au, 1);
    if (r != H264BSD_RDY && r != H264BSD_PIC_RDY && r != H264BSD_HDRS_RDY)
        return (int)r;
    harness_flush(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* syntax group                                                       */
/* ------------------------------------------------------------------ */

static int case_syntax_high_sps_pps(void)
{
    Harness h;
    StreamParams p = {1,1,0,26,1,0,0,0,100};
    static uint8_t yuv[256*3/2];
    AnnexB idr;
    uint32_t r;
    flat_yuv(yuv,16,16,100);
    idr = build_idr_pcm(&p,yuv);
    harness_init(&h);
    r=decode_au(&h,&idr,7);
    TASK_CHECK(r <= H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.count >= 1);
    TASK_CHECK(h.frames.pic_id[0] == 7);
    TASK_CHECK(h.frames.err[0] == 0);
    harness_destroy(&h);
    return 0;
}

static int syntax_expect_reject(const StreamParams *p)
{
    Harness h;
    BitWriter w;
    AnnexB a;
    uint32_t r;
    memset(&a,0,sizeof(a));
    bw_init(&w); write_sps(&w,p); ab_nal_bw(&a,7,3,&w);
    harness_init(&h);
    r=decode_au(&h,&a,0);
    harness_destroy(&h);
    return r >= H264BSD_ERROR ? 0 : 1;
}

static int case_syntax_unsupported(void)
{
    StreamParams p = {1,1,0,26,1,0,0,0,110};
    TASK_CHECK(syntax_expect_reject(&p)==0);
    p.profile_idc=100; p.sps_scaling=1;
    TASK_CHECK(syntax_expect_reject(&p)==0);
    p.sps_scaling=0; p.poc_type=1;
    TASK_CHECK(syntax_expect_reject(&p)==0);
    return 0;
}

static int case_syntax_pps_extension_errors(void)
{
    Harness h;
    StreamParams p = {1,1,0,26,1,0,1,3,100};
    BitWriter sw,pw;
    AnnexB a;
    uint32_t r;
    memset(&a,0,sizeof(a));
    bw_init(&sw); write_sps(&sw,&p); ab_nal_bw(&a,7,3,&sw);
    bw_init(&pw); write_pps(&pw,&p); ab_nal_bw(&a,8,3,&pw);
    harness_init(&h);
    r=decode_au(&h,&a,0);
    TASK_CHECK(r >= H264BSD_ERROR);
    harness_destroy(&h);

    p.pps_scaling=1; p.chroma_offset2=0;
    memset(&a,0,sizeof(a));
    bw_init(&sw); write_sps(&sw,&p); ab_nal_bw(&a,7,3,&sw);
    bw_init(&pw); write_pps(&pw,&p); ab_nal_bw(&a,8,3,&pw);
    harness_init(&h);
    r=decode_au(&h,&a,0);
    TASK_CHECK(r >= H264BSD_ERROR);
    harness_destroy(&h);
    return 0;
}

static void write_intra8_bad(BitWriter *w)
{
    int i;
    bw_ue(w,0); /* I slice mb_type codeNum 0 -> I_4x4 */
    for (i=0;i<16;i++) bw_bit(w,1); /* all prediction mode flags */
    bw_ue(w,0); /* intra chroma pred mode DC */
    bw_ue(w,2); /* intra cbp code 2 -> 15 */
    bw_se(w,0); /* qp delta */
    bw_bit(w,1); /* transform_size_8x8_flag = 1, unsupported */
}

static int case_syntax_intra8_rejected(void)
{
    Harness h;
    StreamParams p = {1,1,0,26,1,0,0,0,100};
    AnnexB a = build_picture(&p,2,0,0,1,(MbWriter)write_intra8_bad,NULL);
    uint32_t r;
    harness_init(&h);
    r=decode_au(&h,&a,3);
    TASK_CHECK(r >= H264BSD_ERROR);
    harness_destroy(&h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* residual group                                                     */
/* ------------------------------------------------------------------ */

static int residual_case_qp(int qp, int16_t levels[4][16])
{
    StreamParams sp = {1,1,0,26,1,0,0,0,100};
    static uint8_t pcm[384];
    Harness h;
    AnnexB p_au;
    BitWriter hw;
    FourGroupContext ctx;
    int expect[64], i, x, y;
    int rc;
    int tc[4] = {0,0,0,0}, g, k;

    /* Flat PCM reference makes zero-MV prediction exactly 128. */
    memset(pcm,128,sizeof(pcm));
    memset(&ctx,0,sizeof(ctx));
    ctx.qp=qp;
    memcpy(ctx.groups,levels,sizeof(ctx.groups));
    for (g=0;g<4;g++)
        for(k=0;k<16;k++)
            if(levels[g][k]) ++tc[g];
    ctx.nc[0]=0;
    ctx.nc[1]=tc[0];
    ctx.nc[2]=tc[0];
    ctx.nc[3]=(tc[2]+tc[1]+1)>>1;
    bw_init(&hw);
    write_slice_header(&hw,&sp,0,0,1,2,1);
    write_four_group_mb(&hw,&ctx,0);
    bw_trailing(&hw);
    memset(&p_au,0,sizeof(p_au));
    append_param_sets(&p_au,&sp);
    ab_nal_bw(&p_au,1,3,&hw);

    rc=decode_one_idr_and_p(&sp,pcm,&p_au,&h);
    TASK_CHECK(rc==0);
    TASK_CHECK(h.frames.count>=2);
    TASK_CHECK(h.frames.err[1]==0);

    model_8x8(levels,(unsigned)qp,expect);
    for (y=0;y<8;y++) for(x=0;x<8;x++) {
        int idx=y*16+x; /* first 8x8 area of one MB */
        int want=128+expect[y*8+x];
        uint8_t got=h.frames.yuv[1][idx];
        if(want<0)want=0; if(want>255)want=255;
        TASK_CHECK(got==(uint8_t)want);
    }
    /* untouched lower/right luma and chroma stay at PCM 128 */
    for(i=8;i<256;i++) {
        int x=i%16,y=i/16;
        if(x<8&&y<8) continue;
        TASK_CHECK(h.frames.yuv[1][i]==128);
    }
    harness_destroy(&h);
    return 0;
}

static int case_residual_basic(void)
{
    int16_t g[4][16] = {{0}};
    /* Distinct TotalCoeff values 1, 2, 3, 4; include DC and tail coeffs. */
    g[0][15]=-7;
    g[1][0]=5; g[1][1]=-1;
    g[2][0]=9; g[2][1]=2; g[2][2]=-3;
    g[3][0]=-11; g[3][5]=4; g[3][10]=6; g[3][15]=-2;
    return residual_case_qp(27,g);
}

static int case_residual_qp_boundaries(void)
{
    int qps[4]={23,24,29,30};
    int qi;
    for(qi=0;qi<4;qi++){
        int16_t g[4][16]={{0}};
        g[0][0]=qi%2?13:-13;
        g[1][15]=qi*2-7;
        g[2][7]=17;
        g[3][1]=-19;
        int rc=residual_case_qp(qps[qi],g);
        TASK_CHECK(rc==0);
    }
    return 0;
}

static int case_residual_zero_and_signs(void)
{
    int16_t g[4][16]={{0}};
    /* all-zero first group is legal and must not affect later groups */
    g[1][15]=1;
    g[2][0]=-1;
    g[3][15]=-1; g[3][14]=1; g[3][13]=-1;
    return residual_case_qp(32,g);
}

/* ------------------------------------------------------------------ */
/* filter group: first pass only checks flag propagation and planes   */
/* ------------------------------------------------------------------ */

static int case_filter_disabled_plane(void)
{
    /* The residual tests already decode with disable_deblocking_filter_idc=1
     * and compare every affected pixel; this case explicitly checks stored
     * 8x8 mode/flag information. */
    Harness h;
    StreamParams sp={1,1,0,26,1,0,0,0,100};
    static uint8_t pcm[384];
    FourGroupContext ctx;
    BitWriter hw; AnnexB pa;
    AnnexB idr=build_idr_pcm(&sp,pcm);
    uint32_t r;
    memset(pcm,120,sizeof(pcm));
    memset(&ctx,0,sizeof(ctx)); ctx.qp=26; ctx.groups[0][0]=8;
    bw_init(&hw); write_slice_header(&hw,&sp,0,0,1,2,1);
    write_four_group_mb(&hw,&ctx,0); bw_trailing(&hw);
    memset(&pa,0,sizeof(pa)); append_param_sets(&pa,&sp); ab_nal_bw(&pa,1,3,&hw);
    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,0)<=H264BSD_HDRS_RDY);
    r=decode_au(&h,&pa,1);
    TASK_CHECK(r<=H264BSD_HDRS_RDY);
    TASK_CHECK(h.dec.mb[0].transform8x8ModeFlag==1);
    TASK_CHECK(h.dec.mb[0].transform8x8Flag==1);
    TASK_CHECK(h.dec.mb[0].disableDeblockingFilterIdc==1);
    harness_flush(&h);
    harness_destroy(&h);
    return 0;
}

static int case_filter_enabled_smoke(void)
{
    Harness h;
    StreamParams sp={1,1,0,28,1,0,0,0,100};
    static uint8_t pcm[384];
    FourGroupContext ctx;
    BitWriter hw; AnnexB pa;
    AnnexB idr;
    uint32_t r;
    memset(pcm,128,sizeof(pcm));
    pcm[0]=160; pcm[15]=96; pcm[16*15]=96; pcm[16*15+15]=160;
    idr=build_idr_pcm(&sp,pcm);
    memset(&ctx,0,sizeof(ctx)); ctx.qp=28; ctx.groups[0][0]=20;
    bw_init(&hw); write_slice_header(&hw,&sp,0,0,1,2,0);
    write_four_group_mb(&hw,&ctx,0); bw_trailing(&hw);
    memset(&pa,0,sizeof(pa)); append_param_sets(&pa,&sp); ab_nal_bw(&pa,1,3,&hw);
    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,0)<=H264BSD_HDRS_RDY);
    r=decode_au(&h,&pa,1);
    TASK_CHECK(r<=H264BSD_HDRS_RDY);
    TASK_CHECK(h.dec.mb[0].disableDeblockingFilterIdc==0);
    TASK_CHECK(h.frames.count>=1);
    harness_flush(&h);
    harness_destroy(&h);
    return 0;
}

static int case_filter_mixed_edge_model(void)
{
    StreamParams sp={2,1,0,38,1,0,0,0,100,1};
    static uint8_t pcm[32*16*3/2];
    static uint8_t pre[32*16];
    static uint8_t filtered[32*16];
    PlanCtx plan;
    Harness off, on;
    AnnexB idr, p_off, p_on;
    int x,y;
    uint32_t r;

    memset(&plan,0,sizeof(plan));
    plan.width_mbs=2; plan.refs=1; plan.transform_mode=1;
    plan.mb[0].transform8=1;
    plan.mb[1].transform8=0;
    for (x=0;x<32;x++) pre[x]= (uint8_t)(96+2*x);
    /* Four DC groups, one in each 8x8 area of MB0. */
    plan.mb[0].coeff[0][0]=1;
    plan.mb[0].coeff[4][0]=1;
    plan.mb[0].coeff[8][0]=1;
    plan.mb[0].coeff[12][0]=1;
    /* Every old 4x4 block in MB1 gets the same DC residual. */
    for (x=0;x<16;x++) plan.mb[1].coeff[x][0]=1;

    memset(pcm,128,sizeof(pcm));
    for (y=0;y<16;y++)
        for (x=0;x<32;x++) {
            int residual = x<16 ? 26 : 13;
            int v=pre[x]-residual;
            pcm[y*32+x]=(uint8_t)v;
        }

    idr=build_idr_pcm(&sp,pcm);
    p_off=build_picture(&sp,0,1,2,1,write_plan_mb,&plan);
    p_on =build_picture(&sp,0,1,2,0,write_plan_mb,&plan);

    harness_init(&off);
    TASK_CHECK(decode_au(&off,&idr,0)<=H264BSD_HDRS_RDY);
    r=decode_au(&off,&p_off,1);
    TASK_CHECK(r<=H264BSD_HDRS_RDY);
    harness_flush(&off);
    TASK_CHECK(off.frames.count>=2);
    TASK_CHECK(off.dec.mb[0].transform8x8Flag==1);
    TASK_CHECK(off.dec.mb[1].transform8x8Flag==0);

    plan_expected_luma(&plan,38,filtered,32,16,pcm);
    /* Disabled deblock: production plane matches scalar residual model. */
    for (y=0;y<16;y++)
        TASK_CHECK(!memcmp(off.frames.yuv[1]+y*32, filtered+y*32, 32));

    memcpy(pre,filtered,sizeof(pre));
    model_filter_32x16(filtered);
    /* The chosen linear ramp must actually exercise the edge model. */
    TASK_CHECK(memcmp(filtered,pre,sizeof(pre))!=0);
    TASK_CHECK(filtered[6] != pre[6] || filtered[9] != pre[9]);
    /* x=4 is inside the first 8x8 transform and must remain untouched. */
    for (y=0;y<16;y++)
        TASK_CHECK(filtered[y*32+4] == pre[y*32+4]);

    harness_init(&on);
    TASK_CHECK(decode_au(&on,&idr,2)<=H264BSD_HDRS_RDY);
    r=decode_au(&on,&p_on,3);
    TASK_CHECK(r<=H264BSD_HDRS_RDY);
    harness_flush(&on);
    TASK_CHECK(on.frames.count>=2);
    for (y=0;y<16;y++)
        for (x=0;x<32;x++)
            TASK_CHECK(on.frames.yuv[1][y*32+x]==filtered[y*32+x]);
    /* chroma remained flat PCM 128 */
    TASK_CHECK(on.frames.yuv[1][512]==128);
    TASK_CHECK(on.frames.yuv[1][640]==128);

    harness_destroy(&off);
    harness_destroy(&on);
    return 0;
}

/* ------------------------------------------------------------------ */
/* compat group                                                       */
/* ------------------------------------------------------------------ */

static int case_compat_baseline_public(void)
{
    /* A High IDR I_PCM frame followed by a skip P frame exercises the public
     * decode/output/flush/picId path without changing Baseline semantics. */
    Harness h;
    StreamParams sp={1,1,2,26,1,0,0,0,100};
    static uint8_t yuv[384];
    AnnexB idr,p;
    BitWriter w;
    memset(yuv,111,256); memset(yuv+256,128,128);
    idr=build_idr_pcm(&sp,yuv);
    bw_init(&w);
    write_slice_header(&w,&sp,0,0,1,2,1);
    bw_ue(&w,1); /* skip_run = 1 */
    bw_trailing(&w);
    memset(&p,0,sizeof(p)); append_param_sets(&p,&sp); ab_nal_bw(&p,1,3,&w);
    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,11)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&h,&p,12)<=H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.count>=2);
    TASK_CHECK(h.frames.pic_id[0]==11 || h.frames.pic_id[1]==11);
    TASK_CHECK(h.frames.err[0]==0 && h.frames.err[1]==0);
    harness_destroy(&h);
    return 0;
}

static int case_compat_output_order_flush(void)
{
    StreamParams sp={1,1,0,26,0,0,0,0,100,1};
    static uint8_t pcm[384];
    Harness h;
    AnnexB idr, p1, p2;
    BitWriter w;
    u32 id=0, idr_flag=0, errs=99;
    u8 *more;

    memset(pcm,132,256); memset(pcm+256,128,128);
    idr=build_idr_pcm(&sp,pcm);

    bw_init(&w);
    write_slice_header(&w,&sp,0,0,1,2,1);
    bw_ue(&w,1); bw_trailing(&w);
    memset(&p1,0,sizeof(p1)); append_param_sets(&p1,&sp); ab_nal_bw(&p1,1,3,&w);

    bw_init(&w);
    write_slice_header(&w,&sp,0,0,2,4,1);
    bw_ue(&w,1); bw_trailing(&w);
    memset(&p2,0,sizeof(p2)); append_param_sets(&p2,&sp); ab_nal_bw(&p2,1,3,&w);

    memset(&h,0,sizeof(h));
    TASK_CHECK(h264bsdInit(&h.dec,0)==HANTRO_OK); /* DPB reordering enabled */
    TASK_CHECK(decode_au(&h,&idr,100)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&h,&p1,101)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&h,&p2,102)<=H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.count==3);
    TASK_CHECK(h.frames.pic_id[0]==100);
    TASK_CHECK(h.frames.pic_id[1]==101);
    TASK_CHECK(h.frames.pic_id[2]==102);
    TASK_CHECK(!h.frames.err[0] && !h.frames.err[1] && !h.frames.err[2]);
    TASK_CHECK(h.frames.yuv[0][0]==132);
    TASK_CHECK(h.frames.yuv[1][0]==132);
    TASK_CHECK(h.frames.yuv[2][0]==132);
    more=h264bsdNextOutputPicture(&h.dec,&id,&idr_flag,&errs);
    TASK_CHECK(more==NULL);
    harness_destroy(&h);
    return 0;
}

static int case_compat_two_instances_zero_mask(void)
{
    Harness a,b;
    StreamParams sp={1,1,0,26,1,0,0,0,100};
    static uint8_t yuv[384];
    AnnexB idr;
    memset(yuv,90,256); memset(yuv+256,128,128);
    idr=build_idr_pcm(&sp,yuv);
    harness_init(&a); harness_init(&b);
    TASK_CHECK(decode_au(&a,&idr,1)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&b,&idr,2)<=H264BSD_HDRS_RDY);
    harness_flush(&a); harness_flush(&b);
    TASK_CHECK(a.frames.count>=1 && b.frames.count>=1);
    TASK_CHECK(a.frames.yuv[0][0]==90);
    TASK_CHECK(b.frames.yuv[0][0]==90);
    TASK_CHECK(a.frames.err[0]==0 && b.frames.err[0]==0);
    harness_destroy(&a); harness_destroy(&b);
    return 0;
}

static int case_syntax_partition8(void)
{
    static const int codes[4] = {1,2,3,4};
    int p;
    for (p=0;p<4;p++) {
        StreamParams sp={1,1,0,26,1,0,0,0,100,1};
        static uint8_t pcm[384];
        One8Context ctx;
        AnnexB au;
        Harness h;
        int rc;
        memset(pcm,128,sizeof(pcm));
        memset(&ctx,0,sizeof(ctx));
        ctx.qp=26; ctx.transform_flag=1; ctx.mb_type_code=codes[p];
        ctx.groups[0][0]=7;
        au=build_picture(&sp,0,1,2,1,write_one_8_mb,&ctx);
        rc=decode_one_idr_and_p(&sp,pcm,&au,&h);
        TASK_CHECK(rc==0);
        TASK_CHECK(h.frames.count>=2);
        TASK_CHECK(h.frames.err[1]==0);
        TASK_CHECK(h.dec.mb[0].transform8x8Flag==1);
        TASK_CHECK(h.frames.yuv[1][0]!=128);
        harness_destroy(&h);
    }
    return 0;
}

static int case_syntax_sub8_and_cbp0(void)
{
    StreamParams sp={1,1,0,26,1,0,0,0,100,1};
    static uint8_t pcm[384];
    BitWriter w;
    AnnexB idr, au;
    Harness h;
    uint32_t r;
    int i;

    memset(pcm,128,sizeof(pcm));
    idr=build_idr_pcm(&sp,pcm);

    /* P_8x8 with four 8x4 sub-partitions: no transform_size_8x8_flag. */
    bw_init(&w);
    write_slice_header(&w,&sp,0,0,1,2,1);
    bw_ue(&w,0); /* skip run */
    bw_ue(&w,3); /* P_8x8 */
    for (i=0;i<4;i++) bw_ue(&w,1); /* P_L0_8x4 */
    for (i=0;i<8;i++) { bw_se(&w,0); bw_se(&w,0); }
    bw_ue(&w,0); /* inter CBP codeNum 0 -> cbp 0 */
    bw_trailing(&w);
    memset(&au,0,sizeof(au));
    append_param_sets(&au,&sp);
    ab_nal_bw(&au,1,3,&w);

    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,0)<=H264BSD_HDRS_RDY);
    r=decode_au(&h,&au,1);
    TASK_CHECK(r<=H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.count>=2);
    TASK_CHECK(h.dec.mb[0].transform8x8ModeFlag==1);
    TASK_CHECK(h.dec.mb[0].transform8x8Flag==0);
    TASK_CHECK(h.frames.yuv[1][0]==128);
    harness_destroy(&h);

    /* P_L0_16x16 with luma CBP zero likewise has no transform flag. */
    bw_init(&w);
    write_slice_header(&w,&sp,0,0,2,4,1);
    bw_ue(&w,0);
    bw_ue(&w,0);
    bw_se(&w,0); bw_se(&w,0);
    bw_ue(&w,0); /* cbp=0: no qp_delta or transform flag */
    bw_trailing(&w);
    memset(&au,0,sizeof(au));
    append_param_sets(&au,&sp);
    ab_nal_bw(&au,1,3,&w);
    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,0)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&h,&au,2)<=H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.err[1]==0);
    TASK_CHECK(h.dec.mb[0].transform8x8Flag==0);
    harness_destroy(&h);
    return 0;
}

static int case_residual_nc_chain(void)
{
    StreamParams sp={2,1,0,26,1,0,0,0,100,1};
    static uint8_t pcm[32*16*3/2];
    static uint8_t expected[32*16];
    PlanCtx plan;
    Harness h;
    AnnexB idr, au;
    int rc,x,y;

    memset(pcm,128,sizeof(pcm));
    memset(&plan,0,sizeof(plan));
    plan.width_mbs=2; plan.refs=1; plan.transform_mode=1;
    plan.mb[0].transform8=1;
    plan.mb[0].coeff[5][0]=5;
    plan.mb[0].coeff[5][1]=-2; /* TotalCoeff=2 feeds MB1 block0 nC */
    plan.mb[1].transform8=0;
    plan.mb[1].coeff[0][0]=11;

    idr=build_idr_pcm(&sp,pcm);
    au=build_picture(&sp,0,1,2,1,write_plan_mb,&plan);
    rc=decode_one_idr_and_p(&sp,pcm,&au,&h);
    TASK_CHECK(rc==0);
    TASK_CHECK(h.dec.mb[0].totalCoeff[5]==2);
    TASK_CHECK(h.dec.mb[1].totalCoeff[0]==1);
    plan_expected_luma(&plan,26,expected,32,16,pcm);
    for(y=0;y<16;y++) for(x=0;x<32;x++)
        TASK_CHECK(h.frames.yuv[1][y*32+x]==expected[y*32+x]);
    harness_destroy(&h);
    return 0;
}

static int case_compat_reference_chain(void)
{
    StreamParams sp={1,1,0,26,1,0,0,0,100,1};
    static uint8_t pcm[384];
    FourGroupContext ctx;
    BitWriter bw;
    AnnexB idr, p1, skip;
    Harness h;
    int rc;

    memset(pcm,128,sizeof(pcm));
    memset(&ctx,0,sizeof(ctx));
    ctx.qp=26; ctx.nc[0]=0;
    ctx.groups[0][0]=17;
    bw_init(&bw);
    write_slice_header(&bw,&sp,0,0,1,2,1);
    write_four_group_mb(&bw,&ctx,0);
    bw_trailing(&bw);
    memset(&p1,0,sizeof(p1));
    append_param_sets(&p1,&sp);
    ab_nal_bw(&p1,1,3,&bw);

    bw_init(&bw);
    write_slice_header(&bw,&sp,0,0,2,4,1);
    bw_ue(&bw,1); /* P_Skip run */
    bw_trailing(&bw);
    memset(&skip,0,sizeof(skip));
    append_param_sets(&skip,&sp);
    ab_nal_bw(&skip,1,3,&bw);

    idr=build_idr_pcm(&sp,pcm);
    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,10)<=H264BSD_HDRS_RDY);
    rc=decode_au(&h,&p1,11);
    TASK_CHECK(rc==0);
    TASK_CHECK(decode_au(&h,&skip,12)<=H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.count>=3);
    TASK_CHECK(h.frames.pic_id[0]==10);
    TASK_CHECK(h.frames.pic_id[1]==11);
    TASK_CHECK(h.frames.pic_id[2]==12);
    TASK_CHECK(!memcmp(h.frames.yuv[2],h.frames.yuv[1],384));
    TASK_CHECK(!h.frames.err[0] && !h.frames.err[1] && !h.frames.err[2]);
    harness_destroy(&h);
    return 0;
}

static int case_compat_multi_slice(void)
{
    StreamParams sp={2,1,0,26,1,0,0,0,100,1};
    static uint8_t pcm[768];
    static uint8_t expected[512];
    PlanCtx plan;
    Harness h;
    AnnexB idr, au;
    int mb,x,y;

    memset(pcm,128,sizeof(pcm));
    memset(&plan,0,sizeof(plan));
    plan.width_mbs=2; plan.transform_mode=1;
    plan.mb[0].transform8=1; plan.mb[0].coeff[0][0]=3;
    plan.mb[1].transform8=0; plan.mb[1].coeff[0][0]=4;

    idr=build_idr_pcm(&sp,pcm);
    memset(&au,0,sizeof(au));
    append_param_sets(&au,&sp);
    for(mb=0;mb<2;mb++) {
        BitWriter w;
        bw_init(&w);
        write_slice_header(&w,&sp,0,mb,1,2,1);
        write_plan_mb(&w,&plan,mb);
        bw_trailing(&w);
        ab_nal_bw(&au,1,3,&w);
    }
    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,20)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&h,&au,21)<=H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.count>=2);
    TASK_CHECK(h.frames.err[1]==0);
    plan_expected_luma(&plan,26,expected,32,16,pcm);
    for(y=0;y<16;y++) for(x=0;x<32;x++)
        TASK_CHECK(h.frames.yuv[1][y*32+x]==expected[y*32+x]);
    harness_destroy(&h);
    return 0;
}

static int case_compat_multi_ref_motion(void)
{
    StreamParams sp={1,1,0,26,0,0,0,0,100,2};
    static uint8_t pcm[384];
    BitWriter bw;
    AnnexB idr, skip, p2;
    PlanCtx plan;
    Harness h;

    memset(pcm,120,256); memset(pcm+256,128,128);
    idr=build_idr_pcm(&sp,pcm);

    bw_init(&bw);
    write_slice_header(&bw,&sp,0,0,1,2,1);
    bw_ue(&bw,1);
    bw_trailing(&bw);
    memset(&skip,0,sizeof(skip));
    append_param_sets(&skip,&sp);
    ab_nal_bw(&skip,1,3,&bw);

    memset(&plan,0,sizeof(plan));
    plan.width_mbs=1; plan.refs=2;
    plan.mb[0].ref_idx=1;
    plan.mb[0].mvd_x=1; /* non-zero fractional horizontal motion */
    plan.mb[0].transform8=0;
    p2=build_picture(&sp,0,2,4,1,write_plan_mb,&plan);

    harness_init(&h);
    TASK_CHECK(decode_au(&h,&idr,30)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&h,&skip,31)<=H264BSD_HDRS_RDY);
    TASK_CHECK(decode_au(&h,&p2,32)<=H264BSD_HDRS_RDY);
    harness_flush(&h);
    TASK_CHECK(h.frames.count>=3);
    TASK_CHECK(h.frames.err[2]==0);
    harness_destroy(&h);
    return 0;
}

const struct task_case task_cases[] = {
    {"syntax", "high-sps-pps", case_syntax_high_sps_pps},
    {"syntax", "unsupported-high", case_syntax_unsupported},
    {"syntax", "pps-extension-errors", case_syntax_pps_extension_errors},
    {"syntax", "intra8x8-rejected", case_syntax_intra8_rejected},
    {"syntax", "partition8-modes", case_syntax_partition8},
    {"syntax", "sub8-and-cbp0", case_syntax_sub8_and_cbp0},
    {"residual", "four-cavlc-groups", case_residual_basic},
    {"residual", "qp-boundaries", case_residual_qp_boundaries},
    {"residual", "zero-sign-tail", case_residual_zero_and_signs},
    {"residual", "nc-adjacency-chain", case_residual_nc_chain},
    {"filter", "disabled-plane-and-flags", case_filter_disabled_plane},
    {"filter", "enabled-smoke", case_filter_enabled_smoke},
    {"filter", "mixed-edge-model", case_filter_mixed_edge_model},
    {"compat", "public-sequence", case_compat_baseline_public},
    {"compat", "reference-chain", case_compat_reference_chain},
    {"compat", "multi-slice", case_compat_multi_slice},
    {"compat", "multi-ref-fractional-mv", case_compat_multi_ref_motion},
    {"compat", "output-order-flush", case_compat_output_order_flush},
    {"compat", "two-instances", case_compat_two_instances_zero_mask}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
