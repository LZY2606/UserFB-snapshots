/*
 * bwf_timeline.c - timeline validation and streaming render for bwf_join.
 *
 * The pass works in two reads of every input: the first pass audits format
 * compatibility and every overlapping frame (byte-exact, per channel), the
 * second pass streams the chosen frames into the output.  Memory use is
 * linear in the number of inputs plus a fixed-size audio block; whole
 * recordings and hole-spanning arrays are never allocated.
 */
#include "bwf.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define BWF_FAIL(...) do { snprintf(err, errSize, __VA_ARGS__); return 0; } while (0)

static int bwf_mul_u64(drwav_uint64 a, drwav_uint64 b, drwav_uint64 *out)
{
    if (a != 0 && b > (drwav_uint64)(-1) / a) return 0;
    *out = a * b;
    return 1;
}

/* dr_wav write callbacks: plain stdio, the FILE* stays owned by the caller. */
static size_t bwf_on_write(void *user, const void *data, size_t n)
{
    return fwrite(data, 1, n, (FILE *)user);
}

static drwav_bool32 bwf_on_seek(void *user, int offset, drwav_seek_origin origin)
{
    int whence = SEEK_SET;
    if (origin == DRWAV_SEEK_CUR) whence = SEEK_CUR;
    else if (origin == DRWAV_SEEK_END) whence = SEEK_END;
    return fseek((FILE *)user, offset, whence) == 0;
}

void bwf_timeline_close(bwf_input *in)
{
    if (in->opened) {
        drwav_uninit(&in->wav);
        in->opened = 0;
    }
    free(in->localIds);
    free(in->newIds);
    in->localIds = in->newIds = NULL;
}

/*
 * Chunk walk callback.  dr_wav rounds the data size down to a frame
 * boundary itself, so the declared header size is captured here to reject
 * non-integer-frame data and to enforce the single-data-chunk rule.
 */
static drwav_uint64 bwf_on_chunk(void *user, drwav_read_proc onRead,
                                 drwav_seek_proc onSeek, void *readSeekUserData,
                                 const drwav_chunk_header *header,
                                 drwav_container container, const drwav_fmt *fmt)
{
    bwf_input *in = (bwf_input *)user;
    (void)onRead; (void)onSeek; (void)readSeekUserData; (void)container; (void)fmt;

    if (memcmp(header->id.fourcc, "data", 4) == 0) {
        ++in->dataChunkCount;
        if (in->dataChunkCount == 1)
            in->declaredDataSize = header->sizeInBytes;
    } else if (memcmp(header->id.fourcc, "wavl", 4) == 0) {
        in->hasWavl = 1;
    }
    return 0;
}

int bwf_timeline_open(bwf_input *in, const char *path, char *err, size_t errSize)
{
    drwav *w;
    drwav_uint32 i;
    drwav_uint32 bextCount = 0, cueCount = 0, smplCount = 0;
    drwav_uint64 end;

    memset(in, 0, sizeof(*in));
    in->path = path;

    if (!drwav_init_file_ex(&in->wav, path, bwf_on_chunk, in,
                            DRWAV_WITH_METADATA, NULL))
        BWF_FAIL("%s: cannot open as a RIFF/WAVE file", path);
    in->opened = 1;
    w = &in->wav;

    /* Only ordinary little-endian RIFF is in scope; RF64/W64/RIFX/AIFF are not. */
    if (w->container != drwav_container_riff)
        BWF_FAIL("%s: only little-endian RIFF containers are supported", path);
    if (w->fmt.formatTag != DR_WAVE_FORMAT_PCM || w->translatedFormatTag != DR_WAVE_FORMAT_PCM)
        BWF_FAIL("%s: only uncompressed integer PCM is supported", path);

    if (w->bitsPerSample != 8 && w->bitsPerSample != 16 &&
        w->bitsPerSample != 24 && w->bitsPerSample != 32)
        BWF_FAIL("%s: unsupported bit depth %u (need 8/16/24/32)", path, (unsigned)w->bitsPerSample);
    if (w->channels == 0 || w->sampleRate == 0)
        BWF_FAIL("%s: invalid fmt chunk", path);
    if ((drwav_uint32)w->fmt.channels != (drwav_uint32)w->channels ||
        (drwav_uint32)w->fmt.bitsPerSample != (drwav_uint32)w->bitsPerSample)
        BWF_FAIL("%s: inconsistent fmt chunk", path);
    if (w->fmt.blockAlign != (drwav_uint16)((w->channels * w->bitsPerSample) / 8))
        BWF_FAIL("%s: block align does not match channels and bit depth", path);

    if (in->hasWavl)
        BWF_FAIL("%s: wavl (wave list) chunks are not supported", path);
    if (in->dataChunkCount != 1)
        BWF_FAIL("%s: expected exactly one data chunk, found %u",
                 path, (unsigned)in->dataChunkCount);
    if (in->declaredDataSize == 0)
        BWF_FAIL("%s: empty data chunk", path);
    if (in->declaredDataSize % (drwav_uint64)w->fmt.blockAlign != 0)
        BWF_FAIL("%s: data chunk size is not a whole number of frames", path);
    if (in->declaredDataSize != w->dataChunkDataSize)
        BWF_FAIL("%s: data chunk size is inconsistent with the file size", path);
    in->frameCount = in->declaredDataSize / (drwav_uint64)w->fmt.blockAlign;

    /* Locate the singleton chunks; duplicates are rejected. */
    for (i = 0; i < w->metadataCount; ++i) {
        drwav_metadata *md = &w->pMetadata[i];
        if (md->type == drwav_metadata_type_bext) {
            if (bextCount++) BWF_FAIL("%s: duplicate bext chunk", path);
            in->bext = md;
        } else if (md->type == drwav_metadata_type_cue) {
            if (cueCount++) BWF_FAIL("%s: duplicate cue chunk", path);
            in->cue = md;
        } else if (md->type == drwav_metadata_type_smpl) {
            if (smplCount++) BWF_FAIL("%s: duplicate smpl chunk", path);
            in->smpl = md;
        }
        /* Everything else (INFO strings, unknown chunks, ...) is ignored by design. */
    }
    if (in->bext == NULL)
        BWF_FAIL("%s: missing bext chunk with timeReference", path);

    in->timeReference = in->bext->data.bext.timeReference;
    if (!bwf_add_u64(in->timeReference, in->frameCount, &end))
        BWF_FAIL("%s: time reference plus frame count overflows", path);

    return 1;
}

typedef struct
{
    drwav_uint64 pos;
    int input;
    int kind;   /* +1 = start, -1 = end; ends sort before starts at the same pos */
} bwf_event;

static int bwf_event_cmp(const void *a, const void *b)
{
    const bwf_event *ea = (const bwf_event *)a;
    const bwf_event *eb = (const bwf_event *)b;
    if (ea->pos < eb->pos) return -1;
    if (ea->pos > eb->pos) return 1;
    if (ea->kind != eb->kind) return ea->kind < eb->kind ? -1 : 1;
    if (ea->input < eb->input) return -1;
    if (ea->input > eb->input) return 1;
    return 0;
}

/* Read exactly n bytes from the current data position or fail. */
static int bwf_read_exact(drwav *w, void *buf, size_t n, const char *path,
                          char *err, size_t errSize)
{
    if (drwav_read_raw(w, n, buf) != n)
        BWF_FAIL("%s: short read while auditing audio", path);
    return 1;
}

/*
 * Compare the raw per-channel PCM bytes of two volumes over [lo, hi).
 * The first mismatching frame is reported as an absolute timeline frame.
 */
static int bwf_compare_span(bwf_join *job, int a, int b,
                            drwav_uint64 lo, drwav_uint64 hi,
                            unsigned char *bufA, unsigned char *bufB,
                            char *err, size_t errSize)
{
    bwf_input *ia = &job->inputs[a];
    bwf_input *ib = &job->inputs[b];
    drwav_uint32 bpf = job->bytesPerFrame;
    drwav_uint64 done = 0, remain = hi - lo, frameInBlock;
    unsigned char *pa, *pb;

    if (!drwav_seek_to_pcm_frame(&ia->wav, lo - ia->timeReference) ||
        !drwav_seek_to_pcm_frame(&ib->wav, lo - ib->timeReference))
        BWF_FAIL("%s: seek failed while auditing overlap", ia->path);

    while (remain > 0) {
        drwav_uint64 chunk = remain < BWF_BLOCK_FRAMES ? remain : BWF_BLOCK_FRAMES;
        size_t bytes = (size_t)chunk * (size_t)bpf;

        if (!bwf_read_exact(&ia->wav, bufA, bytes, ia->path, err, errSize)) return 0;
        if (!bwf_read_exact(&ib->wav, bufB, bytes, ib->path, err, errSize)) return 0;

        if (memcmp(bufA, bufB, bytes) != 0) {
            for (frameInBlock = 0; frameInBlock < chunk; ++frameInBlock) {
                pa = bufA + (size_t)frameInBlock * bpf;
                pb = bufB + (size_t)frameInBlock * bpf;
                if (memcmp(pa, pb, bpf) != 0) {
                    snprintf(err, errSize,
                             "overlap conflict at absolute frame %" PRIu64
                             " between %s and %s",
                             lo + done + frameInBlock, ia->path, ib->path);
                    return 0;
                }
            }
            BWF_FAIL("%s: overlap bookkeeping error", ia->path);
        }

        done += chunk;
        remain -= chunk;
    }
    return 1;
}

int bwf_timeline_check(bwf_join *job, char *err, size_t errSize)
{
    size_t n = job->count, i, j;
    bwf_event *events;
    int *active;
    unsigned char *bufA, *bufB;
    size_t actN = 0, ev = 0;
    drwav_uint64 prev, inEnd;

    if (n == 0)
        BWF_FAIL("no input files");

    job->sampleRate = job->inputs[0].wav.sampleRate;
    job->channels = job->inputs[0].wav.channels;
    job->bitsPerSample = job->inputs[0].wav.bitsPerSample;
    job->bytesPerFrame = job->inputs[0].wav.fmt.blockAlign;
    job->origin = job->inputs[0].timeReference;
    job->end = 0;   /* recomputed with overflow checks below */

    for (i = 0; i < n; ++i) {
        bwf_input *in = &job->inputs[i];

        if (in->wav.sampleRate != job->sampleRate ||
            (drwav_uint32)in->wav.channels != job->channels ||
            (drwav_uint32)in->wav.bitsPerSample != job->bitsPerSample)
            BWF_FAIL("%s: sample rate, channel count or bit depth differs from %s",
                     in->path, job->inputs[0].path);

        if (!bwf_add_u64(in->timeReference, in->frameCount, &inEnd))
            BWF_FAIL("%s: end position overflows", in->path);
        if (in->timeReference < job->origin) job->origin = in->timeReference;
        if (inEnd > job->end) job->end = inEnd;

        for (j = 0; j < i; ++j) {
            if (in->timeReference == job->inputs[j].timeReference)
                BWF_FAIL("%s: duplicate bext.timeReference %" PRIu64,
                         in->path, in->timeReference);
        }
    }

    job->totalFrames = job->end - job->origin;

    events = (bwf_event *)malloc(sizeof(bwf_event) * 2 * n);
    active = (int *)malloc(sizeof(int) * n);
    bufA = (unsigned char *)malloc((size_t)BWF_BLOCK_FRAMES * job->bytesPerFrame);
    bufB = (unsigned char *)malloc((size_t)BWF_BLOCK_FRAMES * job->bytesPerFrame);
    if (events == NULL || active == NULL || bufA == NULL || bufB == NULL) {
        free(events); free(active); free(bufA); free(bufB);
        BWF_FAIL("out of memory");
    }

    for (i = 0; i < n; ++i) {
        events[ev].pos = job->inputs[i].timeReference;
        events[ev].input = (int)i;
        events[ev].kind = +1;
        ++ev;
        events[ev].pos = job->inputs[i].timeReference + job->inputs[i].frameCount;
        events[ev].input = (int)i;
        events[ev].kind = -1;
        ++ev;
    }
    qsort(events, 2 * n, sizeof(bwf_event), bwf_event_cmp);

    /* Sweep the timeline; every frame covered by two or more volumes is compared. */
    prev = events[0].pos;
    for (ev = 0; ev < 2 * n; ++ev) {
        drwav_uint64 lo = prev, hi = events[ev].pos;

        if (hi > lo && actN >= 2) {
            for (i = 0; i < actN; ++i) {
                for (j = i + 1; j < actN; ++j) {
                    if (!bwf_compare_span(job, active[i], active[j], lo, hi,
                                          bufA, bufB, err, errSize)) {
                        free(events); free(active); free(bufA); free(bufB);
                        return 0;
                    }
                }
            }
        }

        if (events[ev].kind == +1) {
            active[actN++] = events[ev].input;
        } else {
            for (i = 0; i < actN; ++i) {
                if (active[i] == events[ev].input) {
                    active[i] = active[--actN];
                    break;
                }
            }
        }
        prev = hi;
    }

    free(events);
    free(active);
    free(bufA);
    free(bufB);
    return 1;
}

/* Write len silence frames through the writer; 8-bit silence is 0x80. */
static int bwf_write_silence(drwav *writer, drwav_uint64 len, drwav_uint32 bpf,
                             unsigned char *block, char *err, size_t errSize)
{
    while (len > 0) {
        drwav_uint64 chunk = len < BWF_BLOCK_FRAMES ? len : BWF_BLOCK_FRAMES;
        size_t bytes = (size_t)chunk * (size_t)bpf;
        memset(block, writer->bitsPerSample == 8 ? 0x80 : 0, bytes);
        if (drwav_write_raw(writer, bytes, block) != bytes)
            BWF_FAIL("short write while filling a gap with silence");
        len -= chunk;
    }
    return 1;
}

int bwf_timeline_render(bwf_join *job, FILE *out, char *err, size_t errSize)
{
    size_t n = job->count, i, ev, actN = 0;
    bwf_event *events;
    int *active;
    unsigned char *block;
    drwav_data_format format;
    drwav writer;
    drwav_uint64 prev;

    events = (bwf_event *)malloc(sizeof(bwf_event) * 2 * n);
    active = (int *)malloc(sizeof(int) * n);
    block = (unsigned char *)malloc((size_t)BWF_BLOCK_FRAMES * job->bytesPerFrame);
    if (events == NULL || active == NULL || block == NULL) {
        free(events); free(active); free(block);
        BWF_FAIL("out of memory");
    }

    ev = 0;
    for (i = 0; i < n; ++i) {
        events[ev].pos = job->inputs[i].timeReference;
        events[ev].input = (int)i;
        events[ev].kind = +1;
        ++ev;
        events[ev].pos = job->inputs[i].timeReference + job->inputs[i].frameCount;
        events[ev].input = (int)i;
        events[ev].kind = -1;
        ++ev;
    }
    qsort(events, 2 * n, sizeof(bwf_event), bwf_event_cmp);

    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = job->channels;
    format.sampleRate = job->sampleRate;
    format.bitsPerSample = job->bitsPerSample;

    if (!drwav_init_write_with_metadata(&writer, &format, bwf_on_write, bwf_on_seek,
                                        out, NULL, job->outMeta, job->outMetaCount)) {
        free(events); free(active); free(block);
        BWF_FAIL("failed to initialize output WAV writer");
    }

    /* Second read pass: gaps become silence, covered frames come from one volume. */
    prev = job->origin;
    for (ev = 0; ev < 2 * n; ++ev) {
        drwav_uint64 lo = prev, hi = events[ev].pos;

        if (hi > lo) {
            drwav_uint64 len = hi - lo;
            if (actN == 0) {
                if (!bwf_write_silence(&writer, len, job->bytesPerFrame, block,
                                       err, errSize)) goto fail;
            } else {
                bwf_input *src = &job->inputs[active[0]];
                if (!drwav_seek_to_pcm_frame(&src->wav, lo - src->timeReference)) {
                    snprintf(err, errSize, "%s: seek failed during render", src->path);
                    goto fail;
                }
                while (len > 0) {
                    drwav_uint64 chunk = len < BWF_BLOCK_FRAMES ? len : BWF_BLOCK_FRAMES;
                    size_t bytes = (size_t)chunk * (size_t)job->bytesPerFrame;
                    if (drwav_read_raw(&src->wav, bytes, block) != bytes) {
                        snprintf(err, errSize, "%s: short read during render", src->path);
                        goto fail;
                    }
                    if (drwav_write_raw(&writer, bytes, block) != bytes) {
                        snprintf(err, errSize, "%s: short write during render", src->path);
                        goto fail;
                    }
                    len -= chunk;
                }
            }
        }

        if (events[ev].kind == +1) {
            active[actN++] = events[ev].input;
        } else {
            for (i = 0; i < actN; ++i) {
                if (active[i] == events[ev].input) {
                    active[i] = active[--actN];
                    break;
                }
            }
        }
        prev = hi;
    }

    if (drwav_uninit(&writer) != DRWAV_SUCCESS) {
        snprintf(err, errSize, "failed to finalize output WAV");
        free(events); free(active); free(block);
        return 0;
    }

    free(events);
    free(active);
    free(block);
    return 1;

fail:
    drwav_uninit(&writer);
    free(events);
    free(active);
    free(block);
    return 0;
}

int bwf_join_run(int argc, char **argv, char *err, size_t errSize)
{
    bwf_join job;
    size_t i, n;
    FILE *probe, *out;
    drwav_uint64 dataBytes;
    int rc = 1;   /* shell convention: 0 = success */

#define JFAIL(...) do { snprintf(err, errSize, __VA_ARGS__); goto cleanup; } while (0)

    if (argc < 3) {
        snprintf(err, errSize, "usage: bwf_join OUTPUT INPUT...");
        return 1;
    }

    memset(&job, 0, sizeof(job));
    n = (size_t)argc - 2;

    /* Refuse before touching anything, so a failed run never replaces a file. */
    probe = fopen(argv[1], "rb");
    if (probe != NULL) {
        fclose(probe);
        snprintf(err, errSize, "%s: output file already exists", argv[1]);
        return 1;
    }

    job.count = n;
    job.inputs = (bwf_input *)calloc(n, sizeof(bwf_input));
    if (job.inputs == NULL) {
        snprintf(err, errSize, "out of memory");
        return 1;
    }

    for (i = 0; i < n; ++i) {
        if (!bwf_timeline_open(&job.inputs[i], argv[i + 2], err, errSize))
            goto cleanup;
    }

    if (!bwf_timeline_check(&job, err, errSize))
        goto cleanup;

    for (i = 0; i < n; ++i) {
        if (!bwf_metadata_validate(&job.inputs[i], err, errSize))
            goto cleanup;
    }

    if (!bwf_metadata_build(&job, err, errSize))
        goto cleanup;

    if (!bwf_mul_u64(job.totalFrames, job.bytesPerFrame, &dataBytes) ||
        dataBytes > (drwav_uint64)0xFFFFFFFFu)
        JFAIL("output data does not fit in a RIFF 32-bit length");

    {
        drwav_data_format format;
        drwav_uint64 overhead, trueSize;
        format.container = drwav_container_riff;
        format.format = DR_WAVE_FORMAT_PCM;
        format.channels = job.channels;
        format.sampleRate = job.sampleRate;
        format.bitsPerSample = job.bitsPerSample;
        /*
         * dr_wav clamps an over-large RIFF size to 0xFFFFFFFF rather than
         * failing, so measure the zero-data overhead and add the data size
         * ourselves with an overflow check instead of trusting the clamp.
         */
        overhead = drwav_target_write_size_bytes(&format, 0, job.outMeta, job.outMetaCount);
        if (overhead == 0)
            JFAIL("cannot determine output size");
        if (!bwf_add_u64(overhead, dataBytes + (dataBytes & 1u), &trueSize) ||
            trueSize > (drwav_uint64)0xFFFFFFFFu)
            JFAIL("output file does not fit in a RIFF 32-bit length");
    }

    out = fopen(argv[1], "wb");
    if (out == NULL)
        JFAIL("%s: cannot create output: %s", argv[1], strerror(errno));

    if (!bwf_timeline_render(&job, out, err, errSize)) {
        fclose(out);
        remove(argv[1]);   /* never leave a half-written target behind */
        goto cleanup;
    }
    if (fclose(out) != 0) {
        remove(argv[1]);
        snprintf(err, errSize, "%s: failed to close output: %s", argv[1], strerror(errno));
        goto cleanup;
    }

    rc = 0;

#undef JFAIL

cleanup:
    bwf_metadata_free(&job);
    for (i = 0; i < n; ++i)
        bwf_timeline_close(&job.inputs[i]);
    free(job.inputs);
    return rc;
}
