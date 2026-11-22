/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/**
 * LC3 Repair Window implementation
 *
 * The window is a ring of at most `capacity` frame slots. Every slot owns
 * private copies of the per-channel payloads, the interleaved S16 PCM of
 * the frame and a full per-channel snapshot of the decoder state taken
 * right after that frame. A separate snapshot keeps the decoder state
 * right before the oldest retained frame. Decoder state is therefore
 * never advanced permanently by a preview, and the fixed-size decoder
 * history (MDCT overlap, LTPF, PLC state) is preserved without ever
 * storing a played packet or re-decoding from frame 0:
 *
 *   - Replacing a frame invalidates it and every later frame. The next
 *     preview or read restores the snapshot right before the first
 *     invalid frame and re-decodes the final packet sequence in order.
 *     Per-channel loss, consecutive PLC runs and loss-after-recovery are
 *     replayed exactly as ordinary `lc3_decode()` calls would.
 *   - Appending a frame resumes from the snapshot of the previous frame.
 *   - Fully reading a frame moves the window-start snapshot to the state
 *     captured right after it and recycles the slot.
 *
 * All retained memory grows only with the configuration, the channel
 * count and the capacity.
 */

#include <lc3.h>
#include <lc3_repair.h>

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define LC3_REPAIR_MIN_CAPACITY  2
#define LC3_REPAIR_MAX_CAPACITY 32


struct lc3_repair_slot {
    uint64_t id;            /* Frame identifier, UINT64_MAX when free */
    bool valid;             /* PCM and state snapshots are up to date */
    bool immutable;         /* Playback started, replacement forbidden */
    int read_pos;           /* Samples per channel already delivered */

    uint8_t *buf[2];        /* Per-channel payload backing storage */
    uint8_t *data[2];       /* Payload pointer, NULL when lost */
    int nbytes[2];

    int16_t *pcm;           /* ns * nchannels interleaved S16 */
    void *state[2];         /* Decoder state right after this frame */
};

struct lc3_repair {
    int dt_us;
    int sr_hz;
    int sr_pcm_hz;
    int nchannels;
    int capacity;
    int ns;                 /* Samples per channel in a frame */
    unsigned decoder_size;

    struct lc3_repair_slot *slots;
    int head;               /* Ring index of the oldest frame */
    int count;              /* Number of retained frames */
    uint64_t next_id;       /* Identifier of the next appended frame */

    void *dec_mem[2];       /* Scratch decoder memory per channel */
    void *start_mem[2];     /* State before the oldest retained frame */
};


/* ----------------------------------------------------------------------------
 *  Helpers
 * -------------------------------------------------------------------------- */

static int slot_index(const struct lc3_repair *ctx, int age)
{
    return (ctx->head + age) % ctx->capacity;
}

static struct lc3_repair_slot *find_slot(
    struct lc3_repair *ctx, uint64_t id)
{
    if (ctx->count == 0)
        return NULL;

    uint64_t oldest = ctx->slots[ctx->head].id;

    if (id < oldest || id - oldest >= (uint64_t)ctx->count)
        return NULL;

    return &ctx->slots[slot_index(ctx, (int)(id - oldest))];
}

static int validate_frames(
    const struct lc3_repair *ctx, const struct lc3_repair_frame *frames)
{
    if (!frames)
        return -1;

    for (int ch = 0; ch < ctx->nchannels; ch++) {
        const struct lc3_repair_frame *f = &frames[ch];

        if (!f->data && f->nbytes == 0)
            continue;   /* Lost channel */

        if (!f->data || f->nbytes < LC3_MIN_FRAME_BYTES
                || f->nbytes > LC3_MAX_FRAME_BYTES)
            return -1;
    }

    return 0;
}

static void clear_slot(struct lc3_repair_slot *slot)
{
    slot->id = UINT64_MAX;
    slot->valid = false;
    slot->immutable = false;
    slot->read_pos = 0;
    slot->data[0] = slot->data[1] = NULL;
    slot->nbytes[0] = slot->nbytes[1] = 0;
}

static void store_frames(
    struct lc3_repair *ctx, struct lc3_repair_slot *slot,
    const struct lc3_repair_frame *frames)
{
    for (int ch = 0; ch < ctx->nchannels; ch++) {
        if (!frames[ch].data && frames[ch].nbytes == 0) {
            slot->data[ch] = NULL;
            slot->nbytes[ch] = 0;
        } else {
            memcpy(slot->buf[ch], frames[ch].data, frames[ch].nbytes);
            slot->data[ch] = slot->buf[ch];
            slot->nbytes[ch] = frames[ch].nbytes;
        }
    }
}

static void snapshot(struct lc3_repair *ctx, void *dst[2])
{
    for (int ch = 0; ch < ctx->nchannels; ch++)
        memcpy(dst[ch], ctx->dec_mem[ch], ctx->decoder_size);
}

static void restore(struct lc3_repair *ctx, void *src[2])
{
    for (int ch = 0; ch < ctx->nchannels; ch++)
        memcpy(ctx->dec_mem[ch], src[ch], ctx->decoder_size);
}

static void fresh_decoder(struct lc3_repair *ctx)
{
    for (int ch = 0; ch < ctx->nchannels; ch++)
        lc3_setup_decoder(ctx->dt_us, ctx->sr_hz,
            ctx->sr_pcm_hz, ctx->dec_mem[ch]);
}

/**
 * Render every frame whose PCM is stale, in sequence order.
 * The decoder state right before the first stale frame is restored from
 * the window-start snapshot or from the previous frame snapshot.
 */
static void materialize(struct lc3_repair *ctx)
{
    if (ctx->count == 0)
        return;

    int first_invalid = 0;
    while (first_invalid < ctx->count &&
           ctx->slots[slot_index(ctx, first_invalid)].valid)
        first_invalid++;

    if (first_invalid == ctx->count)
        return;     /* Everything already rendered */

    if (first_invalid == 0)
        restore(ctx, ctx->start_mem);
    else
        restore(ctx, ctx->slots[slot_index(ctx, first_invalid - 1)].state);

    for (int age = first_invalid; age < ctx->count; age++) {
        struct lc3_repair_slot *slot = &ctx->slots[slot_index(ctx, age)];

        for (int ch = 0; ch < ctx->nchannels; ch++) {
            bool lost = slot->data[ch] == NULL;

            /* A legal-size but corrupt packet falls back to PLC inside
             * lc3_decode(), matching the single-frame interface. */
            lc3_decode(ctx->dec_mem[ch],
                lost ? NULL : slot->data[ch],
                lost ? 0 : slot->nbytes[ch],
                LC3_PCM_FORMAT_S16, slot->pcm + ch, ctx->nchannels);
        }

        snapshot(ctx, slot->state);
        slot->valid = true;
    }
}

static void free_all(struct lc3_repair *ctx)
{
    if (!ctx)
        return;

    if (ctx->slots) {
        for (int i = 0; i < ctx->capacity; i++) {
            struct lc3_repair_slot *slot = &ctx->slots[i];
            free(slot->pcm);
            for (int ch = 0; ch < 2; ch++) {
                free(slot->buf[ch]);
                free(slot->state[ch]);
            }
        }
        free(ctx->slots);
    }

    for (int ch = 0; ch < 2; ch++) {
        free(ctx->dec_mem[ch]);
        free(ctx->start_mem[ch]);
    }

    free(ctx);
}


/* ----------------------------------------------------------------------------
 *  Public API
 * -------------------------------------------------------------------------- */

LC3_EXPORT lc3_repair_t *lc3_repair_create(
    int dt_us, int sr_hz, int sr_pcm_hz, int nchannels, int capacity)
{
    if ((dt_us != 7500 && dt_us != 10000) ||
        !LC3_CHECK_SR_HZ(sr_hz) ||
        (nchannels != 1 && nchannels != 2) ||
        capacity < LC3_REPAIR_MIN_CAPACITY ||
        capacity > LC3_REPAIR_MAX_CAPACITY)
        return NULL;

    if (sr_pcm_hz == 0)
        sr_pcm_hz = sr_hz;

    if (!LC3_CHECK_SR_HZ(sr_pcm_hz) || sr_pcm_hz < sr_hz)
        return NULL;

    int ns = lc3_frame_samples(dt_us, sr_pcm_hz);
    unsigned decoder_size = lc3_decoder_size(dt_us, sr_pcm_hz);

    if (ns <= 0 || decoder_size == 0)
        return NULL;

    struct lc3_repair *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->dt_us = dt_us;
    ctx->sr_hz = sr_hz;
    ctx->sr_pcm_hz = sr_pcm_hz;
    ctx->nchannels = nchannels;
    ctx->capacity = capacity;
    ctx->ns = ns;
    ctx->decoder_size = decoder_size;

    ctx->slots = calloc((size_t)capacity, sizeof(*ctx->slots));
    if (!ctx->slots)
        goto fail;

    for (int i = 0; i < capacity; i++) {
        struct lc3_repair_slot *slot = &ctx->slots[i];

        clear_slot(slot);

        slot->pcm = malloc((size_t)ns * nchannels * sizeof(int16_t));
        if (!slot->pcm)
            goto fail;

        for (int ch = 0; ch < nchannels; ch++) {
            slot->buf[ch] = malloc(LC3_MAX_FRAME_BYTES);
            slot->state[ch] = malloc(decoder_size);
            if (!slot->buf[ch] || !slot->state[ch])
                goto fail;
        }
    }

    for (int ch = 0; ch < nchannels; ch++) {
        ctx->dec_mem[ch] = malloc(decoder_size);
        ctx->start_mem[ch] = malloc(decoder_size);
        if (!ctx->dec_mem[ch] || !ctx->start_mem[ch])
            goto fail;

        if (!lc3_setup_decoder(dt_us, sr_hz, sr_pcm_hz, ctx->dec_mem[ch]))
            goto fail;
    }

    return ctx;

fail:
    free_all(ctx);
    return NULL;
}

LC3_EXPORT void lc3_repair_destroy(lc3_repair_t *ctx)
{
    free_all(ctx);
}

LC3_EXPORT void lc3_repair_reset(lc3_repair_t *ctx)
{
    if (!ctx)
        return;

    for (int i = 0; i < ctx->capacity; i++)
        clear_slot(&ctx->slots[i]);

    ctx->head = 0;
    ctx->count = 0;
    /* next_id is deliberately kept, so old identifiers stay rejected. */

    fresh_decoder(ctx);
    snapshot(ctx, ctx->start_mem);
}

LC3_EXPORT int lc3_repair_append(
    lc3_repair_t *ctx, const struct lc3_repair_frame *frames, uint64_t *id)
{
    if (!ctx || !id || validate_frames(ctx, frames) < 0
            || ctx->count >= ctx->capacity)
        return -1;

    /* If the window was empty, the current decoder state is the state
     * before the new oldest frame. */
    if (ctx->count == 0)
        snapshot(ctx, ctx->start_mem);

    struct lc3_repair_slot *slot =
        &ctx->slots[slot_index(ctx, ctx->count)];

    clear_slot(slot);
    slot->id = ctx->next_id;

    store_frames(ctx, slot, frames);

    *id = ctx->next_id;
    ctx->next_id++;
    ctx->count++;

    return 0;
}

LC3_EXPORT int lc3_repair_replace(
    lc3_repair_t *ctx, uint64_t id, const struct lc3_repair_frame *frames)
{
    if (!ctx || validate_frames(ctx, frames) < 0)
        return -1;

    struct lc3_repair_slot *slot = find_slot(ctx, id);
    if (!slot || slot->immutable)
        return -1;

    /* All parameters checked: commit the new payloads. */
    store_frames(ctx, slot, frames);

    /* The frame and every successor must be rendered again. */
    uint64_t oldest = ctx->slots[ctx->head].id;
    int age = (int)(id - oldest);

    for ( ; age < ctx->count; age++)
        ctx->slots[slot_index(ctx, age)].valid = false;

    return 0;
}

LC3_EXPORT int lc3_repair_preview(
    lc3_repair_t *ctx, uint64_t id, int16_t *pcm)
{
    if (!ctx || !pcm)
        return -1;

    struct lc3_repair_slot *slot = find_slot(ctx, id);
    if (!slot)
        return -1;

    materialize(ctx);

    memcpy(pcm, slot->pcm,
        (size_t)ctx->ns * ctx->nchannels * sizeof(int16_t));

    return 0;
}

LC3_EXPORT int lc3_repair_read(
    lc3_repair_t *ctx, int16_t *pcm, int capacity_samples)
{
    if (!ctx || !pcm || capacity_samples <= 0 || ctx->count == 0)
        return 0;

    materialize(ctx);

    int delivered = 0;

    while (delivered < capacity_samples && ctx->count > 0) {
        struct lc3_repair_slot *slot = &ctx->slots[ctx->head];
        int avail = ctx->ns - slot->read_pos;
        int take = LC3_MIN(avail, capacity_samples - delivered);

        memcpy(pcm + delivered * ctx->nchannels,
               slot->pcm + slot->read_pos * ctx->nchannels,
               (size_t)take * ctx->nchannels * sizeof(int16_t));

        slot->read_pos += take;
        delivered += take;

        if (slot->read_pos > 0)
            slot->immutable = true;

        if (slot->read_pos == ctx->ns) {
            /* Whole frame delivered: the state captured right after it
             * becomes the window-start state, then the slot recycles. */
            for (int ch = 0; ch < ctx->nchannels; ch++)
                memcpy(ctx->start_mem[ch], slot->state[ch],
                       ctx->decoder_size);

            clear_slot(slot);

            ctx->head = slot_index(ctx, 1);
            ctx->count--;
        }
    }

    return delivered;
}
