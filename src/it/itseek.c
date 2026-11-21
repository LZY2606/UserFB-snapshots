/*  _______         ____    __         ___    ___
 * \    _  \       \    /  \  /       \   \  /   /       '   '  '
 *  |  | \  \       |  |    ||         |   \/   |         .      .
 *  |  |  |  |      |  |    ||         ||\  /|  |
 *  |  |  |  |      |  |    ||         || \/ |  |         '  '  '
 *  |  |  |  |      |  |    ||         ||    |  |         .      .
 *  |  |_/  /        \  \__//          ||    |  |
 * /_______/ynamic    \____/niversal  /__\  /____\usic   /|  .  . ibliotheque
 *                                                      /  \
 *                                                     / .  \
 * itseek.c - Deterministic, PCM-frame addressed      / / \  \
 *            playback for IT signals.               | <  /   \_
 *                                                   |  \/ /\   /
 * This implements the seeded renderer and the        \_  /  > /
 * checkpoint index behind duh_it_start_seeded(),       | \ / /
 * dumb_it_build_seek_index(),                          |  ' /
 * dumb_it_seek_index_start() and                        \__/
 * dumb_it_free_seek_index().
 *
 * The index is built by rendering one sequential pass and duplicating the
 * whole renderer every `stride` output frames.  Seeking picks the newest
 * checkpoint at or before the target frame and renders at most stride - 1
 * frames of real audio to reach the exact frame, so the first sample handed
 * back is the same sample sequential playback would have produced there.
 * The checkpoints are deep copies of the playback state (voices, resamplers,
 * filters and the RNG position), so random effects and filter/declick tails
 * line up exactly.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "dumb.h"
#include "internal/dumb.h"
#include "internal/it.h"

#define IT_SEEK_MIN_RATE 8000
#define IT_SEEK_MAX_RATE 192000

typedef struct DUMB_IT_SEEK_CHECKPOINT {
    unsigned long long frame;
    DUMB_IT_SIGRENDERER *sigrenderer;
} DUMB_IT_SEEK_CHECKPOINT;

struct DUMB_IT_SEEK_INDEX {
    DUH *duh;
    DUMB_IT_SIGDATA *sigdata;
    int channels;
    int rate;
    int quality;
    unsigned seed;
    unsigned stride;

    unsigned long long frames; /* frames covered (0 .. frame_limit) */

    DUMB_IT_SEEK_CHECKPOINT *checkpoints;
    unsigned n_checkpoints;
    unsigned cap_checkpoints;
};

/* ------------------------------------------------------------------ */
/* Parameter validation                                               */
/* ------------------------------------------------------------------ */

static int it_seek_validate(DUH *duh, int channels, int rate, int quality) {
    DUMB_IT_SIGDATA *sigdata;

    if (!duh)
        return 0;
    if (channels != 1 && channels != 2)
        return 0;
    if (rate < IT_SEEK_MIN_RATE || rate > IT_SEEK_MAX_RATE)
        return 0;
    if (quality != DUMB_RQ_CUBIC && quality != DUMB_RQ_FIR)
        return 0;

    sigdata = duh_get_it_sigdata(duh);
    if (!sigdata)
        return 0;
    return 1;
}

static float it_seek_delta(int rate) {
    return (float)(65536.0 / (double)rate);
}

/* ------------------------------------------------------------------ */
/* Renderer wrappers                                                  */
/* ------------------------------------------------------------------ */

static DUH_SIGRENDERER *it_seek_wrap(DUMB_IT_SIGRENDERER *it, int channels,
                                     int rate, unsigned long long frame) {
    DUH_SIGRENDERER *sigrenderer;
    LONG_LONG total;
    int dt;

    if (!it)
        return NULL;

    /* Sequential playback accumulates dt = round(delta * 65536) subframes per
     * output frame without losing the fractional part, so starting a seek at
     * `frame` must present the same outer position and remainder.  Reject a
     * frame whose time position cannot be represented by the outer renderer
     * (a `long` of whole frames plus a 16-bit subframe remainder). */
    dt = (int)(it_seek_delta(rate) * 65536.0 + 0.5);
    if (dt <= 0 ||
        frame > (unsigned long long)(LONG_MAX / dt))
        goto reject;
    total = (LONG_LONG)frame * dt;

    sigrenderer = duh_encapsulate_it_sigrenderer(it, channels, (long)0);
    if (!sigrenderer) {
        _dumb_it_free_sigrenderer(it);
        return NULL;
    }

    _dumb_duh_sigrenderer_set_fixed_params(sigrenderer, 1, 1.0f,
                                           it_seek_delta(rate));
    _dumb_duh_sigrenderer_set_position(sigrenderer, (long)(total >> 16),
                                       (int)(total & 65535));
    return sigrenderer;

reject:
    _dumb_it_free_sigrenderer(it);
    return NULL;
}

/* Deep-copy a live renderer for use as a checkpoint. dup_sigrenderer() copies
 * every voice, resampler, filter and RNG field; the click remover lives
 * outside the duplicated state, so copy its tails explicitly. */
static DUMB_IT_SIGRENDERER *it_seek_checkpoint_copy(DUMB_IT_SIGRENDERER *src) {
    DUMB_IT_SIGRENDERER *copy =
        _dumb_it_dup_sigrenderer(src, src->n_channels);
    int i;
    if (!copy)
        return NULL;
    if (copy->click_remover && src->click_remover) {
        for (i = 0; i < copy->n_channels; i++)
            dumb_click_remover_copy(copy->click_remover[i],
                                    src->click_remover[i]);
    }
    return copy;
}

/* ------------------------------------------------------------------ */
/* Seeded sequential renderer                                          */
/* ------------------------------------------------------------------ */

DUH_SIGRENDERER *duh_it_start_seeded(DUH *duh, int channels, int rate,
                                     int quality, unsigned seed) {
    DUMB_IT_SIGDATA *sigdata;
    DUMB_IT_SIGRENDERER *it;

    if (!it_seek_validate(duh, channels, rate, quality))
        return NULL;

    sigdata = duh_get_it_sigdata(duh);
    it = _dumb_it_init_seeded_sigrenderer(sigdata, channels, quality, seed);
    if (!it)
        return NULL;

    return it_seek_wrap(it, channels, rate, 0);
}

/* ------------------------------------------------------------------ */
/* Index construction                                                  */
/* ------------------------------------------------------------------ */

static int it_seek_add_checkpoint(DUMB_IT_SEEK_INDEX *index,
                                  unsigned long long frame,
                                  DUMB_IT_SIGRENDERER *renderer) {
    DUMB_IT_SIGRENDERER *copy;

    if (index->n_checkpoints == index->cap_checkpoints) {
        unsigned cap = index->cap_checkpoints ? index->cap_checkpoints * 2 : 16;
        DUMB_IT_SEEK_CHECKPOINT *grown =
            realloc(index->checkpoints, cap * sizeof(*grown));
        if (!grown)
            return 0;
        index->checkpoints = grown;
        index->cap_checkpoints = cap;
    }

    copy = it_seek_checkpoint_copy(renderer);
    if (!copy)
        return 0;

    index->checkpoints[index->n_checkpoints].frame = frame;
    index->checkpoints[index->n_checkpoints].sigrenderer = copy;
    index->n_checkpoints++;
    return 1;
}

/* Render up to `frames` frames through the reference renderer, capturing a
 * checkpoint every `stride` frames.  Audio is rendered for real (into a
 * scratch buffer of at most `stride` frames) rather than fast-forwarded
 * silently: the FIR resampler and click-removal tails only evolve when
 * samples are actually produced, so silent advancement would make later
 * checkpoints diverge from sequential playback.
 *
 * Returns the number of frames actually rendered (less than requested if the
 * song ended first). */
static unsigned long long it_seek_build(DUH_SIGRENDERER *ref,
                                        DUMB_IT_SEEK_INDEX *index,
                                        unsigned long long frames) {
    DUMB_IT_SIGRENDERER *it = duh_get_it_sigrenderer(ref);
    unsigned long long done = 0;
    unsigned long long next_checkpoint = index->stride;
    long scratch_len = (long)(index->stride > 65536 ? 65536 : index->stride);
    sample_t **scratch;

    if (scratch_len < 1)
        scratch_len = 1;
    scratch = allocate_sample_buffer(index->channels, scratch_len);
    if (!scratch)
        return 0;

    /* Frame 0 is always a checkpoint. */
    if (!it_seek_add_checkpoint(index, 0, it)) {
        destroy_sample_buffer(scratch);
        return 0;
    }

    while (done < frames) {
        unsigned long long remaining = frames - done;
        unsigned long long room = next_checkpoint - done;
        long chunk;

        chunk = (long)(remaining > (unsigned long long)scratch_len
                           ? (unsigned long long)scratch_len
                           : remaining);
        if (room < (unsigned long long)chunk)
            chunk = (long)room;
        if (chunk <= 0)
            chunk = 1;

        dumb_silence(scratch[0], index->channels * chunk);
        {
            long produced = _dumb_it_generate(it, 1.0f,
                                              it_seek_delta(index->rate),
                                              chunk, scratch);
            if (produced <= 0)
                break;
            done += (unsigned long long)produced;
        }

        /* Once we have reached the boundary, snapshot the state there. */
        while (done >= next_checkpoint && next_checkpoint < frames) {
            if (!it_seek_add_checkpoint(index, next_checkpoint, it)) {
                destroy_sample_buffer(scratch);
                return done;
            }
            next_checkpoint += index->stride;
        }
    }

    /* Guarantee that the final covered frame also has a checkpoint, so no
     * seekable frame ever needs a full `stride` of preroll.  This matters when
     * the song (or the limit) stops exactly on a stride boundary. */
    if (done > 0 &&
        index->checkpoints[index->n_checkpoints - 1].frame != done) {
        if (!it_seek_add_checkpoint(index, done, it)) {
            destroy_sample_buffer(scratch);
            return done;
        }
    }

    destroy_sample_buffer(scratch);
    return done;
}

DUMB_IT_SEEK_INDEX *dumb_it_build_seek_index(DUH *duh, int channels, int rate,
                                             int quality, unsigned seed,
                                             unsigned long long frame_limit,
                                             unsigned stride) {
    DUMB_IT_SEEK_INDEX *index;
    DUH_SIGRENDERER *ref;
    unsigned long long covered;

    if (!it_seek_validate(duh, channels, rate, quality))
        return NULL;
    if (frame_limit == 0 || stride == 0)
        return NULL;

    index = malloc(sizeof(*index));
    if (!index)
        return NULL;
    memset(index, 0, sizeof(*index));
    index->duh = duh;
    index->sigdata = duh_get_it_sigdata(duh);
    index->channels = channels;
    index->rate = rate;
    index->quality = quality;
    index->seed = seed;
    index->stride = stride;

    ref = duh_it_start_seeded(duh, channels, rate, quality, seed);
    if (!ref) {
        free(index);
        return NULL;
    }

    covered = it_seek_build(ref, index, frame_limit);
    duh_end_sigrenderer(ref);

    if (covered == 0) {
        dumb_it_free_seek_index(index);
        return NULL;
    }

    index->frames = covered;
    return index;
}

/* ------------------------------------------------------------------ */
/* Seeks and cleanup                                                   */
/* ------------------------------------------------------------------ */

unsigned long long dumb_it_seek_index_frames(DUMB_IT_SEEK_INDEX *index) {
    return index ? index->frames : 0;
}

DUH_SIGRENDERER *dumb_it_seek_index_start(DUMB_IT_SEEK_INDEX *index,
                                          unsigned long long frame,
                                          unsigned *preroll) {
    const DUMB_IT_SEEK_CHECKPOINT *checkpoint;
    DUMB_IT_SIGRENDERER *it;
    unsigned lo, hi;

    if (preroll)
        *preroll = 0;

    if (!index || index->n_checkpoints == 0)
        return NULL;
    if (frame > index->frames)
        return NULL;

    /* Find the newest checkpoint at or before the requested frame.  The
     * checkpoints are stored in increasing frame order. */
    lo = 0;
    hi = index->n_checkpoints; /* first index with frame > target */
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (index->checkpoints[mid].frame <= frame)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL; /* frame precedes the first (frame 0) checkpoint */
    checkpoint = &index->checkpoints[lo - 1];

    it = it_seek_checkpoint_copy(checkpoint->sigrenderer);
    if (!it)
        return NULL;

    {
        unsigned long long preroll_frames = frame - checkpoint->frame;
        if (preroll_frames > 0) {
            /* Render at most one stride of real audio to land exactly on the
             * target frame; never fast-forward silently. */
            long scratch_len = (long)preroll_frames;
            sample_t **scratch =
                allocate_sample_buffer(index->channels, scratch_len);
            long produced;
            if (!scratch) {
                _dumb_it_free_sigrenderer(it);
                return NULL;
            }
            dumb_silence(scratch[0], index->channels * scratch_len);
            produced = _dumb_it_generate(it, 1.0f,
                                         it_seek_delta(index->rate),
                                         scratch_len, scratch);
            destroy_sample_buffer(scratch);
            if (produced != (long)preroll_frames) {
                _dumb_it_free_sigrenderer(it);
                return NULL;
            }
        }
        if (preroll)
            *preroll = (unsigned)preroll_frames;
    }

    return it_seek_wrap(it, index->channels, index->rate, frame);
}

void dumb_it_free_seek_index(DUMB_IT_SEEK_INDEX *index) {
    unsigned i;
    if (!index)
        return;
    for (i = 0; i < index->n_checkpoints; i++)
        _dumb_it_free_sigrenderer(index->checkpoints[i].sigrenderer);
    free(index->checkpoints);
    free(index);
}
