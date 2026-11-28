/*
 * transition_oracle: independent reference for the chained xfade /
 * acrossfade transition regression.
 *
 * The reference uses only the C standard library and libm.  It never links
 * against FFmpeg and never derives expected values from the filters under
 * test: every expected pixel and sample is recomputed from the edit list and
 * the deterministic source generators in transition_spec.h.
 *
 * Subcommands:
 *   validate <spec-id> <tri|qsin> <video.gray> <audio.f32> <probe.txt>
 *       Compare a decoded NUT output (raw gray8 + packed pcm_f32le, plus an
 *       ffprobe packet/stream dump) against the recomputed timeline.  Exits
 *       non-zero and prints the first discrepancy (source position, pixel,
 *       channel, sample or timestamp) on mismatch.
 *   mutate-pixel <spec-id> <video-in.gray> <video-out.gray>
 *       Write a copy whose pixel at a known second-transition frame is
 *       changed by more than the 1-code-value tolerance.
 *   mutate-audio <spec-id> <audio-in.f32> <audio-out.f32>
 *       Write a copy with one stereo pair swapped inside the second
 *       crossfade.
 *
 * All timing is handled as integer PTS in the time bases ffprobe reports;
 * decimal pts_time strings are never parsed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "transition_spec.h"

#define VIDEO_TOLERANCE 1
#define AUDIO_TOLERANCE 1e-6f
#define MAX_STREAMS 8

struct packet {
    int stream_index;
    int64_t pts;
    int64_t duration;
    int64_t size;
};

struct stream_info {
    int present;
    int is_video;
    int is_audio;
    int tb_num;
    int tb_den;
};

struct probe_data {
    struct stream_info streams[MAX_STREAMS];
    struct packet *packets;
    size_t nb_packets;
    size_t cap_packets;
};

struct timeline_point {
    int seg0, pos0;           /* first source segment and position  */
    int seg1, pos1;           /* second source segment and position */
    double w0, w1;            /* linear weights                     */
    int transition;           /* 0 = passthrough, 1/2 = transition  */
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static const struct transition_spec *find_spec(const char *id)
{
    int i;

    for (i = 0; i < TASK_SPEC_COUNT; ++i)
        if (!strcmp(transition_specs[i].id, id))
            return &transition_specs[i];
    return NULL;
}

static uint8_t *slurp_bytes(const char *path, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long len;

    if (!fp) {
        fprintf(stderr, "ORACLE_ERROR cannot open %s\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) || (len = ftell(fp)) < 0) {
        fprintf(stderr, "ORACLE_ERROR cannot measure %s\n", path);
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fprintf(stderr, "ORACLE_ERROR cannot read %s\n", path);
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *len_out = (size_t)len;
    return buf;
}

/* round(a * num / den) for non-negative int64 values */
static int64_t rescale_round(int64_t a, int64_t num, int64_t den)
{
    return (2 * a * num + den) / (2 * den);
}

static double curve_gain(int curve, double w)
{
    if (curve == 1) /* qsin */
        return sin(w * (M_PI / 2.0));
    return w; /* tri */
}

/* ------------------------------------------------------------------ */
/* timeline mapping, computed from the edit list only                 */
/* ------------------------------------------------------------------ */

/*
 * Video output frame t.  With F frames per clip and V-frame overlaps, the
 * first xfade occupies output frames [F-V, F-1] and the second one
 * [2F-2V, 2F-V-1]; the second offset is a position in the merged timeline.
 */
static struct timeline_point video_point(const struct transition_spec *s, int t)
{
    struct timeline_point p;
    const int F = s->frames_per_seg, V = s->overlap_frames;
    const int first_start = F - V;
    const int second_start = 2 * F - 2 * V;

    memset(&p, 0, sizeof(p));
    if (t < first_start) {                 /* clip 0 passthrough */
        p.seg0 = 0; p.pos0 = t; p.w0 = 1.0;
    } else if (t < F) {                    /* first xfade */
        int k = t - first_start;
        p.transition = 1;
        p.seg0 = 0; p.pos0 = t;
        p.seg1 = 1; p.pos1 = k;
        p.w0 = 1.0 - (double)k / V;
        p.w1 = (double)k / V;
    } else if (t < second_start) {         /* clip 1 passthrough */
        p.seg0 = 1; p.pos0 = t - first_start; p.w0 = 1.0;
    } else if (t < second_start + V) {     /* second xfade */
        int k = t - second_start;
        p.transition = 2;
        p.seg0 = 1; p.pos0 = t - first_start;
        p.seg1 = 2; p.pos1 = k;
        p.w0 = 1.0 - (double)k / V;
        p.w1 = (double)k / V;
    } else {                               /* clip 2 passthrough */
        p.seg0 = 2; p.pos0 = t - second_start; p.w0 = 1.0;
    }
    return p;
}

/*
 * Audio output sample t.  Same construction with A samples per clip and an
 * N-sample overlap; weights follow the filter convention (N-1-k)/N and k/N.
 */
static struct timeline_point audio_point(const struct transition_spec *s, long t)
{
    struct timeline_point p;
    const long A = s->samples_per_seg, N = s->overlap_samples;
    const long first_start = A - N;
    const long second_start = 2 * A - 2 * N;

    memset(&p, 0, sizeof(p));
    if (t < first_start) {
        p.seg0 = 0; p.pos0 = (int)t; p.w0 = 1.0;
    } else if (t < A) {
        long k = t - first_start;
        p.transition = 1;
        p.seg0 = 0; p.pos0 = (int)t;
        p.seg1 = 1; p.pos1 = (int)k;
        p.w0 = (double)(N - 1 - k) / N;
        p.w1 = (double)k / N;
    } else if (t < second_start) {
        p.seg0 = 1; p.pos0 = (int)(t - first_start); p.w0 = 1.0;
    } else if (t < second_start + N) {
        long k = t - second_start;
        p.transition = 2;
        p.seg0 = 1; p.pos0 = (int)(t - first_start);
        p.seg1 = 2; p.pos1 = (int)k;
        p.w0 = (double)(N - 1 - k) / N;
        p.w1 = (double)k / N;
    } else {
        p.seg0 = 2; p.pos0 = (int)(t - second_start); p.w0 = 1.0;
    }
    return p;
}

static uint8_t expected_pixel(const struct transition_spec *s, int t, int pixel)
{
    struct timeline_point p = video_point(s, t);
    double v;

    v = (double)transition_source_pixel(p.seg0, p.pos0, pixel) * p.w0;
    if (p.transition)
        v += (double)transition_source_pixel(p.seg1, p.pos1, pixel) * p.w1;
    return (uint8_t)v; /* filter stores float mix into uint8_t (truncates) */
}

static float expected_sample(const struct transition_spec *s,
                             int curve0, int curve1,
                             long t, int channel)
{
    struct timeline_point p = audio_point(s, t);
    double v;

    v = (double)transition_source_sample(p.seg0, p.pos0, channel)
        * curve_gain(curve0, p.w0);
    if (p.transition)
        v += (double)transition_source_sample(p.seg1, p.pos1, channel)
            * curve_gain(curve1, p.w1);
    return (float)v;
}

/* ------------------------------------------------------------------ */
/* ffprobe dump parsing (default format, integer fields only)         */
/* ------------------------------------------------------------------ */

static void probe_free(struct probe_data *pr)
{
    free(pr->packets);
    memset(pr, 0, sizeof(*pr));
}

static int parse_timebase(const char *v, int *num, int *den)
{
    return sscanf(v, "%d/%d", num, den) == 2 && *num > 0 && *den > 0;
}

static int parse_probe(const char *path, struct probe_data *pr)
{
    FILE *fp = fopen(path, "rb");
    char line[512];
    int section = 0; /* 0 none, 1 stream, 2 packet */
    struct packet cur_pkt;
    struct stream_info cur_str;
    int cur_index = -1;
    char codec[64] = "";
    char tb[64] = "";

    if (!fp) {
        fprintf(stderr, "ORACLE_ERROR cannot open probe %s\n", path);
        return 1;
    }
    memset(pr, 0, sizeof(*pr));
    memset(&cur_pkt, 0, sizeof(cur_pkt));
    memset(&cur_str, 0, sizeof(cur_str));
    while (fgets(line, sizeof(line), fp)) {
        char *eq;
        char key[64], val[256];

        if (!strncmp(line, "[STREAM]", 8)) {
            section = 1; cur_index = -1; codec[0] = tb[0] = 0;
            memset(&cur_str, 0, sizeof(cur_str));
            continue;
        }
        if (!strncmp(line, "[/STREAM]", 9)) {
            if (cur_index >= 0 && cur_index < MAX_STREAMS) {
                cur_str.is_video = !strcmp(codec, "video");
                cur_str.is_audio = !strcmp(codec, "audio");
                if (tb[0] && !parse_timebase(tb, &cur_str.tb_num,
                                             &cur_str.tb_den)) {
                    fprintf(stderr, "ORACLE_ERROR bad time_base '%s'\n", tb);
                    fclose(fp); probe_free(pr); return 1;
                }
                cur_str.present = 1;
                pr->streams[cur_index] = cur_str;
            }
            section = 0;
            continue;
        }
        if (!strncmp(line, "[PACKET]", 8)) {
            section = 2;
            memset(&cur_pkt, 0, sizeof(cur_pkt));
            cur_pkt.pts = -1;
            continue;
        }
        if (!strncmp(line, "[/PACKET]", 9)) {
            if (pr->nb_packets == pr->cap_packets) {
                size_t ncap = pr->cap_packets ? pr->cap_packets * 2 : 64;
                struct packet *np = realloc(pr->packets,
                                            ncap * sizeof(*np));
                if (!np) { fclose(fp); probe_free(pr); return 1; }
                pr->packets = np; pr->cap_packets = ncap;
            }
            pr->packets[pr->nb_packets++] = cur_pkt;
            section = 0;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq)
            continue;
        if ((size_t)(eq - line) >= sizeof(key))
            continue;
        memcpy(key, line, (size_t)(eq - line));
        key[eq - line] = 0;
        snprintf(val, sizeof(val), "%s", eq + 1);
        val[strcspn(val, "\r\n")] = 0;
        if (section == 1) {
            if (!strcmp(key, "index")) cur_index = atoi(val);
            else if (!strcmp(key, "codec_type")) snprintf(codec, sizeof(codec), "%s", val);
            else if (!strcmp(key, "time_base")) snprintf(tb, sizeof(tb), "%s", val);
        } else if (section == 2) {
            if (!strcmp(key, "stream_index")) cur_pkt.stream_index = atoi(val);
            else if (!strcmp(key, "pts")) {
                if (val[0] == 'N') {
                    fprintf(stderr, "ORACLE_ERROR packet without PTS\n");
                    fclose(fp); probe_free(pr); return 1;
                }
                cur_pkt.pts = strtoll(val, NULL, 10);
            } else if (!strcmp(key, "duration")) {
                if (val[0] != 'N') cur_pkt.duration = strtoll(val, NULL, 10);
            } else if (!strcmp(key, "size")) {
                cur_pkt.size = strtoll(val, NULL, 10);
            }
        }
    }
    fclose(fp);
    return 0;
}

static int find_stream(const struct probe_data *pr, int want_video)
{
    int i;

    for (i = 0; i < MAX_STREAMS; ++i) {
        if (!pr->streams[i].present)
            continue;
        if (want_video ? pr->streams[i].is_video : pr->streams[i].is_audio)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* validation                                                         */
/* ------------------------------------------------------------------ */

static int check_clocks(const struct transition_spec *s,
                        const struct probe_data *pr, int out_frames,
                        long out_samples, int *video_packets_out,
                        long *audio_samples_out, const char **fail)
{
    static char msg[512];
    int vi = find_stream(pr, 1), ai = find_stream(pr, 0);
    size_t k;
    int video_packets = 0, audio_packets = 0;
    long sample_axis = 0;

    if (vi < 0 || ai < 0) {
        strcpy(msg, "ORACLE_MISMATCH clock stream missing video/audio");
        *fail = msg;
        return 1;
    }
    for (k = 0; k < pr->nb_packets; ++k) {
        const struct packet *pk = &pr->packets[k];
        const struct stream_info *st;
        int64_t expect_pts;

        if (pk->stream_index != vi && pk->stream_index != ai)
            continue;
        st = &pr->streams[pk->stream_index];
        if (!st->tb_num || !st->tb_den) {
            snprintf(msg, sizeof(msg),
                     "ORACLE_MISMATCH clock packet=%zu stream=%d missing time_base",
                     k, pk->stream_index);
            *fail = msg;
            return 1;
        }
        if (pk->stream_index == vi) {
            expect_pts = rescale_round(video_packets,
                                       (int64_t)s->rate_den * st->tb_den,
                                       (int64_t)s->rate_num * st->tb_num);
            if (pk->pts != expect_pts) {
                snprintf(msg, sizeof(msg),
                         "ORACLE_MISMATCH clock video packet=%d frame=%d "
                         "pts=%lld expected=%lld tb=%d/%d",
                         video_packets, video_packets,
                         (long long)pk->pts, (long long)expect_pts,
                         st->tb_num, st->tb_den);
                *fail = msg;
                return 1;
            }
            ++video_packets;
        } else {
            /* audio packet pts must equal the running sample axis */
            expect_pts = rescale_round(sample_axis, st->tb_den,
                                       (int64_t)TASK_SAMPLE_RATE * st->tb_num);
            if (pk->pts != expect_pts) {
                snprintf(msg, sizeof(msg),
                         "ORACLE_MISMATCH clock audio packet=%d sample=%ld "
                         "pts=%lld expected=%lld tb=%d/%d",
                         audio_packets, sample_axis,
                         (long long)pk->pts, (long long)expect_pts,
                         st->tb_num, st->tb_den);
                *fail = msg;
                return 1;
            }
            if (pk->duration > 0 && pk->pts + pk->duration !=
                rescale_round(sample_axis + pk->size / (TASK_CHANNELS * (int64_t)sizeof(float)),
                              st->tb_den,
                              (int64_t)TASK_SAMPLE_RATE * st->tb_num)) {
                snprintf(msg, sizeof(msg),
                         "ORACLE_MISMATCH clock audio packet=%d discontinuous "
                         "pts=%lld duration=%lld",
                         audio_packets, (long long)pk->pts,
                         (long long)pk->duration);
                *fail = msg;
                return 1;
            }
            sample_axis += pk->size / (TASK_CHANNELS * (int64_t)sizeof(float));
            ++audio_packets;
        }
    }
    if (video_packets != out_frames) {
        snprintf(msg, sizeof(msg),
                 "ORACLE_MISMATCH clock video_packets=%d expected=%d",
                 video_packets, out_frames);
        *fail = msg;
        return 1;
    }
    if (sample_axis != out_samples) {
        snprintf(msg, sizeof(msg),
                 "ORACLE_MISMATCH clock audio_samples=%ld expected=%ld",
                 sample_axis, out_samples);
        *fail = msg;
        return 1;
    }
    if (!video_packets || !audio_packets) {
        strcpy(msg, "ORACLE_MISMATCH clock empty packet stream");
        *fail = msg;
        return 1;
    }
    *video_packets_out = video_packets;
    *audio_samples_out = sample_axis;
    return 0;
}

static const char *region_name(int transition)
{
    return transition == 0 ? "passthrough" :
           transition == 1 ? "xfade1" : "xfade2";
}

static int validate(const struct transition_spec *s,
                    int curve0, int curve1,
                    const char *video_path, const char *audio_path,
                    const char *probe_path)
{
    const int out_frames = transition_output_frames(s);
    const long out_samples = transition_output_samples(s);
    size_t vlen, alen;
    uint8_t *vbuf, *abuf;
    struct probe_data pr;
    const char *fail = NULL;
    int t, pixel, rc = 0;
    int video_packets = 0;
    long samp, audio_axis = 0;

    vbuf = slurp_bytes(video_path, &vlen);
    if (!vbuf)
        return 1;
    abuf = slurp_bytes(audio_path, &alen);
    if (!abuf) {
        free(vbuf);
        return 1;
    }
    if (vlen != (size_t)out_frames * TASK_PIXELS) {
        fprintf(stderr,
                "ORACLE_MISMATCH video bytes=%zu expected=%d (%d frames)\n",
                vlen, out_frames * TASK_PIXELS, out_frames);
        rc = 1; goto out;
    }
    if (alen != (size_t)out_samples * TASK_CHANNELS * sizeof(float)) {
        fprintf(stderr,
                "ORACLE_MISMATCH audio bytes=%zu expected=%ld (%ld samples)\n",
                alen, out_samples * TASK_CHANNELS * (long)sizeof(float),
                out_samples);
        rc = 1; goto out;
    }
    if (parse_probe(probe_path, &pr)) {
        rc = 1; goto out;
    }
    if (check_clocks(s, &pr, out_frames, out_samples, &video_packets,
                     &audio_axis, &fail)) {
        fprintf(stderr, "%s\n", fail);
        rc = 1; goto out_probe;
    }

    /* video: every frame, every pixel, including both transitions and tail */
    for (t = 0; t < out_frames; ++t) {
        for (pixel = 0; pixel < TASK_PIXELS; ++pixel) {
            uint8_t got = vbuf[t * TASK_PIXELS + pixel];
            uint8_t exp = expected_pixel(s, t, pixel);
            int diff = (int)got - (int)exp;
            if (diff < 0) diff = -diff;
            if (diff > VIDEO_TOLERANCE) {
                struct timeline_point p = video_point(s, t);
                fprintf(stderr,
                        "ORACLE_MISMATCH video frame=%d pixel=%d (x=%d,y=%d) "
                        "region=%s got=%d expected=%d "
                        "src0=seg%d:frame%d w=%.6f",
                        t, pixel, pixel % TASK_WIDTH, pixel / TASK_WIDTH,
                        region_name(p.transition), got, exp,
                        p.seg0, p.pos0, p.w0);
                if (p.transition)
                    fprintf(stderr,
                            " src1=seg%d:frame%d w=%.6f\n",
                            p.seg1, p.pos1, p.w1);
                else
                    fputc('\n', stderr);
                rc = 1; goto out_probe;
            }
        }
    }

    /* audio: every sample on both channels */
    for (samp = 0; samp < out_samples; ++samp) {
        int ch;
        for (ch = 0; ch < TASK_CHANNELS; ++ch) {
            float got = ((const float *)abuf)[(size_t)samp * TASK_CHANNELS + ch];
            float exp = expected_sample(s, curve0, curve1, samp, ch);
            double d = fabs((double)got - (double)exp);
            if (d > AUDIO_TOLERANCE) {
                struct timeline_point p = audio_point(s, samp);
                fprintf(stderr,
                        "ORACLE_MISMATCH audio sample=%ld channel=%d "
                        "region=%s got=%.9g expected=%.9g diff=%.3g "
                        "src0=seg%d:sample%d w=%.6f",
                        samp, ch, region_name(p.transition), got, exp, d,
                        p.seg0, p.pos0, p.w0);
                if (p.transition)
                    fprintf(stderr, " src1=seg%d:sample%d w=%.6f\n",
                            p.seg1, p.pos1, p.w1);
                else
                    fputc('\n', stderr);
                rc = 1; goto out_probe;
            }
        }
    }
    printf("ORACLE_OK id=%s curve1=%s curve2=%s frames=%d samples=%ld "
           "video_packets=%d audio_samples=%ld\n",
           s->id, curve0 == 1 ? "qsin" : "tri",
           curve1 == 1 ? "qsin" : "tri", out_frames, out_samples,
           video_packets, audio_axis);

out_probe:
    probe_free(&pr);
out:
    free(vbuf);
    free(abuf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* mutation helpers: legal-format, legal-length negative artifacts    */
/* ------------------------------------------------------------------ */

static int mutate_pixel(const struct transition_spec *s,
                        const char *in, const char *out)
{
    size_t len;
    uint8_t *buf = slurp_bytes(in, &len);
    const int second_start = 2 * s->frames_per_seg - 2 * s->overlap_frames;
    const int frame = second_start + 2;          /* inside second xfade */
    const int pixel = 3;
    size_t off = (size_t)frame * TASK_PIXELS + pixel;
    uint8_t oldv, newv;
    FILE *fp;

    if (!buf)
        return 1;
    if (len != (size_t)transition_output_frames(s) * TASK_PIXELS) {
        fprintf(stderr, "ORACLE_ERROR mutate-pixel bad input length\n");
        free(buf);
        return 1;
    }
    oldv = buf[off];
    newv = (uint8_t)(oldv + 17);                 /* exceeds 1-code tolerance */
    if (newv == oldv) newv = (uint8_t)(oldv + 1);
    buf[off] = newv;
    fp = fopen(out, "wb");
    if (!fp || fwrite(buf, 1, len, fp) != len) {
        fprintf(stderr, "ORACLE_ERROR mutate-pixel write failed\n");
        free(buf);
        return 1;
    }
    fclose(fp);
    printf("MUTATE_PIXEL frame=%d pixel=%d (x=%d,y=%d) %d->%d\n",
           frame, pixel, pixel % TASK_WIDTH, pixel / TASK_WIDTH, oldv, newv);
    free(buf);
    return 0;
}

static int mutate_audio(const struct transition_spec *s,
                        const char *in, const char *out)
{
    size_t len;
    uint8_t *buf = slurp_bytes(in, &len);
    const long second_start = 2L * s->samples_per_seg
                            - 2L * s->overlap_samples;
    const long sample = second_start + 3;        /* inside second crossfade */
    size_t off = (size_t)sample * TASK_CHANNELS * sizeof(float);
    float pair[2];
    float tmp;
    FILE *fp;

    if (!buf)
        return 1;
    if (len != (size_t)transition_output_samples(s) * TASK_CHANNELS * sizeof(float)) {
        fprintf(stderr, "ORACLE_ERROR mutate-audio bad input length\n");
        free(buf);
        return 1;
    }
    memcpy(pair, buf + off, sizeof(pair));
    tmp = pair[0]; pair[0] = pair[1]; pair[1] = tmp;
    memcpy(buf + off, pair, sizeof(pair));
    fp = fopen(out, "wb");
    if (!fp || fwrite(buf, 1, len, fp) != len) {
        fprintf(stderr, "ORACLE_ERROR mutate-audio write failed\n");
        free(buf);
        return 1;
    }
    fclose(fp);
    printf("MUTATE_AUDIO sample=%ld swapped L/R (%.9g, %.9g)->(%.9g, %.9g)\n",
           sample, (double)tmp, (double)pair[0], (double)pair[0], (double)tmp);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "validate")) {
        const struct transition_spec *s;
        int curve0, curve1;
        if (argc != 8) {
            fprintf(stderr,
                    "usage: %s validate <id> <c1:tri|qsin> <c2:tri|qsin> "
                    "<gray> <f32> <probe>\n", argv[0]);
            return 2;
        }
        s = find_spec(argv[2]);
        if (!s) {
            fprintf(stderr, "ORACLE_ERROR unknown id '%s'\n", argv[2]);
            return 2;
        }
        if (!strcmp(argv[3], "tri")) curve0 = 0;
        else if (!strcmp(argv[3], "qsin")) curve0 = 1;
        else {
            fprintf(stderr, "ORACLE_ERROR unknown curve '%s'\n", argv[3]);
            return 2;
        }
        if (!strcmp(argv[4], "tri")) curve1 = 0;
        else if (!strcmp(argv[4], "qsin")) curve1 = 1;
        else {
            fprintf(stderr, "ORACLE_ERROR unknown curve '%s'\n", argv[4]);
            return 2;
        }
        return validate(s, curve0, curve1, argv[5], argv[6], argv[7]);
    }
    if (argc >= 2 && !strcmp(argv[1], "mutate-pixel")) {
        const struct transition_spec *s;
        if (argc != 5) {
            fprintf(stderr, "usage: %s mutate-pixel <id> <in> <out>\n", argv[0]);
            return 2;
        }
        s = find_spec(argv[2]);
        if (!s) {
            fprintf(stderr, "ORACLE_ERROR unknown id '%s'\n", argv[2]);
            return 2;
        }
        return mutate_pixel(s, argv[3], argv[4]);
    }
    if (argc >= 2 && !strcmp(argv[1], "mutate-audio")) {
        const struct transition_spec *s;
        if (argc != 5) {
            fprintf(stderr, "usage: %s mutate-audio <id> <in> <out>\n", argv[0]);
            return 2;
        }
        s = find_spec(argv[2]);
        if (!s) {
            fprintf(stderr, "ORACLE_ERROR unknown id '%s'\n", argv[2]);
            return 2;
        }
        return mutate_audio(s, argv[3], argv[4]);
    }
    fprintf(stderr,
            "usage: %s validate|mutate-pixel|mutate-audio ...\n", argv[0]);
    return 2;
}
