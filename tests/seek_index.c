/*  tests/seek_index.c - Exercises the deterministic, PCM-frame seek API.
 *
 *  Groups:
 *    pcm       - sample-exact equivalence with seeded sequential playback
 *    clock     - frame coverage, end-of-song and preroll reporting
 *    ownership - index/renderer lifetime and independence
 *
 *  Every reference comes from duh_it_start_seeded(), the sequential oracle.
 */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dumb.h"
#include "fixture.h"

#define MAX_FRAMES 60000

/* Render `frames` output frames from `renderer`, feeding it blocks described
 * by `pattern` (cycled).  Writes PCM into `out` when non-NULL and returns the
 * number of frames produced. */
static long render_blocks(DUH_SIGRENDERER *renderer, int channels, long frames,
                          const long *pattern, int pattern_len,
                          sample_t *out) {
    sample_t **buffer;
    long done = 0;
    int index = 0;

    if (!renderer)
        return 0;

    buffer = allocate_sample_buffer(channels, frames > 0 ? frames : 1);
    if (!buffer)
        return 0;

    while (done < frames) {
        long want = pattern[index % pattern_len];
        long got;
        index++;
        if (want > frames - done)
            want = frames - done;
        if (want <= 0)
            want = 1;
        memset(buffer[0], 0, want * channels * sizeof(sample_t));
        got = duh_sigrenderer_generate_samples(renderer, 1.0f, 2.0f, want,
                                              buffer);
        if (got <= 0)
            break;
        if (out)
            memcpy(out + done * channels, buffer[0],
                   (size_t)got * channels * sizeof(sample_t));
        done += got;
    }

    destroy_sample_buffer(buffer);
    return done;
}


/* Variant of render_blocks for the plain API, where volume and delta are not
 * pinned by the seeded entry point. */
static long render_blocks_params(DUH_SIGRENDERER *renderer, int channels,
                                 long frames, const long *pattern,
                                 int pattern_len, float volume, float delta,
                                 sample_t *out) {
    sample_t **buffer;
    long done = 0;
    int index = 0;

    if (!renderer)
        return 0;

    buffer = allocate_sample_buffer(channels, frames > 0 ? frames : 1);
    if (!buffer)
        return 0;

    while (done < frames) {
        long want = pattern[index % pattern_len];
        long got;
        index++;
        if (want > frames - done)
            want = frames - done;
        if (want <= 0)
            want = 1;
        memset(buffer[0], 0, want * channels * sizeof(sample_t));
        got = duh_sigrenderer_generate_samples(renderer, volume, delta, want,
                                              buffer);
        if (got <= 0)
            break;
        if (out)
            memcpy(out + done * channels, buffer[0],
                   (size_t)got * channels * sizeof(sample_t));
        done += got;
    }

    destroy_sample_buffer(buffer);
    return done;
}

/* Render the seeded oracle for the whole song (up to `limit` frames). */
static long render_reference(DUH *duh, int channels, int rate, int quality,
                             unsigned seed, long limit, sample_t *out) {
    static const long ones[] = {1};
    DUH_SIGRENDERER *renderer =
        duh_it_start_seeded(duh, channels, rate, quality, seed);
    long produced;
    if (!renderer)
        return -1;
    produced = render_blocks(renderer, channels, limit, ones, 1, out);
    duh_end_sigrenderer(renderer);
    return produced;
}

/* Largest absolute per-sample difference, and the first differing cell. */
static long long max_difference(const sample_t *a, const sample_t *b, long n,
                                long *first) {
    long long worst = 0;
    long i;
    if (first)
        *first = -1;
    for (i = 0; i < n; i++) {
        long long diff = (long long)a[i] - (long long)b[i];
        if (diff < 0)
            diff = -diff;
        if (diff && first && *first < 0)
            *first = i;
        if (diff > worst)
            worst = diff;
    }
    return worst;
}

/* Compare a seek result at `frame` against `reference`, using several block
 * patterns so chunking cannot accidentally hide a mismatch. */
static int compare_seek(DUH *duh, DUMB_IT_SEEK_INDEX *index,
                        unsigned long long frame, int channels, int rate,
                        const sample_t *reference, long reference_frames,
                        unsigned expect_preroll_max, unsigned *saw_preroll) {
    static const long patterns[][6] = {
        {1}, {1, 7, 64, 3, 129, 2}, {1024}, {5, 1},
    };
    long avail = (long)((unsigned long long)reference_frames > frame
                            ? (unsigned long long)reference_frames - frame
                            : 0);
    long compare_len = avail < 512 ? avail : 512;
    sample_t *out;
    unsigned p;

    if (compare_len <= 0)
        return 0;

    out = malloc((size_t)compare_len * channels * sizeof(sample_t));
    if (!out)
        return 1;

    for (p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        unsigned preroll = 12345;
        DUH_SIGRENDERER *renderer =
            dumb_it_seek_index_start(index, frame, &preroll);
        long first;
        long long diff;
        long got;

        if (!renderer) {
            free(out);
            return 1;
        }
        if (preroll > expect_preroll_max) {
            duh_end_sigrenderer(renderer);
            free(out);
            return 1;
        }
        if (saw_preroll)
            *saw_preroll |= preroll > 0 ? 1u : 0u;

        got = render_blocks(renderer, channels, compare_len, patterns[p],
                            p == 0 ? 1 : (p == 1 ? 6 : (p == 2 ? 1 : 2)), out);
        duh_end_sigrenderer(renderer);

        if (got != compare_len) {
            free(out);
            return 1;
        }
        diff = max_difference(out,
                              reference + frame * (unsigned long long)channels,
                              compare_len * channels, &first);
        if (diff != 0) {
            free(out);
            return 1;
        }
    }

    free(out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* PCM group                                                           */
/* ------------------------------------------------------------------ */

static int case_pcm_mod(void) {
    DUH *duh = task_load_mod();
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 30000 };
        const unsigned seed = 0x1234u;
        const unsigned stride = 137;
        DUMB_IT_SEEK_INDEX *index;
        sample_t *reference;
        unsigned long long covered;
        unsigned saw_preroll = 0;
        int quality;

        reference = malloc((size_t)frames * channels * sizeof(sample_t));
        TASK_CHECK(reference != NULL);

        for (quality = 0; quality < 2; quality++) {
            int q = quality == 0 ? DUMB_RQ_FIR : DUMB_RQ_CUBIC;
            long produced = render_reference(duh, channels, rate, q, seed,
                                             frames, reference);
            TASK_CHECK(produced == frames);

            index = dumb_it_build_seek_index(duh, channels, rate, q, seed,
                                             frames, stride);
            TASK_CHECK(index != NULL);
            covered = dumb_it_seek_index_frames(index);
            TASK_CHECK(covered == (unsigned long long)frames);

            /* 1 frame, irregular chunks, checkpoint boundaries and both
             * sides of the last covered frame. */
            TASK_CHECK(compare_seek(duh, index, 0, channels, rate, reference,
                                    frames, stride - 1, &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, 1, channels, rate, reference,
                                    frames, stride - 1, &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, stride - 1, channels, rate,
                                    reference, frames, stride - 1,
                                    &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, stride, channels, rate,
                                    reference, frames, 0, &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, stride + 1, channels, rate,
                                    reference, frames, stride - 1,
                                    &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, stride * 7 + 13, channels,
                                    rate, reference, frames, stride - 1,
                                    &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, covered - 1, channels, rate,
                                    reference, frames, stride - 1,
                                    &saw_preroll) == 0);

            dumb_it_free_seek_index(index);
        }

        /* Real prereoll happened for at least one frame. */
        TASK_CHECK(saw_preroll);
        free(reference);
    }
    unload_duh(duh);
    return 0;
}

static int case_pcm_it_random(void) {
    /* Random tremolo + random instrument volume/pan + NNA voices. */
    DUH *duh = task_load_it_full(9, 7, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 48000, frames = 40000 };
        const unsigned stride = 251;
        DUMB_IT_SEEK_INDEX *index;
        sample_t *reference;
        unsigned long long covered;
        unsigned saw_preroll = 0;
        int quality;

        reference = malloc((size_t)frames * channels * sizeof(sample_t));
        TASK_CHECK(reference != NULL);

        for (quality = 0; quality < 2; quality++) {
            int q = quality == 0 ? DUMB_RQ_FIR : DUMB_RQ_CUBIC;
            long produced = render_reference(duh, channels, rate, q, 0xBEEFu,
                                             frames, reference);
            TASK_CHECK(produced == frames);

            index = dumb_it_build_seek_index(duh, channels, rate, q, 0xBEEFu,
                                             frames, stride);
            TASK_CHECK(index != NULL);
            covered = dumb_it_seek_index_frames(index);
            TASK_CHECK(covered == (unsigned long long)frames);

            TASK_CHECK(compare_seek(duh, index, 0, channels, rate, reference,
                                    frames, 0, &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, 1, channels, rate, reference,
                                    frames, stride - 1, &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, stride, channels, rate,
                                    reference, frames, 0, &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, stride * 3 + 5, channels, rate,
                                    reference, frames, stride - 1,
                                    &saw_preroll) == 0);
            TASK_CHECK(compare_seek(duh, index, covered - 2, channels, rate,
                                    reference, frames, stride - 1,
                                    &saw_preroll) == 0);

            dumb_it_free_seek_index(index);
        }
        TASK_CHECK(saw_preroll);
        free(reference);
    }
    unload_duh(duh);
    return 0;
}

static int case_pcm_it_norandom(void) {
    /* No random effect at all: the seeded renderer must equal the ordinary
     * duh_start_sigrenderer() result with the same delta and volume. */
    DUH *duh = task_load_it_full(0, 0, 0, 0, 0);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 20000 };
        static const long irregular[] = {1, 7, 64, 3, 129, 2};
        sample_t *legacy;
        sample_t *seeded;
        DUH_SIGRENDERER *renderer;
        long n, m;

        legacy = malloc((size_t)frames * channels * sizeof(sample_t));
        seeded = malloc((size_t)frames * channels * sizeof(sample_t));
        TASK_CHECK(legacy != NULL && seeded != NULL);

        renderer = duh_start_sigrenderer(duh, 0, channels, 0);
        TASK_CHECK(renderer != NULL);
        dumb_it_set_resampling_quality(duh_get_it_sigrenderer(renderer),
                                       DUMB_RQ_FIR);
        n = render_blocks_params(renderer, channels, frames, irregular, 6,
                                 1.0f, (float)(65536.0 / rate), legacy);
        duh_end_sigrenderer(renderer);
        TASK_CHECK(n == frames);

        renderer = duh_it_start_seeded(duh, channels, rate, DUMB_RQ_FIR, 99);
        TASK_CHECK(renderer != NULL);
        m = render_blocks(renderer, channels, frames, irregular, 6, seeded);
        duh_end_sigrenderer(renderer);
        TASK_CHECK(m == frames);

        TASK_CHECK(max_difference(legacy, seeded,
                                  (long)frames * channels, NULL) == 0);

        free(legacy);
        free(seeded);
    }
    unload_duh(duh);
    return 0;
}

static int case_pcm_it_pingpong(void) {
    DUH *duh = task_load_it_pingpong();
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 30000 };
        const unsigned stride = 173;
        DUMB_IT_SEEK_INDEX *index;
        sample_t *reference;
        unsigned long long covered;
        unsigned saw_preroll = 0;

        reference = malloc((size_t)frames * channels * sizeof(sample_t));
        TASK_CHECK(reference != NULL);

        TASK_CHECK(render_reference(duh, channels, rate, DUMB_RQ_FIR, 0x51u,
                                    frames, reference) == frames);

        index = dumb_it_build_seek_index(duh, channels, rate, DUMB_RQ_FIR,
                                         0x51u, frames, stride);
        TASK_CHECK(index != NULL);
        covered = dumb_it_seek_index_frames(index);
        TASK_CHECK(covered == (unsigned long long)frames);

        TASK_CHECK(compare_seek(duh, index, 0, channels, rate, reference,
                                frames, 0, &saw_preroll) == 0);
        TASK_CHECK(compare_seek(duh, index, 1, channels, rate, reference,
                                frames, stride - 1, &saw_preroll) == 0);
        TASK_CHECK(compare_seek(duh, index, stride, channels, rate, reference,
                                frames, 0, &saw_preroll) == 0);
        TASK_CHECK(compare_seek(duh, index, stride * 2 + 17, channels, rate,
                                reference, frames, stride - 1,
                                &saw_preroll) == 0);
        TASK_CHECK(compare_seek(duh, index, covered - 3, channels, rate,
                                reference, frames, stride - 1,
                                &saw_preroll) == 0);
        TASK_CHECK(saw_preroll);

        dumb_it_free_seek_index(index);
        free(reference);
    }
    unload_duh(duh);
    return 0;
}

static int case_pcm_seed_repeatable(void) {
    DUH *duh = task_load_it_full(9, 7, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 20000 };
        static const long chunks_a[] = {1};
        static const long chunks_b[] = {7, 1, 129, 3};
        sample_t *a = malloc((size_t)frames * channels * sizeof(sample_t));
        sample_t *b = malloc((size_t)frames * channels * sizeof(sample_t));
        DUH_SIGRENDERER *renderer;
        long n, m;

        TASK_CHECK(a != NULL && b != NULL);

        /* Same seed, same audio, regardless of block sizes. */
        renderer = duh_it_start_seeded(duh, channels, rate, DUMB_RQ_FIR, 7);
        TASK_CHECK(renderer != NULL);
        n = render_blocks(renderer, channels, frames, chunks_a, 1, a);
        duh_end_sigrenderer(renderer);

        renderer = duh_it_start_seeded(duh, channels, rate, DUMB_RQ_FIR, 7);
        TASK_CHECK(renderer != NULL);
        m = render_blocks(renderer, channels, frames, chunks_b, 4, b);
        duh_end_sigrenderer(renderer);

        TASK_CHECK(n == frames && m == frames);
        TASK_CHECK(max_difference(a, b, (long)frames * channels, NULL) == 0);

        /* Interleaving an unrelated seeded renderer must not change anything. */
        {
            DUH_SIGRENDERER *interleaved =
                duh_it_start_seeded(duh, channels, rate, DUMB_RQ_FIR, 7);
            sample_t *tmp =
                malloc((size_t)frames * channels * sizeof(sample_t));
            TASK_CHECK(interleaved != NULL && tmp != NULL);
            memset(b, 0, (size_t)frames * channels * sizeof(sample_t));
            {
                sample_t **scratch = allocate_sample_buffer(channels, 64);
                long done = 0;
                int flip = 0;
                TASK_CHECK(scratch != NULL);
                while (done < frames) {
                    long want = flip ? 33 : 17;
                    long got;
                    if (want > frames - done)
                        want = frames - done;
                    if (want > 64)
                        want = 64;
                    memset(scratch[0], 0,
                           (size_t)want * channels * sizeof(sample_t));
                    got = duh_sigrenderer_generate_samples(
                        interleaved, 1.0f, 2.0f, want, scratch);
                    if (got <= 0)
                        break;
                    flip = !flip;
                    done += got;
                }
                destroy_sample_buffer(scratch);
            }
            duh_end_sigrenderer(interleaved);
            free(tmp);
        }

        renderer = duh_it_start_seeded(duh, channels, rate, DUMB_RQ_FIR, 7);
        TASK_CHECK(renderer != NULL);
        memset(b, 0, (size_t)frames * channels * sizeof(sample_t));
        m = render_blocks(renderer, channels, frames, chunks_b, 4, b);
        duh_end_sigrenderer(renderer);
        TASK_CHECK(m == frames);
        TASK_CHECK(max_difference(a, b, (long)frames * channels, NULL) == 0);

        free(a);
        free(b);
    }
    unload_duh(duh);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clock group                                                         */
/* ------------------------------------------------------------------ */

static int case_clock_coverage(void) {
    DUH *duh = task_load_it_full(0, 0, 0, 0, 0);
    TASK_CHECK(duh != NULL);
    {
        long song_length = render_reference(duh, 2, 44100, DUMB_RQ_FIR, 1,
                                            2000000, NULL);
        DUMB_IT_SEEK_INDEX *index;
        unsigned long long covered;

        TASK_CHECK(song_length > 0);

        /* A limit below the song end is honoured exactly. */
        index = dumb_it_build_seek_index(duh, 2, 44100, DUMB_RQ_FIR, 1, 12000,
                                         500);
        TASK_CHECK(index != NULL);
        covered = dumb_it_seek_index_frames(index);
        TASK_CHECK(covered == 12000);
        dumb_it_free_seek_index(index);

        /* A limit past the song end stops at the first-pass end. */
        index = dumb_it_build_seek_index(
            duh, 2, 44100, DUMB_RQ_FIR, 1,
            (unsigned long long)song_length + 5000, 500);
        TASK_CHECK(index != NULL);
        covered = dumb_it_seek_index_frames(index);
        TASK_CHECK(covered == (unsigned long long)song_length);
        dumb_it_free_seek_index(index);
    }
    unload_duh(duh);
    return 0;
}

static int case_clock_boundary_preroll(void) {
    /* When the covered range ends exactly on a stride boundary, the final
     * frame must still be reachable with less than a full stride of preroll. */
    DUH *duh = task_load_it_full(7, 8, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, limit = 5120 };
        const unsigned strides[] = {256, 500, 128};
        size_t k;

        for (k = 0; k < sizeof(strides) / sizeof(strides[0]); k++) {
            unsigned stride = strides[k];
            DUMB_IT_SEEK_INDEX *index = dumb_it_build_seek_index(
                duh, channels, rate, DUMB_RQ_FIR, 3, limit, stride);
            unsigned long long covered;
            unsigned preroll = 12345;
            DUH_SIGRENDERER *r;
            long got;
            sample_t *out;

            TASK_CHECK(index != NULL);
            covered = dumb_it_seek_index_frames(index);
            TASK_CHECK(covered == limit);

            r = dumb_it_seek_index_start(index, covered, &preroll);
            TASK_CHECK(r != NULL);
            TASK_CHECK(preroll < stride);

            out = malloc((size_t)64 * channels * sizeof(sample_t));
            TASK_CHECK(out != NULL);
            {
                static const long ones[] = {1};
                got = render_blocks(r, channels, 64, ones, 1, out);
            }
            TASK_CHECK(got == 64); /* coverage limit does not stop playback */
            free(out);
            duh_end_sigrenderer(r);
            dumb_it_free_seek_index(index);
        }
    }
    unload_duh(duh);
    return 0;
}

static int case_clock_preroll(void) {
    DUH *duh = task_load_it_full(9, 7, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { stride = 200 };
        DUMB_IT_SEEK_INDEX *index =
            dumb_it_build_seek_index(duh, 2, 44100, DUMB_RQ_FIR, 3, 20000,
                                     stride);
        unsigned preroll;
        unsigned long long f;

        TASK_CHECK(index != NULL);

        /* At a checkpoint no audio is prerolled. */
        preroll = 99;
        {
            DUH_SIGRENDERER *r =
                dumb_it_seek_index_start(index, stride, &preroll);
            TASK_CHECK(r != NULL);
            TASK_CHECK(preroll == 0);
            duh_end_sigrenderer(r);
        }

        /* Between checkpoints the preroll is exactly frame % stride and is
         * always less than one stride. */
        for (f = 1; f < 20000; f = f * 3 + 1) {
            unsigned expected = (unsigned)(f % stride);
            preroll = 0;
            {
                DUH_SIGRENDERER *r =
                    dumb_it_seek_index_start(index, f, &preroll);
                TASK_CHECK(r != NULL);
                TASK_CHECK(preroll == expected);
                TASK_CHECK(preroll < stride);
                duh_end_sigrenderer(r);
            }
        }

        /* A NULL preroll pointer is allowed. */
        {
            DUH_SIGRENDERER *r =
                dumb_it_seek_index_start(index, 12345, NULL);
            TASK_CHECK(r != NULL);
            duh_end_sigrenderer(r);
        }

        dumb_it_free_seek_index(index);
    }
    unload_duh(duh);
    return 0;
}

static int case_clock_end_and_limits(void) {
    DUH *duh = task_load_it_full(0, 0, 0, 0, 0);
    TASK_CHECK(duh != NULL);
    {
        static const long ones[] = {1};
        long song_length = render_reference(duh, 2, 44100, DUMB_RQ_FIR, 1,
                                            2000000, NULL);
        DUMB_IT_SEEK_INDEX *limited;
        DUMB_IT_SEEK_INDEX *full;
        DUH_SIGRENDERER *r;
        sample_t **buffer;

        TASK_CHECK(song_length > 0);

        limited = dumb_it_build_seek_index(duh, 2, 44100, DUMB_RQ_FIR, 1,
                                           10000, 400);
        TASK_CHECK(limited != NULL);
        /* Reaching the coverage limit does not stop playback. */
        r = dumb_it_seek_index_start(limited, 10000, NULL);
        TASK_CHECK(r != NULL);
        {
            sample_t *out = malloc((size_t)600 * 2 * sizeof(sample_t));
            TASK_CHECK(out != NULL);
            TASK_CHECK(render_blocks(r, 2, 600, ones, 1, out) == 600);
            free(out);
        }
        duh_end_sigrenderer(r);
        dumb_it_free_seek_index(limited);

        full = dumb_it_build_seek_index(
            duh, 2, 44100, DUMB_RQ_FIR, 1,
            (unsigned long long)song_length + 1000, 400);
        TASK_CHECK(full != NULL);

        /* One frame before the true end still plays. */
        r = dumb_it_seek_index_start(
            full, (unsigned long long)song_length - 1, NULL);
        TASK_CHECK(r != NULL);
        {
            sample_t *out = malloc((size_t)32 * 2 * sizeof(sample_t));
            TASK_CHECK(out != NULL);
            TASK_CHECK(render_blocks(r, 2, 32, ones, 1, out) == 1);
            free(out);
        }
        duh_end_sigrenderer(r);

        /* At the true end of the song, further reads produce 0 frames. */
        r = dumb_it_seek_index_start(full,
                                     (unsigned long long)song_length, NULL);
        TASK_CHECK(r != NULL);
        buffer = allocate_sample_buffer(2, 128);
        TASK_CHECK(buffer != NULL);
        {
            long got = duh_sigrenderer_generate_samples(r, 1.0f, 2.0f, 128,
                                                        buffer);
            TASK_CHECK(got == 0);
        }
        destroy_sample_buffer(buffer);
        duh_end_sigrenderer(r);

        /* Past the covered range the seek is rejected. */
        TASK_CHECK(dumb_it_seek_index_start(
                       full, (unsigned long long)song_length + 1, NULL) ==
                   NULL);

        dumb_it_free_seek_index(full);
    }
    unload_duh(duh);
    return 0;
}

static int case_clock_arguments(void) {
    DUH *duh = task_load_it_full(0, 0, 0, 0, 0);
    TASK_CHECK(duh != NULL);

    /* Invalid channels / rate / quality / stride / limit. */
    TASK_CHECK(duh_it_start_seeded(NULL, 2, 44100, DUMB_RQ_FIR, 1) == NULL);
    TASK_CHECK(duh_it_start_seeded(duh, 0, 44100, DUMB_RQ_FIR, 1) == NULL);
    TASK_CHECK(duh_it_start_seeded(duh, 3, 44100, DUMB_RQ_FIR, 1) == NULL);
    TASK_CHECK(duh_it_start_seeded(duh, 2, 7999, DUMB_RQ_FIR, 1) == NULL);
    TASK_CHECK(duh_it_start_seeded(duh, 2, 192001, DUMB_RQ_FIR, 1) == NULL);
    TASK_CHECK(duh_it_start_seeded(duh, 2, 44100, DUMB_RQ_LINEAR, 1) == NULL);
    TASK_CHECK(duh_it_start_seeded(duh, 2, 44100, DUMB_RQ_BLEP, 1) == NULL);

    TASK_CHECK(dumb_it_build_seek_index(duh, 2, 44100, DUMB_RQ_FIR, 1, 0,
                                        10) == NULL);
    TASK_CHECK(dumb_it_build_seek_index(duh, 2, 44100, DUMB_RQ_FIR, 1, 100,
                                        0) == NULL);
    TASK_CHECK(dumb_it_build_seek_index(duh, 2, 44100, DUMB_RQ_FIR, 1, 100,
                                        4) != NULL);
    TASK_CHECK(dumb_it_seek_index_frames(NULL) == 0);
    TASK_CHECK(dumb_it_seek_index_start(NULL, 0, NULL) == NULL);

    /* Quality range endpoints accepted. */
    {
        DUH_SIGRENDERER *r =
            duh_it_start_seeded(duh, 1, 8000, DUMB_RQ_CUBIC, 0);
        TASK_CHECK(r != NULL);
        duh_end_sigrenderer(r);
        r = duh_it_start_seeded(duh, 2, 192000, DUMB_RQ_FIR, 0xFFFFFFFFu);
        TASK_CHECK(r != NULL);
        duh_end_sigrenderer(r);
    }

    unload_duh(duh);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ownership group                                                     */
/* ------------------------------------------------------------------ */

static int case_ownership_index_immutable(void) {
    DUH *duh = task_load_it_full(9, 7, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 30000 };
        DUMB_IT_SEEK_INDEX *index =
            dumb_it_build_seek_index(duh, channels, rate, DUMB_RQ_FIR, 21,
                                     frames, 90);
        sample_t *reference =
            malloc((size_t)frames * channels * sizeof(sample_t));
        unsigned long long covered;
        int i;

        TASK_CHECK(index != NULL && reference != NULL);
        covered = dumb_it_seek_index_frames(index);
        TASK_CHECK(covered == frames);
        TASK_CHECK(render_reference(duh, channels, rate, DUMB_RQ_FIR, 21,
                                    frames, reference) == frames);

        /* Repeated seeks, in any order, must not perturb the index. */
        for (i = 0; i < 4; i++) {
            TASK_CHECK(compare_seek(duh, index, 2000, channels, rate,
                                    reference, frames, 89, NULL) == 0);
            TASK_CHECK(compare_seek(duh, index, 17000, channels, rate,
                                    reference, frames, 89, NULL) == 0);
            TASK_CHECK(dumb_it_seek_index_frames(index) == covered);
        }

        free(reference);
        dumb_it_free_seek_index(index);
    }
    unload_duh(duh);
    return 0;
}

static int case_ownership_independent_results(void) {
    DUH *duh = task_load_it_full(9, 7, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 30000 };
        static const long ones[] = {1};
        DUMB_IT_SEEK_INDEX *index =
            dumb_it_build_seek_index(duh, channels, rate, DUMB_RQ_FIR, 33,
                                     frames, 111);
        sample_t *reference =
            malloc((size_t)frames * channels * sizeof(sample_t));
        sample_t *out_a = malloc((size_t)400 * channels * sizeof(sample_t));
        sample_t *out_b = malloc((size_t)400 * channels * sizeof(sample_t));
        DUH_SIGRENDERER *a;
        DUH_SIGRENDERER *b;
        long na, nb;

        TASK_CHECK(index != NULL && reference != NULL);
        TASK_CHECK(render_reference(duh, channels, rate, DUMB_RQ_FIR, 33,
                                    frames, reference) == frames);

        a = dumb_it_seek_index_start(index, 5000, NULL);
        b = dumb_it_seek_index_start(index, 5000, NULL);
        TASK_CHECK(a != NULL && b != NULL);

        /* Interleave reads; both must keep producing the same suffix. */
        na = render_blocks(a, channels, 200, ones, 1, out_a);
        nb = render_blocks(b, channels, 200, ones, 1, out_b);
        TASK_CHECK(na == 200 && nb == 200);
        TASK_CHECK(max_difference(out_a, out_b, 200 * channels, NULL) == 0);

        na = render_blocks(a, channels, 200, ones, 1, out_a);
        nb = render_blocks(b, channels, 200, ones, 1, out_b);
        TASK_CHECK(na == 200 && nb == 200);
        TASK_CHECK(max_difference(out_a, out_b, 200 * channels, NULL) == 0);
        TASK_CHECK(max_difference(out_a,
                                  reference + ((long)5000 + 200) * channels,
                                  200 * channels, NULL) == 0);

        /* Freeing one must leave the other usable. */
        duh_end_sigrenderer(a);
        memset(out_b, 0, (size_t)400 * channels * sizeof(sample_t));
        nb = render_blocks(b, channels, 400, ones, 1, out_b);
        TASK_CHECK(nb == 400);
        TASK_CHECK(max_difference(out_b,
                                  reference + ((long)5000 + 400) * channels,
                                  400 * channels, NULL) == 0);
        duh_end_sigrenderer(b);

        free(reference);
        free(out_a);
        free(out_b);
        dumb_it_free_seek_index(index);
    }
    unload_duh(duh);
    return 0;
}

static int case_ownership_index_free_first(void) {
    DUH *duh = task_load_it_full(9, 7, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 30000 };
        static const long ones[] = {1};
        DUMB_IT_SEEK_INDEX *index =
            dumb_it_build_seek_index(duh, channels, rate, DUMB_RQ_FIR, 44,
                                     frames, 120);
        sample_t *reference =
            malloc((size_t)frames * channels * sizeof(sample_t));
        sample_t *out = malloc((size_t)300 * channels * sizeof(sample_t));
        DUH_SIGRENDERER *r;

        TASK_CHECK(index != NULL && reference != NULL);
        TASK_CHECK(render_reference(duh, channels, rate, DUMB_RQ_FIR, 44,
                                    frames, reference) == frames);

        r = dumb_it_seek_index_start(index, 9000, NULL);
        TASK_CHECK(r != NULL);

        /* The renderer owns no reference to the index internals. */
        dumb_it_free_seek_index(index);

        TASK_CHECK(render_blocks(r, channels, 300, ones, 1, out) == 300);
        TASK_CHECK(max_difference(out, reference + 9000 * channels,
                                  300 * channels, NULL) == 0);
        duh_end_sigrenderer(r);

        free(reference);
        free(out);
    }
    unload_duh(duh);
    return 0;
}

static int case_ownership_two_indexes(void) {
    DUH *duh = task_load_it_full(9, 7, 4, 3, 1);
    TASK_CHECK(duh != NULL);
    {
        enum { channels = 2, rate = 44100, frames = 25000 };
        DUMB_IT_SEEK_INDEX *one =
            dumb_it_build_seek_index(duh, channels, rate, DUMB_RQ_FIR, 55,
                                     frames, 64);
        DUMB_IT_SEEK_INDEX *two =
            dumb_it_build_seek_index(duh, channels, rate, DUMB_RQ_FIR, 55,
                                     frames, 512);
        sample_t *reference =
            malloc((size_t)frames * channels * sizeof(sample_t));
        int ok;

        TASK_CHECK(one != NULL && two != NULL);
        TASK_CHECK(render_reference(duh, channels, rate, DUMB_RQ_FIR, 55,
                                    frames, reference) == frames);

        ok = compare_seek(duh, one, 12345, channels, rate, reference, frames,
                          63, NULL) == 0;
        ok = compare_seek(duh, two, 12345, channels, rate, reference, frames,
                          511, NULL) == 0 &&
             ok;
        TASK_CHECK(ok);

        /* Freeing one index does not disturb the other. */
        dumb_it_free_seek_index(one);
        TASK_CHECK(compare_seek(duh, two, 6789, channels, rate, reference,
                                frames, 511, NULL) == 0);
        dumb_it_free_seek_index(two);

        free(reference);
    }
    unload_duh(duh);
    return 0;
}

static int case_ownership_seeded_stops(void) {
    DUH *duh = task_load_it_full(0, 0, 0, 0, 0);
    TASK_CHECK(duh != NULL);
    {
        static const long ones[] = {1};
        long song_length = render_reference(duh, 2, 44100, DUMB_RQ_FIR, 1,
                                            2000000, NULL);
        DUH_SIGRENDERER *r;
        sample_t *out;
        long before, after;

        TASK_CHECK(song_length > 0);

        r = duh_it_start_seeded(duh, 2, 44100, DUMB_RQ_FIR, 1);
        TASK_CHECK(r != NULL);
        out = malloc((size_t)song_length * 2 * sizeof(sample_t));
        TASK_CHECK(out != NULL);
        before = render_blocks(r, 2, song_length, ones, 1, out);
        TASK_CHECK(before == song_length);
        /* First pass over, the seeded renderer stops: further reads are zero
         * frames. */
        after = render_blocks(r, 2, 64, ones, 1, NULL);
        TASK_CHECK(after == 0);
        duh_end_sigrenderer(r);
        free(out);
    }
    unload_duh(duh);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"pcm", "mod", case_pcm_mod},
    {"pcm", "it_random", case_pcm_it_random},
    {"pcm", "it_norandom", case_pcm_it_norandom},
    {"pcm", "it_pingpong", case_pcm_it_pingpong},
    {"pcm", "seed_repeatable", case_pcm_seed_repeatable},
    {"clock", "coverage", case_clock_coverage},
    {"clock", "preroll", case_clock_preroll},
    {"clock", "boundary_preroll", case_clock_boundary_preroll},
    {"clock", "end_and_limits", case_clock_end_and_limits},
    {"clock", "arguments", case_clock_arguments},
    {"ownership", "index_immutable", case_ownership_index_immutable},
    {"ownership", "independent_results", case_ownership_independent_results},
    {"ownership", "index_free_first", case_ownership_index_free_first},
    {"ownership", "two_indexes", case_ownership_two_indexes},
    {"ownership", "seeded_stops", case_ownership_seeded_stops},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
