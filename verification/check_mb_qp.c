/*
    Scenario test for H264E_run_param_t::mb_qp (per-macroblock QP map).

    Encodes synthetic 64x64 AVC sequences with various QP maps and writes,
    per scenario, into the output directory (argv[1]):
        <name>.264      - Annex-B stream
        <name>.ref.yuv  - encoder local reconstruction (decoder must match)
        <name>.expect   - expected per-MB QP for mb_qp_dump (map scenarios)

    Additionally verifies in-process:
      - no-map and all-zero-map outputs are byte-identical (compat)
      - invalid maps are rejected with H264E_STATUS_BAD_PARAMETER before
        any side effect, and a corrected retry produces a stream identical
        to a never-failed encoder (retry)
      - a map is not inherited by the next frame, even when run params are
        reused by passing opt == NULL (inherit)
      - NULL map ignores stride/bytes garbage

    The bitstreams are decoded with FFmpeg and the QP maps are verified
    against mb_qp_dump by verification/check_mb_qp.sh.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#if defined(__APPLE__) && defined(__aarch64__)
/* Old Apple intrinsic spelling used by this upstream snapshot. */
#define vtbl2q_u8 vqtbl2_u8
#endif
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

#define W   64
#define H   64
#define NMBX (W/16)
#define NMBY (H/16)
#define FRAME_BYTES (W*H*3/2)
#define QP_BG    30                 // qp_max: rate control is pinned here (desired_frame_bytes == 0)
#define QP_TEXT  20                 // qp_min and the forced QP carried by the map

static char g_dir[1024];

/************************************************************************/
/*      Minimal spawn-and-join thread runner for the encoder callback   */
/************************************************************************/
#if H264E_MAX_THREADS
#include <pthread.h>
typedef struct
{
    void (*func)(void*);
    void *data;
} job_t;

static void *job_trampoline(void *arg)
{
    job_t *j = (job_t *)arg;
    j->func(j->data);
    return 0;
}

static void run_jobs_in_threads(void *token, void (*job_func)(void*), void *job_data[], int njobs)
{
    pthread_t th[H264E_MAX_THREADS];
    job_t jobs[H264E_MAX_THREADS];
    int i;
    (void)token;
    for (i = 0; i < njobs; i++)
    {
        jobs[i].func = job_func;
        jobs[i].data = job_data[i];
        pthread_create(&th[i], 0, job_trampoline, &jobs[i]);
    }
    for (i = 0; i < njobs; i++)
        pthread_join(th[i], 0);
}
#endif /* H264E_MAX_THREADS */

/************************************************************************/
/*      Synthetic input: static smooth left half (skip-friendly),       */
/*      busy right half that changes every frame (guaranteed residual)  */
/************************************************************************/
static void gen_frame(unsigned char *img, int t)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            img[y*W + x] = (unsigned char)(x < W/2
                ? 96 + ((x + y) & 7)                    // static gradient
                : (x*7 + y*13 + t*31 + ((x*y) >> 3)));  // moving texture
    memset(img + W*H, 128, W*H/4);
    memset(img + W*H*5/4, 128, W*H/4);
}

/************************************************************************/
/*      Encoder driver                                                  */
/************************************************************************/
typedef struct
{
    int threads;                    // max_threads (0/1 = single-threaded)
    int nalu_bytes;                 // desired_nalu_bytes
    const unsigned char *map;       // QP map or NULL
    int map_stride;
    size_t map_bytes;
    int reuse_prev_params;          // if set, pass opt == NULL from frame 1 on
    int num_layers;                 // create_param.num_layers
} enc_opt_t;

typedef struct
{
    unsigned char *data;            // concatenated bitstream
    int size;
    unsigned char *recon;           // concatenated local reconstruction
    int error;                      // first encode error, if any
} enc_out_t;

/* Encode nframes; on the frame indexed by fail_frame, substitute bad_map
   (expected to be rejected); the same frame is then retried with the good
   map, so the resulting sequence must match a never-failed encoder. */
static int encode_seq(enc_out_t *out, const enc_opt_t *opt, int nframes,
    int fail_frame, const H264E_run_param_t *bad_params)
{
    H264E_create_param_t cp;
    H264E_run_param_t rp;
    H264E_io_yuv_t yuv;
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    unsigned char *img, *coded;
    int np = 0, ns = 0, err, t, coded_size = 0;

    memset(&cp, 0, sizeof(cp));
    cp.width = W;
    cp.height = H;
    cp.gop = 30;
    cp.enableNEON = 1;
    cp.num_layers = opt->num_layers ? opt->num_layers : 1;
#if H264E_MAX_THREADS
    if (opt->threads > 1)
    {
        cp.max_threads = opt->threads;
        cp.run_func_in_thread = run_jobs_in_threads;
    }
#endif
    err = H264E_sizeof(&cp, &np, &ns);
    if (err)
        return err;
    enc = (H264E_persist_t*)malloc((size_t)np);
    scratch = (H264E_scratch_t*)malloc((size_t)ns);
    img = (unsigned char*)malloc(FRAME_BYTES);
    err = H264E_init(enc, &cp);
    if (err)
        return err;

    out->data = (unsigned char*)malloc((size_t)nframes*65536);
    out->recon = (unsigned char*)malloc((size_t)nframes*FRAME_BYTES);
    out->size = 0;
    out->error = 0;

    yuv.yuv[0] = img;             yuv.stride[0] = W;
    yuv.yuv[1] = img + W*H;       yuv.stride[1] = W/2;
    yuv.yuv[2] = img + W*H*5/4;   yuv.stride[2] = W/2;

    memset(&rp, 0, sizeof(rp));
    rp.encode_speed = 5;
    rp.desired_frame_bytes = 0;     // pins the rate-control QP at qp_max
    rp.qp_min = QP_TEXT;
    rp.qp_max = QP_BG;
    rp.desired_nalu_bytes = opt->nalu_bytes;
    rp.mb_qp = opt->map;
    rp.mb_qp_stride = opt->map_stride;
    rp.mb_qp_bytes = opt->map_bytes;

    for (t = 0; t < nframes; t++)
    {
        const H264E_run_param_t *p = &rp;
        gen_frame(img, t);
        if (t == fail_frame && bad_params)
        {
            err = H264E_encode(enc, scratch, bad_params, &yuv, &coded, &coded_size);
            if (err != H264E_STATUS_BAD_PARAMETER)
            {
                out->error = err ? err : -1000;   // rejected maps must fail with BAD_PARAMETER
                break;
            }
        }
        if (t && opt->reuse_prev_params)
            p = 0;                              // reuse previous run params
        err = H264E_encode(enc, scratch, p, &yuv, &coded, &coded_size);
        if (err)
        {
            out->error = err;
            break;
        }
        memcpy(out->data + out->size, coded, (size_t)coded_size);
        out->size += coded_size;
        memcpy(out->recon + (size_t)t*FRAME_BYTES, img, FRAME_BYTES);
    }
    free(img);
    free(scratch);
    free(enc);
    return out->error;
}

static void write_file(const char *name, const char *ext, const void *data, int size)
{
    char path[1200];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s.%s", g_dir, name, ext);
    f = fopen(path, "wb");
    if (!f || (int)fwrite(data, 1, (size_t)size, f) != size)
    {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fclose(f);
}

/* expected QP per MB: forced map value where set, pinned rate-control QP elsewhere */
static void write_expect(const char *name, const unsigned char *map, int stride, int nframes)
{
    char path[1200];
    FILE *f;
    int t, x, y;
    snprintf(path, sizeof(path), "%s/%s.expect", g_dir, name);
    f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fprintf(f, "%d %d %d\n", NMBX, NMBY, nframes);
    for (t = 0; t < nframes; t++)
        for (y = 0; y < NMBY; y++)
            for (x = 0; x < NMBX; x++)
            {
                int v = map ? map[y*stride + x] : 0;
                fprintf(f, "%d\n", v ? v : QP_BG);
            }
    fclose(f);
}

static void run_scenario(const char *name, const enc_opt_t *opt, int nframes, int with_expect)
{
    enc_out_t out;
    int err = encode_seq(&out, opt, nframes, -1, 0);
    if (err)
    {
        fprintf(stderr, "scenario %s: encode error %d\n", name, err);
        exit(1);
    }
    write_file(name, "264", out.data, out.size);
    write_file(name, "ref.yuv", out.recon, nframes*FRAME_BYTES);
    if (with_expect)
        write_expect(name, opt->map, opt->map_stride, nframes);
    printf("scenario %-8s: %d frames, %d bytes\n", name, nframes, out.size);
    free(out.data);
    free(out.recon);
}

/************************************************************************/
/*      QP maps                                                         */
/************************************************************************/
static unsigned char g_checker[NMBY][NMBX];         // checkerboard QP_TEXT/0
static unsigned char g_padded[NMBY][NMBX + 3];      // checkerboard + 0xFF padding
static unsigned char g_zero[NMBY][NMBX];            // all-zero map

static void make_maps(void)
{
    int x, y;
    for (y = 0; y < NMBY; y++)
        for (x = 0; x < NMBX; x++)
        {
            g_checker[y][x] = ((x + y) & 1) ? 0 : QP_TEXT;
            g_padded[y][x] = ((x + y) & 1) ? 0 : QP_TEXT;
            g_zero[y][x] = 0;
        }
    for (y = 0; y < NMBY; y++)
        for (x = NMBX; x < NMBX + 3; x++)
            g_padded[y][x] = 0xFF;              // garbage row padding
}

/************************************************************************/
/*      main                                                            */
/************************************************************************/
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } \
} while (0)

int main(int argc, char **argv)
{
    enc_opt_t opt;
    enc_out_t a, b;
    int i;
    H264E_run_param_t bad;

    if (argc != 2)
    {
        fprintf(stderr, "usage: check_mb_qp <outdir>\n");
        return 2;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    make_maps();

    memset(&opt, 0, sizeof(opt));

    // 1. checkerboard map, I/P sequence with skipped MBs in between
    opt.map = &g_checker[0][0];
    opt.map_stride = NMBX;
    opt.map_bytes = sizeof(g_checker);
    run_scenario("checker", &opt, 3, 1);

    // 2. same map with row padding (garbage padding bytes must be ignored)
    opt.map = &g_padded[0][0];
    opt.map_stride = NMBX + 3;
    opt.map_bytes = (NMBY - 1)*(NMBX + 3) + NMBX;
    run_scenario("padding", &opt, 2, 1);

    // 3. two encoder threads: map must be indexed by absolute MB coordinates
    opt.map = &g_checker[0][0];
    opt.map_stride = NMBX;
    opt.map_bytes = sizeof(g_checker);
    opt.threads = 2;
    run_scenario("threads", &opt, 2, 1);
    opt.threads = 0;

    // 4. single thread, small NALUs: slice restarts must not reset map rows
    opt.nalu_bytes = 60;
    run_scenario("slices", &opt, 2, 1);
    opt.nalu_bytes = 0;

    // 5. compat: no map vs all-zero map must be byte-identical
    {
        enc_opt_t o2 = opt;
        o2.map = 0;
        CHECK(!encode_seq(&a, &o2, 3, -1, 0), "compat encode (no map)");
        o2.map = &g_zero[0][0];
        o2.map_stride = NMBX;
        o2.map_bytes = sizeof(g_zero);
        CHECK(!encode_seq(&b, &o2, 3, -1, 0), "compat encode (zero map)");
        CHECK(a.size == b.size && !memcmp(a.data, b.data, (size_t)a.size),
            "all-zero map output differs from no-map output");
        write_file("compat", "264", b.data, b.size);
        write_file("compat", "ref.yuv", b.recon, 3*FRAME_BYTES);
        printf("scenario compat  : no-map == zero-map (%d bytes)\n", b.size);
        free(a.data); free(a.recon);
        free(b.data); free(b.recon);
    }

    // 6. invalid maps: rejected with BAD_PARAMETER; retry == never-failed
    {
        enc_opt_t o2 = opt;
        o2.map = &g_checker[0][0];
        o2.map_stride = NMBX;
        o2.map_bytes = sizeof(g_checker);

        memset(&bad, 0, sizeof(bad));
        bad.encode_speed = 5;
        bad.desired_frame_bytes = 0;
        bad.qp_min = QP_TEXT;
        bad.qp_max = QP_BG;
        bad.mb_qp = &g_checker[0][0];
        bad.mb_qp_stride = NMBX;
        bad.mb_qp_bytes = sizeof(g_checker);

        // stride too small
        bad.mb_qp_stride = NMBX - 1;
        CHECK(encode_seq(&a, &o2, 1, 0, &bad) == 0 || a.error == H264E_STATUS_BAD_PARAMETER,
            "small stride not rejected");
        // negative stride
        bad.mb_qp_stride = -4;
        CHECK(encode_seq(&a, &o2, 1, 0, &bad) == 0 || a.error == H264E_STATUS_BAD_PARAMETER,
            "negative stride not rejected");
        // bytes not covering the last valid cell
        bad.mb_qp_stride = NMBX;
        bad.mb_qp_bytes = sizeof(g_checker) - 1;
        CHECK(encode_seq(&a, &o2, 1, 0, &bad) == 0 || a.error == H264E_STATUS_BAD_PARAMETER,
            "short buffer not rejected");
        // QP below qp_min / above qp_max / below MIN_QP
        {
            unsigned char badmap[NMBY][NMBX];
            static const int badqp[3] = { QP_TEXT - 1, QP_BG + 1, 9 };
            memcpy(badmap, g_checker, sizeof(badmap));
            for (i = 0; i < 3; i++)
            {
                badmap[1][2] = (unsigned char)badqp[i];
                bad.mb_qp_bytes = sizeof(badmap);
                bad.mb_qp = &badmap[0][0];
                CHECK(encode_seq(&a, &o2, 1, 0, &bad) == 0 || a.error == H264E_STATUS_BAD_PARAMETER,
                    "out-of-range map QP not rejected");
            }
            bad.mb_qp = &g_checker[0][0];
        }
        // SVC multi-layer + map must be refused (before touching any layer)
        {
            enc_opt_t svc = o2;
            enc_out_t s;
            svc.num_layers = 2;
            CHECK(encode_seq(&s, &svc, 1, 0, &bad) == 0 || s.error == H264E_STATUS_BAD_PARAMETER,
                "SVC multi-layer map not rejected");
        }
        // NULL map: stride/bytes garbage is ignored
        {
            enc_opt_t o3 = o2;
            o3.map = 0;
            o3.map_stride = -7;
            o3.map_bytes = 0;
            CHECK(!encode_seq(&a, &o3, 1, -1, 0), "NULL map with garbage stride/bytes");
            free(a.data); free(a.recon);
        }

        // retry after a rejected call == never-failed sequence
        bad.mb_qp_stride = NMBX - 1;    // invalid on the 2nd frame
        bad.mb_qp_bytes = sizeof(g_checker);
        CHECK(!encode_seq(&a, &o2, 3, 1, &bad), "retry sequence");
        CHECK(!encode_seq(&b, &o2, 3, -1, 0), "reference sequence");
        CHECK(a.size == b.size && !memcmp(a.data, b.data, (size_t)a.size),
            "retry after rejected map differs from never-failed sequence");
        write_file("retry", "264", a.data, a.size);
        write_file("retry", "ref.yuv", a.recon, 3*FRAME_BYTES);
        printf("scenario retry   : rejected call left no trace (%d bytes)\n", a.size);
        free(a.data); free(a.recon);
        free(b.data); free(b.recon);
    }

    // 7. map is borrowed per call: not inherited, even with opt == NULL reuse
    {
        enc_opt_t o2 = opt;
        o2.map = &g_checker[0][0];
        o2.map_stride = NMBX;
        o2.map_bytes = sizeof(g_checker);
        o2.reuse_prev_params = 1;       // frame 1+ encoded with opt == NULL
        CHECK(!encode_seq(&a, &o2, 2, -1, 0), "inherit check (reuse params)");

        o2.reuse_prev_params = 0;
        o2.map = 0;                     // frame 1+ explicitly without map...
        {
            // ...but frame 0 must still use the map: encode frame 0 with map,
            // frame 1 without, by switching the map between calls
            H264E_create_param_t cp;
            H264E_run_param_t rp;
            H264E_io_yuv_t yuv;
            H264E_persist_t *enc;
            H264E_scratch_t *scratch;
            unsigned char *img, *coded;
            int np, ns, err, t, coded_size;
            unsigned char checker[NMBY][NMBX];

            memcpy(checker, g_checker, sizeof(checker));
            memset(&cp, 0, sizeof(cp));
            cp.width = W; cp.height = H; cp.gop = 30; cp.enableNEON = 1; cp.num_layers = 1;
            CHECK(!H264E_sizeof(&cp, &np, &ns), "sizeof");
            enc = (H264E_persist_t*)malloc((size_t)np);
            scratch = (H264E_scratch_t*)malloc((size_t)ns);
            img = (unsigned char*)malloc(FRAME_BYTES);
            CHECK(!H264E_init(enc, &cp), "init");
            yuv.yuv[0] = img;             yuv.stride[0] = W;
            yuv.yuv[1] = img + W*H;       yuv.stride[1] = W/2;
            yuv.yuv[2] = img + W*H*5/4;   yuv.stride[2] = W/2;
            memset(&rp, 0, sizeof(rp));
            rp.encode_speed = 5;
            rp.desired_frame_bytes = 0;
            rp.qp_min = QP_TEXT;
            rp.qp_max = QP_BG;
            b.data = (unsigned char*)malloc(2*65536);
            b.recon = (unsigned char*)malloc(2*FRAME_BYTES);
            b.size = 0;
            for (t = 0; t < 2; t++)
            {
                gen_frame(img, t);
                rp.mb_qp = t ? 0 : &checker[0][0];  // map only on frame 0
                rp.mb_qp_stride = NMBX;
                rp.mb_qp_bytes = sizeof(checker);
                if (t == 1)
                    memset(checker, 0xAB, sizeof(checker));  // clobber the borrowed map
                err = H264E_encode(enc, scratch, &rp, &yuv, &coded, &coded_size);
                CHECK(!err, "inherit check encode");
                memcpy(b.data + b.size, coded, (size_t)coded_size);
                b.size += coded_size;
                memcpy(b.recon + (size_t)t*FRAME_BYTES, img, FRAME_BYTES);
            }
            free(img); free(scratch); free(enc);
        }
        CHECK(a.size == b.size && !memcmp(a.data, b.data, (size_t)a.size),
            "map leaked into the next frame (opt == NULL reuse)");
        printf("scenario inherit : map not carried over (%d bytes)\n", a.size);
        free(a.data); free(a.recon);
        free(b.data); free(b.recon);
    }

    printf("CHECK_MB_QP_SCENARIOS_OK\n");
    return 0;
}
