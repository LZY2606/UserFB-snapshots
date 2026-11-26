// Independent support code for the seek-origin regression test.
//
// Nothing here calls into the library's decoder internals or reuses its
// header macros: the block catalog, block checksum (re)computation and the
// 40-bit field readers below are all implemented from the on-disk format as
// described in include/wavpack.h, so they can act as an oracle for the
// production code.

#ifndef SEEK_ORIGIN_SUPPORT_H
#define SEEK_ORIGIN_SUPPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wavpack.h"

//////////////////////////// little-endian helpers ///////////////////////////

static uint32_t so_rd32 (const unsigned char *p)
{
    return (uint32_t) p [0] | ((uint32_t) p [1] << 8) |
           ((uint32_t) p [2] << 16) | ((uint32_t) p [3] << 24);
}

static void so_wr32 (unsigned char *p, uint32_t v)
{
    p [0] = (unsigned char) v;
    p [1] = (unsigned char) (v >> 8);
    p [2] = (unsigned char) (v >> 16);
    p [3] = (unsigned char) (v >> 24);
}

// The 40-bit sample position fields are stored as a little-endian 32-bit
// low half plus a separate high byte in the header. For total_samples a low
// half of all-ones means "unknown" regardless of the high byte.

static int64_t so_block_index (const unsigned char *h)
{
    return (int64_t) so_rd32 (h + 16) + ((int64_t) h [10] << 32);
}

static int64_t so_total_samples (const unsigned char *h)
{
    uint32_t low = so_rd32 (h + 12);

    if (low == 0xffffffffU)
        return -1;

    return (int64_t) low + ((int64_t) h [11] << 32) - h [11];
}

static void so_set_block_index (unsigned char *h, int64_t value)
{
    so_wr32 (h + 16, (uint32_t) value);
    h [10] = (unsigned char) (value >> 32);
}

// total_samples uses the "lower 32 bits == -1 means unknown" convention and a
// biased high byte, exactly like the format (and the library's own writer).

static void so_set_total_samples (unsigned char *h, int64_t value)
{
    if (value < 0) {
        so_wr32 (h + 12, 0xffffffffU);
        h [11] = 0;
    }
    else {
        int64_t biased = value + value / 0xffffffffLL;
        so_wr32 (h + 12, (uint32_t) biased);
        h [11] = (unsigned char) (biased >> 32);
    }
}

////////////////////////// block checksum (format) ///////////////////////////

// WavPack 5 blocks carry an optional trailing checksum whose value is the
// running sum (seed 0xffffffff, multiplier 3) over every 16-bit word of the
// block before the checksum metadata item, computed with the header in
// little-endian form. This mirrors the format, not the library helper.

// Walk the metadata items of a validated little-endian block. Returns 1 if an
// item with the low six ID bits "want" was found, storing its payload offset
// and byte length. Trailing checksum verification is done by the caller.

static int so_find_meta (const unsigned char *block, size_t len, unsigned want,
    size_t *payload, uint32_t *payload_len, int *odd)
{
    uint32_t ckSize = so_rd32 (block + 4), bcount;
    size_t pos;

    if (len < 32 || memcmp (block, "wvpk", 4) || (size_t) ckSize + 8 != len || (ckSize & 1))
        return 0;

    bcount = ckSize - 24;
    pos = 32;

    while (bcount >= 2) {
        unsigned char id = block [pos], c1 = block [pos + 1];
        uint32_t meta_bc = (uint32_t) c1 << 1;

        pos += 2;
        bcount -= 2;

        if (id & 0x80) {
            if (bcount < 2)
                return 0;

            meta_bc += ((uint32_t) block [pos] << 9) + ((uint32_t) block [pos + 1] << 17);
            pos += 2;
            bcount -= 2;
        }

        if (bcount < meta_bc)
            return 0;

        if ((id & 0x3f) == want) {
            *payload = pos;
            *payload_len = meta_bc;
            *odd = !!(id & 0x40);
            return 1;
        }

        pos += meta_bc;
        bcount -= meta_bc;
    }

    return 0;
}

static uint32_t so_checksum_over (const unsigned char *block, size_t end)
{
    uint32_t csum = 0xffffffffU;
    size_t word_pos = 0;

    while (word_pos + 1 < end) {
        csum = csum * 3 + block [word_pos] + ((uint32_t) block [word_pos + 1] << 8);
        word_pos += 2;
    }

    return csum;
}

static int so_block_verify (const unsigned char *block, size_t len)
{
    size_t payload;
    uint32_t meta_bc;
    int odd;

    if (len < 32 || memcmp (block, "wvpk", 4))
        return 0;

    if (!(so_rd32 (block + 24) & 0x10000000U))     // HAS_CHECKSUM
        return 1;

    if (!so_find_meta (block, len, 0x2f, &payload, &meta_bc, &odd))
        return 0;

    if (odd || meta_bc < 2 || meta_bc > 4)
        return 0;

    {
        uint32_t csum = so_checksum_over (block, payload - 2);

        if (meta_bc == 4)
            return so_rd32 (block + payload) == csum;

        csum ^= csum >> 16;
        return block [payload] == (unsigned char) csum &&
               block [payload + 1] == (unsigned char) (csum >> 8);
    }
}

// Recompute the trailing checksum item of a little-endian block in place. The
// caller must have already updated whatever header fields it cares about.

static int so_block_fix_checksum (unsigned char *block, size_t len)
{
    size_t payload;
    uint32_t meta_bc;
    int odd;

    if (len < 32 || memcmp (block, "wvpk", 4) || !(so_rd32 (block + 24) & 0x10000000U))
        return 0;

    if (!so_find_meta (block, len, 0x2f, &payload, &meta_bc, &odd) || odd ||
        (meta_bc != 2 && meta_bc != 4))
            return 0;

    {
        uint32_t csum = so_checksum_over (block, payload - 2);

        if (meta_bc == 4)
            so_wr32 (block + payload, csum);
        else {
            csum ^= csum >> 16;
            block [payload] = (unsigned char) csum;
            block [payload + 1] = (unsigned char) (csum >> 8);
        }
    }

    return so_block_verify (block, len);
}

/////////////////////////////// byte buffers /////////////////////////////////

typedef struct {
    unsigned char *data;
    size_t size, cap;
} SoBuf;

static void so_buf_free (SoBuf *b)
{
    free (b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

static int so_buf_append (SoBuf *b, const void *data, size_t n)
{
    if (b->size + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;

        while (cap < b->size + n)
            cap *= 2;

        b->data = (unsigned char *) realloc (b->data, cap);

        if (!b->data)
            return 0;

        b->cap = cap;
    }

    memcpy (b->data + b->size, data, n);
    b->size += n;
    return 1;
}

static int so_blockout (void *id, void *data, int32_t bcount)
{
    return bcount >= 0 && so_buf_append ((SoBuf *) id, data, (size_t) bcount);
}

///////////////////////////////// test PCM ///////////////////////////////////

// The origin coordinate that every block of a test file is tagged with. This
// is chosen so that origin coordinates, relative indexes and 16-bit sample
// values can never alias with each other.

#define SO_ORIGIN_BASE 0x10000000LL

#define SO_MARK_LIMIT 30000

static int32_t so_mark (int64_t origin, int channel, int frame)
{
    int64_t v = origin + (int64_t) channel * 1000003 + (int64_t) frame * 7919;

    v %= SO_MARK_LIMIT;

    if (v < 0)
        v += SO_MARK_LIMIT;

    return (int32_t) (v - SO_MARK_LIMIT / 2);
}

// Fill an interleaved buffer with marks addressed by absolute origin frame.

static void so_fill_marks (int32_t *pcm, int channels, int64_t first_frame, int frames, int64_t origin)
{
    int i, c;

    for (i = 0; i < frames; ++i)
        for (c = 0; c < channels; ++c)
            pcm [i * channels + c] = so_mark (origin, c, first_frame + i);
}

///////////////////////////// physical catalog ///////////////////////////////

typedef struct {
    size_t phys_offset;         // byte offset of the block in its own file
    size_t phys_length;         // ckSize + 8
    int64_t raw_index;          // 40-bit block_index as stored
    int64_t rel_index;          // raw_index minus the file's first origin
    uint32_t block_samples;
    uint32_t flags;
    int stream;                 // sub-stream within the multichannel group
    int group;                  // index of the multichannel block group
    int initial, final_;
    int hybrid;
} SoBlock;

#define SO_MAX_GROUPS 256

typedef struct {
    SoBlock *blocks;
    size_t count, cap;
    int64_t rel_start [SO_MAX_GROUPS];
    int64_t group_samples [SO_MAX_GROUPS];
    size_t group_start [SO_MAX_GROUPS];
    size_t group_end_idx [SO_MAX_GROUPS];
    int groups;
    int64_t total_frames;
} SoCatalog;

static void so_catalog_free (SoCatalog *cat)
{
    free (cat->blocks);
    memset (cat, 0, sizeof (*cat));
}

static SoBlock *so_catalog_push (SoCatalog *cat)
{
    if (cat->count == cat->cap) {
        cat->cap = cat->cap ? cat->cap * 2 : 64;
        cat->blocks = (SoBlock *) realloc (cat->blocks, cat->cap * sizeof (SoBlock));

        if (!cat->blocks)
            return NULL;
    }

    memset (cat->blocks + cat->count, 0, sizeof (SoBlock));
    return cat->blocks + cat->count++;
}

// Walk a complete .wv (or .wvc) image by physical block length only and
// record every block. Flags and the 40-bit index are decoded from the raw
// bytes (see the helpers above), never through production macros.

static int so_build_catalog (const unsigned char *data, size_t size, SoCatalog *cat, int64_t origin)
{
    size_t p = 0;
    int group = -1, stream = 0, need_group = 1, final_seen = 1;

    memset (cat, 0, sizeof (*cat));

    while (p < size) {
        uint32_t ck, flags, samples;
        size_t len;
        SoBlock *blk;

        if (p + 32 > size || memcmp (data + p, "wvpk", 4))
            return 0;

        ck = so_rd32 (data + p + 4);
        samples = so_rd32 (data + p + 20);
        flags = so_rd32 (data + p + 24);
        len = (size_t) ck + 8;

        if (ck < 24 || ck > 1024 * 1024 || p + len > size)
            return 0;

        if (!so_block_verify (data + p, len))
            return 0;

        // An audio block after a completed group must itself open a group.

        if (samples && (flags & 0x800))     // INITIAL_BLOCK starts a group
            need_group = 1;

        if (samples && need_group) {
            if (!final_seen)
                return 0;

            if (++group >= SO_MAX_GROUPS)
                return 0;

            stream = 0;
            need_group = 0;
            final_seen = 0;
            cat->group_start [group] = cat->count;
            cat->rel_start [group] = so_block_index (data + p) - origin;
        }
        else if (samples)
            ++stream;
        else if (group < 0)
            return 0;

        blk = so_catalog_push (cat);

        if (!blk)
            return 0;

        blk->phys_offset = p;
        blk->phys_length = len;
        blk->raw_index = so_block_index (data + p);
        blk->rel_index = blk->raw_index - origin;
        blk->block_samples = samples;
        blk->flags = flags;
        blk->stream = samples ? stream : -1;
        blk->group = group;
        blk->initial = !!(flags & 0x800);
        blk->final_ = !!(flags & 0x1000);
        blk->hybrid = !!(flags & 0x8);

        if (samples && stream == 0)
            cat->group_samples [group] = (int64_t) samples;

        if (samples && (flags & 0x1000))    // FINAL_BLOCK closes the group's streams
            final_seen = 1;

        p += len;

        if (final_seen && group >= 0 && cat->group_end_idx [group] == 0) {
            cat->group_end_idx [group] = cat->count;
            cat->groups = group + 1;
            need_group = 1;
        }
    }

    if (p != size || !final_seen)
        return 0;

    for (group = 0; group < cat->groups; ++group) {
        cat->total_frames += cat->group_samples [group];

        if (group + 1 < cat->groups &&
            cat->rel_start [group + 1] != cat->rel_start [group] + cat->group_samples [group])
                return 0;
    }

    return cat->groups > 0;
}

// Locate the block carrying a relative frame in the given sub-stream.

static const SoBlock *so_find_block (const SoCatalog *cat, int64_t rel_frame, int stream)
{
    int g;

    for (g = 0; g < cat->groups; ++g)
        if (rel_frame >= cat->rel_start [g] &&
            rel_frame < cat->rel_start [g] + cat->group_samples [g]) {
                size_t i;

                for (i = cat->group_start [g]; i < cat->group_end_idx [g]; ++i)
                    if (cat->blocks [i].stream == stream)
                        return cat->blocks + i;

                return NULL;
            }

    return NULL;
}

// Confirm the catalog is internally coherent: every audio block in a group
// shares that group's origin and sample count, sub-streams are numbered
// contiguously from zero, and per-file byte intervals tile the image exactly.

static int so_catalog_consistent (const SoCatalog *cat)
{
    size_t i, next = 0;
    int g;

    if (!cat->blocks || !cat->groups || cat->total_frames <= 0)
        return 0;

    for (i = 0; i < cat->count; ++i) {
        const SoBlock *b = cat->blocks + i;

        if (b->phys_offset != next || b->phys_length < 32)
            return 0;

        next += b->phys_length;
    }

    for (g = 0; g < cat->groups; ++g) {
        int64_t raw = cat->blocks [cat->group_start [g]].raw_index;
        uint32_t samples = cat->blocks [cat->group_start [g]].block_samples;
        int expected_stream = 0;
        size_t j;

        if (cat->group_end_idx [g] <= cat->group_start [g])
            return 0;

        for (j = cat->group_start [g]; j < cat->group_end_idx [g]; ++j) {
            const SoBlock *b = cat->blocks + j;

            if (!b->block_samples)          // metadata-only block inside a group
                continue;

            if (b->group != g || b->raw_index != raw || b->block_samples != samples)
                return 0;

            if (b->stream < 0 || b->stream != expected_stream++)
                return 0;
        }

        if (cat->rel_start [g] != raw - (cat->blocks [0].raw_index - cat->rel_start [0]))
            return 0;
    }

    return 1;
}

////////////////////////////// relabeling ////////////////////////////////////

// Shift every audio block's stored 40-bit sample origin by "delta" and
// re-seal each block's checksum. The compressed payload is untouched. The
// total-samples field is either preserved as is (known) or set to "unknown".

static int so_relabel_shift (unsigned char *data, size_t size, int64_t delta, int keep_total)
{
    size_t p = 0;

    while (p < size) {
        uint32_t ck = so_rd32 (data + p + 4);
        uint32_t samples = so_rd32 (data + p + 20);
        size_t len;

        if (p + 32 > size || memcmp (data + p, "wvpk", 4) || ck < 24 || p + ck + 8 > size)
            return 0;

        len = (size_t) ck + 8;

        if (samples) {
            so_set_block_index (data + p, so_block_index (data + p) + delta);
            so_set_total_samples (data + p, keep_total ? so_total_samples (data + p) : -1);
        }

        if (!so_block_fix_checksum (data + p, len))
            return 0;

        p += len;
    }

    return p == size;
}

/////////////////////////// generator (public API) ///////////////////////////

typedef struct {
    SoBuf wv, wvc;
    int32_t *pcm;           // exact pre-encode array; the PCM oracle
    int channels, frames, block_samples;
    int hybrid;
    int64_t origin;
} SoMaterial;

static void so_material_free (SoMaterial *m)
{
    so_buf_free (&m->wv);
    so_buf_free (&m->wvc);
    free (m->pcm);
    m->pcm = NULL;
}

// Encode a test signal through the public encoder. "origin" only affects the
// sample values (so every PCM integer is distinguishable); the header's
// block_index starts at zero here and is shifted afterwards by the caller with
// so_relabel_shift(). "total_samples" may be -1 for an unknown length.

static int so_material_create (SoMaterial *m, int channels, int frames, int block_samples,
    int hybrid, int64_t origin, int64_t total_samples)
{
    WavpackConfig config;
    WavpackContext *ctx;
    int ok;

    memset (m, 0, sizeof (*m));
    m->channels = channels;
    m->frames = frames;
    m->block_samples = block_samples;
    m->hybrid = hybrid;
    m->origin = origin;

    m->pcm = (int32_t *) malloc (sizeof (int32_t) * (size_t) channels * frames);

    if (!m->pcm)
        return 0;

    so_fill_marks (m->pcm, channels, 0, frames, origin);

    memset (&config, 0, sizeof (config));
    config.bits_per_sample = 16;
    config.bytes_per_sample = 2;
    config.num_channels = channels;
    config.channel_mask = channels == 1 ? 4 : channels == 2 ? 3 : 0x3f;
    config.sample_rate = 48000;
    config.block_samples = block_samples;

    if (hybrid) {
        config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC;
        config.bitrate = 2.5f;
    }

    ctx = WavpackOpenFileOutput (so_blockout, &m->wv, hybrid ? &m->wvc : NULL);

    ok = ctx && WavpackSetConfiguration64 (ctx, &config, total_samples, NULL) &&
        WavpackPackInit (ctx) && WavpackPackSamples (ctx, m->pcm, frames) && WavpackFlushSamples (ctx);

    if (ctx)
        WavpackCloseFile (ctx);

    if (!ok) {
        so_material_free (m);
        return 0;
    }

    return 1;
}

//////////////////////////// memory readers //////////////////////////////////

typedef struct {
    const unsigned char *data;
    size_t size, pos;
    unsigned long reads, seeks, pushes, bytes_read;
} SoMem;

static int32_t so_mem_read (void *id, void *out, int32_t want)
{
    SoMem *m = (SoMem *) id;
    size_t n = (size_t) want;

    ++m->reads;

    if (n > m->size - m->pos)
        n = m->size - m->pos;

    memcpy (out, m->data + m->pos, n);
    m->pos += n;
    m->bytes_read += (unsigned long) n;
    return (int32_t) n;
}

static int64_t so_mem_get_pos (void *id)
{
    return (int64_t) ((SoMem *) id)->pos;
}

static int so_mem_set_abs (void *id, int64_t pos)
{
    SoMem *m = (SoMem *) id;

    ++m->seeks;

    if (pos < 0 || (uint64_t) pos > m->size)
        return -1;

    m->pos = (size_t) pos;
    return 0;
}

static int so_mem_set_rel (void *id, int64_t delta, int mode)
{
    SoMem *m = (SoMem *) id;
    int64_t base;

    ++m->seeks;

    if (mode == SEEK_SET)
        base = 0;
    else if (mode == SEEK_CUR)
        base = (int64_t) m->pos;
    else if (mode == SEEK_END)
        base = (int64_t) m->size;
    else
        return -1;

    if (base + delta < 0 || (uint64_t) (base + delta) > m->size)
        return -1;

    m->pos = (size_t) (base + delta);
    return 0;
}

static int so_mem_push (void *id, int c)
{
    SoMem *m = (SoMem *) id;

    ++m->pushes;

    // The reader contract expects the pushed byte to be returned (or EOF at
    // the start of the image); honour that rather than a byte comparison.
    if (!m->pos)
        return EOF;

    --m->pos;
    return c;
}

static int64_t so_mem_length (void *id)
{
    return (int64_t) ((SoMem *) id)->size;
}

static int so_mem_can_seek (void *id)
{
    (void) id;
    return 1;
}

static int so_mem_truncate (void *id)
{
    (void) id;
    return 0;
}

static int so_mem_close (void *id)
{
    (void) id;
    return 0;
}

// A reader that additionally records the byte offset of every read request,
// which is how the tests observe the physical search a seek performs.

#define SO_MEM_TRACE_MAX 8192

typedef struct {
    SoMem mem;
    size_t trace [SO_MEM_TRACE_MAX];
    int trace_count;
} SoTraceMem;

static int32_t so_trace_read (void *id, void *out, int32_t want)
{
    SoTraceMem *t = (SoTraceMem *) id;

    if (t->trace_count < SO_MEM_TRACE_MAX)
        t->trace [t->trace_count++] = t->mem.pos;

    return so_mem_read (&t->mem, out, want);
}

static WavpackStreamReader64 so_reader = {
    so_mem_read, NULL, so_mem_get_pos, so_mem_set_abs, so_mem_set_rel,
    so_mem_push, so_mem_length, so_mem_can_seek, so_mem_truncate, so_mem_close
};

static WavpackStreamReader64 so_trace_reader = {
    so_trace_read, NULL, so_mem_get_pos, so_mem_set_abs, so_mem_set_rel,
    so_mem_push, so_mem_length, so_mem_can_seek, so_mem_truncate, so_mem_close
};

#endif
