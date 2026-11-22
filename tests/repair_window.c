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
 * Tests of the LC3 repair window.
 *
 * Sound references are always produced by ordinary lc3_decode() calls
 * running the final packet event sequence from a fresh decoder state;
 * the lossy codec output is never compared against the encoder input.
 */

#include <lc3.h>
#include <lc3_repair.h>

#include "task_support/check.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define DT_US    10000
#define SR_HZ    32000
#define PCM_HZ   48000
#define NCH      2
#define NS       480
#define NF       10
#define FBYTES   80

#define MONO_DT     7500
#define MONO_SR     16000
#define MONO_NS     120
#define MONO_NF     4
#define MONO_BYTES  60


struct fixture {
    void *emem[NCH];
    lc3_encoder_t enc[NCH];
    uint8_t pkt[NCH][NF][LC3_MAX_FRAME_BYTES];
    int16_t in[NCH][NF * NS];
};


static int fixture_setup(struct fixture *fx)
{
    memset(fx, 0, sizeof(*fx));

    for (int ch = 0; ch < NCH; ch++) {
        fx->emem[ch] = calloc(1, lc3_encoder_size(DT_US, PCM_HZ));
        TASK_CHECK(fx->emem[ch] != NULL);
        fx->enc[ch] = lc3_setup_encoder(
            DT_US, SR_HZ, PCM_HZ, fx->emem[ch]);
        TASK_CHECK(fx->enc[ch] != NULL);
    }

    for (int ch = 0; ch < NCH; ch++) {
        for (int idx = 0; idx < NF * NS; idx++) {
            if (ch == 0)
                fx->in[ch][idx] =
                    (int16_t)(((idx * 7919 + 101) % 20000) - 10000);
            else
                fx->in[ch][idx] =
                    (int16_t)(((idx * 6271 + 211) % 16000) - 8000);
        }
    }

    long energy = 0;
    for (int f = 0; f < NF; f++) {
        for (int ch = 0; ch < NCH; ch++) {
            TASK_CHECK(lc3_encode(fx->enc[ch], LC3_PCM_FORMAT_S16,
                &fx->in[ch][f * NS], 1, FBYTES,
                fx->pkt[ch][f]) == 0);
            for (int i = 0; i < NS; i++)
                energy += abs(fx->in[ch][f * NS + i]);
        }
    }
    TASK_CHECK(energy > 100000);

    return 0;
}

static void fixture_cleanup(struct fixture *fx)
{
    for (int ch = 0; ch < NCH; ch++)
        free(fx->emem[ch]);
}

/**
 * Reference interleaved stereo decoding of the final event sequence.
 * events[f][ch] is 0 for a good packet, 1 for a lost channel.
 * Packet index `base + f` is used for frame f, so the helper also
 * covers windows restarted after a reset.
 */
static int build_ref(const struct fixture *fx,
    const int events[NF][NCH], int base, int nframes, int16_t *ref)
{
    void *dmem[NCH];
    lc3_decoder_t dec[NCH];

    for (int ch = 0; ch < NCH; ch++) {
        dmem[ch] = calloc(1, lc3_decoder_size(DT_US, PCM_HZ));
        TASK_CHECK(dmem[ch] != NULL);
        dec[ch] = lc3_setup_decoder(DT_US, SR_HZ, PCM_HZ, dmem[ch]);
        TASK_CHECK(dec[ch] != NULL);
    }

    for (int f = 0; f < nframes; f++) {
        for (int ch = 0; ch < NCH; ch++) {
            int lost = events[f][ch];
            int ret = lc3_decode(dec[ch],
                lost ? NULL : fx->pkt[ch][base + f],
                lost ? 0 : FBYTES, LC3_PCM_FORMAT_S16,
                ref + (f * NS) * NCH + ch, NCH);
            TASK_CHECK(ret == lost);
        }
    }

    for (int ch = 0; ch < NCH; ch++)
        free(dmem[ch]);
    return 0;
}

static void make_frame(const struct fixture *fx, int ch, int f, int event,
    struct lc3_repair_frame *fr)
{
    if (event) {
        fr->data = NULL;
        fr->nbytes = 0;
    } else {
        fr->data = fx->pkt[ch][f];
        fr->nbytes = FBYTES;
    }
}

static void make_frames(const struct fixture *fx, int f,
    const int events[NCH], struct lc3_repair_frame fr[NCH])
{
    for (int ch = 0; ch < NCH; ch++)
        make_frame(fx, ch, f, events[ch], &fr[ch]);
}


/* ----------------------------------------------------------------------------
 *  replay group: decoded sound must match the final event sequence
 * -------------------------------------------------------------------------- */

static int replay_late_preview(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 4);
    TASK_CHECK(w != NULL);

    /* Initial sequence: frame 1 fully lost, frame 2 channel 0 lost. */
    int initial[4][NCH] = {
        {0, 0}, {1, 1}, {1, 0}, {0, 1}
    };
    int final_ev[4][NCH] = {
        {0, 0}, {0, 0}, {0, 0}, {0, 1}
    };

    uint64_t id[4];
    for (int f = 0; f < 4; f++) {
        struct lc3_repair_frame fr[NCH];
        make_frames(&fx, f, initial[f], fr);
        TASK_CHECK(lc3_repair_append(w, fr, &id[f]) == 0);
        TASK_CHECK(id[f] == (uint64_t)f);
    }

    /* Queue is full: append must fail without touching the id. */
    uint64_t sentinel = 0xdeadbeef;
    struct lc3_repair_frame extra[NCH];
    make_frames(&fx, 4, final_ev[0], extra);
    TASK_CHECK(lc3_repair_append(w, extra, &sentinel) == -1);
    TASK_CHECK(sentinel == 0xdeadbeef);

    /* Out-of-order and repeated previews, before any correction. */
    int16_t pv[NS * NCH], pv_again[NS * NCH], old_plc[NS * NCH];
    TASK_CHECK(lc3_repair_preview(w, id[3], pv) == 0);
    TASK_CHECK(lc3_repair_preview(w, id[1], old_plc) == 0);
    TASK_CHECK(lc3_repair_preview(w, id[2], pv_again) == 0);
    TASK_CHECK(lc3_repair_preview(w, id[2], pv) == 0);
    TASK_CHECK(memcmp(pv, pv_again, sizeof(pv)) == 0);

    /* Late packets arrive: replace frame 1 and the missing channel of 2. */
    struct lc3_repair_frame fr1[NCH], fr2[NCH];
    make_frames(&fx, 1, final_ev[1], fr1);
    make_frames(&fx, 2, final_ev[2], fr2);
    TASK_CHECK(lc3_repair_replace(w, id[1], fr1) == 0);
    TASK_CHECK(lc3_repair_replace(w, id[2], fr2) == 0);

    /* Repeated replacement of the same mutable frame is allowed. */
    TASK_CHECK(lc3_repair_replace(w, id[1], fr1) == 0);

    /* Reference built from the final packet sequence. */
    int all_events[NF][NCH] = {0};
    memcpy(all_events, final_ev, sizeof(final_ev));
    int16_t ref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_events, 0, NF, ref) == 0);

    for (int f = 0; f < 4; f++) {
        TASK_CHECK(lc3_repair_preview(w, id[f], pv) == 0);
        TASK_CHECK(memcmp(pv, ref + f * NS * NCH,
                          (size_t)NS * NCH * sizeof(int16_t)) == 0);
    }

    /* The corrected frame must sound different from the old PLC output. */
    TASK_CHECK(memcmp(old_plc, ref + 1 * NS * NCH, sizeof(old_plc)) != 0);

    /* Preview never consumes capacity. */
    TASK_CHECK(lc3_repair_append(w, extra, &sentinel) == -1);

    /* Sequential read gives the same corrected sound. */
    int16_t played[4 * NS * NCH];
    int got = lc3_repair_read(w, played, 4 * NS);
    TASK_CHECK(got == 4 * NS);
    TASK_CHECK(memcmp(played, ref, sizeof(played)) == 0);

    /* Old identifiers are gone once fully read. */
    TASK_CHECK(lc3_repair_preview(w, id[0], pv) == -1);

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}

static int replay_plc_history(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 6);
    TASK_CHECK(w != NULL);

    /* Good, two consecutive losses, recovery, loss again. */
    int events[6][NCH] = {
        {0, 0}, {1, 1}, {1, 1}, {0, 0}, {1, 0}, {0, 1}
    };
    int all_events[NF][NCH] = {0};
    memcpy(all_events, events, sizeof(events));

    uint64_t id[6];
    for (int f = 0; f < 6; f++) {
        struct lc3_repair_frame fr[NCH];
        make_frames(&fx, f, events[f], fr);
        TASK_CHECK(lc3_repair_append(w, fr, &id[f]) == 0);
    }

    int16_t ref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_events, 0, NF, ref) == 0);

    /* Read in odd chunks, spanning frames and leaving partial frames. */
    int16_t out[6 * NS * NCH];
    int produced = 0;
    const int chunks[] = { NS / 3, NS - NS / 3, 37, NS * 2, NS };
    for (int i = 0; produced < 6 * NS; i++) {
        int n = lc3_repair_read(w, out + produced * NCH, chunks[i % 5]);
        TASK_CHECK(n > 0);
        produced += n;
    }
    TASK_CHECK(produced == 6 * NS);
    TASK_CHECK(memcmp(out, ref, (size_t)produced * NCH *
                      sizeof(int16_t)) == 0);
    TASK_CHECK(lc3_repair_read(w, out, 10) == 0);

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}

static int replay_stereo_side_loss(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 5);
    TASK_CHECK(w != NULL);

    int events[5][NCH] = {
        {0, 1}, {1, 0}, {0, 0}, {1, 1}, {1, 0}
    };
    int all_events[NF][NCH] = {0};
    memcpy(all_events, events, sizeof(events));

    for (int f = 0; f < 5; f++) {
        struct lc3_repair_frame fr[NCH];
        uint64_t id;
        make_frames(&fx, f, events[f], fr);
        TASK_CHECK(lc3_repair_append(w, fr, &id) == 0);
        TASK_CHECK(id == (uint64_t)f);
    }

    int16_t ref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_events, 0, NF, ref) == 0);

    /* Every channel must carry non-zero, distinct energy. */
    long energy[NCH] = {0, 0};
    for (int f = 0; f < 5; f++)
        for (int i = 0; i < NS; i++)
            for (int ch = 0; ch < NCH; ch++)
                energy[ch] += abs(ref[(f * NS + i) * NCH + ch]);
    TASK_CHECK(energy[0] > 10000 && energy[1] > 10000);
    TASK_CHECK(energy[0] != energy[1]);

    for (int f = 4; f >= 0; f--) {
        int16_t pv[NS * NCH];
        TASK_CHECK(lc3_repair_preview(w, (uint64_t)f, pv) == 0);
        TASK_CHECK(memcmp(pv, ref + f * NS * NCH,
                          sizeof(pv)) == 0);
    }

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}


/* ----------------------------------------------------------------------------
 *  commit group: mutation, replacement rules and ring recycling
 * -------------------------------------------------------------------------- */

static int commit_partial_read(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 3);
    TASK_CHECK(w != NULL);

    int all_events[NF][NCH] = {0};
    uint64_t id[3];
    for (int f = 0; f < 3; f++) {
        struct lc3_repair_frame fr[NCH];
        make_frames(&fx, f, all_events[f], fr);
        TASK_CHECK(lc3_repair_append(w, fr, &id[f]) == 0);
    }

    int16_t ref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_events, 0, NF, ref) == 0);

    /* Deliver half of frame 0: it becomes immutable. */
    int16_t out[3 * NS * NCH];
    int half = NS / 2;
    TASK_CHECK(lc3_repair_read(w, out, half) == half);
    TASK_CHECK(memcmp(out, ref, (size_t)half * NCH *
                      sizeof(int16_t)) == 0);

    struct lc3_repair_frame fr[NCH];
    make_frames(&fx, 0, all_events[0], fr);
    TASK_CHECK(lc3_repair_replace(w, id[0], fr) == -1);

    /* Frame 1 has not started: replacement (same final payload) works. */
    make_frames(&fx, 1, all_events[1], fr);
    TASK_CHECK(lc3_repair_replace(w, id[1], fr) == 0);

    /* Drain everything; the remaining half of frame 0 stays intact. */
    int produced = half;
    while (produced < 3 * NS) {
        int n = lc3_repair_read(w, out + produced * NCH, NS);
        TASK_CHECK(n > 0);
        produced += n;
    }
    TASK_CHECK(produced == 3 * NS);
    TASK_CHECK(memcmp(out, ref, (size_t)produced * NCH *
                      sizeof(int16_t)) == 0);

    /* Fully read frames cannot be addressed anymore. */
    TASK_CHECK(lc3_repair_replace(w, id[0], fr) == -1);
    TASK_CHECK(lc3_repair_preview(w, id[2], out) == -1);

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}

static int commit_ring_reuse(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 3);
    TASK_CHECK(w != NULL);

    int all_good[NF][NCH] = {0};
    int16_t ref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_good, 0, NF, ref) == 0);

    int16_t out[NF * NS * NCH];
    int produced = 0;
    uint64_t id;

    for (int f = 0; f < NF; f++) {
        int initial[NCH];
        initial[0] = initial[1] = (f % 3 == 1);

        struct lc3_repair_frame fr[NCH];
        make_frames(&fx, f, initial, fr);

        while (lc3_repair_append(w, fr, &id) == -1) {
            int n = lc3_repair_read(
                w, out + produced * NCH, (f & 1) ? NS / 2 : NS);
            TASK_CHECK(n > 0);
            produced += n;
        }
        TASK_CHECK(id == (uint64_t)f);

        /* Late correction while the just-appended frame is retained. */
        if (f % 3 == 1) {
            struct lc3_repair_frame good[NCH];
            make_frames(&fx, f, all_good[f], good);
            TASK_CHECK(lc3_repair_replace(w, id, good) == 0);
        }
    }

    while (produced < NF * NS) {
        int n = lc3_repair_read(w, out + produced * NCH, NS / 2);
        TASK_CHECK(n > 0);
        produced += n;
    }
    TASK_CHECK(produced == NF * NS);
    TASK_CHECK(memcmp(out, ref, (size_t)produced * NCH *
                      sizeof(int16_t)) == 0);

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}

static int commit_atomic(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 2);
    TASK_CHECK(w != NULL);

    uint8_t dummy[FBYTES];
    memset(dummy, 0x5a, sizeof(dummy));

    struct lc3_repair_frame good[NCH];
    make_frames(&fx, 0, (int[NCH]){0, 0}, good);

    /* Invalid payload on the second channel: whole call rejected. */
    struct lc3_repair_frame bad1[NCH] = {
        { fx.pkt[0][0], FBYTES }, { dummy, 7 }
    };
    uint64_t sentinel = 0x12345678;
    TASK_CHECK(lc3_repair_append(w, bad1, &sentinel) == -1);
    TASK_CHECK(sentinel == 0x12345678);

    /* Invalid first channel as well. */
    struct lc3_repair_frame bad2[NCH] = {
        { dummy, 7 }, { fx.pkt[1][0], FBYTES }
    };
    TASK_CHECK(lc3_repair_append(w, bad2, &sentinel) == -1);
    TASK_CHECK(sentinel == 0x12345678);

    /* NULL data with non-zero size is invalid. */
    struct lc3_repair_frame bad3[NCH] = {
        { NULL, 5 }, { fx.pkt[1][0], FBYTES }
    };
    TASK_CHECK(lc3_repair_append(w, bad3, &sentinel) == -1);
    TASK_CHECK(sentinel == 0x12345678);

    /* Nothing was consumed: the first valid frame gets id 0. */
    uint64_t id = 999;
    TASK_CHECK(lc3_repair_append(w, good, &id) == 0);
    TASK_CHECK(id == 0);

    int all_events[NF][NCH] = {0};
    int16_t ref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_events, 0, NF, ref) == 0);

    int16_t before[NS * NCH];
    TASK_CHECK(lc3_repair_preview(w, 0, before) == 0);
    TASK_CHECK(memcmp(before, ref, sizeof(before)) == 0);

    /* A failed replacement must leave the retained sound untouched. */
    TASK_CHECK(lc3_repair_replace(w, 0, bad1) == -1);
    int16_t after[NS * NCH];
    TASK_CHECK(lc3_repair_preview(w, 0, after) == 0);
    TASK_CHECK(memcmp(before, after, sizeof(before)) == 0);

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}

static int commit_overwrite(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 2);
    TASK_CHECK(w != NULL);

    /* Caller-owned copies that are clobbered right after append. */
    uint8_t copy[NCH][FBYTES];
    struct lc3_repair_frame fr[NCH];
    for (int ch = 0; ch < NCH; ch++) {
        memcpy(copy[ch], fx.pkt[ch][0], FBYTES);
        fr[ch].data = copy[ch];
        fr[ch].nbytes = FBYTES;
    }

    uint64_t id;
    TASK_CHECK(lc3_repair_append(w, fr, &id) == 0);
    memset(copy, 0, sizeof(copy));

    int all_events[NF][NCH] = {0};
    int16_t ref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_events, 0, NF, ref) == 0);

    int16_t pv[NS * NCH];
    TASK_CHECK(lc3_repair_preview(w, id, pv) == 0);
    TASK_CHECK(memcmp(pv, ref, sizeof(pv)) == 0);

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}

static int commit_reset(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 3);
    TASK_CHECK(w != NULL);

    struct lc3_repair_frame fr[NCH];
    uint64_t id0, id1;
    make_frames(&fx, 0, (int[NCH]){0, 0}, fr);
    TASK_CHECK(lc3_repair_append(w, fr, &id0) == 0);
    make_frames(&fx, 1, (int[NCH]){1, 1}, fr);
    TASK_CHECK(lc3_repair_append(w, fr, &id1) == 0);

    int16_t pv[NS * NCH];
    TASK_CHECK(lc3_repair_preview(w, id1, pv) == 0);

    lc3_repair_reset(w);

    /* Old identifiers are permanently rejected. */
    TASK_CHECK(lc3_repair_preview(w, id0, pv) == -1);
    TASK_CHECK(lc3_repair_preview(w, id1, pv) == -1);
    TASK_CHECK(lc3_repair_replace(w, id0, fr) == -1);
    TASK_CHECK(lc3_repair_read(w, pv, 10) == 0);

    /* Numbering keeps increasing after the reset. */
    make_frames(&fx, 2, (int[NCH]){0, 0}, fr);
    uint64_t id2;
    TASK_CHECK(lc3_repair_append(w, fr, &id2) == 0);
    TASK_CHECK(id2 == 2);
    make_frames(&fx, 3, (int[NCH]){1, 0}, fr);
    uint64_t id3;
    TASK_CHECK(lc3_repair_append(w, fr, &id3) == 0);
    TASK_CHECK(id3 == 3);

    /* History was cleared: the same events on a fresh decoder, fed with
     * the packets carrying ids 2 and 3, must match. */
    int events[2][NCH] = { {0, 0}, {1, 0} };
    int all_events[NF][NCH] = {0};
    memcpy(all_events, events, sizeof(events));
    int16_t ref[2 * NS * NCH];
    TASK_CHECK(build_ref(&fx, all_events, 2, 2, ref) == 0);

    int16_t out[2 * NS * NCH];
    TASK_CHECK(lc3_repair_read(w, out, 2 * NS) == 2 * NS);
    TASK_CHECK(memcmp(out, ref, sizeof(out)) == 0);

    lc3_repair_destroy(w);
    fixture_cleanup(&fx);
    return 0;
}

static int commit_params(void)
{
    TASK_CHECK(lc3_repair_create(DT_US, SR_HZ, PCM_HZ, 1, 1) == NULL);
    TASK_CHECK(lc3_repair_create(DT_US, SR_HZ, PCM_HZ, 1, 33) == NULL);
    TASK_CHECK(lc3_repair_create(5000, SR_HZ, PCM_HZ, 1, 2) == NULL);
    TASK_CHECK(lc3_repair_create(DT_US, 44100, PCM_HZ, 1, 2) == NULL);
    TASK_CHECK(lc3_repair_create(DT_US, SR_HZ, 16000, 1, 2) == NULL);
    TASK_CHECK(lc3_repair_create(DT_US, SR_HZ, PCM_HZ, 3, 2) == NULL);

    /* sr_pcm_hz == 0 follows the bitstream sample rate. */
    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, 0, 1, 2);
    TASK_CHECK(w != NULL);
    TASK_CHECK(lc3_frame_samples(DT_US, SR_HZ) == 320);

    int16_t tmp[4];
    TASK_CHECK(lc3_repair_read(w, tmp, 4) == 0);

    lc3_repair_destroy(w);
    lc3_repair_destroy(NULL);   /* Must be a no-op. */
    return 0;
}


/* ----------------------------------------------------------------------------
 *  isolation group: instances, corrupt packets and bad addressing
 * -------------------------------------------------------------------------- */

static int isolation_two_windows(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *a =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 4);
    lc3_repair_t *b =
        lc3_repair_create(MONO_DT, MONO_SR, MONO_SR, 1, 2);
    TASK_CHECK(a != NULL && b != NULL);

    /* Independent mono encoder and packets for window B. */
    void *emem = calloc(1, lc3_encoder_size(MONO_DT, MONO_SR));
    lc3_encoder_t enc =
        lc3_setup_encoder(MONO_DT, MONO_SR, MONO_SR, emem);
    TASK_CHECK(enc != NULL);

    uint8_t mpkt[MONO_NF][LC3_MAX_FRAME_BYTES];
    int16_t min[MONO_NF * MONO_NS];
    for (int idx = 0; idx < MONO_NF * MONO_NS; idx++)
        min[idx] = (int16_t)(((idx * 5173 + 77) % 18000) - 9000);
    for (int f = 0; f < MONO_NF; f++)
        TASK_CHECK(lc3_encode(enc, LC3_PCM_FORMAT_S16,
            &min[f * MONO_NS], 1, MONO_BYTES, mpkt[f]) == 0);

    /* Interleave operations on the two windows. */
    uint64_t aid[3], bid;
    struct lc3_repair_frame afr[NCH];
    make_frames(&fx, 0, (int[NCH]){1, 0}, afr);
    TASK_CHECK(lc3_repair_append(a, afr, &aid[0]) == 0);

    struct lc3_repair_frame bfr = { mpkt[0], MONO_BYTES };
    TASK_CHECK(lc3_repair_append(b, &bfr, &bid) == 0);
    TASK_CHECK(bid == 0);

    make_frames(&fx, 1, (int[NCH]){0, 1}, afr);
    TASK_CHECK(lc3_repair_append(a, afr, &aid[1]) == 0);

    /* Window B holds one frame; a second one (PLC) is accepted, then
     * the full window rejects a third without consuming its id. */
    struct lc3_repair_frame bloc = { NULL, 0 };
    uint64_t bid1;
    TASK_CHECK(lc3_repair_append(b, &bloc, &bid1) == 0);
    TASK_CHECK(bid1 == 1);
    uint64_t rejected = 777;
    TASK_CHECK(lc3_repair_append(b, &bloc, &rejected) == -1);
    TASK_CHECK(rejected == 777);

    make_frames(&fx, 2, (int[NCH]){0, 0}, afr);
    TASK_CHECK(lc3_repair_append(a, afr, &aid[2]) == 0);

    /* Late correction of frame 0 in A. */
    make_frames(&fx, 0, (int[NCH]){0, 0}, afr);
    TASK_CHECK(lc3_repair_replace(a, aid[0], afr) == 0);

    /* Reference for A: all three frames good except frame 1 ch1 lost. */
    int a_events[NF][NCH] = {0};
    a_events[1][1] = 1;
    int16_t aref[NF * NS * NCH];
    TASK_CHECK(build_ref(&fx, a_events, 0, NF, aref) == 0);

    int16_t aout[3 * NS * NCH];
    TASK_CHECK(lc3_repair_read(a, aout, 3 * NS) == 3 * NS);
    TASK_CHECK(memcmp(aout, aref, sizeof(aout)) == 0);

    /* Reference for B: good frame then PLC, on a fresh mono decoder. */
    void *dmem = calloc(1, lc3_decoder_size(MONO_DT, MONO_SR));
    lc3_decoder_t dec =
        lc3_setup_decoder(MONO_DT, MONO_SR, MONO_SR, dmem);
    TASK_CHECK(dec != NULL);
    int16_t bref[2 * MONO_NS];
    TASK_CHECK(lc3_decode(dec, mpkt[0], MONO_BYTES,
        LC3_PCM_FORMAT_S16, bref, 1) == 0);
    TASK_CHECK(lc3_decode(dec, NULL, 0,
        LC3_PCM_FORMAT_S16, bref + MONO_NS, 1) == 1);

    int16_t bfirst[MONO_NS];
    TASK_CHECK(lc3_repair_read(b, bfirst, MONO_NS) == MONO_NS);
    TASK_CHECK(memcmp(bfirst, bref, sizeof(bfirst)) == 0);
    int16_t bsecond[MONO_NS];
    TASK_CHECK(lc3_repair_read(b, bsecond, MONO_NS) == MONO_NS);
    TASK_CHECK(memcmp(bsecond, bref + MONO_NS,
                      sizeof(bsecond)) == 0);

    /* Destroying B leaves A fully usable. */
    lc3_repair_destroy(b);
    make_frames(&fx, 3, (int[NCH]){1, 1}, afr);
    uint64_t aid3;
    TASK_CHECK(lc3_repair_append(a, afr, &aid3) == 0);
    TASK_CHECK(aid3 == 3);
    int16_t pv[NS * NCH];
    TASK_CHECK(lc3_repair_preview(a, aid3, pv) == 0);

    lc3_repair_destroy(a);
    free(emem);
    free(dmem);
    fixture_cleanup(&fx);
    return 0;
}

static int isolation_corrupt_plc(void)
{
    struct fixture fx;
    TASK_CHECK(fixture_setup(&fx) == 0);

    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, 1, 2);
    TASK_CHECK(w != NULL);

    /* Probe a legal-size packet the decoder rejects, so the reference
     * genuinely exercises the PLC fallback path. */
    void *dmem = calloc(1, lc3_decoder_size(DT_US, PCM_HZ));
    lc3_decoder_t dec =
        lc3_setup_decoder(DT_US, SR_HZ, PCM_HZ, dmem);
    TASK_CHECK(dec != NULL);

    uint8_t corrupt[FBYTES];
    int16_t probe[NS];
    bool found = false;
    static const uint8_t fills[] = { 0x00, 0xff, 0xa5, 0x5a };
    for (int i = 0; !found && i < (int)(sizeof(fills)); i++) {
        memset(corrupt, fills[i], sizeof(corrupt));
        if (lc3_decode(dec, corrupt, FBYTES,
                LC3_PCM_FORMAT_S16, probe, 1) == 1)
            found = true;
    }
    TASK_CHECK(found);

    struct lc3_repair_frame fr = { corrupt, FBYTES };
    uint64_t id0, id1;
    TASK_CHECK(lc3_repair_append(w, &fr, &id0) == 0);
    fr = (struct lc3_repair_frame){ fx.pkt[0][1], FBYTES };
    TASK_CHECK(lc3_repair_append(w, &fr, &id1) == 0);

    /* Ordinary decoder on the same legal-size but bad packets, on a
     * fresh state so the sequences are comparable. */
    lc3_setup_decoder(DT_US, SR_HZ, PCM_HZ, dmem);

    int16_t ref[2 * NS];
    TASK_CHECK(lc3_decode(dec, corrupt, FBYTES,
        LC3_PCM_FORMAT_S16, ref, 1) == 1);
    TASK_CHECK(lc3_decode(dec, fx.pkt[0][1], FBYTES,
        LC3_PCM_FORMAT_S16, ref + NS, 1) == 0);

    int16_t pv[NS];
    TASK_CHECK(lc3_repair_preview(w, id0, pv) == 0);
    TASK_CHECK(memcmp(pv, ref, sizeof(pv)) == 0);
    TASK_CHECK(lc3_repair_preview(w, id1, pv) == 0);
    TASK_CHECK(memcmp(pv, ref + NS, sizeof(pv)) == 0);

    int16_t out[2 * NS];
    TASK_CHECK(lc3_repair_read(w, out, 2 * NS) == 2 * NS);
    TASK_CHECK(memcmp(out, ref, sizeof(out)) == 0);

    lc3_repair_destroy(w);
    free(dmem);
    fixture_cleanup(&fx);
    return 0;
}

static int isolation_empty_ids(void)
{
    lc3_repair_t *w =
        lc3_repair_create(DT_US, SR_HZ, PCM_HZ, NCH, 2);
    TASK_CHECK(w != NULL);

    int16_t tmp[8];
    TASK_CHECK(lc3_repair_read(w, tmp, 8) == 0);
    TASK_CHECK(lc3_repair_read(w, NULL, 8) == 0);
    TASK_CHECK(lc3_repair_preview(w, 0, tmp) == -1);

    struct lc3_repair_frame fr[NCH] = {
        { NULL, 0 }, { NULL, 0 }
    };
    TASK_CHECK(lc3_repair_replace(w, 123, fr) == -1);
    TASK_CHECK(lc3_repair_append(w, NULL, &(uint64_t){0}) == -1);

    lc3_repair_destroy(w);
    return 0;
}


/* ----------------------------------------------------------------------------
 *  Registration
 * -------------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    { "replay",    "late_preview",     replay_late_preview    },
    { "replay",    "plc_history",      replay_plc_history     },
    { "replay",    "stereo_side_loss", replay_stereo_side_loss },
    { "commit",    "partial_read",     commit_partial_read    },
    { "commit",    "ring_reuse",       commit_ring_reuse      },
    { "commit",    "atomic",           commit_atomic          },
    { "commit",    "overwrite",        commit_overwrite       },
    { "commit",    "reset",            commit_reset           },
    { "commit",    "params",           commit_params          },
    { "isolation", "two_windows",      isolation_two_windows  },
    { "isolation", "corrupt_plc",      isolation_corrupt_plc  },
    { "isolation", "empty_ids",        isolation_empty_ids    },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
