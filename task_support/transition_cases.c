/*
 * transition_cases.c - chained xfade / acrossfade regression, registered
 * through task_support/check.h.
 *
 * Three groups are registered:
 *   rational_timeline  - both edit lists end to end, PTS/sample axes and
 *                        analytic content checked against transition_oracle
 *   chunk_invariance   - upstream audio reblocked with asetnsamples 127 and
 *                        4096 (pad=0): identical pixels/PCM, continuous
 *                        clock, different packet grouping
 *   oracle_guards      - three legal-but-wrong artifacts (one transition
 *                        pixel changed, one stereo pair swapped, a real
 *                        remux carrying a global clock shift) must each be
 *                        rejected with the first difference located
 *
 * Only C and the system ffmpeg / ffprobe / transition_oracle binaries are
 * used.  No media samples, Python, browser or sound card are involved.
 */

#include "task_support/check.h"
#include "transition_spec.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BASELINE_CHUNK 1024

struct Program {
    char nut[256];
    char gray[256];
    char f32[256];
    char vpts[256];
    char apts[256];
    int  vtb_num, vtb_den;
    int  atb_num, atb_den;
    int  audio_packets;
};

static char g_work[256];

static long file_len(const char *path);

static const char *env_bin(const char *name)
{
    const char *v = getenv(name);
    return (v && *v) ? v : 0;
}

static int run_cmd(const char *fmt, ...)
{
    char cmd[8192];
    va_list ap;
    int rc;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    fprintf(stderr, "RUN %s\n", cmd);
    rc = system(cmd);
    if (rc == -1)
        return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static int ensure_workdir(void)
{
    const char *tmp = getenv("TMPDIR");
    char tmpl[320];

    if (g_work[0])
        return 0;
    snprintf(tmpl, sizeof(tmpl), "%s/transition-reg-XXXXXX",
             tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(tmpl))
        return -1;
    snprintf(g_work, sizeof(g_work), "%s", tmpl);
    fprintf(stderr, "WORKDIR %s\n", g_work);
    return 0;
}

/* Materialize the six raw source streams through the standalone generator
 * (transition_assets), which derives every byte from ts_source_*(). */
static int ensure_assets(const TransitionCase *c)
{
    const char *gen = env_bin("TRANSITION_ASSETS_BIN");
    char dir[320], logf[360];

    if (!gen)
        return -1;
    snprintf(dir, sizeof(dir), "%s/case%d", g_work, c->id);
    if (access(dir, F_OK) == 0)
        return 0;
    if (mkdir(dir, 0700) && errno != EEXIST)
        return -1;
    snprintf(logf, sizeof(logf), "%s/assets.log", dir);
    if (run_cmd("%s %d %s >%s 2>&1", gen, c->id, dir, logf))
        return -1;

    char path[512];
    const long vbytes = (long)c->frames_per_seg * TS_PLANE_SIZE;
    const long abytes = (long)c->audio_per_seg * TS_CHANNELS *
                        (long)sizeof(float);
    for (int seg = 0; seg < 3; seg++) {
        snprintf(path, sizeof(path), "%s/v%d.gray", dir, seg);
        if (file_len(path) != vbytes)
            return -1;
        snprintf(path, sizeof(path), "%s/a%d.f32", dir, seg);
        if (file_len(path) != abytes)
            return -1;
    }
    return 0;
}

static void graph_video(char *dst, size_t n, const TransitionCase *c)
{
    if (c->id == 25)
        snprintf(dst, n,
                 "[0:v][1:v]xfade=transition=fade:duration=0.2:offset=0.8[x1];"
                 "[x1][2:v]xfade=transition=fade:duration=0.2:offset=1.6[xv]");
    else
        /* 30000/1001: 10-frame fade, second offset is the finished-program
         * position (40 frames), not a re-zeroed offset from shot two. */
        snprintf(dst, n,
                 "[0:v][1:v]xfade=transition=fade:duration=333667us:"
                 "offset=667333us[x1];"
                 "[x1][2:v]xfade=transition=fade:duration=333667us:"
                 "offset=1334667us[xv]");
}

static void graph_audio(char *dst, size_t n, const TransitionCase *c,
                        int chunk)
{
    snprintf(dst, n,
             "[3:a]asetnsamples=%d:p=0[i0];"
             "[4:a]asetnsamples=%d:p=0[i1];"
             "[5:a]asetnsamples=%d:p=0[i2];"
             "[i0][i1]acrossfade=ns=%d:c1=tri:c2=qsin[m1];"
             "[m1][i2]acrossfade=ns=%d:c1=tri:c2=qsin[aa]",
             chunk, chunk, chunk,
             c->overlap_samples, c->overlap_samples);
}

static int parse_timebase(const char *path, int *num, int *den)
{
    FILE *f = fopen(path, "r");
    int ok = 0;
    if (f) {
        char line[64];
        if (fgets(line, sizeof(line), f))
            ok = sscanf(line, "%d/%d", num, den) == 2;
        fclose(f);
    }
    return ok ? 0 : -1;
}

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0;
    int ch;
    if (!f)
        return -1;
    while ((ch = fgetc(f)) != EOF)
        if (ch == '\n')
            n++;
    fclose(f);
    return n;
}

static int first_pts(const char *path, long long *pts, long long *dur)
{
    FILE *f = fopen(path, "r");
    int ok;
    if (!f)
        return -1;
    ok = fscanf(f, "%lld,%lld", pts, dur) == 2;
    fclose(f);
    return ok ? 0 : -1;
}

static long file_len(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f || fseek(f, 0, SEEK_END))
        return -1;
    n = ftell(f);
    if (f)
        fclose(f);
    return n;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    char buf[4096];
    size_t r;
    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, r, out) != r)
            return -1;
    fclose(in);
    fclose(out);
    return 0;
}

static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[4096], bb[4096];
    size_t ra, rb;
    int equal = 1;
    if (!fa || !fb)
        return 0;
    for (;;) {
        ra = fread(ba, 1, sizeof(ba), fa);
        rb = fread(bb, 1, sizeof(bb), fb);
        if (ra != rb || (ra && memcmp(ba, bb, ra))) {
            equal = 0;
            break;
        }
        if (ra == 0)
            break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return equal;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char buf[4096];
    size_t nl = strlen(needle);
    int found = 0;
    if (!f)
        return 0;
    while (fgets(buf, sizeof(buf), f))
        if (strstr(buf, needle) && nl) {
            found = 1;
            break;
        }
    fclose(f);
    return found;
}

/* Build one finished NUT plus decoded gray/f32 and ffprobe packet lists. */
static int build_program(const TransitionCase *c, int chunk, struct Program *p)
{
    const char *ff = env_bin("FFMPEG_BIN");
    const char *fp = env_bin("FFPROBE_BIN");
    char dir[320], vg[512], ag[512], rate[32], tag[32], err[320];

    memset(p, 0, sizeof(*p));
    if (!ff || !fp)
        return -1;
    if (ensure_workdir() || ensure_assets(c))
        return -1;

    snprintf(dir, sizeof(dir), "%s/case%d", g_work, c->id);
    snprintf(tag, sizeof(tag), "n%d", chunk);
    snprintf(p->nut,  sizeof(p->nut),  "%s/out_%s.nut",  dir, tag);
    snprintf(p->gray, sizeof(p->gray), "%s/out_%s.gray", dir, tag);
    snprintf(p->f32,  sizeof(p->f32),  "%s/out_%s.f32",  dir, tag);
    snprintf(p->vpts, sizeof(p->vpts), "%s/out_%s.vpts", dir, tag);
    snprintf(p->apts, sizeof(p->apts), "%s/out_%s.apts", dir, tag);
    snprintf(err, sizeof(err), "%s/out_%s.err", dir, tag);
    snprintf(rate, sizeof(rate), "%d/%d", c->fps_num, c->fps_den);
    graph_video(vg, sizeof(vg), c);
    graph_audio(ag, sizeof(ag), c, chunk);

    if (run_cmd("%s -v error -nostdin "
                "-f rawvideo -pix_fmt gray -s %dx%d -r %s -i %s/v0.gray "
                "-f rawvideo -pix_fmt gray -s %dx%d -r %s -i %s/v1.gray "
                "-f rawvideo -pix_fmt gray -s %dx%d -r %s -i %s/v2.gray "
                "-f f32le -ar %d -ac %d -i %s/a0.f32 "
                "-f f32le -ar %d -ac %d -i %s/a1.f32 "
                "-f f32le -ar %d -ac %d -i %s/a2.f32 "
                "-filter_complex '%s;%s' "
                "-map '[xv]' -map '[aa]' -c:v rawvideo -c:a pcm_f32le "
                "-fps_mode passthrough -y %s 2>%s",
                ff, TS_WIDTH, TS_HEIGHT, rate, dir,
                    TS_WIDTH, TS_HEIGHT, rate, dir,
                    TS_WIDTH, TS_HEIGHT, rate, dir,
                TS_SAMPLE_RATE, TS_CHANNELS, dir,
                TS_SAMPLE_RATE, TS_CHANNELS, dir,
                TS_SAMPLE_RATE, TS_CHANNELS, dir,
                vg, ag, p->nut, err))
        return -1;

    if (run_cmd("%s -v error -nostdin -i %s -map 0:v -c:v copy "
                "-f rawvideo -y %s 2>>%s", ff, p->nut, p->gray, err))
        return -1;
    if (run_cmd("%s -v error -nostdin -i %s -map 0:a -c:a copy "
                "-f f32le -y %s 2>>%s", ff, p->nut, p->f32, err))
        return -1;
    if (run_cmd("%s -v error -select_streams v -show_entries packet=pts,duration "
                "-of csv=p=0 %s >%s 2>>%s", fp, p->nut, p->vpts, err))
        return -1;
    if (run_cmd("%s -v error -select_streams a -show_entries packet=pts,duration "
                "-of csv=p=0 %s >%s 2>>%s", fp, p->nut, p->apts, err))
        return -1;

    char tbf[256], tba[256];
    snprintf(tbf, sizeof(tbf), "%s/tb_v.txt", dir);
    snprintf(tba, sizeof(tba), "%s/tb_a.txt", dir);
    if (run_cmd("%s -v error -select_streams v -show_entries stream=time_base "
                "-of csv=p=0 %s >%s 2>>%s", fp, p->nut, tbf, err))
        return -1;
    if (run_cmd("%s -v error -select_streams a -show_entries stream=time_base "
                "-of csv=p=0 %s >%s 2>>%s", fp, p->nut, tba, err))
        return -1;
    if (parse_timebase(tbf, &p->vtb_num, &p->vtb_den) ||
        parse_timebase(tba, &p->atb_num, &p->atb_den))
        return -1;

    p->audio_packets = count_lines(p->apts);
    return p->audio_packets > 0 ? 0 : -1;
}

static int oracle(const TransitionCase *c, const char *mode,
                  const struct Program *p, char *errpath, size_t errlen)
{
    const char *or = env_bin("TRANSITION_ORACLE_BIN");
    if (!or)
        return -1;
    snprintf(errpath, errlen, "%s/oracle_%s_%d.log", g_work, mode, c->id);
    if (!strcmp(mode, "content"))
        return run_cmd("%s check-content %d %s %s >%s 2>&1",
                       or, c->id, p->gray, p->f32, errpath);
    if (!strcmp(mode, "clock"))
        return run_cmd("%s check-clock %d %d %d %d %d %s %s >%s 2>&1",
                       or, c->id, p->vtb_num, p->vtb_den,
                       p->atb_num, p->atb_den, p->vpts, p->apts, errpath);
    return -1;
}

static int oracle_selftest(const TransitionCase *c, char *logpath, size_t len)
{
    const char *or = env_bin("TRANSITION_ORACLE_BIN");
    if (!or)
        return -1;
    snprintf(logpath, len, "%s/oracle_selftest_%d.log", g_work, c->id);
    return run_cmd("%s selftest %d >%s 2>&1", or, c->id, logpath);
}

static int case_rational(int id)
{
    const TransitionCase *c = ts_find(id);
    struct Program p;
    char err[320];

    TASK_CHECK(c != 0);
    TASK_CHECK(env_bin("FFMPEG_BIN") && env_bin("FFPROBE_BIN") &&
               env_bin("TRANSITION_ORACLE_BIN"));
    TASK_CHECK(build_program(c, BASELINE_CHUNK, &p) == 0);

    TASK_CHECK(file_len(p.gray) ==
               (long)c->total_frames * TS_PLANE_SIZE);
    TASK_CHECK(file_len(p.f32) ==
               (long)c->total_samples * TS_CHANNELS * (long)sizeof(float));
    TASK_CHECK(count_lines(p.vpts) == c->total_frames);
    TASK_CHECK(p.audio_packets >= 1);
    TASK_CHECK(p.atb_num == 1 && p.atb_den == TS_SAMPLE_RATE);

    TASK_CHECK(oracle(c, "content", &p, err, sizeof(err)) == 0);
    TASK_CHECK(file_contains(err, "ORACLE_CONTENT_OK"));
    TASK_CHECK(oracle(c, "clock", &p, err, sizeof(err)) == 0);
    TASK_CHECK(file_contains(err, "ORACLE_CLOCK_OK"));
    TASK_CHECK(oracle_selftest(c, err, sizeof(err)) == 0);
    TASK_CHECK(file_contains(err, "ORACLE_SELFTEST_OK"));
    return 0;
}

static int rational_timeline_25(void)    { return case_rational(25); }
static int rational_timeline_30000(void) { return case_rational(30); }

static int case_chunks(int id)
{
    const TransitionCase *c = ts_find(id);
    struct Program base, c127, c4096;
    char err[320];

    TASK_CHECK(c != 0);
    TASK_CHECK(build_program(c, BASELINE_CHUNK, &base) == 0);
    TASK_CHECK(build_program(c, 127, &c127) == 0);
    TASK_CHECK(build_program(c, 4096, &c4096) == 0);

    /* Decoded pixels and PCM must be identical whatever the blocking. */
    TASK_CHECK(files_equal(base.gray, c127.gray));
    TASK_CHECK(files_equal(base.gray, c4096.gray));
    TASK_CHECK(files_equal(base.f32, c127.f32));
    TASK_CHECK(files_equal(base.f32, c4096.f32));

    /* Packet grouping deliberately differs; continuity must not rely on it. */
    TASK_CHECK(c127.audio_packets > base.audio_packets);
    TASK_CHECK(c4096.audio_packets < base.audio_packets);

    TASK_CHECK(oracle(c, "content", &c127, err, sizeof(err)) == 0);
    TASK_CHECK(oracle(c, "content", &c4096, err, sizeof(err)) == 0);
    TASK_CHECK(oracle(c, "clock", &c127, err, sizeof(err)) == 0);
    TASK_CHECK(oracle(c, "clock", &c4096, err, sizeof(err)) == 0);
    return 0;
}

static int chunk_invariance_25(void)    { return case_chunks(25); }
static int chunk_invariance_30000(void) { return case_chunks(30); }

/* Change one pixel inside the first video transition of the 25 fps cut. */
static int guard_pixel(void)
{
    const TransitionCase *c = ts_case_25();
    struct Program p;
    char bad[320], err[320];
    const int frame = 22;                 /* inside the 5-frame xfade      */
    const long off = (long)frame * TS_PLANE_SIZE + 4 * TS_WIDTH + 3;
    FILE *f;
    int v;

    TASK_CHECK(build_program(c, BASELINE_CHUNK, &p) == 0);
    snprintf(bad, sizeof(bad), "%s/guard_pixel.gray", g_work);
    TASK_CHECK(copy_file(p.gray, bad) == 0);

    f = fopen(bad, "r+b");
    TASK_CHECK(f != 0);
    TASK_CHECK(fseek(f, off, SEEK_SET) == 0);
    v = fgetc(f);
    TASK_CHECK(v >= 0);
    TASK_CHECK(fseek(f, off, SEEK_SET) == 0);
    fputc(v >= 128 ? v - 23 : v + 23, f);
    fclose(f);

    struct Program tampered = p;
    snprintf(tampered.gray, sizeof(tampered.gray), "%s", bad);
    TASK_CHECK(oracle(c, "content", &tampered, err, sizeof(err)) == 1);
    TASK_CHECK(file_contains(err, "ORACLE_MISMATCH"));
    TASK_CHECK(file_contains(err, "kind=pixel"));
    TASK_CHECK(file_contains(err, "frame=22"));
    TASK_CHECK(file_contains(err, "x=3 y=4"));

    /* The untouched program still passes: the guard is content-specific. */
    TASK_CHECK(oracle(c, "content", &p, err, sizeof(err)) == 0);
    return 0;
}

/* Swap the left/right samples of one frame inside the second crossfade. */
static int guard_channel_swap(void)
{
    const TransitionCase *c = ts_case_25();
    struct Program p;
    char bad[320], err[320];
    const long sample = 80000;           /* second crossfade [76800,86400) */
    const long off = sample * TS_CHANNELS * (long)sizeof(float);
    float pair[2], tmp;
    FILE *f;

    TASK_CHECK(build_program(c, BASELINE_CHUNK, &p) == 0);
    snprintf(bad, sizeof(bad), "%s/guard_swap.f32", g_work);
    TASK_CHECK(copy_file(p.f32, bad) == 0);

    f = fopen(bad, "r+b");
    TASK_CHECK(f != 0);
    TASK_CHECK(fseek(f, off, SEEK_SET) == 0);
    TASK_CHECK(fread(pair, sizeof(float), 2, f) == 2);
    /* The two channels must carry different markers at this point. */
    TASK_CHECK(pair[0] != pair[1]);
    tmp = pair[0]; pair[0] = pair[1]; pair[1] = tmp;
    TASK_CHECK(fseek(f, off, SEEK_SET) == 0);
    TASK_CHECK(fwrite(pair, sizeof(float), 2, f) == 2);
    fclose(f);

    struct Program tampered = p;
    snprintf(tampered.f32, sizeof(tampered.f32), "%s", bad);
    TASK_CHECK(oracle(c, "content", &tampered, err, sizeof(err)) == 1);
    TASK_CHECK(file_contains(err, "ORACLE_MISMATCH"));
    TASK_CHECK(file_contains(err, "kind=pcm"));
    TASK_CHECK(file_contains(err, "sample=80000"));
    TASK_CHECK(file_contains(err, "channel="));

    TASK_CHECK(oracle(c, "content", &p, err, sizeof(err)) == 0);
    return 0;
}

/* A genuine remux that moves the whole presentation clock. */
static int guard_clock_shift(void)
{
    const TransitionCase *c = ts_case_25();
    struct Program p;
    char shifted_nut[320], shifted_gray[320], shifted_f32[320];
    char shifted_vpts[320], shifted_apts[320], err[320], logf[320];
    struct Program shifted;
    long long pts0, dur0;
    const char *ff = env_bin("FFMPEG_BIN");
    const char *fp = env_bin("FFPROBE_BIN");

    TASK_CHECK(ff && fp);
    TASK_CHECK(build_program(c, BASELINE_CHUNK, &p) == 0);

    snprintf(shifted_nut,  sizeof(shifted_nut),  "%s/shifted.nut", g_work);
    snprintf(shifted_gray, sizeof(shifted_gray), "%s/shifted.gray", g_work);
    snprintf(shifted_f32,  sizeof(shifted_f32),  "%s/shifted.f32", g_work);
    snprintf(shifted_vpts, sizeof(shifted_vpts), "%s/shifted.vpts", g_work);
    snprintf(shifted_apts, sizeof(shifted_apts), "%s/shifted.apts", g_work);
    snprintf(logf, sizeof(logf), "%s/shifted.log", g_work);

    TASK_CHECK(run_cmd("%s -v error -nostdin -i %s -c copy "
                       "-output_ts_offset 0.007 -y %s 2>%s",
                       ff, p.nut, shifted_nut, logf) == 0);
    TASK_CHECK(run_cmd("%s -v error -nostdin -i %s -map 0:v -c:v copy "
                       "-f rawvideo -y %s 2>>%s",
                       ff, shifted_nut, shifted_gray, logf) == 0);
    TASK_CHECK(run_cmd("%s -v error -nostdin -i %s -map 0:a -c:a copy "
                       "-f f32le -y %s 2>>%s",
                       ff, shifted_nut, shifted_f32, logf) == 0);
    TASK_CHECK(run_cmd("%s -v error -select_streams v -show_entries "
                       "packet=pts,duration -of csv=p=0 %s >%s 2>>%s",
                       fp, shifted_nut, shifted_vpts, logf) == 0);
    TASK_CHECK(run_cmd("%s -v error -select_streams a -show_entries "
                       "packet=pts,duration -of csv=p=0 %s >%s 2>>%s",
                       fp, shifted_nut, shifted_apts, logf) == 0);

    TASK_CHECK(first_pts(shifted_vpts, &pts0, &dur0) == 0);
    TASK_CHECK(pts0 != 0);                 /* clock really moved */

    shifted = p;
    snprintf(shifted.vpts, sizeof(shifted.vpts), "%s", shifted_vpts);
    snprintf(shifted.apts, sizeof(shifted.apts), "%s", shifted_apts);
    TASK_CHECK(oracle(c, "clock", &shifted, err, sizeof(err)) == 1);
    TASK_CHECK(file_contains(err, "ORACLE_MISMATCH"));
    TASK_CHECK(file_contains(err, "kind=clock"));
    TASK_CHECK(file_contains(err, "packet=0"));

    /* Content is untouched, so the content check must still pass: the
     * failure above is purely a clock difference. */
    shifted = p;
    snprintf(shifted.gray, sizeof(shifted.gray), "%s", shifted_gray);
    snprintf(shifted.f32,  sizeof(shifted.f32),  "%s", shifted_f32);
    TASK_CHECK(oracle(c, "content", &shifted, err, sizeof(err)) == 0);
    return 0;
}

const struct task_case task_cases[] = {
    { "rational_timeline", "r25fps",        rational_timeline_25    },
    { "rational_timeline", "r30000-1001",   rational_timeline_30000 },
    { "chunk_invariance",  "r25fps",        chunk_invariance_25     },
    { "chunk_invariance",  "r30000-1001",   chunk_invariance_30000  },
    { "oracle_guards",     "pixel",         guard_pixel             },
    { "oracle_guards",     "channel_swap",  guard_channel_swap      },
    { "oracle_guards",     "clock_shift",   guard_clock_shift       },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
