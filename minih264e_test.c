#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#define MINIH264_IMPLEMENTATION
//#define MINIH264_ONLY_SIMD
#include "minih264e.h"
#include "minih264e_2pass.h"

#define DEFAULT_GOP 20
#define DEFAULT_QP 33
#define DEFAULT_DENOISE 0

#define ENABLE_TEMPORAL_SCALABILITY 0
#define MAX_LONG_TERM_FRAMES        8 // used only if ENABLE_TEMPORAL_SCALABILITY==1

#define DEFAULT_MAX_FRAMES  99999

H264E_create_param_t create_param;
H264E_run_param_t run_param;
H264E_io_yuv_t yuv;
uint8_t *buf_in, *buf_save;
uint8_t *coded_data;
FILE *fin, *fout;
int sizeof_coded_data, frame_size, g_w, g_h, _qp;

#ifdef _WIN32
// only vs2017 have aligned_alloc
#define ALIGNED_ALLOC(n, size) malloc(size)
#else
#define ALIGNED_ALLOC(n, size) aligned_alloc(n, (size + n - 1)/n*n)
#endif

#if H264E_MAX_THREADS
#include "system.h"
typedef struct
{
    void *event_start;
    void *event_done;
    void (*callback)(void*);
    void *job;
    void *thread;
    int terminated;
} h264e_thread_t;

static THREAD_RET THRAPI minih264_thread_func(void *arg)
{
    h264e_thread_t *t = (h264e_thread_t *)arg;
    thread_name("h264");
    for (;;)
    {
        event_wait(t->event_start, INFINITE);
        if (t->terminated)
            break;
        t->callback(t->job);
        event_set(t->event_done);
    }
    return 0;
}

void *h264e_thread_pool_init(int max_threads)
{
    int i;
    h264e_thread_t *threads = (h264e_thread_t *)calloc(sizeof(h264e_thread_t), max_threads);
    if (!threads)
        return 0;
    for (i = 0; i < max_threads; i++)
    {
        h264e_thread_t *t = threads + i;
        t->event_start = event_create(0, 0);
        t->event_done  = event_create(0, 0);
        t->thread = thread_create(minih264_thread_func, t);
    }
    return threads;
}

void h264e_thread_pool_close(void *pool, int max_threads)
{
    int i;
    h264e_thread_t *threads = (h264e_thread_t *)pool;
    for (i = 0; i < max_threads; i++)
    {
        h264e_thread_t *t = threads + i;
        t->terminated = 1;
        event_set(t->event_start);
        thread_wait(t->thread);
        thread_close(t->thread);
        event_destroy(t->event_start);
        event_destroy(t->event_done);
    }
    free(pool);
}

void h264e_thread_pool_run(void *pool, void (*callback)(void*), void *callback_job[], int njobs)
{
    h264e_thread_t *threads = (h264e_thread_t*)pool;
    int i;
    for (i = 0; i < njobs; i++)
    {
        h264e_thread_t *t = threads + i;
        t->callback = (void (*)(void *))callback;
        t->job = callback_job[i];
        event_set(t->event_start);
    }
    for (i = 0; i < njobs; i++)
    {
        h264e_thread_t *t = threads + i;
        event_wait(t->event_done, INFINITE);
    }
}
#endif

struct
{
    const char *input_file;
    const char *output_file;
    const char *pass1_file;     // -pass1<stats>
    const char *pass2_file;     // -pass2<stats>
    const char *target_bytes;   // -targetbytes<N>
    int gen, gop, qp, kbps, max_frames, threads, speed, denoise, stats, psnr;
    int qp_min, qp_max;         // -qpmin / -qpmax (two-pass QP window)
    int qp_set;                 // -qp given explicitly on the command line
} cmdline[1];

static int str_equal(const char *pattern, char **p)
{
    if (!strncmp(pattern, *p, strlen(pattern)))
    {
        *p += strlen(pattern);
        return 1;
    } else
    {
        return 0;
    }
}

static int read_cmdline_options(int argc, char *argv[])
{
    int i;
    memset(cmdline, 0, sizeof(*cmdline));
    cmdline->gop = DEFAULT_GOP;
    cmdline->qp = DEFAULT_QP;
    cmdline->qp_min = 10;
    cmdline->qp_max = 51;
    cmdline->max_frames = DEFAULT_MAX_FRAMES;
    cmdline->kbps = 0;
    //cmdline->kbps = 2048;
    cmdline->denoise = DEFAULT_DENOISE;
    for (i = 1; i < argc; i++)
    {
        char *p = argv[i];
        if (*p == '-')
        {
            p++;
            if (str_equal(("gen"), &p))
            {
                cmdline->gen = 1;
            } else if (str_equal(("gop"), &p))
            {
                cmdline->gop = atoi(p);
            } else if (str_equal(("qp"), &p))
            {
                cmdline->qp = atoi(p);
                cmdline->qp_set = 1;
            } else if (str_equal(("qpmin"), &p))
            {
                cmdline->qp_min = atoi(p);
            } else if (str_equal(("qpmax"), &p))
            {
                cmdline->qp_max = atoi(p);
            } else if (str_equal(("kbps"), &p))
            {
                cmdline->kbps = atoi(p);
            } else if (str_equal(("maxframes"), &p))
            {
                cmdline->max_frames = atoi(p);
            } else if (str_equal(("threads"), &p))
            {
                cmdline->threads = atoi(p);
            } else if (str_equal(("speed"), &p))
            {
                cmdline->speed = atoi(p);
            } else if (str_equal(("denoise"), &p))
            {
                cmdline->denoise = 1;
            } else if (str_equal(("stats"), &p))
            {
                cmdline->stats = 1;
            } else if (str_equal(("psnr"), &p))
            {
                cmdline->psnr = 1;
            } else if (str_equal(("pass1"), &p))
            {
                if (!*p)
                {
                    printf("ERROR: -pass1 requires a statistics path\n");
                    return 0;
                }
                cmdline->pass1_file = p;
            } else if (str_equal(("pass2"), &p))
            {
                if (!*p)
                {
                    printf("ERROR: -pass2 requires a statistics path\n");
                    return 0;
                }
                cmdline->pass2_file = p;
            } else if (str_equal(("targetbytes"), &p))
            {
                if (!*p)
                {
                    printf("ERROR: -targetbytes requires a value\n");
                    return 0;
                }
                cmdline->target_bytes = p;
            } else
            {
                printf("ERROR: Unknown option %s\n", p - 1);
                return 0;
            }
        } else if (!cmdline->input_file && !cmdline->gen)
        {
            cmdline->input_file = p;
        } else if (!cmdline->output_file)
        {
            cmdline->output_file = p;
        } else
        {
            printf("ERROR: Unknown option %s\n", p);
            return 0;
        }
    }
    if (!cmdline->input_file && !cmdline->gen)
    {
        printf("Usage:\n"
               "    h264e_test [options] <input[frame_size].yuv> <output.264>\n"
               "Frame size can be: WxH sqcif qvga svga 4vga sxga xga vga qcif 4cif\n"
               "    4sif cif sif pal ntsc d1 16cif 16sif 720p 4SVGA 4XGA 16VGA 16VGA\n"
               "Options:\n"
               "    -gen            - generate input instead of passing <input.yuv>\n"
               "    -qop<n>         - key frame period >= 0\n"
               "    -qp<n>          - set QP [10..51]\n"
               "    -kbps<n>        - set bitrate (fps=30 assumed)\n"
               "    -maxframes<n>   - encode no more than given number of frames\n"
               "    -threads<n>     - use <n> threads for encode\n"
               "    -speed<n>       - speed [0..10], 0 means best quality\n"
               "    -denoise        - use temporal noise supression\n"
               "    -stats          - print frame statistics\n"
               "    -psnr           - print psnr statistics\n"
               "    -pass1<stats>   - two-pass pass 1: encode and write mb statistics\n"
               "    -pass2<stats>   - two-pass pass 2: plan per-frame QP from statistics\n"
               "    -targetbytes<n> - whole clip byte target for -pass2 (ignored by -pass1)\n"
               "    -qpmin<n>       - pass-2 lower QP bound [10..51], default 10\n"
               "    -qpmax<n>       - pass-2 upper QP bound [10..51], default 51\n"
               "\n"
               "Two-pass mode requires a single spatial layer, single thread, no\n"
               "long-term reference, no denoise, and no VBV stuffing/dropping.\n");
        return 0;
    }
    return 1;
}

typedef struct
{
    const char *size_name;
    int g_w;
    int h;
} frame_size_descriptor_t;

static const frame_size_descriptor_t g_frame_size_descriptor[] =
{
    {"sqcif",  128,   96},
    { "qvga",  320,  240},
    { "svga",  800,  600},
    { "4vga", 1280,  960},
    { "sxga", 1280, 1024},
    {  "xga", 1024,  768},
    {  "vga",  640,  480},
    { "qcif",  176,  144},
    { "4cif",  704,  576},
    { "4sif",  704,  480},
    {  "cif",  352,  288},
    {  "sif",  352,  240},
    {  "pal",  720,  576},
    { "ntsc",  720,  480},
    {   "d1",  720,  480},
    {"16cif", 1408, 1152},
    {"16sif", 1408,  960},
    { "720p", 1280,  720},
    {"4SVGA", 1600, 1200},
    { "4XGA", 2048, 1536},
    {"16VGA", 2560, 1920},
    {"16VGA", 2560, 1920},
    {NULL, 0, 0},
};

/**
*   Guess image size specification from ASCII string.
*   If string have several specs, only last one taken.
*   Spec may look like "352x288" or "qcif", "cif", etc.
*/
static int guess_format_from_name(const char *file_name, int *w, int *h)
{
    int i = (int)strlen(file_name);
    int found = 0;
    while(--i >= 0)
    {
        const frame_size_descriptor_t *fmt = g_frame_size_descriptor;
        const char *p = file_name + i;
        int prev_found = found;
        found = 0;
        if (*p >= '0' && *p <= '9')
        {
            char * end;
            int width = strtoul(p, &end, 10);
            if (width && (*end == 'x' || *end == 'X') && (end[1] >= '1' && end[1] <= '9'))
            {
                int height = strtoul(end + 1, &end, 10);
                if (height)
                {
                    *w = width;
                    *h = height;
                    found = 1;
                }
            }
        }
        do
        {
            if (!strncmp(file_name + i, fmt->size_name, strlen(fmt->size_name)))
            {
                *w = fmt->g_w;
                *h = fmt->h;
                found = 1;
            }
        } while((++fmt)->size_name);

        if (!found && prev_found)
        {
            return prev_found;
        }
    }
    return found;
}

// PSNR estimation results
typedef struct
{
    double psnr[4];             // PSNR, db
    double kpbs_30fps;          // bitrate, kbps, assuming 30 fps
    double psnr_to_logkbps_ratio;  // cumulative quality metric
    double psnr_to_kbps_ratio;  // another variant of cumulative quality metric
} rd_t;


static struct
{
    // Y,U,V,Y+U+V
    double noise[4];
    double count[4];
    double bytes;
    int frames;
} g_psnr;

static void psnr_init()
{
    memset(&g_psnr, 0, sizeof(g_psnr));
}

static void psnr_add(unsigned char *p0, unsigned char *p1, int w, int h, int bytes)
{
    int i, k;
    for (k = 0; k < 3; k++)
    {
        double s = 0;
        for (i = 0; i < w*h; i++)
        {
            int d = *p0++ - *p1++;
            s += d*d;
        }
        g_psnr.count[k] += w*h;
        g_psnr.noise[k] += s;
        if (!k) w >>= 1, h >>= 1;
    }
    g_psnr.count[3] = g_psnr.count[0] + g_psnr.count[1] + g_psnr.count[2];
    g_psnr.noise[3] = g_psnr.noise[0] + g_psnr.noise[1] + g_psnr.noise[2];
    g_psnr.frames++;
    g_psnr.bytes += bytes;
}

static rd_t psnr_get()
{
    int i;
    rd_t rd;
    double fps = 30;
    double realkbps = g_psnr.bytes*8./((double)g_psnr.frames/(fps))/1000;
    double db = 10*log10(255.*255/(g_psnr.noise[0]/g_psnr.count[0]));
    for (i = 0; i < 4; i++)
    {
        rd.psnr[i] = 10*log10(255.*255/(g_psnr.noise[i]/g_psnr.count[i]));
    }
    rd.psnr_to_kbps_ratio = 10*log10((double)g_psnr.count[0]*g_psnr.count[0]*3/2 * 255*255/(g_psnr.noise[0] * g_psnr.bytes));
    rd.psnr_to_logkbps_ratio = db / log10(realkbps);
    rd.kpbs_30fps = realkbps;
    return rd;
}

static void psnr_print(rd_t rd)
{
    int i;
    printf("%5.0f kbps@30fps  ", rd.kpbs_30fps);
    for (i = 0; i < 3; i++)
    {
        //printf("  %.2f db ", rd.psnr[i]);
        printf(" %s=%.2f db ", i ? (i == 1 ? "UPSNR" : "VPSNR") : "YPSNR", rd.psnr[i]);
    }
    printf("  %6.2f db/rate ", rd.psnr_to_kbps_ratio);
    printf("  %6.3f db/lgrate ", rd.psnr_to_logkbps_ratio);
    printf("  \n");
}

static int pixel_of_chessboard(double x, double y)
{
#if 0
    int mid = (fabs(x) < 4 && fabs(y) < 4);
    int i = (int)(x);
    int j = (int)(y);
    int cx, cy;
    cx = (i & 16) ? 255 : 0;
    cy = (j & 16) ? 255 : 0;
    if ((i & 15) == 0) cx *= (x - i);
    if ((j & 15) == 0) cx *= (y - j);
    return (cx + cy + 1) >> 1;
#else
    int mid = (fabs(x ) < 4 && fabs(y) < 4);
    int i = (int)(x);
    int j = (int)(y);
    int black = (mid) ? 128 : i/16;
    int white = (mid) ? 128 : 255 - j/16;
    int c00 = (((i >> 4) + (j >> 4)) & 1) ? white : black;
    int c01 = ((((i + 1)>> 4) + (j >> 4)) & 1) ? white : black;
    int c10 = (((i >> 4) + ((j + 1) >> 4)) & 1) ? white : black;
    int c11 = ((((i + 1) >> 4) + ((j + 1) >> 4)) & 1) ? white : black;
    int s    = (int)((c00 * (1 - (x - i)) + c01*(x - i))*(1 - (y - j)) +
                     (c10 * (1 - (x - i)) + c11*(x - i))*((y - j)) + 0.5);
    return s < 0 ? 0 : s > 255 ? 255 : s;
#endif
}

static void gen_chessboard_rot(unsigned char *p, int w, int h, int frm)
{
    int r, c;
    double x, y;
    double co = cos(.01*frm);
    double si = sin(.01*frm);
    int hw = w >> 1;
    int hh = h >> 1;
    for (r = 0; r < h; r++)
    {
        for (c = 0; c < w; c++)
        {
            x =  co*(c - hw) + si*(r - hh);
            y = -si*(c - hw) + co*(r - hh);
            p[r*w + c] = pixel_of_chessboard(x, y);
        }
    }
}

int main(int argc, char *argv[])
{
    int i, frames = 0;
    const char *fnin, *fnout;

    if (!read_cmdline_options(argc, argv))
        return 1;

    if (cmdline->pass1_file && cmdline->pass2_file)
    {
        printf("ERROR: -pass1 and -pass2 are separate processes\n");
        return 1;
    }
    if (cmdline->pass2_file && !cmdline->target_bytes)
    {
        printf("ERROR: -pass2 requires -targetbytes<N>\n");
        return 1;
    }
    if (cmdline->target_bytes && !cmdline->pass1_file && !cmdline->pass2_file)
    {
        printf("ERROR: -targetbytes is only valid with -pass1 or -pass2\n");
        return 1;
    }
    if ((cmdline->pass1_file || cmdline->pass2_file) &&
        (cmdline->threads || cmdline->denoise))
    {
        printf("ERROR: two-pass mode does not support -threads or -denoise\n");
        return 1;
    }

    fnin  = cmdline->input_file;
    fnout = cmdline->output_file;

    if (!cmdline->gen)
    {
        g_w = 352;
        g_h = 288;
        guess_format_from_name(fnin, &g_w, &g_h);
        fin = fopen(fnin, "rb");
        if (!fin)
        {
            printf("ERROR: cant open input file %s\n", fnin);
            return 1;
        }
    } else
    {
        g_w = 1024;
        g_h = 768;
    }

    if (!fnout)
        fnout = "out.264";
    fout = fopen(fnout, "wb");
    if (!fout)
    {
        printf("ERROR: cant open output file %s\n", fnout);
        return 1;
    }

    create_param.enableNEON = 1;
#if H264E_SVC_API
    create_param.num_layers = 1;
    create_param.inter_layer_pred_flag = 0;
#endif
    create_param.gop = cmdline->gop;
    create_param.height = g_h;
    create_param.width  = g_w;
    create_param.max_long_term_reference_frames = 0;
#if ENABLE_TEMPORAL_SCALABILITY
    create_param.max_long_term_reference_frames = MAX_LONG_TERM_FRAMES;
#endif
    create_param.fine_rate_control_flag = 0;
    create_param.const_input_flag = cmdline->psnr ? 0 : 1;
    //create_param.vbv_overflow_empty_frame_flag = 1;
    //create_param.vbv_underflow_stuffing_flag = 1;
    create_param.vbv_size_bytes = cmdline->kbps*1000/8*2; // 2 seconds vbv buffer for quality, so rate control can allocate more bits for intra frame
    create_param.temporal_denoise_flag = cmdline->denoise;

#if H264E_MAX_THREADS
    void *thread_pool = NULL;
    create_param.max_threads = cmdline->threads;
    if (cmdline->threads)
    {
        thread_pool = h264e_thread_pool_init(cmdline->threads);
        create_param.token = thread_pool;
        create_param.run_func_in_thread = h264e_thread_pool_run;
    }
#endif

    frame_size = g_w*g_h*3/2;
    buf_in   = (uint8_t*)ALIGNED_ALLOC(64, frame_size);
    buf_save = (uint8_t*)ALIGNED_ALLOC(64, frame_size);

    if (!buf_in || !buf_save)
    {
        printf("ERROR: not enough memory\n");
        return 1;
    }
    //for (cmdline->qp = 10; cmdline->qp <= 51; cmdline->qp += 10)
    //for (cmdline->qp = 40; cmdline->qp <= 51; cmdline->qp += 10)
    //for (cmdline->qp = 50; cmdline->qp <= 51; cmdline->qp += 2)
    //printf("encoding %s to %s with qp = %d\n", fnin, fnout, cmdline->qp);
    {
        int sum_bytes = 0;
        int max_bytes = 0;
        int min_bytes = 10000000;
        int sizeof_persist = 0, sizeof_scratch = 0, error;
        H264E_persist_t *enc = NULL;
        H264E_scratch_t *scratch = NULL;
        H264E_2pass_writer_t *p1writer = NULL;
        H264E_2pass_file_t *p2file = NULL;
        H264E_2pass_planner_t *planner = NULL;
        double target_bytes = 0.0;
        if (cmdline->psnr)
            psnr_init();

        if (cmdline->pass2_file)
        {
            error = H264E_2pass_file_open(cmdline->pass2_file, &p2file);
            if (error)
            {
                printf("ERROR: pass2 stats %s: %s\n", cmdline->pass2_file,
                    H264E_2pass_strerror(error));
                return 1;
            }
            target_bytes = strtod(cmdline->target_bytes, NULL);
            if (!(target_bytes > 0.0))
            {
                printf("ERROR: -targetbytes must be positive\n");
                return 1;
            }
            if (H264E_2pass_file_width(p2file) != g_w ||
                H264E_2pass_file_height(p2file) != g_h ||
                H264E_2pass_file_gop(p2file) != cmdline->gop)
            {
                printf("ERROR: pass2 stats do not match this clip "
                       "(size %dx%d gop %d)\n",
                       H264E_2pass_file_width(p2file),
                       H264E_2pass_file_height(p2file),
                       H264E_2pass_file_gop(p2file));
                return 1;
            }
            error = H264E_2pass_planner_open(&planner,
                H264E_2pass_file_record(p2file, 0),
                H264E_2pass_file_frames(p2file), target_bytes,
                cmdline->qp_set ? cmdline->qp : cmdline->qp_min,
                cmdline->qp_set ? cmdline->qp : cmdline->qp_max);
            if (error)
            {
                printf("ERROR: planner: %s\n", H264E_2pass_strerror(error));
                return 1;
            }
        }
        if (cmdline->pass1_file)
        {
            error = H264E_2pass_writer_open(&p1writer, cmdline->pass1_file,
                g_w, g_h, cmdline->gop);
            if (error)
            {
                printf("ERROR: pass1 stats %s: %s\n", cmdline->pass1_file,
                    H264E_2pass_strerror(error));
                return 1;
            }
        }

        error = H264E_sizeof(&create_param, &sizeof_persist, &sizeof_scratch);
        if (error)
        {
            printf("H264E_init error = %d\n", error);
            return 0;
        }
        printf("sizeof_persist = %d sizeof_scratch = %d\n", sizeof_persist, sizeof_scratch);
        enc     = (H264E_persist_t *)ALIGNED_ALLOC(64, sizeof_persist);
        scratch = (H264E_scratch_t *)ALIGNED_ALLOC(64, sizeof_scratch);
        error = H264E_init(enc, &create_param);

        if (p1writer)
        {
            H264E_mb_stats_hook_t hook;
            H264E_2pass_writer_hook(p1writer, &hook);
            H264E_set_mb_stats_hook(enc, &hook);
        }

        if (fin)
            fseek(fin, 0, SEEK_SET);

        for (i = 0; cmdline->max_frames; i++)
        {
            if (!fin)
            {
                if (i > 300) break;
                memset(buf_in + g_w*g_h, 128, g_w*g_h/2);
                gen_chessboard_rot(buf_in, g_w, g_h, i);
            } else
                if (!fread(buf_in, frame_size, 1, fin)) break;
            if (cmdline->psnr)
                memcpy(buf_save, buf_in, frame_size);

            yuv.yuv[0] = buf_in; yuv.stride[0] = g_w;
            yuv.yuv[1] = buf_in + g_w*g_h; yuv.stride[1] = g_w/2;
            yuv.yuv[2] = buf_in + g_w*g_h*5/4; yuv.stride[2] = g_w/2;

            run_param.frame_type = 0;
            run_param.encode_speed = cmdline->speed;
            //run_param.desired_nalu_bytes = 100;

            if (cmdline->kbps)
            {
                run_param.desired_frame_bytes = cmdline->kbps*1000/8/30;
                run_param.qp_min = 10;
                run_param.qp_max = 50;
            } else
            {
                run_param.qp_min = run_param.qp_max = cmdline->qp;
            }

            if (planner)
            {
                H264E_2pass_plan_t plan;
                const H264E_2pass_record_t *rec;
                int is_idr;

                error = H264E_2pass_planner_next(planner, &plan);
                if (error)
                {
                    printf("ERROR: pass2 frame %d: %s (statistics end after "
                           "%d frames)\n", i, H264E_2pass_strerror(error),
                           H264E_2pass_planner_frames(planner));
                    return 1;
                }
                // Before encoding: verify this frame is the one the record
                // describes, with the same size and the same I/P class.
                rec = H264E_2pass_file_record(p2file, plan.record_index);
                if (plan.record_index != i || !rec ||
                    rec->width != g_w || rec->height != g_h ||
                    rec->fnv1a != H264E_2pass_fnv1a_i420(&yuv, g_w, g_h))
                {
                    printf("ERROR: pass2 frame %d does not match record "
                           "(identity/size)\n", i);
                    return 1;
                }
                is_idr = !i || (cmdline->gop && !(i % cmdline->gop));
                if (rec->is_idr != is_idr)
                {
                    printf("ERROR: pass2 frame %d type mismatch: record %d, "
                           "expected %d\n", i, rec->is_idr, is_idr);
                    return 1;
                }
                // One fixed QP for the whole frame drives mode selection,
                // quantisation, reconstruction and deblocking in pass 2.
                run_param.qp_min = run_param.qp_max = plan.q;
                run_param.desired_frame_bytes = 0;
            }
            if (p1writer)
            {
                error = H264E_2pass_writer_begin(p1writer, i, &yuv);
                if (error)
                {
                    printf("ERROR: pass1 record: %s\n",
                        H264E_2pass_strerror(error));
                    return 1;
                }
            }

#if ENABLE_TEMPORAL_SCALABILITY
            {
            int level, logmod = 1;
            int j, mod = 1 << logmod;
            static int fresh[200] = {-1,-1,-1,-1};

            run_param.frame_type = H264E_FRAME_TYPE_CUSTOM;

            for (level = logmod; level && (~i & (mod >> level)); level--){}

            run_param.long_term_idx_update = level + 1;
            if (level == logmod && logmod > 0)
                run_param.long_term_idx_update = -1;
            if (level == logmod - 1 && logmod > 1)
                run_param.long_term_idx_update = 0;

            //if (run_param.long_term_idx_update > logmod) run_param.long_term_idx_update -= logmod+1;
            //run_param.long_term_idx_update = logmod - 0 - level;
            //if (run_param.long_term_idx_update > 0)
            //{
            //    run_param.long_term_idx_update = logmod - run_param.long_term_idx_update;
            //}
            run_param.long_term_idx_use    = fresh[level];
            for (j = level; j <= logmod; j++)
            {
                fresh[j] = run_param.long_term_idx_update;
            }
            if (!i)
            {
                run_param.long_term_idx_use = -1;
            }
            }
#endif
            error = H264E_encode(enc, scratch, &run_param, &yuv, &coded_data, &sizeof_coded_data);
            assert(!error);

            if (p1writer)
            {
                H264E_2pass_frame_stats_t stats;
                error = H264E_2pass_writer_end(p1writer, sizeof_coded_data,
                    H264E_2pass_au_is_idr(coded_data, sizeof_coded_data) == 1,
                    &stats);
                if (error)
                {
                    printf("ERROR: pass1 record: %s\n",
                        H264E_2pass_strerror(error));
                    return 1;
                }
                if (cmdline->stats)
                    printf("pass1 frame=%d bytes=%d c=%.3f h=%.3f idr=%d\n",
                        stats.input_index, sizeof_coded_data,
                        stats.c, stats.h, stats.is_idr);
            }
            if (planner)
            {
                H264E_2pass_diag_t diag;
                error = H264E_2pass_planner_commit(planner,
                    sizeof_coded_data);
                if (error || H264E_2pass_planner_diag(planner, &diag))
                {
                    printf("ERROR: pass2 commit: %s\n",
                        H264E_2pass_strerror(error));
                    return 1;
                }
                if (cmdline->stats)
                    printf("pass2 frame=%d q=%d pred=%.0f actual=%ld "
                           "left=%.0f a_i=%.4f a_p=%.4f sat=%d%s\n",
                        diag.record_index, diag.q,
                        diag.frame_predicted_bits, diag.actual_bits,
                        diag.remaining_bytes, diag.a_i, diag.a_p,
                        diag.saturated, diag.saturated ? " SATURATED" : "");
            }

            if (i)
            {
                sum_bytes += sizeof_coded_data - 4;
                if (min_bytes > sizeof_coded_data - 4) min_bytes = sizeof_coded_data - 4;
                if (max_bytes < sizeof_coded_data - 4) max_bytes = sizeof_coded_data - 4;
            }

            frames++;
            if (cmdline->stats)
                printf("frame=%d, bytes=%d\n", frames - 1, sizeof_coded_data);

            if (fout)
            {
                if (!fwrite(coded_data, sizeof_coded_data, 1, fout))
                {
                    printf("ERROR writing output file\n");
                    break;
                }
            }
            if (cmdline->psnr)
                psnr_add(buf_save, buf_in, g_w, g_h, sizeof_coded_data);
        }
        //fprintf(stderr, "%d avr = %6d  [%6d %6d]\n", qp, sum_bytes/299, min_bytes, max_bytes);

        if (cmdline->psnr)
            psnr_print(psnr_get());

        if (p1writer)
        {
            int recorded = 0;
            error = H264E_2pass_writer_close(p1writer, &recorded);
            if (error)
            {
                printf("ERROR: pass1 close: %s\n",
                    H264E_2pass_strerror(error));
                return 1;
            }
            printf("pass1 frames=%d stats=%s\n", recorded, cmdline->pass1_file);
            H264E_2pass_writer_destroy(p1writer);
            p1writer = NULL;
        }
        if (planner)
        {
            if (!H264E_2pass_planner_done(planner))
            {
                printf("ERROR: pass2 ended with %d unprocessed records\n",
                    H264E_2pass_planner_remaining(planner));
                return 1;
            }
            printf("pass2 target_bytes=%.0f\n", target_bytes);
            H264E_2pass_planner_close(planner);
            planner = NULL;
            if (p2file)
            {
                H264E_2pass_file_close(p2file);
                p2file = NULL;
            }
        }

        if (enc)
            free(enc);
        if (scratch)
            free(scratch);
    }
    free(buf_in);
    free(buf_save);

    if (fin)
        fclose(fin);
    if (fout)
        fclose(fout);
#if H264E_MAX_THREADS
    if (thread_pool)
    {
        h264e_thread_pool_close(thread_pool, cmdline->threads);
    }
#endif
    return 0;
}
