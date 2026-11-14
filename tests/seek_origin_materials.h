// seek_origin_materials.h
//
// Test-material generator for the seek-origin regression.  Materials are
// produced entirely through the public encoding API, then optionally
// "re-headed": every physical block's 40-bit sample origin is translated by a
// constant delta while the compressed audio is left untouched.  Block checksums
// are recomputed (checksum verification is never disabled).  The expected PCM
// is the exact array handed to the encoder; no decoder output or library
// header-field macro is ever used as an expected value.

#ifndef SEEK_ORIGIN_MATERIALS_H
#define SEEK_ORIGIN_MATERIALS_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wavpack.h"
#include "seek_origin_catalog.h"

#define SO_HAS_CHECKSUM   0x10000000u
#define SO_ID_BLOCK_CKSUM 0x2fu

struct so_blob {
    unsigned char *data;
    size_t         size;
};

struct so_material {
    char            name[48];
    int             channels;
    int             hybrid;
    int             block_samples;
    int64_t         total_frames;
    int64_t         origin;         // raw 40-bit origin of frame 0
    int             unknown_total;  // header total field set to "unknown"
    int32_t        *pcm;            // expected PCM, total_frames * channels
    struct so_blob  wv;
    struct so_blob  wvc;            // hybrid only
    struct pv_catalog wv_cat;
    struct pv_catalog wvc_cat;
};

static inline void so_material_free(struct so_material *m);

// Distinctive integer marker for every (frame, channel) pair.  Values stay in
// the 16-bit range because the materials are encoded as 16-bit PCM.
static inline int32_t so_marker(int64_t frame, int channel)
{
    uint64_t v = (uint64_t) frame * 7919u + (uint64_t) channel * 1543u +
                 (uint64_t) frame * frame * (channel + 1);
    return (int32_t) (v % 60001u) - 30000;
}

static inline int so_append(void *id, void *data, int32_t count)
{
    struct so_blob *blob = (struct so_blob *) id;
    unsigned char *grown = realloc(blob->data, blob->size + (size_t) count);

    if (!grown)
        return 0;

    blob->data = grown;
    memcpy(blob->data + blob->size, data, (size_t) count);
    blob->size += (size_t) count;
    return 1;
}

static inline void so_put32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

// WavPack block checksum over the little-endian block image, matching the
// library's on-disk algorithm (csum = csum * 3 + next little-endian word).
static inline uint32_t so_block_checksum(const unsigned char *block, int word_count)
{
    uint32_t csum = (uint32_t) -1;

    while (word_count--) {
        csum = csum * 3 + block[0] + ((uint32_t) block[1] << 8);
        block += 2;
    }

    return csum;
}

// Locate the trailing block-checksum metadata item and return its byte offset
// from the block start, or 0 when the block carries no checksum.  The item's
// payload size is written to *payload_bytes.
static inline size_t so_find_checksum_meta(const unsigned char *block,
                                           size_t block_length,
                                           uint32_t *payload_bytes)
{
    size_t dp = 32;

    while (dp + 2 <= block_length) {
        unsigned char meta_id = block[dp];
        unsigned char c1 = block[dp + 1];
        uint32_t meta_bc = (uint32_t) c1 << 1;
        size_t header = 2;

        if (meta_id & 0x80u) {          // ID_LARGE: two extra size bytes
            if (dp + 4 > block_length)
                return 0;
            meta_bc += ((uint32_t) block[dp + 2] << 9) +
                       ((uint32_t) block[dp + 3] << 17);
            header = 4;
        }

        if ((meta_id & 0x3fu) == SO_ID_BLOCK_CKSUM) {
            *payload_bytes = meta_bc;
            return dp;
        }

        dp += header + meta_bc;
    }

    return 0;
}

// Translate every physical block's 40-bit block_index by delta, optionally
// mark the header total as unknown, and recompute each block checksum so the
// stream still verifies with checksum enforcement enabled.
static inline int so_retag_lane(struct so_blob *blob, int64_t delta,
                                int unknown_total)
{
    size_t pos = 0;

    while (pos + 32 <= blob->size) {
        unsigned char *block = blob->data + pos;
        uint32_t ckSize = pv_rd32le(block + 4);
        size_t block_length;
        int64_t current, shifted;
        uint32_t flags, csum_bytes;
        size_t csum_meta;

        if (memcmp(block, "wvpk", 4) || ckSize < 24 || (ckSize & 1u))
            return 0;

        block_length = (size_t) ckSize + 8;

        if (block_length > blob->size - pos)
            return 0;

        current = pv_rd_block_index(block);
        shifted = current + delta;

        if (shifted < 0 || shifted > (int64_t) 0xffffffffffLL)
            return 0;

        so_put32le(block + 16, (uint32_t) shifted);
        block[10] = (unsigned char) ((uint64_t) shifted >> 32);

        if (unknown_total) {
            so_put32le(block + 12, 0xffffffffu);
            block[11] = 0xffu;
        }

        flags = pv_rd32le(block + 24);

        if (flags & SO_HAS_CHECKSUM) {
            csum_meta = so_find_checksum_meta(block, block_length, &csum_bytes);

            if (!csum_meta || (csum_bytes != 2 && csum_bytes != 4))
                return 0;

            uint32_t csum = so_block_checksum(block, (int) (csum_meta >> 1));
            unsigned char *dp = block + csum_meta + 2;

            if (csum_bytes == 4) {
                dp[0] = (unsigned char) csum;
                dp[1] = (unsigned char) (csum >> 8);
                dp[2] = (unsigned char) (csum >> 16);
                dp[3] = (unsigned char) (csum >> 24);
            }
            else {
                csum ^= csum >> 16;
                dp[0] = (unsigned char) csum;
                dp[1] = (unsigned char) (csum >> 8);
            }
        }

        pos += block_length;
    }

    return pos == blob->size;
}

static inline WavpackStreamReader64 so_stream_reader(void)
{
    WavpackStreamReader64 reader = {
        pv_lane_read, pv_lane_write, pv_lane_get_pos, pv_lane_set_abs,
        pv_lane_set_rel, pv_lane_push, pv_lane_length, pv_lane_can_seek,
        NULL, pv_lane_close
    };
    return reader;
}

// Build one material.  origin_shift is added to every block's raw sample
// origin; unknown_total replaces the header total with "unknown".
static inline int so_material_build(struct so_material *m, const char *name,
                                    int channels, int hybrid, int block_samples,
                                    int64_t frames, int64_t origin_shift,
                                    int unknown_total)
{
    WavpackConfig config;
    WavpackContext *enc;
    WavpackStreamReader64 reader = so_stream_reader();
    struct pv_lane wv_lane, wvc_lane;
    char error[128];

    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->channels = channels;
    m->hybrid = hybrid;
    m->block_samples = block_samples;
    m->total_frames = frames;
    m->origin = origin_shift;
    m->unknown_total = unknown_total;

    m->pcm = (int32_t *) malloc((size_t) frames * channels * sizeof(int32_t));
    if (!m->pcm)
        return 0;

    for (int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            m->pcm[i * channels + c] = so_marker(i, c);

    memset(&config, 0, sizeof(config));
    config.bits_per_sample = 16;
    config.bytes_per_sample = 2;
    config.num_channels = channels;
    config.channel_mask = channels == 1 ? 0x4 :
                          (channels == 2 ? 0x3 : 0x3f);
    config.sample_rate = 48000;
    config.block_samples = block_samples;

    if (hybrid) {
        config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC;
        config.bitrate = 2.5f;
    }

    enc = WavpackOpenFileOutput(so_append, &m->wv, hybrid ? &m->wvc : NULL);
    if (!enc)
        goto fail;

    if (!WavpackSetConfiguration64(enc, &config, frames, NULL))
        goto fail;

    if (!WavpackPackInit(enc) ||
        !WavpackPackSamples(enc, m->pcm, (uint32_t) frames) ||
        !WavpackFlushSamples(enc))
        goto fail;

    WavpackCloseFile(enc);
    enc = NULL;

    if (!so_retag_lane(&m->wv, origin_shift, unknown_total))
        goto fail;

    if (hybrid && !so_retag_lane(&m->wvc, origin_shift, unknown_total))
        goto fail;

    if (!pv_catalog_build(&m->wv_cat, PV_LANE_WV, m->wv.data, m->wv.size))
        goto fail;

    if (hybrid && !pv_catalog_build(&m->wvc_cat, PV_LANE_WVC,
                                    m->wvc.data, m->wvc.size))
        goto fail;

    if (m->wv_cat.origin != origin_shift ||
        m->wv_cat.total_frames != frames)
        goto fail;

    // Independently confirm the physical material decodes in plain sequential
    // order back to the exact pre-encode PCM with no decoder errors.
    {
        WavpackContext *dec;
        int32_t *got = (int32_t *) malloc((size_t) frames * channels *
                                          sizeof(int32_t));
        uint32_t got_frames;

        if (!got)
            goto fail;

        pv_lane_init(&wv_lane, "wv", m->wv.data, m->wv.size);

        if (hybrid)
            pv_lane_init(&wvc_lane, "wvc", m->wvc.data, m->wvc.size);

        dec = WavpackOpenFileInputEx64(&reader, &wv_lane,
                                       hybrid ? &wvc_lane : NULL,
                                       error, 0, 0);
        if (!dec) {
            free(got);
            goto fail;
        }

        got_frames = WavpackUnpackSamples(dec, got, (uint32_t) frames);

        if (got_frames != (uint32_t) frames ||
            WavpackGetNumErrors(dec) ||
            WavpackGetNumSamples64(dec) != frames ||
            memcmp(got, m->pcm, (size_t) frames * channels * sizeof(int32_t))) {
            WavpackCloseFile(dec);
            free(got);
            pv_lane_free(&wv_lane);
            if (hybrid)
                pv_lane_free(&wvc_lane);
            goto fail;
        }

        WavpackCloseFile(dec);
        free(got);
        pv_lane_free(&wv_lane);
        if (hybrid)
            pv_lane_free(&wvc_lane);
    }

    return 1;

fail:
    if (enc)
        WavpackCloseFile(enc);
    so_material_free(m);
    return 0;
}

static inline void so_material_free(struct so_material *m)
{
    free(m->pcm);
    free(m->wv.data);
    free(m->wvc.data);
    pv_catalog_free(&m->wv_cat);
    pv_catalog_free(&m->wvc_cat);
    memset(m, 0, sizeof(*m));
}

// Open a seekable decoder on the material with independent per-lane ledgers.
// Caller owns the lanes and the returned context.
static inline WavpackContext *
so_open_decoder(const struct so_material *m, WavpackStreamReader64 *reader,
                struct pv_lane *wv_lane, struct pv_lane *wvc_lane)
{
    char error[128];

    pv_lane_init(wv_lane, "wv", m->wv.data, m->wv.size);

    if (m->hybrid) {
        pv_lane_init(wvc_lane, "wvc", m->wvc.data, m->wvc.size);
        return WavpackOpenFileInputEx64(reader, wv_lane, wvc_lane,
                                        error, 0, 0);
    }

    return WavpackOpenFileInputEx64(reader, wv_lane, NULL, error, 0, 0);
}

#endif // SEEK_ORIGIN_MATERIALS_H
