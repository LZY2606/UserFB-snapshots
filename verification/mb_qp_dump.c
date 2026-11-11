/*
    Standalone H.264/AVC (CAVLC, baseline subset) macroblock QP tracer.

    Parses an Annex-B stream and independently reconstructs, for every
    macroblock, the luma QP a decoder would use (slice QP + mb_qp_delta
    accumulation, H.264 syntax inheritance for skipped/no-residual MBs).
    Only the QP-relevant syntax is evaluated; residual coefficients are
    parsed for bit alignment but their values are discarded.

    The normative CAVLC VLC tables are taken from minih264e.h (they are
    the standard H.264 tables); all parsing logic here is independent of
    the encoder. End-to-end correctness of the parse is cross-validated
    by decoding the same streams with FFmpeg and comparing against the
    encoder's local reconstruction (see verification/check_mb_qp.sh).

    Usage:
        mb_qp_dump stream.264
            print one line per macroblock:
                frame=<F> mb=<addr> mbx=<x> mby=<y> qp=<Q> <SKIP|DELTA|inherit>
        mb_qp_dump stream.264 expect.txt
            additionally verify: for every macroblock that carries a
            mb_qp_delta in the bitstream, qp == expected value from
            expect.txt (format: "nmbx nmby nframes" then one integer per
            macroblock, frame-major raster order). Exit status != 0 on
            any mismatch.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(__APPLE__) && defined(__aarch64__)
/* Old Apple intrinsic spelling used by this upstream snapshot. */
#define vtbl2q_u8 vqtbl2_u8
#endif
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

// encoder-internal macros that clash with local identifiers below
#undef log2_max_frame_num_minus4
#undef quality_id
#undef dependency_id
#undef default_base_mode_flag

static void die(const char *msg)
{
    fprintf(stderr, "mb_qp_dump: %s\n", msg);
    exit(2);
}

/************************************************************************/
/*      Bit reader (over RBSP, emulation prevention already removed)    */
/************************************************************************/
typedef struct
{
    const uint8_t *p;
    size_t nbits;
    size_t pos;
} gb_t;

static int gb_u1(gb_t *g)
{
    int b;
    if (g->pos >= g->nbits)
        die("bitstream overrun");
    b = (g->p[g->pos >> 3] >> (7 - (g->pos & 7))) & 1;
    g->pos++;
    return b;
}

static unsigned gb_u(gb_t *g, int n)
{
    unsigned v = 0;
    while (n-- > 0)
        v = (v << 1) | (unsigned)gb_u1(g);
    return v;
}

static unsigned gb_ue(gb_t *g)
{
    int z = 0;
    while (!gb_u1(g))
        if (++z > 24)
            die("bad exp-golomb code");
    return ((1u << z) - 1) + gb_u(g, z);
}

static int gb_se(gb_t *g)
{
    unsigned k = gb_ue(g);
    return (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1);
}

/* At a macroblock boundary: is there real data left, or only rbsp_trailing_bits? */
static int gb_more_rbsp(gb_t *g)
{
    size_t i;
    int ones = 0;
    for (i = g->pos; i < g->nbits; i++)
        ones += (g->p[i >> 3] >> (7 - (i & 7))) & 1;
    return ones >= 2;   // a single '1' bit is the rbsp stop bit
}

/************************************************************************/
/*      CAVLC residual parsing (mirror of the encoder's table layout)   */
/************************************************************************/

/* VLC table lookup: match accumulated code against CODE/CODE8 entries */
static int vlc_lookup(const uint8_t *tab, int off, int size, int len_m1, int code, int len)
{
    int idx;
    for (idx = 0; idx < size; idx++)
    {
        int e = tab[off + idx];
        int elen, ecode;
        if (!e)
            continue;               // invalid placeholder
        if (len_m1)
        {
            elen = (e & 15) + 1;    // CODE(val, len): low nibble = len - 1
            ecode = e >> 4;
        } else
        {
            elen = e & 15;          // CODE8(val, len): low nibble = len
            ecode = e >> 4;
        }
        if (elen == len && ecode == code)
            return idx;
    }
    return -1;
}

static int vlc_decode(gb_t *g, const uint8_t *tab, int off, int size, int len_m1)
{
    int code = 0, len = 0, idx;
    for (;;)
    {
        code = (code << 1) | gb_u1(g);
        if (++len > 16)
            die("bad VLC code");
        idx = vlc_lookup(tab, off, size, len_m1, code, len);
        if (idx >= 0)
            return idx;
    }
}

/* coeff_token: returns trailing_ones, sets *total_coeff */
static int decode_coeff_token(gb_t *g, int nC, int *total_coeff)
{
    int off = h264e_g_coeff_token[nC];
    int idx, t1;
    if (off == 230)
    {
        // fixed 6-bit code: table values are the codes themselves
        int code = (int)gb_u(g, 6);
        for (idx = 0; idx < 65; idx++)
            if (h264e_g_coeff_token[230 + idx] == code)
                break;
        if (idx == 65)
            die("bad fixed coeff_token");
        // first occurrence wins: invalid slots (value 0) come after idx 4
    } else
    {
        int size = (off == 18) ? 17 : 65;   // chroma DC table vs luma tables
        idx = vlc_decode(g, h264e_g_coeff_token, off, size, 1);
    }
    t1 = idx & 3;
    *total_coeff = t1 + (idx >> 2);
    return t1;
}

static int decode_total_zeros(gb_t *g, int total_coeff, int chroma_dc)
{
    const uint8_t *vlc = chroma_dc ? h264e_g_total_zeros_cr_2x2 : h264e_g_total_zeros;
    int ctx = total_coeff - 1;
    int off = vlc[ctx];
    int size = chroma_dc ? 4 - ctx : 16 - ctx;
    return vlc_decode(g, vlc, off, size, 0);
}

static int decode_run_before(gb_t *g, int zeros_left)
{
    int ctx = zeros_left - 1;               // encoder table is indexed by zeros_left - 1
    int off = h264e_g_run_before[ctx];
    int size = ctx < 6 ? ctx + 2 : 15;
    return vlc_decode(g, h264e_g_run_before, off, size, 0);
}

/* Level codes: bit-exact reverse of the encoder's level VLC scheme */
static void decode_levels(gb_t *g, int nnz, int t1)
{
    int nlevels = nnz - t1;
    int vlcnum = 1;
    int k;
    for (k = 0; k < nlevels; k++)
    {
        int p = 0, L, v, suffix = 0, sym_tx;
        while (!gb_u1(g))
            if (++p > 15)
                die("bad level prefix");
        if (k == 0)
        {
            int special = (nnz > 10 && t1 < 3);
            if (special)            { v = 1; L = (p >= 15) ? 12 : 1; }
            else if (p >= 15)       { v = 1; L = 12; }
            else if (p == 14)       { v = 0; L = 4; }
            else                    { v = 0; L = 0; }
        } else
        {
            v = vlcnum;
            L = (p >= 15) ? 12 : vlcnum;
        }
        if (L)
            suffix = (int)gb_u(g, L);
        sym_tx = (p << v) + suffix;
        if (k == 0)
        {
            // suffixLength for the 2nd level depends on the 1st level's value:
            // the encoder bumps vlcnum when fold(level) >= 6, where
            // fold(level) = sym_tx + 2 for trailing_ones < 3; a first-level
            // escape resets vlcnum to 1 and the prefix>=3 rule bumps it to 2
            int folded = sym_tx + ((t1 < 3) ? 2 : 0);
            if (p >= 15 && !(nnz > 10 && t1 < 3))
                vlcnum = 2;
            else
                vlcnum = (folded >= 6) ? 2 : 1;
        } else if (p >= 3 && vlcnum < 6)
        {
            vlcnum++;
        }
    }
}

/* Parse one residual block; updates nz_ctx[0] with TotalCoeff */
static void decode_residual(gb_t *g, int maxNumCoeff, uint8_t *nz_ctx)
{
    int nC = nz_ctx[-1] + nz_ctx[1];
    int t1, nnz;
    if (nC <= 34)
        nC = (nC + 1) >> 1;
    nC &= 31;
    t1 = decode_coeff_token(g, nC, &nnz);
    nz_ctx[0] = (uint8_t)nnz;
    if (!nnz)
        return;
    if (t1)
        (void)gb_u(g, t1);          // trailing ones sign bits
    if (nnz > t1)
        decode_levels(g, nnz, t1);
    if (nnz < maxNumCoeff)
    {
        int zeros_left = decode_total_zeros(g, nnz, maxNumCoeff == 4);
        int k;
        for (k = 1; k < nnz && zeros_left > 0; k++)
            zeros_left -= decode_run_before(g, zeros_left);
    }
}

/************************************************************************/
/*      Sequence / picture parameter sets                               */
/************************************************************************/
typedef struct
{
    int valid;
    int log2_max_frame_num_minus4;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb_minus4;
    int nmbx, nmby;
} sps_t;

typedef struct
{
    int valid;
    int sps_id;
    int pic_init_qp_minus26;
    int deblocking_filter_control_present_flag;
} pps_t;

static sps_t g_sps[32];
static pps_t g_pps[256];

static void parse_sps(gb_t *g)
{
    unsigned profile_idc = gb_u(g, 8);
    sps_t *sps;
    (void)gb_u(g, 8);                           // constraint flags
    (void)gb_u(g, 8);                           // level_idc
    sps = &g_sps[gb_ue(g)];
    memset(sps, 0, sizeof(*sps));
    sps->valid = 1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc ==  44 || profile_idc ==  83 ||
        profile_idc ==  86 || profile_idc == 118 || profile_idc == 128)
    {
        unsigned chroma_format_idc = gb_ue(g);
        if (chroma_format_idc == 3)
            (void)gb_u1(g);
        (void)gb_ue(g);                         // bit_depth_luma_minus8
        (void)gb_ue(g);                         // bit_depth_chroma_minus8
        (void)gb_u1(g);                         // qpprime_y_zero_transform_bypass_flag
        if (gb_u1(g))                           // seq_scaling_matrix_present_flag
            die("scaling lists not supported");
    }
    sps->log2_max_frame_num_minus4 = (int)gb_ue(g);
    sps->pic_order_cnt_type = (int)gb_ue(g);
    if (sps->pic_order_cnt_type == 0)
    {
        sps->log2_max_pic_order_cnt_lsb_minus4 = (int)gb_ue(g);
    } else if (sps->pic_order_cnt_type == 1)
    {
        unsigned i, n;
        (void)gb_u1(g); (void)gb_se(g); (void)gb_se(g);
        n = gb_ue(g);
        for (i = 0; i < n; i++)
            (void)gb_se(g);
    }
    (void)gb_ue(g);                             // num_ref_frames
    (void)gb_u1(g);                             // gaps_in_frame_num_value_allowed_flag
    sps->nmbx = (int)gb_ue(g) + 1;              // pic_width_in_mbs_minus1
    sps->nmby = (int)gb_ue(g) + 1;              // pic_height_in_map_units_minus1
    if (!gb_u1(g))                              // frame_mbs_only_flag
        (void)gb_u1(g);                         // mb_adaptive_frame_field_flag
    (void)gb_u1(g);                             // direct_8x8_inference_flag
    if (gb_u1(g))                               // frame_cropping_flag
    {
        (void)gb_ue(g); (void)gb_ue(g); (void)gb_ue(g); (void)gb_ue(g);
    }
    if (gb_u1(g))                               // vui_parameters_present_flag
        die("VUI not supported");
}

static void parse_pps(gb_t *g)
{
    pps_t *pps = &g_pps[gb_ue(g)];
    memset(pps, 0, sizeof(*pps));
    pps->valid = 1;
    pps->sps_id = (int)gb_ue(g);
    (void)gb_u1(g);                             // entropy_coding_mode_flag (0 = CAVLC)
    (void)gb_u1(g);                             // pic_order_present_flag
    if (gb_ue(g))                               // num_slice_groups_minus1
        die("slice groups not supported");
    (void)gb_ue(g);                             // num_ref_idx_l0_active_minus1
    (void)gb_ue(g);                             // num_ref_idx_l1_active_minus1
    (void)gb_u1(g);                             // weighted_pred_flag
    (void)gb_u(g, 2);                           // weighted_bipred_idc
    pps->pic_init_qp_minus26 = gb_se(g);
    (void)gb_se(g);                             // pic_init_qs_minus26
    (void)gb_se(g);                             // chroma_qp_index_offset
    pps->deblocking_filter_control_present_flag = gb_u1(g);
    (void)gb_u1(g);                             // constrained_intra_pred_flag
    (void)gb_u1(g);                             // redundant_pic_cnt_present_flag
}

/************************************************************************/
/*      Macroblock layer                                                */
/************************************************************************/

/* coded_block_pattern codeNum -> cbp, inverse of Table 7-5 (as used by the encoder) */
static const uint8_t cbp2code[2][48] = { // [0] = intra, [1] = inter
    {3, 29, 30, 17, 31, 18, 37,  8, 32, 38, 19,  9, 20, 10, 11,  2, 16, 33, 34, 21, 35, 22, 39,  4,
    36, 40, 23,  5, 24,  6,  7,  1, 41, 42, 43, 25, 44, 26, 46, 12, 45, 47, 27, 13, 28, 14, 15,  0},
    {0,  2,  3,  7,  4,  8, 17, 13,  5, 18,  9, 14, 10, 15, 16, 11,  1, 32, 33, 36, 34, 37, 44, 40,
    35, 45, 38, 41, 39, 42, 43, 19,  6, 24, 25, 20, 26, 21, 46, 28, 27, 47, 22, 29, 23, 30, 31, 12}
};

typedef struct
{
    int frame;          // frame index
    int addr;           // mb address (raster)
    int qp;             // decoder QP for this macroblock
    int skip;           // coded as P-skip
    int qp_delta;       // mb_qp_delta present in the bitstream
} mb_rec_t;

static mb_rec_t *g_mbs;
static int g_mb_cap, g_mb_cnt;
static int g_nmbx, g_nmby;
static int g_skips;

static void record_mb(int frame, int addr, int qp, int skip, int qp_delta)
{
    mb_rec_t *r;
    if (g_mb_cnt == g_mb_cap)
    {
        g_mb_cap = g_mb_cap ? g_mb_cap*2 : 4096;
        g_mbs = (mb_rec_t*)realloc(g_mbs, (size_t)g_mb_cap*sizeof(*g_mbs));
        if (!g_mbs)
            die("out of memory");
    }
    r = &g_mbs[g_mb_cnt++];
    r->frame = frame;
    r->addr = addr;
    r->qp = qp;
    r->skip = skip;
    r->qp_delta = qp_delta;
    g_skips += skip;
}

/* nnz context bookkeeping mirrors the encoder's diagonal slot layout */
static void parse_slice_mb_layer(gb_t *g, int slice_type, int first_mb, int qp, int frame)
{
    int mb_addr = first_mb;
    int nmb = g_nmbx*g_nmby;
    uint8_t *nnz = (uint8_t*)malloc((size_t)(g_nmbx + 1)*8);
    if (!nnz)
        die("out of memory");
    memset(nnz, NNZ_NA, (size_t)(g_nmbx + 1)*8);

    for (;;)
    {
        int skip_run = 0;
        if (slice_type == SLICE_TYPE_P)
        {
            if (!gb_more_rbsp(g))
                break;
            skip_run = (int)gb_ue(g);
            while (skip_run-- > 0)
            {
                int mbx;
                uint8_t *nnz_top, *nnz_left;
                if (mb_addr >= nmb)
                    die("skip run beyond frame");
                mbx = mb_addr % g_nmbx;
                nnz_top = nnz + 8 + mbx*8;
                nnz_left = nnz;
                if (mbx == 0)
                    memset(nnz_left, NNZ_NA, 8);
                // skipped MB: all nnz are zero, QP inherited
                memset(nnz_top, 0, 4);
                memset(nnz_left, 0, 4);
                memset(nnz_top + 4, 0, 4);
                memset(nnz_left + 4, 0, 4);
                record_mb(frame, mb_addr, qp, 1, 0);
                mb_addr++;
            }
            if (!gb_more_rbsp(g))
                break;
        } else if (!gb_more_rbsp(g))
        {
            break;
        }

        // one coded macroblock
        {
            int mbx, mb_type, i;
            int is_i16 = 0, is_intra = 0, cbp = 0, cbpl = 0, cbpc = 0;
            int qp_delta_present;
            uint8_t *nnz_top, *nnz_left;
            uint8_t nz[9];

            if (mb_addr >= nmb)
                die("macroblock beyond frame");
            mbx = mb_addr % g_nmbx;
            nnz_top = nnz + 8 + mbx*8;
            nnz_left = nnz;
            if (mbx == 0)
                memset(nnz_left, NNZ_NA, 8);

            for (i = 0; i < 4; i++)
            {
                nz[5 + i] = nnz_top[i];
                nnz_top[i] = 0;
                nz[3 - i] = nnz_left[i];
                nnz_left[i] = 0;
            }

            mb_type = (int)gb_ue(g);
            if (slice_type == SLICE_TYPE_P)
            {
                if (mb_type < 5)
                {
                    // inter: parse mvd
                    static const int nparts[4] = { 1, 2, 2, 4 };
                    int part, np = nparts[mb_type & 3];
                    is_intra = 0;
                    if (mb_type >= 3)
                    {
                        // P_8x8: sub_mb_type per 8x8, then mvd per sub-partition
                        np = 0;
                        for (i = 0; i < 4; i++)
                        {
                            static const int sub_np[4] = { 1, 2, 2, 4 };
                            int st = (int)gb_ue(g);
                            if (st > 3)
                                die("bad sub_mb_type");
                            np += sub_np[st];
                        }
                    }
                    for (part = 0; part < np; part++)
                    {
                        (void)gb_se(g);         // mvd_l0 x
                        (void)gb_se(g);         // mvd_l0 y
                    }
                } else if (mb_type == 30)
                {
                    die("I_PCM not supported");
                } else
                {
                    mb_type -= 5;               // fall through to intra numbering
                    is_intra = 1;
                }
            } else
            {
                is_intra = 1;
            }

            if (is_intra)
            {
                if (mb_type == 0)
                {
                    // I_4x4
                    for (i = 0; i < 16; i++)
                        if (!gb_u1(g))          // prev_intra4x4_pred_mode_flag
                            (void)gb_u(g, 3);   // rem_intra4x4_pred_mode
                    (void)gb_ue(g);             // intra_chroma_pred_mode
                } else if (mb_type <= 24)
                {
                    // I_16x16: mb_type-1 = pred_mode + 4*cbpc + 12*(cbpl?1:0)
                    int idx = mb_type - 1;
                    is_i16 = 1;
                    cbpl = (idx >= 12) ? 15 : 0;
                    cbpc = (idx - (cbpl ? 12 : 0)) >> 2;
                    cbp = cbpl | (cbpc << 4);
                    (void)gb_ue(g);             // intra_chroma_pred_mode
                } else
                {
                    die("I_PCM not supported");
                }
            }

            if (!is_i16)
            {
                unsigned code = gb_ue(g);       // coded_block_pattern
                int c;
                if (code > 47)
                    die("bad coded_block_pattern");
                // invert Table 7-5: intra (I_4x4) uses table 0, inter table 1
                cbp = -1;
                for (c = 0; c < 48; c++)
                    if (cbp2code[(is_intra && mb_type == 0) ? 0 : 1][c] == (int)code)
                    {
                        cbp = c;
                        break;
                    }
                if (cbp < 0)
                    die("coded_block_pattern not found");
                cbpl = cbp & 15;
                cbpc = (cbp >> 4) & 3;
            }

            qp_delta_present = (cbp || is_i16);
            if (qp_delta_present)
            {
                qp += gb_se(g);                 // mb_qp_delta
                qp = (qp % 52 + 52) % 52;
            }

            // residual
            if (is_i16)
            {
                decode_residual(g, 16, nz + 4);    // luma DC always coded
            }
            if (cbpl)
            {
                for (i = 0; i < 16; i++)
                {
                    int j = decode_block_scan[i];
                    int slot = 4 + (j & 3) - (j >> 2);
                    if (cbp & (1 << (i >> 2)))
                        decode_residual(g, 16 - is_i16, nz + slot);
                    else
                        nz[slot] = 0;
                }
                for (i = 0; i < 4; i++)
                {
                    nnz_top[i] = nz[1 + i];
                    nnz_left[i] = nz[7 - i];
                }
            }
            if (cbpc)
            {
                int uv;
                for (uv = 0; uv < 2; uv++)
                {
                    uint8_t nzcdc[3];
                    nzcdc[0] = nzcdc[2] = 17;   // fixed chroma-DC context
                    decode_residual(g, 4, nzcdc + 1);
                }
                if (cbpc > 1)
                {
                    for (uv = 0; uv < 2; uv++)
                    {
                        uint8_t nzc[5];
                        int nnz_off = uv ? 6 : 4;
                        for (i = 0; i < 2; i++)
                        {
                            nzc[3 + i] = nnz_top[nnz_off + i];
                            nzc[1 - i] = nnz_left[nnz_off + i];
                        }
                        for (i = 0; i < 4; i++)
                        {
                            int k = 2 + (i & 1) - (i >> 1);
                            decode_residual(g, 15, nzc + k);
                        }
                        for (i = 0; i < 2; i++)
                        {
                            nnz_top[nnz_off + i]  = nzc[1 + i];
                            nnz_left[nnz_off + i] = nzc[3 - i];
                        }
                    }
                }
            }
            if (cbpc != 2)
            {
                memset(nnz_top + 4, 0, 4);
                memset(nnz_left + 4, 0, 4);
            }

            record_mb(frame, mb_addr, qp, 0, qp_delta_present);
            mb_addr++;
        }
    }
    free(nnz);
}

static void parse_slice(const uint8_t *rbsp, size_t size, int nal_type, int nal_ref_idc, int *pframe)
{
    gb_t g;
    unsigned first_mb, slice_type, pps_id;
    pps_t *pps;
    sps_t *sps;
    int qp;

    g.p = rbsp;
    g.nbits = size*8;
    g.pos = 0;

    first_mb = gb_ue(&g);
    slice_type = gb_ue(&g);
    pps_id = gb_ue(&g);
    if (pps_id >= 256 || !g_pps[pps_id].valid)
        die("unknown pps");
    pps = &g_pps[pps_id];
    sps = &g_sps[pps->sps_id];
    if (!sps->valid)
        die("unknown sps");
    g_nmbx = sps->nmbx;
    g_nmby = sps->nmby;

    (void)gb_u(&g, sps->log2_max_frame_num_minus4 + 4); // frame_num
    if (nal_type == 5)
        (void)gb_ue(&g);                                // idr_pic_id
    if (sps->pic_order_cnt_type == 0)
        (void)gb_u(&g, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);

    if (slice_type != SLICE_TYPE_I && slice_type != SLICE_TYPE_P)
        die("unsupported slice_type");
    if (slice_type == SLICE_TYPE_P)
    {
        if (gb_u1(&g))                                  // num_ref_idx_active_override_flag
            (void)gb_ue(&g);                            // num_ref_idx_l0_active_minus1
        if (gb_u1(&g))                                  // ref_pic_list_modification_flag_l0
        {
            unsigned op;
            do
            {
                op = gb_ue(&g);
                if (op == 0 || op == 2)
                    (void)gb_ue(&g);
            } while (op != 3);
        }
    }
    if (nal_ref_idc)
    {
        if (nal_type == 5)
        {
            (void)gb_u1(&g);                            // no_output_of_prior_pics_flag
            (void)gb_u1(&g);                            // long_term_reference_flag
        } else if (gb_u1(&g))                           // adaptive_ref_pic_marking_mode_flag
        {
            unsigned mmco;
            do
            {
                mmco = gb_ue(&g);
                if (mmco == 1 || mmco == 3)
                    (void)gb_ue(&g);
                if (mmco == 2)
                    (void)gb_ue(&g);
                if (mmco == 3 || mmco == 6)
                    (void)gb_ue(&g);
                if (mmco == 4)
                    (void)gb_ue(&g);
            } while (mmco != 0);
        }
    }
    qp = 26 + pps->pic_init_qp_minus26 + gb_se(&g);     // slice_qp_delta
    if (pps->deblocking_filter_control_present_flag)
    {
        if (gb_ue(&g) != 1)                             // disable_deblocking_filter_idc
        {
            (void)gb_se(&g);                            // slice_alpha_c0_offset_div2
            (void)gb_se(&g);                            // slice_beta_offset_div2
        }
    }

    if (first_mb == 0)
        (*pframe)++;                                    // new frame starts
    parse_slice_mb_layer(&g, (int)slice_type, (int)first_mb, qp, *pframe);
}

/************************************************************************/
/*      Annex-B stream splitting                                        */
/************************************************************************/
static void parse_stream(const uint8_t *buf, size_t size, int *pframe)
{
    size_t pos = 0;
    while (pos + 4 < size)
    {
        size_t sc, next, i, rs;
        const uint8_t *nal;
        uint8_t *rbsp;
        unsigned nal_type, nal_ref_idc;
        // find start code
        for (sc = pos; sc + 3 < size; sc++)
            if (buf[sc] == 0 && buf[sc + 1] == 0 && buf[sc + 2] == 1)
                break;
        if (sc + 3 >= size)
            break;
        nal = buf + sc + 3;
        for (next = sc + 3; next + 3 < size; next++)
            if (buf[next] == 0 && buf[next + 1] == 0 && buf[next + 2] == 1)
                break;
        if (next + 3 >= size)
            next = size;
        nal_ref_idc = (nal[0] >> 5) & 3;
        nal_type = nal[0] & 31;
        // remove emulation prevention
        rbsp = (uint8_t*)malloc(next - (nal - buf) + 1);
        if (!rbsp)
            die("out of memory");
        for (i = 1, rs = 0; nal + i < buf + next; i++)
        {
            if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0 &&
                i + 1 < (size_t)(buf + next - nal) && nal[i + 1] <= 3)
                continue;                           // drop emulation_prevention_three_byte
            rbsp[rs++] = nal[i];
        }
        switch (nal_type)
        {
        case 7: { gb_t g; g.p = rbsp; g.nbits = rs*8; g.pos = 0; parse_sps(&g); break; }
        case 8: { gb_t g; g.p = rbsp; g.nbits = rs*8; g.pos = 0; parse_pps(&g); break; }
        case 5:
        case 1: parse_slice(rbsp, rs, (int)nal_type, (int)nal_ref_idc, pframe); break;
        default: break;                             // SPS/PPS/SEI/filler/etc.
        }
        free(rbsp);
        pos = next;
    }
}

/************************************************************************/
/*      main                                                            */
/************************************************************************/
int main(int argc, char **argv)
{
    FILE *f;
    uint8_t *buf;
    long size;
    int frame = -1, i, rc = 0;

    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: mb_qp_dump stream.264 [expect.txt]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f)
        die("cannot open input");
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t*)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size)
        die("cannot read input");
    fclose(f);

    parse_stream(buf, (size_t)size, &frame);
    free(buf);

    for (i = 0; i < g_mb_cnt; i++)
    {
        mb_rec_t *r = &g_mbs[i];
        printf("frame=%d mb=%d mbx=%d mby=%d qp=%d %s\n",
            r->frame, r->addr, r->addr % g_nmbx, r->addr / g_nmbx, r->qp,
            r->skip ? "SKIP" : r->qp_delta ? "DELTA" : "inherit");
    }
    printf("frames=%d mbs=%d skips=%d\n", frame + 1, g_mb_cnt, g_skips);

    if (argc == 3)
    {
        // verify signaled QPs against expectations
        int xmbx, xmby, xframes, n, mismatches = 0, checked = 0;
        f = fopen(argv[2], "r");
        if (!f)
            die("cannot open expect file");
        if (fscanf(f, "%d %d %d", &xmbx, &xmby, &xframes) != 3)
            die("bad expect header");
        if (xmbx != g_nmbx || xmby != g_nmby || xframes != frame + 1)
        {
            fprintf(stderr, "expect geometry mismatch: file %dx%d x%d, stream %dx%d x%d\n",
                xmbx, xmby, xframes, g_nmbx, g_nmby, frame + 1);
            rc = 1;
        }
        for (i = 0; i < g_mb_cnt && !rc; i++)
        {
            mb_rec_t *r = &g_mbs[i];
            if (fscanf(f, "%d", &n) != 1)
                die("expect file too short");
            if (r->qp_delta)
            {
                checked++;
                if (n != r->qp)
                {
                    fprintf(stderr, "MISMATCH frame=%d mb=%d (mbx=%d mby=%d): stream qp=%d expected %d\n",
                        r->frame, r->addr, r->addr % g_nmbx, r->addr / g_nmbx, r->qp, n);
                    mismatches++;
                }
            }
        }
        if (mismatches)
        {
            printf("MB_QP_CHECK_FAIL mismatches=%d\n", mismatches);
            rc = 1;
        } else if (!rc)
        {
            printf("MB_QP_CHECK_PASS delta_mbs_checked=%d\n", checked);
        }
        fclose(f);
    }
    return rc;
}
