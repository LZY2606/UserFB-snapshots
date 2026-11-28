/*
 * transition_cases: independent regression for the chained xfade /
 * acrossfade A/V transition pipeline.
 *
 * Three registered groups:
 *   rational_timeline  - both edit lists end to end through real NUT
 *   chunk_invariance   - asetnsamples 127 and 4096 repacking
 *   oracle_guards      - pixel / channel / clock negative artifacts
 *
 * Every external tool is a real ffmpeg / ffprobe / transition_oracle
 * invocation located through FFMPEG_BIN / FFPROBE_BIN /
 * TRANSITION_ORACLE_BIN.  No media samples are downloaded and no Python,
 * browser or sound card is involved: all source material is synthesized by
 * the C generator and moved through rawvideo / pcm_f32le / NUT.
 */
#include "check.h"
#include "transition_spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* environment and small file utilities                               */
/* ------------------------------------------------------------------ */

#define MAX_ARGS 64
#define LOG_LIMIT 8192

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static const char *bin_ffmpeg(void)
{
    return env_or("FFMPEG_BIN", ".task-build/ffmpeg/ffmpeg");
}

static const char *bin_ffprobe(void)
{
    return env_or("FFPROBE_BIN", ".task-build/ffmpeg/ffprobe");
}

static const char *bin_oracle(void)
{
    return env_or("TRANSITION_ORACLE_BIN", ".task-build/transition_oracle");
}

static const char *bin_gen(void)
{
    return env_or("TRANSITION_GEN_BIN", ".task-build/transition_gen");
}

static const char *work_root(void)
{
    return env_or("TASK_WORKDIR", ".task-build/target-work");
}

static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i]; ++i) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST)
            return 1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) && errno != EEXIST)
        return 1;
    return 0;
}

static int spec_dir(const struct transition_spec *s, char *out, size_t n)
{
    int rc = snprintf(out, n, "%s/%s", work_root(), s->id);
    if (rc < 0 || (size_t)rc >= n)
        return 1;
    return mkdir_p(out);
}

static int run_argv(char *const argv[], const char *logpath)
{
    pid_t pid;
    int status, fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

static int run_capture(char *const argv[], const char *logpath)
{
    int rc = run_argv(argv, logpath);
    if (rc != 0) {
        char buf[LOG_LIMIT];
        FILE *fp = fopen(logpath, "rb");
        size_t n = 0;
        fprintf(stderr, "CMD_FAIL rc=%d argv0=%s log=%s\n",
                rc, argv[0], logpath);
        if (fp) {
            n = fread(buf, 1, sizeof(buf) - 1, fp);
            buf[n] = 0;
            fputs(buf, stderr);
            fclose(fp);
        }
    }
    return rc;
}

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *fp = fopen(path, "rb");
    size_t n;

    if (!fp)
        return -1;
    n = fread(buf, 1, cap - 1, fp);
    buf[n] = 0;
    fclose(fp);
    return (int)n;
}

static int file_contains(const char *path, const char *needle)
{
    char buf[LOG_LIMIT * 2];

    if (read_file(path, buf, sizeof(buf)) < 0)
        return 0;
    return strstr(buf, needle) != NULL;
}

static long file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st))
        return -1;
    return (long)st.st_size;
}

static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int equal = 0;

    if (!fa || !fb)
        goto out;
    for (;;) {
        char ba[4096], bb[4096];
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || (na && memcmp(ba, bb, na)))
            break;
        if (na == 0) {
            equal = 1;
            break;
        }
    }
out:
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return equal;
}

/* concatenate the three per-segment files into one single-demuxer input */
static int concat_three(const char *out, const char *a, const char *b,
                        const char *c)
{
    const char *parts[3] = { a, b, c };
    FILE *fo = fopen(out, "wb");
    int i;

    if (!fo)
        return 1;
    for (i = 0; i < 3; ++i) {
        FILE *fp = fopen(parts[i], "rb");
        char buf[4096];
        size_t n;

        if (!fp) {
            fclose(fo);
            return 1;
        }
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
            if (fwrite(buf, 1, n, fo) != n) {
                fclose(fp); fclose(fo);
                return 1;
            }
        fclose(fp);
    }
    if (fclose(fo))
        return 1;
    return 0;
}

static void join2(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

/* ------------------------------------------------------------------ */
/* pipeline construction                                              */
/* ------------------------------------------------------------------ */

/* microsecond duration/offset, integer math on the rational rate */
static long us_per_frames(const struct transition_spec *s, int frames)
{
    return (long)frames * s->rate_den * 1000000L / s->rate_num;
}

static int generate_sources(const struct transition_spec *s, const char *dir)
{
    char *argv[] = { (char *)bin_gen(), (char *)dir, (char *)s->id, NULL };
    char log[1024];
    int i;

    join2(log, sizeof(log), dir, "gen.log");
    if (run_capture(argv, log))
        return 1;
    for (i = 0; i < TASK_SEGMENTS; ++i) {
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s_v%d.gray", dir, s->id, i);
        if (file_size(p) != (long)(s->frames_per_seg * TASK_PIXELS))
            return 1;
        snprintf(p, sizeof(p), "%s/%s_a%d.f32", dir, s->id, i);
        if (file_size(p) != (long)s->samples_per_seg * TASK_CHANNELS
                            * (long)sizeof(float))
            return 1;
    }
    return 0;
}

/* video: three demuxers feeding two chained xfades (deterministic) */
static int build_video_stage(const struct transition_spec *s, const char *dir)
{
    char rate[64], in0[1024], in1[1024], in2[1024], out[1024], log[1024];
    char graph[2048];
    long dur = us_per_frames(s, s->overlap_frames);
    long off1 = us_per_frames(s, s->frames_per_seg - s->overlap_frames);
    long off2 = us_per_frames(s, 2 * (s->frames_per_seg - s->overlap_frames));
    char *argv[MAX_ARGS];
    int k = 0;

    snprintf(rate, sizeof(rate), "%d/%d", s->rate_num, s->rate_den);
    snprintf(in0, sizeof(in0), "%s/%s_v0.gray", dir, s->id);
    snprintf(in1, sizeof(in1), "%s/%s_v1.gray", dir, s->id);
    snprintf(in2, sizeof(in2), "%s/%s_v2.gray", dir, s->id);
    join2(out, sizeof(out), dir, "stage.gray");
    join2(log, sizeof(log), dir, "video.log");
    snprintf(graph, sizeof(graph),
             "[0:v][1:v]xfade=transition=fade:duration=%ldus:offset=%ldus,"
             "format=gray[x1];[x1][2:v]xfade=transition=fade:duration=%ldus:"
             "offset=%ldus,format=gray[xv]",
             dur, off1, dur, off2);
    argv[k++] = (char *)bin_ffmpeg();
    argv[k++] = (char *)"-v"; argv[k++] = (char *)"error";
    argv[k++] = (char *)"-nostdin";
    argv[k++] = (char *)"-filter_complex_threads"; argv[k++] = (char *)"1";
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"rawvideo";
    argv[k++] = (char *)"-pix_fmt"; argv[k++] = (char *)"gray";
    argv[k++] = (char *)"-s"; argv[k++] = (char *)"8x8";
    argv[k++] = (char *)"-framerate"; argv[k++] = rate;
    argv[k++] = (char *)"-i"; argv[k++] = in0;
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"rawvideo";
    argv[k++] = (char *)"-pix_fmt"; argv[k++] = (char *)"gray";
    argv[k++] = (char *)"-s"; argv[k++] = (char *)"8x8";
    argv[k++] = (char *)"-framerate"; argv[k++] = rate;
    argv[k++] = (char *)"-i"; argv[k++] = in1;
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"rawvideo";
    argv[k++] = (char *)"-pix_fmt"; argv[k++] = (char *)"gray";
    argv[k++] = (char *)"-s"; argv[k++] = (char *)"8x8";
    argv[k++] = (char *)"-framerate"; argv[k++] = rate;
    argv[k++] = (char *)"-i"; argv[k++] = in2;
    argv[k++] = (char *)"-filter_complex"; argv[k++] = graph;
    argv[k++] = (char *)"-map"; argv[k++] = (char *)"[xv]";
    argv[k++] = (char *)"-c:v"; argv[k++] = (char *)"rawvideo";
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"rawvideo";
    argv[k++] = (char *)"-y"; argv[k++] = out;
    argv[k] = NULL;
    return run_capture(argv, log);
}

/*
 * Audio: one demuxer on the concatenated clip, split into three atrim
 * branches then two chained acrossfades.  A single demuxer avoids the
 * cross-thread EOF race that truncates chained acrossfade with multiple
 * demuxers.  chunk_samples > 0 inserts asetnsamples=p=0 on every branch.
 */
static int build_audio_stage(const struct transition_spec *s, const char *dir,
                             int chunk_samples, const char *out_name)
{
    char in[1024], out[1024], log[1024], graph[4096], ns[64], chunk[96];
    const long A = s->samples_per_seg;
    char *argv[MAX_ARGS];
    int k = 0;

    join2(in, sizeof(in), dir, "all_a.f32");
    join2(out, sizeof(out), dir, out_name);
    snprintf(log, sizeof(log), "%s/audio_%s.log", dir, out_name);
    chunk[0] = 0;
    if (chunk_samples > 0)
        snprintf(chunk, sizeof(chunk), ",asetnsamples=n=%d:p=0",
                 chunk_samples);
    snprintf(ns, sizeof(ns), "ns=%ld", (long)s->overlap_samples);
    snprintf(graph, sizeof(graph),
             "asplit=3[a0][a1][a2];"
             "[a0]atrim=end_sample=%ld,asetpts=N/SR/TB%s[b0];"
             "[a1]atrim=start_sample=%ld:end_sample=%ld,asetpts=N/SR/TB%s[b1];"
             "[a2]atrim=start_sample=%ld:end_sample=%ld,asetpts=N/SR/TB%s[b2];"
             "[b0][b1]acrossfade=%s:c1=tri:c2=qsin[ax1];"
             "[ax1][b2]acrossfade=%s:c1=tri:c2=qsin[aa]",
             A, chunk, A, 2 * A, chunk, 2 * A, 3 * A, chunk, ns, ns);
    argv[k++] = (char *)bin_ffmpeg();
    argv[k++] = (char *)"-v"; argv[k++] = (char *)"error";
    argv[k++] = (char *)"-nostdin";
    argv[k++] = (char *)"-filter_complex_threads"; argv[k++] = (char *)"1";
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"f32le";
    argv[k++] = (char *)"-ar"; argv[k++] = (char *)"48000";
    argv[k++] = (char *)"-ac"; argv[k++] = (char *)"2";
    argv[k++] = (char *)"-i"; argv[k++] = in;
    argv[k++] = (char *)"-filter_complex"; argv[k++] = graph;
    argv[k++] = (char *)"-map"; argv[k++] = (char *)"[aa]";
    argv[k++] = (char *)"-c:a"; argv[k++] = (char *)"pcm_f32le";
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"f32le";
    argv[k++] = (char *)"-y"; argv[k++] = out;
    argv[k] = NULL;
    return run_capture(argv, log);
}

static int mux_nut(const struct transition_spec *s, const char *dir,
                   const char *audio_name, const char *nut_name)
{
    char rate[64], vin[1024], ain[1024], out[1024], log[1024];
    char *argv[MAX_ARGS];
    int k = 0;

    snprintf(rate, sizeof(rate), "%d/%d", s->rate_num, s->rate_den);
    join2(vin, sizeof(vin), dir, "stage.gray");
    join2(ain, sizeof(ain), dir, audio_name);
    join2(out, sizeof(out), dir, nut_name);
    snprintf(log, sizeof(log), "%s/mux_%s.log", dir, nut_name);
    argv[k++] = (char *)bin_ffmpeg();
    argv[k++] = (char *)"-v"; argv[k++] = (char *)"error";
    argv[k++] = (char *)"-nostdin";
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"rawvideo";
    argv[k++] = (char *)"-pix_fmt"; argv[k++] = (char *)"gray";
    argv[k++] = (char *)"-s"; argv[k++] = (char *)"8x8";
    argv[k++] = (char *)"-framerate"; argv[k++] = rate;
    argv[k++] = (char *)"-i"; argv[k++] = vin;
    argv[k++] = (char *)"-f"; argv[k++] = (char *)"f32le";
    argv[k++] = (char *)"-ar"; argv[k++] = (char *)"48000";
    argv[k++] = (char *)"-ac"; argv[k++] = (char *)"2";
    argv[k++] = (char *)"-i"; argv[k++] = ain;
    argv[k++] = (char *)"-map"; argv[k++] = (char *)"0:v";
    argv[k++] = (char *)"-map"; argv[k++] = (char *)"1:a";
    argv[k++] = (char *)"-c"; argv[k++] = (char *)"copy";
    argv[k++] = (char *)"-y"; argv[k++] = out;
    argv[k] = NULL;
    return run_capture(argv, log);
}

static int probe_nut(const char *nut, const char *probe_out)
{
    char *argv[] = {
        (char *)bin_ffprobe(), (char *)"-v", (char *)"error",
        (char *)"-show_streams", (char *)"-show_packets",
        (char *)"-of", (char *)"default", (char *)nut, NULL
    };
    return run_capture(argv, probe_out);
}

static int decode_nut(const char *nut, const char *gray, const char *f32,
                      const char *logpath)
{
    char *argv_v[] = {
        (char *)bin_ffmpeg(), (char *)"-v", (char *)"error",
        (char *)"-nostdin", (char *)"-i", (char *)nut,
        (char *)"-map", (char *)"0:v", (char *)"-c:v", (char *)"copy",
        (char * )"-f", (char *)"rawvideo", (char *)"-y", (char *)gray, NULL
    };
    char *argv_a[] = {
        (char *)bin_ffmpeg(), (char *)"-v", (char *)"error",
        (char *)"-nostdin", (char *)"-i", (char *)nut,
        (char *)"-map", (char *)"0:a", (char *)"-c:a", (char *)"copy",
        (char *)"-f", (char *)"f32le", (char *)"-y", (char *)f32, NULL
    };
    if (run_capture(argv_v, logpath))
        return 1;
    return run_capture(argv_a, logpath);
}

static int oracle_validate(const struct transition_spec *s, const char *gray,
                           const char *f32, const char *probe,
                           const char *logpath)
{
    char *argv[] = {
        (char *)bin_oracle(), (char *)"validate", (char *)s->id,
        (char *)"tri", (char *)"qsin",
        (char *)gray, (char *)f32, (char *)probe, NULL
    };
    return run_capture(argv, logpath);
}

static int oracle_mutate_pixel(const struct transition_spec *s,
                               const char *in, const char *out,
                               const char *logpath)
{
    char *argv[] = {
        (char *)bin_oracle(), (char *)"mutate-pixel", (char *)s->id,
        (char *)in, (char *)out, NULL
    };
    return run_capture(argv, logpath);
}

static int oracle_mutate_audio(const struct transition_spec *s,
                               const char *in, const char *out,
                               const char *logpath)
{
    char *argv[] = {
        (char *)bin_oracle(), (char *)"mutate-audio", (char *)s->id,
        (char *)in, (char *)out, NULL
    };
    return run_capture(argv, logpath);
}

/* Build the canonical (unchunked) NUT plus decoded gray/f32 and probe. */
static int build_canonical(const struct transition_spec *s, const char *dir)
{
    char all[1024], a0[1024], a1[1024], a2[1024];
    char nut[1024], probe[1024], gray[1024], f32[1024], log[1024];

    join2(all, sizeof(all), dir, "all_a.f32");
    snprintf(a0, sizeof(a0), "%s/%s_a0.f32", dir, s->id);
    snprintf(a1, sizeof(a1), "%s/%s_a1.f32", dir, s->id);
    snprintf(a2, sizeof(a2), "%s/%s_a2.f32", dir, s->id);
    join2(nut, sizeof(nut), dir, "main.nut");
    join2(probe, sizeof(probe), dir, "main.probe");
    join2(gray, sizeof(gray), dir, "main.gray");
    join2(f32, sizeof(f32), dir, "main.f32");
    join2(log, sizeof(log), dir, "oracle.log");

    if (concat_three(all, a0, a1, a2))
        return 1;
    if (build_video_stage(s, dir))
        return 1;
    if (build_audio_stage(s, dir, 0, "stage.f32"))
        return 1;
    if (mux_nut(s, dir, "stage.f32", "main.nut"))
        return 1;
    if (probe_nut(nut, probe))
        return 1;
    if (decode_nut(nut, gray, f32, log))
        return 1;
    if (file_size(gray) !=
        (long)transition_output_frames(s) * TASK_PIXELS)
        return 1;
    if (file_size(f32) !=
        transition_output_samples(s) * TASK_CHANNELS * (long)sizeof(float))
        return 1;
    return oracle_validate(s, gray, f32, probe, log);
}

/* ------------------------------------------------------------------ */
/* group 1: rational timeline, both edit lists                        */
/* ------------------------------------------------------------------ */

static int case_timeline_r25(void)
{
    const struct transition_spec *s = &transition_specs[0];
    char dir[1024], log[1024], gray[1024], f32[1024];

    TASK_CHECK(spec_dir(s, dir, sizeof(dir)) == 0);
    TASK_CHECK(generate_sources(s, dir) == 0);
    TASK_CHECK(build_canonical(s, dir) == 0);
    TASK_CHECK(transition_output_frames(s) == 65);
    TASK_CHECK(transition_output_samples(s) == 124800L);
    join2(gray, sizeof(gray), dir, "main.gray");
    join2(f32, sizeof(f32), dir, "main.f32");
    TASK_CHECK(file_size(gray) == 65L * TASK_PIXELS);
    TASK_CHECK(file_size(f32) == 124800L * TASK_CHANNELS
                               * (long)sizeof(float));
    join2(log, sizeof(log), dir, "oracle.log");
    TASK_CHECK(file_contains(log, "ORACLE_OK id=r25"));
    return 0;
}

static int case_timeline_ntsc(void)
{
    const struct transition_spec *s = &transition_specs[1];
    char dir[1024], log[1024], gray[1024], f32[1024];

    TASK_CHECK(spec_dir(s, dir, sizeof(dir)) == 0);
    TASK_CHECK(generate_sources(s, dir) == 0);
    TASK_CHECK(build_canonical(s, dir) == 0);
    TASK_CHECK(transition_output_frames(s) == 70);
    TASK_CHECK(transition_output_samples(s) == 112112L);
    join2(gray, sizeof(gray), dir, "main.gray");
    join2(f32, sizeof(f32), dir, "main.f32");
    TASK_CHECK(file_size(gray) == 70L * TASK_PIXELS);
    TASK_CHECK(file_size(f32) == 112112L * TASK_CHANNELS
                               * (long)sizeof(float));
    join2(log, sizeof(log), dir, "oracle.log");
    TASK_CHECK(file_contains(log, "ORACLE_OK id=ntsc"));
    return 0;
}

/* ------------------------------------------------------------------ */
/* group 2: asetnsamples chunk invariance                             */
/* ------------------------------------------------------------------ */

static int build_chunked(const struct transition_spec *s, const char *dir,
                         int chunk)
{
    char audio[64], nut[64], probe[64], gray[64], f32[64], log[64];
    char p_nut[1024], p_probe[1024], p_gray[1024], p_f32[1024], p_log[1024];

    snprintf(audio, sizeof(audio), "chunk%d.f32", chunk);
    snprintf(nut, sizeof(nut), "chunk%d.nut", chunk);
    snprintf(probe, sizeof(probe), "chunk%d.probe", chunk);
    snprintf(gray, sizeof(gray), "chunk%d.gray", chunk);
    snprintf(f32, sizeof(f32), "chunk%d.out.f32", chunk);
    snprintf(log, sizeof(log), "chunk%d.log", chunk);
    join2(p_nut, sizeof(p_nut), dir, nut);
    join2(p_probe, sizeof(p_probe), dir, probe);
    join2(p_gray, sizeof(p_gray), dir, gray);
    join2(p_f32, sizeof(p_f32), dir, f32);
    join2(p_log, sizeof(p_log), dir, log);

    if (build_audio_stage(s, dir, chunk, audio))
        return 1;
    if (mux_nut(s, dir, audio, nut))
        return 1;
    if (probe_nut(p_nut, p_probe))
        return 1;
    if (decode_nut(p_nut, p_gray, p_f32, p_log))
        return 1;
    /* clocks must stay continuous under the new packet grouping */
    if (oracle_validate(s, p_gray, p_f32, p_probe, p_log))
        return 1;
    return 0;
}

static int case_chunks_r25(void)
{
    const struct transition_spec *s = &transition_specs[0];
    char dir[1024], main_gray[1024], main_f32[1024];
    int chunks[2] = { 127, 4096 };
    int i;

    TASK_CHECK(spec_dir(s, dir, sizeof(dir)) == 0);
    TASK_CHECK(generate_sources(s, dir) == 0);
    TASK_CHECK(build_canonical(s, dir) == 0);
    join2(main_gray, sizeof(main_gray), dir, "main.gray");
    join2(main_f32, sizeof(main_f32), dir, "main.f32");
    for (i = 0; i < 2; ++i) {
        int n = chunks[i];
        char c_gray[1024], c_f32[1024];
        TASK_CHECK(build_chunked(s, dir, n) == 0);
        snprintf(c_gray, sizeof(c_gray), "%s/chunk%d.gray", dir, n);
        snprintf(c_f32, sizeof(c_f32), "%s/chunk%d.out.f32", dir, n);
        TASK_CHECK(files_equal(main_gray, c_gray));
        TASK_CHECK(files_equal(main_f32, c_f32));
    }
    return 0;
}

static int case_chunks_ntsc(void)
{
    const struct transition_spec *s = &transition_specs[1];
    char dir[1024], main_gray[1024], main_f32[1024];
    int chunks[2] = { 127, 4096 };
    int i;

    TASK_CHECK(spec_dir(s, dir, sizeof(dir)) == 0);
    TASK_CHECK(generate_sources(s, dir) == 0);
    TASK_CHECK(build_canonical(s, dir) == 0);
    join2(main_gray, sizeof(main_gray), dir, "main.gray");
    join2(main_f32, sizeof(main_f32), dir, "main.f32");
    for (i = 0; i < 2; ++i) {
        int n = chunks[i];
        char c_gray[1024], c_f32[1024];
        TASK_CHECK(build_chunked(s, dir, n) == 0);
        snprintf(c_gray, sizeof(c_gray), "%s/chunk%d.gray", dir, n);
        snprintf(c_f32, sizeof(c_f32), "%s/chunk%d.out.f32", dir, n);
        TASK_CHECK(files_equal(main_gray, c_gray));
        TASK_CHECK(files_equal(main_f32, c_f32));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* group 3: oracle guards (legal-format, legal-length negatives)      */
/* ------------------------------------------------------------------ */

/* remux a (possibly mutated) gray/f32 pair into a real NUT and decode it */
static int remux_and_decode(const struct transition_spec *s, const char *dir,
                            const char *gray_in, const char *f32_in,
                            const char *tag)
{
    char rate[64], nut[1024], probe[1024], gray[1024], f32[1024], log[1024];
    char *argv_mux[MAX_ARGS];
    int k = 0;

    snprintf(rate, sizeof(rate), "%d/%d", s->rate_num, s->rate_den);
    snprintf(nut, sizeof(nut), "%s/%s.nut", dir, tag);
    snprintf(probe, sizeof(probe), "%s/%s.probe", dir, tag);
    snprintf(gray, sizeof(gray), "%s/%s.gray", dir, tag);
    snprintf(f32, sizeof(f32), "%s/%s.f32", dir, tag);
    snprintf(log, sizeof(log), "%s/%s.log", dir, tag);

    argv_mux[k++] = (char *)bin_ffmpeg();
    argv_mux[k++] = (char *)"-v"; argv_mux[k++] = (char *)"error";
    argv_mux[k++] = (char *)"-nostdin";
    argv_mux[k++] = (char *)"-f"; argv_mux[k++] = (char *)"rawvideo";
    argv_mux[k++] = (char *)"-pix_fmt"; argv_mux[k++] = (char *)"gray";
    argv_mux[k++] = (char *)"-s"; argv_mux[k++] = (char *)"8x8";
    argv_mux[k++] = (char *)"-framerate"; argv_mux[k++] = rate;
    argv_mux[k++] = (char *)"-i"; argv_mux[k++] = (char *)gray_in;
    argv_mux[k++] = (char *)"-f"; argv_mux[k++] = (char *)"f32le";
    argv_mux[k++] = (char *)"-ar"; argv_mux[k++] = (char *)"48000";
    argv_mux[k++] = (char * )"-ac"; argv_mux[k++] = (char *)"2";
    argv_mux[k++] = (char *)"-i"; argv_mux[k++] = (char *)f32_in;
    argv_mux[k++] = (char *)"-map"; argv_mux[k++] = (char *)"0:v";
    argv_mux[k++] = (char *)"-map"; argv_mux[k++] = (char *)"1:a";
    argv_mux[k++] = (char *)"-c"; argv_mux[k++] = (char *)"copy";
    argv_mux[k++] = (char *)"-y"; argv_mux[k++] = nut;
    argv_mux[k] = NULL;
    if (run_capture(argv_mux, log))
        return 1;
    if (probe_nut(nut, probe))
        return 1;
    if (decode_nut(nut, gray, f32, log))
        return 1;
    if (oracle_validate(s, gray, f32, probe, log) == 0)
        return 1; /* a corrupted artifact must NOT validate */
    return 0;
}

static int case_guard_pixel(void)
{
    const struct transition_spec *s = &transition_specs[0];
    char dir[1024], main_gray[1024], main_f32[1024], bad_gray[1024], log[1024];

    TASK_CHECK(spec_dir(s, dir, sizeof(dir)) == 0);
    TASK_CHECK(generate_sources(s, dir) == 0);
    TASK_CHECK(build_canonical(s, dir) == 0);
    join2(main_gray, sizeof(main_gray), dir, "main.gray");
    join2(main_f32, sizeof(main_f32), dir, "main.f32");
    snprintf(bad_gray, sizeof(bad_gray), "%s/bad_pixel.gray", dir);
    join2(log, sizeof(log), dir, "mutate_pixel.log");
    TASK_CHECK(oracle_mutate_pixel(s, main_gray, bad_gray, log) == 0);
    /* same byte length, legal format, one pixel changed */
    TASK_CHECK(file_size(bad_gray) == file_size(main_gray));
    TASK_CHECK(remux_and_decode(s, dir, bad_gray, main_f32, "guard_pixel") == 0);
    TASK_CHECK(file_contains(log, "MUTATE_PIXEL"));
    {
        char glog[1024];
        join2(glog, sizeof(glog), dir, "guard_pixel.log");
        TASK_CHECK(file_contains(glog, "ORACLE_MISMATCH video"));
    }
    return 0;
}

static int case_guard_audio(void)
{
    const struct transition_spec *s = &transition_specs[0];
    char dir[1024], main_gray[1024], main_f32[1024], bad_f32[1024], log[1024];

    TASK_CHECK(spec_dir(s, dir, sizeof(dir)) == 0);
    TASK_CHECK(generate_sources(s, dir) == 0);
    TASK_CHECK(build_canonical(s, dir) == 0);
    join2(main_gray, sizeof(main_gray), dir, "main.gray");
    join2(main_f32, sizeof(main_f32), dir, "main.f32");
    snprintf(bad_f32, sizeof(bad_f32), "%s/bad_audio.f32", dir);
    join2(log, sizeof(log), dir, "mutate_audio.log");
    TASK_CHECK(oracle_mutate_audio(s, main_f32, bad_f32, log) == 0);
    TASK_CHECK(file_size(bad_f32) == file_size(main_f32));
    TASK_CHECK(remux_and_decode(s, dir, main_gray, bad_f32, "guard_audio") == 0);
    TASK_CHECK(file_contains(log, "MUTATE_AUDIO"));
    {
        char glog[1024];
        join2(glog, sizeof(glog), dir, "guard_audio.log");
        TASK_CHECK(file_contains(glog, "ORACLE_MISMATCH audio"));
    }
    return 0;
}

static int case_guard_clock(void)
{
    const struct transition_spec *s = &transition_specs[0];
    char dir[1024], main_nut[1024], off_nut[1024], probe[1024];
    char gray[1024], f32[1024], log[1024];
    char *argv[] = {
        (char *)bin_ffmpeg(), (char *)"-v", (char *)"error",
        (char *)"-nostdin", (char *)"-i", NULL,
        (char *)"-c", (char *)"copy",
        (char *)"-output_ts_offset", (char *)"1.0",
        (char *)"-y", NULL, NULL
    };

    TASK_CHECK(spec_dir(s, dir, sizeof(dir)) == 0);
    TASK_CHECK(generate_sources(s, dir) == 0);
    TASK_CHECK(build_canonical(s, dir) == 0);
    join2(main_nut, sizeof(main_nut), dir, "main.nut");
    snprintf(off_nut, sizeof(off_nut), "%s/guard_clock.nut", dir);
    snprintf(probe, sizeof(probe), "%s/guard_clock.probe", dir);
    snprintf(gray, sizeof(gray), "%s/guard_clock.gray", dir);
    snprintf(f32, sizeof(f32), "%s/guard_clock.f32", dir);
    join2(log, sizeof(log), dir, "guard_clock.log");
    argv[5] = main_nut;
    argv[11] = off_nut;
    /* real remux applying a whole-stream timestamp offset; content unchanged */
    TASK_CHECK(run_capture(argv, log) == 0);
    TASK_CHECK(probe_nut(off_nut, probe) == 0);
    TASK_CHECK(decode_nut(off_nut, gray, f32, log) == 0);
    /* decoded samples are intact, only the clock moved: the clock check fails */
    TASK_CHECK(file_size(gray) == 65L * TASK_PIXELS);
    TASK_CHECK(file_size(f32) == 124800L * TASK_CHANNELS
                               * (long)sizeof(float));
    TASK_CHECK(oracle_validate(s, gray, f32, probe, log) != 0);
    TASK_CHECK(file_contains(log, "ORACLE_MISMATCH clock"));
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "rational_timeline", "r25_chain",        case_timeline_r25 },
    { "rational_timeline", "ntsc_chain",       case_timeline_ntsc },
    { "chunk_invariance",  "asetnsamples_r25", case_chunks_r25 },
    { "chunk_invariance",  "asetnsamples_ntsc",case_chunks_ntsc },
    { "oracle_guards",     "mutated_pixel",    case_guard_pixel },
    { "oracle_guards",     "swapped_channel",  case_guard_audio },
    { "oracle_guards",     "shifted_clock",    case_guard_clock },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
