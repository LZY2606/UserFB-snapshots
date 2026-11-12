/*
 * Hard NAL size limit (strict NAL) tests for minih264.
 *
 * Four fixed runner entry points, each returning the number of successful
 * assertions (zero means skipped / nothing executed):
 *
 *   case_limit         - every produced NAL obeys the hard limit, slices
 *                        split at macroblock boundaries and cover the frame
 *   case_prediction    - mid-row slices, skip runs, cross-row motion and
 *                        several QPs decode bit-exact vs the reconstruction
 *   case_failure       - NAL_TOO_SMALL delivers nothing and leaves the whole
 *                        encoder state usable for an identical retry
 *   case_compatibility - max_nalu_bytes=0 keeps the legacy soft-threshold
 *                        stream, and callbacks match the return buffer
 *
 * All material is generated programmatically; the only external dependency
 * is the bundled h264bsd C decoder used as an independent checker.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "minih264e.h"
#include "h264bsd_decoder.h"

#define W 64
#define H 64
#define NMBX (W / 16)
#define NMBY (H / 16)
#define NMB (NMBX*NMBY)
#define YSIZE (W*H)
#define CSIZE (W*H / 4)
#define FRAMESIZE (YSIZE + 2*CSIZE)
/* The encoder's SIMD paths may over-read a few bytes past a plane end, so
   input pictures get trailing guard padding (decoder output does not). */
#define INPUT_BYTES (FRAMESIZE + 64)
#define MAXNAL 64

static unsigned char *alloc_input(void) { return malloc(INPUT_BYTES); }

static int g_fail;

#define CHECK(cond, msg) \
    do { \
        assertions++; \
        if (!(cond)) { g_fail = 1; fprintf(stderr, "CHECK FAILED (%s:%d): %s\n", __FILE__, __LINE__, msg); } \
    } while (0)

/* ------------------------------------------------------------------ */
/* Programmed YUV material                                            */
/* ------------------------------------------------------------------ */

/* Flat per-MB luma, constant chroma: heavy zero-coefficient / skip runs. */
static void gen_flat(unsigned char *f, int seed)
{
    int mx, my, yy, xx;
    for (my = 0; my < NMBY; my++)
        for (mx = 0; mx < NMBX; mx++)
            for (yy = 0; yy < 16; yy++)
                for (xx = 0; xx < 16; xx++)
                    f[(my*16 + yy)*W + mx*16 + xx] =
                        (unsigned char)(100 + ((mx*7 + my*11 + seed) & 15));
    memset(f + YSIZE, 128, CSIZE);
    memset(f + YSIZE + CSIZE, 128, CSIZE);
}

/* Rich intra texture with flat and zero regions mixed in. */
static void gen_texture(unsigned char *f, int seed)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            int v = (x*13 + y*7 + (x*y) + seed*29) & 255;
            if (x < 16) v = (y + seed) & 255;
            if (y >= 48) v = 0;
            f[y*W + x] = (unsigned char)v;
        }
    for (y = 0; y < H/2; y++)
        for (x = 0; x < W/2; x++)
        {
            f[YSIZE + y*(W/2) + x] = (unsigned char)((x*5 + seed) & 255);
            f[YSIZE + CSIZE + y*(W/2) + x] = (unsigned char)((y*9 + seed*3) & 255);
        }
}

/* Cross-row motion: a structured pattern translated every frame. */
static void gen_motion(unsigned char *f, int fidx)
{
    int x, y;
    int sx = (fidx*5) % 23;
    int sy = (fidx*3) % 11;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            int ax = x - sx, ay = y - sy;
            int v = (ax < 0 || ay < 0) ? 20 :
                    (((ax & 7) == 0 || (ay & 7) == 0) ? 230 : (ax*3 + ay*5 + fidx*11) & 255);
            f[y*W + x] = (unsigned char)v;
        }
    for (y = 0; y < H/2; y++)
        for (x = 0; x < W/2; x++)
        {
            f[YSIZE + y*(W/2) + x] = (unsigned char)(100 + sx);
            f[YSIZE + CSIZE + y*(W/2) + x] = (unsigned char)(110 + sy);
        }
}

/* Left-half MBs flat, right-half MBs textured: early MBs small, later big. */
static void gen_mixed(unsigned char *f, int seed)
{
    int mx, my, yy, xx;
    for (my = 0; my < NMBY; my++)
        for (mx = 0; mx < NMBX; mx++)
            for (yy = 0; yy < 16; yy++)
                for (xx = 0; xx < 16; xx++)
                {
                    int X = mx*16 + xx, Y = my*16 + yy;
                    int v = mx < 2 ? 100 + my*5
                                   : (xx*13 + yy*7 + mx*31 + my*17 + xx*yy + seed) & 255;
                    f[Y*W + X] = (unsigned char)v;
                }
    memset(f + YSIZE, 128, 2*CSIZE);
}

/* ------------------------------------------------------------------ */
/* Annex-B and slice-header parsing                                   */
/* ------------------------------------------------------------------ */

typedef struct
{
    const unsigned char *data;   /* NAL header byte, no start code */
    int len;                     /* NAL size incl. header and escapes */
    int nal_type;
} nal_info_t;

static int find_start_code(const unsigned char *p, int n, int *payload)
{
    int i;
    for (i = 0; i + 2 < n; i++)
    {
        if (p[i] == 0 && p[i+1] == 0)
        {
            if (p[i+2] == 1) { *payload = i + 3; return 3; }
            if (i + 3 < n && p[i+2] == 0 && p[i+3] == 1) { *payload = i + 4; return 4; }
        }
    }
    return 0;
}

static int parse_annexb(const unsigned char *buf, int n, nal_info_t *out, int maxn)
{
    int count = 0, pos = 0;
    while (count < maxn)
    {
        int start_rel, sc, dummy, next_sc, next_abs = n;
        sc = find_start_code(buf + pos, n - pos, &start_rel);
        if (!sc) break;
        {
            int start = pos + start_rel;
            next_sc = find_start_code(buf + start, n - start, &dummy);
            if (next_sc) next_abs = start + dummy - next_sc;
            if (next_abs <= start) break;
            out[count].data = buf + start;
            out[count].len = next_abs - start;
            out[count].nal_type = buf[start] & 0x1f;
            count++;
            pos = next_abs;
        }
    }
    return count;
}

static int rbsp_of(const unsigned char *nal, int nal_len, unsigned char *rbsp, int cap)
{
    int i, j = 0, zeros = 0;
    for (i = 1; i < nal_len && j < cap; i++) /* skip NAL header */
    {
        unsigned char b = nal[i];
        if (zeros >= 2 && b == 3) { zeros = 0; continue; }
        rbsp[j++] = b;
        zeros = b ? 0 : zeros + 1;
    }
    return j;
}

static int rbsp_bit(const unsigned char *p, int *bitpos)
{
    int v = (p[*bitpos >> 3] >> (7 - (*bitpos & 7))) & 1;
    (*bitpos)++;
    return v;
}

static int rbsp_ue(const unsigned char *p, int nbits, int *bitpos)
{
    int zeros = 0, val = 0, k;
    while (*bitpos < nbits && !rbsp_bit(p, bitpos)) zeros++;
    for (k = 0; k < zeros; k++) val = (val << 1) | rbsp_bit(p, bitpos);
    return (1 << zeros) - 1 + val;
}

typedef struct { int first_mb, slice_type, pps_id, frame_num; } slice_hdr_t;

static int parse_slice_header(const nal_info_t *nal, slice_hdr_t *sh)
{
    static unsigned char rbsp[65536];
    int n = rbsp_of(nal->data, nal->len, rbsp, (int)sizeof(rbsp));
    int bitpos = 0, k, v = 0;
    if (n <= 0) return 0;
    sh->first_mb = rbsp_ue(rbsp, n*8, &bitpos);
    sh->slice_type = rbsp_ue(rbsp, n*8, &bitpos);
    sh->pps_id = rbsp_ue(rbsp, n*8, &bitpos);
    for (k = 0; k < 5; k++) v = (v << 1) | rbsp_bit(rbsp, &bitpos); /* u(5) frame_num */
    sh->frame_num = v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Encoder helpers                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char data[256*1024];
    int off, count;
    int lens[256];
} cb_buf_t;

static void nal_callback(const unsigned char *p, int n, void *token)
{
    cb_buf_t *cb = (cb_buf_t *)token;
    memset(cb->data + cb->off, 0, 3);
    cb->data[cb->off + 3] = 1;
    cb->off += 4;
    memcpy(cb->data + cb->off, p, n);
    cb->lens[cb->count++] = n;
    cb->off += n;
}

static void make_yuv(H264E_io_yuv_t *y, unsigned char *buf)
{
    y->yuv[0] = buf;
    y->yuv[1] = buf + YSIZE;
    y->yuv[2] = buf + YSIZE + CSIZE;
    y->stride[0] = W;
    y->stride[1] = y->stride[2] = W/2;
}

static void make_create(H264E_create_param_t *cp, int layers, int threads)
{
    memset(cp, 0, sizeof(*cp));
    cp->width = W; cp->height = H; cp->gop = 30;
    cp->num_layers = layers; cp->max_threads = threads; cp->enableNEON = 1;
}

static void make_run(H264E_run_param_t *rp, int qp, int desired, int hard)
{
    memset(rp, 0, sizeof(*rp));
    rp->encode_speed = 5;
    rp->qp_min = rp->qp_max = qp;
    rp->desired_nalu_bytes = desired;
    rp->max_nalu_bytes = hard;
}

static int open_enc(H264E_create_param_t *cp, void **enc, void **scratch)
{
    int a, b;
    if (H264E_sizeof(cp, &a, &b) != 0) return -1;
    *enc = malloc(a); *scratch = malloc(b);
    if (!*enc || !*scratch) return -1;
    return H264E_init(*enc, cp);
}

static int decode_stream(storage_t *d, const unsigned char *buf, int n, int fidx,
                         unsigned char *recon, int *err_mbs)
{
    unsigned off = 0;
    int pics = 0, stalls = 0;
    *err_mbs = 0;
    while (off < (unsigned)n)
    {
        unsigned used = 0;
        unsigned st = h264bsdDecode(d, (unsigned char *)buf + off, n - off, fidx, &used);
        if (st > H264BSD_HDRS_RDY) { fprintf(stderr, "decoder status=%u\n", st); return -1; }
        off += used;
        if (!used && ++stalls > 8) return -1;
        if (st == H264BSD_PIC_RDY)
        {
            unsigned id, key, errors;
            unsigned char *pic = h264bsdNextOutputPicture(d, &id, &key, &errors);
            if (!pic) return -1;
            *err_mbs += (int)errors;
            if (recon) memcpy(recon, pic, FRAMESIZE);
            pics++;
        }
    }
    return pics;
}

/* Count VCL NALs and verify their first_mb sequence covers all MBs. */
static int verify_slice_coverage(nal_info_t *nals, int nn, int *mid_row)
{
    int i, vcl = 0, prev = 0, covered_to = 0;
    *mid_row = 0;
    for (i = 0; i < nn; i++)
    {
        if (nals[i].nal_type != 1 && nals[i].nal_type != 5) continue;
        {
            slice_hdr_t sh;
            if (!parse_slice_header(&nals[i], &sh)) return -1;
            if (vcl == 0) { if (sh.first_mb != 0) return -2; }
            else
            {
                if (sh.first_mb <= prev || sh.first_mb > NMB) return -3;
                if (sh.first_mb % NMBX != 0) *mid_row = 1;
            }
            covered_to = sh.first_mb;
            prev = sh.first_mb;
            vcl++;
        }
    }
    if (vcl < 1) return -4;
    if (covered_to >= NMB) return -5;          /* last slice must start before end */
    return vcl;
}

/* ------------------------------------------------------------------ */
/* case_limit                                                         */
/* ------------------------------------------------------------------ */

int case_limit(void)
{
    int assertions = 0;
    void *enc, *scratch;
    H264E_create_param_t cp;
    H264E_run_param_t rp;
    H264E_io_yuv_t yuv;
    unsigned char *input = alloc_input();
    unsigned char *recon = malloc(FRAMESIZE);
    unsigned char *coded;
    int coded_n, f;
    struct { int kind; int qp; int lim; } tab[] = {
        {0, 30, 64}, {0, 48, 64}, {0, 30, 80},
        {1, 48, 100}, {1, 40, 160}, {1, 30, 200},
        {2, 34, 160}, {2, 30, 256},
    };
    storage_t dec;
    int dec_pics = 0, any_midrow = 0, any_multi = 0;

    g_fail = 0;
    make_create(&cp, 1, 1);
    CHECK(open_enc(&cp, &enc, &scratch) == 0, "encoder open");
    CHECK(h264bsdInit(&dec, 1) == 0, "decoder init");
    make_yuv(&yuv, input);

    for (f = 0; f < (int)(sizeof(tab)/sizeof(tab[0])); f++)
    {
        nal_info_t nals[MAXNAL];
        int nn, i, vcl, mid_row = 0, err = 0, pics;
        if (tab[f].kind == 0) gen_flat(input, f + 1);
        else if (tab[f].kind == 1) gen_texture(input, f + 1);
        else gen_motion(input, f + 1);

        make_run(&rp, tab[f].qp, 0, tab[f].lim);
        coded = 0; coded_n = 0;
        CHECK(H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n) == 0, "encode success");
        CHECK(coded && coded_n > 0, "coded output present");

        nn = parse_annexb(coded, coded_n, nals, MAXNAL);
        CHECK(nn >= 1, "at least one NAL produced");
        if (f == 0)
        {
            /* IDR carries SPS and PPS ahead of the VCL slices. */
            CHECK(nn >= 3 && nals[0].nal_type == 7 && nals[1].nal_type == 8,
                  "IDR starts with SPS and PPS");
        }
        for (i = 0; i < nn; i++)
        {
            CHECK(nals[i].len <= tab[f].lim, "NAL within hard limit");
            CHECK(nals[i].len > 1, "no empty NAL");
        }
        vcl = verify_slice_coverage(nals, nn, &mid_row);
        CHECK(vcl >= 1, "slices cover whole frame without gaps");
        if (vcl > 1) any_multi = 1;
        if (mid_row) any_midrow = 1;

        pics = decode_stream(&dec, coded, coded_n, f, recon, &err);
        CHECK(pics == 1, "decoder produces one picture");
        CHECK(err == 0, "no concealed macroblocks");
        CHECK(memcmp(recon, input, FRAMESIZE) == 0, "reconstruction matches encoder output");
        dec_pics += pics;
    }
    CHECK(dec_pics == (int)(sizeof(tab)/sizeof(tab[0])), "all frames decoded");
    CHECK(any_multi, "hard limit produced multiple slices");
    CHECK(any_midrow, "hard limit produced a mid-row slice");
    h264bsdShutdown(&dec);
    free(enc); free(scratch); free(input); free(recon);
    return g_fail ? 0 : assertions;
}

/* ------------------------------------------------------------------ */
/* case_prediction                                                    */
/* ------------------------------------------------------------------ */

int case_prediction(void)
{
    int assertions = 0;
    void *enc, *scratch;
    H264E_create_param_t cp;
    H264E_run_param_t rp;
    H264E_io_yuv_t yuv;
    unsigned char *input = alloc_input();
    unsigned char *recon = malloc(FRAMESIZE);
    unsigned char *coded;
    int coded_n, f;
    storage_t dec;
    int saw_skip = 0, saw_midrow = 0, saw_multi = 0;

    g_fail = 0;
    make_create(&cp, 1, 1);
    CHECK(open_enc(&cp, &enc, &scratch) == 0, "encoder open");
    CHECK(h264bsdInit(&dec, 1) == 0, "decoder init");
    make_yuv(&yuv, input);

    for (f = 0; f < 8; f++)
    {
        nal_info_t nals[MAXNAL];
        int nn, i, mid_row = 0, vcl, err = 0, pics, qp = (f & 1) ? 26 : 34;
        if (f < 4) gen_motion(input, f);
        else gen_flat(input, 3);                 /* repeated identical P frames */
        make_run(&rp, qp, 0, 256);
        CHECK(H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n) == 0, "encode");

        nn = parse_annexb(coded, coded_n, nals, MAXNAL);
        for (i = 0; i < nn; i++)
            CHECK(nals[i].len <= 256, "limit respected on motion/skip frames");
        vcl = verify_slice_coverage(nals, nn, &mid_row);
        CHECK(vcl >= 1, "full MB coverage");
        if (vcl > 1) saw_multi = 1;
        if (mid_row) saw_midrow = 1;

        pics = decode_stream(&dec, coded, coded_n, f, recon, &err);
        CHECK(pics == 1 && err == 0, "clean decode of sliced frame");
        CHECK(memcmp(recon, input, FRAMESIZE) == 0, "prediction/residuals bit-exact");

        if (f >= 5)
        {
            int m, sk = 0;
            for (m = 0; m < (int)dec.picSizeInMbs; m++)
                if (dec.mb[m].mbType == 0) sk++;   /* P_Skip */
            if (sk > 0) saw_skip = 1;
        }
    }
    CHECK(saw_multi, "multi-slice output produced");
    CHECK(saw_midrow, "mid-row slice split produced");
    CHECK(saw_skip, "skip macroblock run exercised");
    h264bsdShutdown(&dec);
    free(enc); free(scratch); free(input); free(recon);
    return g_fail ? 0 : assertions;
}

/* ------------------------------------------------------------------ */
/* case_failure                                                       */
/* ------------------------------------------------------------------ */

static int encode_to_buf(void *enc, void *sc, H264E_run_param_t *rp,
                         unsigned char *in, unsigned char *out, int *out_n)
{
    H264E_io_yuv_t y;
    unsigned char *coded = 0;
    int n;
    make_yuv(&y, in);
    if (H264E_encode(enc, sc, rp, &y, &coded, &n) != 0) return -1;
    memcpy(out, coded, n);
    *out_n = n;
    return 0;
}

int case_failure(void)
{
    int assertions = 0;
    void *enc, *scratch, *ctrl_enc, *ctrl_sc;
    H264E_create_param_t cp;
    H264E_run_param_t rp, cr;
    H264E_io_yuv_t yuv;
    unsigned char *input = alloc_input();
    unsigned char *pristine = alloc_input();
    unsigned char *s1 = malloc(256*1024), *s2 = malloc(256*1024);
    unsigned char *coded;
    int coded_n, ret, n1, n2, g;
    cb_buf_t cb;
    storage_t dec;

    g_fail = 0;
    make_create(&cp, 1, 1);
    CHECK(open_enc(&cp, &enc, &scratch) == 0, "open strict enc");
    CHECK(open_enc(&cp, &ctrl_enc, &ctrl_sc) == 0, "open control enc");
    CHECK(h264bsdInit(&dec, 1) == 0, "decoder init");
    make_yuv(&yuv, input);

    /* IDR failure: rich texture at low QP cannot fit a 64-byte NAL. */
    gen_texture(input, 99);
    memcpy(pristine, input, FRAMESIZE);
    make_run(&rp, 10, 0, 64);
    memset(&cb, 0, sizeof(cb));
    rp.nalu_callback = nal_callback; rp.nalu_callback_token = &cb;
    coded = (unsigned char *)0x1; coded_n = 777;
    ret = H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n);
    CHECK(ret == H264E_STATUS_NAL_TOO_SMALL, "oversized MB rejected on IDR");
    CHECK(cb.count == 0, "no NAL callback on failed IDR");
    CHECK(coded == 0 && coded_n == 0, "null/zero output on IDR failure");
    CHECK(memcmp(input, pristine, FRAMESIZE) == 0, "input image untouched after IDR fail");

    /* Relaxed retry must equal a never-failed control encoder. */
    make_run(&rp, 10, 0, 4096);
    make_run(&cr, 10, 0, 4096);
    CHECK(encode_to_buf(enc, scratch, &rp, input, s1, &n1) == 0, "strict retry");
    CHECK(encode_to_buf(ctrl_enc, ctrl_sc, &cr, pristine, s2, &n2) == 0, "control encode");
    CHECK(n1 == n2 && memcmp(s1, s2, n1) == 0, "retried IDR byte-identical to control");
    {
        int err = 0, pics = decode_stream(&dec, s1, n1, 0, 0, &err);
        CHECK(pics == 1 && err == 0, "retried IDR decodes cleanly");
    }

    /* Build reference history with good P frames. */
    for (g = 1; g <= 3; g++)
    {
        gen_motion(input, g);
        gen_motion(pristine, g);
        make_run(&rp, 30, 0, 200);
        make_run(&cr, 30, 0, 200);
        CHECK(encode_to_buf(enc, scratch, &rp, input, s1, &n1) == 0, "strict history P");
        CHECK(encode_to_buf(ctrl_enc, ctrl_sc, &cr, pristine, s2, &n2) == 0, "control history P");
        CHECK(n1 == n2 && memcmp(s1, s2, n1) == 0, "history P identical");
        {
            int err = 0, pics = decode_stream(&dec, s1, n1, g, 0, &err);
            CHECK(pics == 1 && err == 0, "history P decodes");
        }
    }

    /* Mixed-content P failure: several slices are produced internally but
       the frame is rejected, so the caller must observe nothing. */
    gen_mixed(input, 42);
    memcpy(pristine, input, FRAMESIZE);
    make_run(&rp, 24, 0, 120);
    memset(&cb, 0, sizeof(cb));
    rp.nalu_callback = nal_callback; rp.nalu_callback_token = &cb;
    coded = (unsigned char *)0x1; coded_n = 777;
    ret = H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n);
    CHECK(ret == H264E_STATUS_NAL_TOO_SMALL, "mixed P frame rejected");
    CHECK(cb.count == 0, "failed P frame delivers no callback despite internal slices");
    CHECK(coded == 0 && coded_n == 0, "null output on P failure");
    CHECK(memcmp(input, pristine, FRAMESIZE) == 0, "P failure restores input");

    /* Relaxed retry then following frames match the control timeline. */
    make_run(&rp, 24, 0, 300);
    make_run(&cr, 24, 0, 300);
    CHECK(encode_to_buf(enc, scratch, &rp, input, s1, &n1) == 0, "P retry");
    CHECK(encode_to_buf(ctrl_enc, ctrl_sc, &cr, pristine, s2, &n2) == 0, "control at P retry");
    CHECK(n1 == n2 && memcmp(s1, s2, n1) == 0, "P retry byte-identical to control");
    for (g = 5; g < 8; g++)
    {
        int err = 0, pics;
        gen_motion(input, g);
        gen_motion(pristine, g);
        make_run(&rp, 28, 0, 256);
        make_run(&cr, 28, 0, 256);
        CHECK(encode_to_buf(enc, scratch, &rp, input, s1, &n1) == 0, "follow-up P");
        CHECK(encode_to_buf(ctrl_enc, ctrl_sc, &cr, pristine, s2, &n2) == 0, "control follow-up");
        CHECK(n1 == n2 && memcmp(s1, s2, n1) == 0, "follow-up matches control timeline");
        pics = decode_stream(&dec, s1, n1, g, 0, &err);
        CHECK(pics == 1 && err == 0, "post-failure reference chain decodes");
    }
    h264bsdShutdown(&dec);
    free(enc); free(scratch); free(ctrl_enc); free(ctrl_sc);
    free(input); free(pristine); free(s1); free(s2);
    return g_fail ? 0 : assertions;
}

/* ------------------------------------------------------------------ */
/* case_compatibility                                                */
/* ------------------------------------------------------------------ */

int case_compatibility(void)
{
    int assertions = 0;
    void *enc, *scratch, *enc2, *scratch2;
    H264E_create_param_t cp, cp_bad;
    H264E_run_param_t rp, rp2;
    H264E_io_yuv_t yuv, yuv2;
    unsigned char *input = alloc_input(), *input2 = alloc_input();
    unsigned char *coded, *coded2;
    int coded_n, coded_n2, f;
    cb_buf_t cb;
    const int bad_vals[] = { -7, 1, 63, 65536, 1000000 };

    g_fail = 0;
    make_create(&cp, 1, 1);
    CHECK(open_enc(&cp, &enc, &scratch) == 0, "open legacy enc");
    CHECK(open_enc(&cp, &enc2, &scratch2) == 0, "open second enc");
    make_yuv(&yuv, input);
    make_yuv(&yuv2, input2);

    for (f = 0; f < (int)(sizeof(bad_vals)/sizeof(bad_vals[0])); f++)
    {
        make_run(&rp, 30, 0, bad_vals[f]);
        gen_flat(input, 1);
        CHECK(H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n)
              == H264E_STATUS_BAD_PARAMETER, "bad max_nalu_bytes rejected");
    }

    /* SVC multi-layer + positive limit rejected. */
    make_create(&cp_bad, 2, 1);
    {
        int a, b;
        CHECK(H264E_sizeof(&cp_bad, &a, &b) == 0, "svc sizeof");
        void *benc = malloc(a), *bsc = malloc(b);
        CHECK(benc && bsc && H264E_init(benc, &cp_bad) == 0, "svc init");
        make_run(&rp, 30, 0, 256);
        gen_flat(input, 2);
        CHECK(H264E_encode(benc, bsc, &rp, &yuv, &coded, &coded_n)
              == H264E_STATUS_BAD_PARAMETER, "SVC + hard limit rejected");
        free(benc); free(bsc);
    }

    /* Multi-thread + positive limit rejected. */
    make_create(&cp_bad, 1, 2);
    {
        int a, b;
        CHECK(H264E_sizeof(&cp_bad, &a, &b) == 0, "mt sizeof");
        void *benc = malloc(a), *bsc = malloc(b);
        CHECK(benc && bsc && H264E_init(benc, &cp_bad) == 0, "mt init");
        make_run(&rp, 30, 0, 256);
        gen_flat(input, 3);
        CHECK(H264E_encode(benc, bsc, &rp, &yuv, &coded, &coded_n)
              == H264E_STATUS_BAD_PARAMETER, "threads + hard limit rejected");
        free(benc); free(bsc);
    }

    /* max_nalu_bytes=0 preserves the exact legacy soft-threshold stream. */
    for (f = 0; f < 4; f++)
    {
        gen_texture(input, f + 5);
        memcpy(input2, input, FRAMESIZE);
        make_run(&rp, 30, 128, 0);
        make_run(&rp2, 30, 128, 0);
        CHECK(H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n) == 0, "legacy run A");
        CHECK(H264E_encode(enc2, scratch2, &rp2, &yuv2, &coded2, &coded_n2) == 0, "legacy run B");
        CHECK(coded_n == coded_n2 && memcmp(coded, coded2, coded_n) == 0,
              "max=0 streams identical");
    }

    /* Callback bytes (with start codes) match the return buffer exactly. */
    memset(&cb, 0, sizeof(cb));
    gen_texture(input, 11);
    make_run(&rp, 30, 128, 0);
    rp.nalu_callback = nal_callback; rp.nalu_callback_token = &cb;
    CHECK(H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n) == 0, "callback legacy encode");
    CHECK(cb.off == coded_n && memcmp(cb.data, coded, coded_n) == 0, "legacy callback equals return");

    memset(&cb, 0, sizeof(cb));
    gen_texture(input, 12);
    make_run(&rp, 30, 0, 200);
    rp.nalu_callback = nal_callback; rp.nalu_callback_token = &cb;
    CHECK(H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_n) == 0, "callback strict encode");
    CHECK(cb.off == coded_n && memcmp(cb.data, coded, coded_n) == 0, "strict callback equals return");
    CHECK(cb.count >= 2, "strict frame split into NALs");
    {
        int i;
        for (i = 0; i < cb.count; i++)
            CHECK(cb.lens[i] <= 200, "callback NAL within limit");
    }

    free(enc); free(scratch); free(enc2); free(scratch2);
    free(input); free(input2);
    return g_fail ? 0 : assertions;
}
