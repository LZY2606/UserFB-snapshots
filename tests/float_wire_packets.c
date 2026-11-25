////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//             Public container material and chunk traversal               //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// float_wire_packets.c

#include "float_wire_packets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////// little endian //////////////////////////////

static uint32_t rd32 (const unsigned char *p)
{
    return (uint32_t) p [0] | ((uint32_t) p [1] << 8) |
        ((uint32_t) p [2] << 16) | ((uint32_t) p [3] << 24);
}

static void wr32 (unsigned char *p, uint32_t v)
{
    p [0] = (unsigned char) v;
    p [1] = (unsigned char) (v >> 8);
    p [2] = (unsigned char) (v >> 16);
    p [3] = (unsigned char) (v >> 24);
}

////////////////////////////////// block parsing //////////////////////////////

int fwp_parse_block (const unsigned char *block, size_t size, fwp_block *out)
{
    const unsigned char *dp, *bend;

    if (size < sizeof (WavpackHeader) || memcmp (block, "wvpk", 4))
        return -1;

    memset (out, 0, sizeof (*out));
    out->block = block;
    out->size = size;
    memcpy (out->ckID, block, 4);
    out->ckSize = rd32 (block + 4);
    out->block_samples = rd32 (block + 20);
    out->flags = rd32 (block + 24);
    out->block_index = (int64_t) rd32 (block + 16) | ((int64_t) block [12] << 32);

    if (out->ckSize + 8u > size)
        return -1;

    dp = block + sizeof (WavpackHeader);
    bend = block + out->ckSize + 8;
    out->nchunks = 0;

    while (bend - dp >= 2 && out->nchunks < FWP_MAX_CHUNKS) {
        fwp_chunk *ch = out->chunks + out->nchunks;
        const unsigned char *raw = dp;
        unsigned char meta_id = *dp++;
        uint32_t byte_length = (uint32_t) (*dp++) << 1;

        if (meta_id & ID_LARGE) {
            if (bend - dp < 2)
                return -1;

            byte_length += (uint32_t) (*dp++) << 9;
            byte_length += (uint32_t) (*dp++) << 17;
        }

        if (meta_id & ID_ODD_SIZE) {
            if (!byte_length)
                return -1;

            byte_length--;
        }

        if (bend - dp < (int32_t) (byte_length + (byte_length & 1)))
            return -1;

        ch->raw = raw;
        ch->raw_id = meta_id;
        ch->id = meta_id & ID_UNIQUE;
        ch->byte_length = (int) byte_length;
        ch->data = byte_length ? dp : NULL;
        dp += byte_length + (byte_length & 1);
        out->nchunks++;
    }

    return (dp == bend) ? 0 : -1;
}

const fwp_chunk *fwp_find_chunk (const fwp_block *b, unsigned char id)
{
    int i;

    for (i = 0; i < b->nchunks; i++)
        if (b->chunks [i].id == id)
            return b->chunks + i;

    return NULL;
}

int fwp_for_each_block (const unsigned char *data, size_t size,
                        int (*cb) (const fwp_block *block, void *opaque), void *opaque)
{
    size_t off = 0;

    while (off + sizeof (WavpackHeader) <= size) {
        fwp_block block;
        size_t block_size;

        if (memcmp (data + off, "wvpk", 4))
            return -1;

        block_size = (size_t) rd32 (data + off + 4) + 8;

        if (!block_size || off + block_size > size)
            return -1;

        if (fwp_parse_block (data + off, block_size, &block))
            return -1;

        if (cb && !cb (&block, opaque))
            return 0;

        off += block_size;
    }

    return off == size ? 0 : -1;
}

////////////////////////////////// output sink ////////////////////////////////

void fwp_sink_init (fwp_sink *sink, unsigned char *storage, size_t cap)
{
    sink->data = storage;
    sink->len = 0;
    sink->cap = cap;
    sink->failed = 0;
}

int fwp_sink_block (void *id, void *data, int32_t n)
{
    fwp_sink *sink = (fwp_sink *) id;

    if (n < 0 || (size_t) n > sink->cap - sink->len) {
        sink->failed = 1;
        return 0;
    }

    memcpy (sink->data + sink->len, data, (size_t) n);
    sink->len += (size_t) n;
    return 1;
}

////////////////////////////////// memory reader ///////////////////////////////

static int32_t mem_read (void *id, void *data, int32_t bcount)
{
    fwp_memory *mem = (fwp_memory *) id;
    int32_t n = 0;
    unsigned char *out = (unsigned char *) data;

    if (mem->has_pushback && bcount > 0) {
        *out++ = (unsigned char) mem->pushed;
        mem->has_pushback = 0;
        bcount--;
        n++;
    }

    while (bcount > 0 && mem->pos < (int64_t) mem->size) {
        *out++ = mem->data [mem->pos++];
        bcount--;
        n++;
    }

    return n;
}

static int32_t mem_write (void *id, void *data, int32_t bcount)
{
    (void) id;
    (void) data;
    (void) bcount;
    return 0;
}

static int64_t mem_get_pos (void *id)
{
    return ((fwp_memory *) id)->pos;
}

static int mem_set_pos_abs (void *id, int64_t pos)
{
    fwp_memory *mem = (fwp_memory *) id;

    if (pos < 0 || pos > (int64_t) mem->size)
        return -1;

    mem->pos = pos;
    mem->has_pushback = 0;
    return 0;
}

static int mem_set_pos_rel (void *id, int64_t delta, int mode)
{
    fwp_memory *mem = (fwp_memory *) id;
    int64_t base = 0;

    if (mode == SEEK_CUR)
        base = mem->pos;
    else if (mode == SEEK_END)
        base = (int64_t) mem->size;

    return mem_set_pos_abs (id, base + delta);
}

static int mem_push_back (void *id, int c)
{
    fwp_memory *mem = (fwp_memory *) id;
    mem->pushed = c & 0xff;
    mem->has_pushback = 1;
    return c;
}

static int64_t mem_get_length (void *id)
{
    return (int64_t) ((fwp_memory *) id)->size;
}

static int mem_can_seek (void *id)
{
    (void) id;
    return 1;
}

WavpackStreamReader64 fwp_memory_reader = {
    mem_read, mem_write, mem_get_pos, mem_set_pos_abs, mem_set_pos_rel,
    mem_push_back, mem_get_length, mem_can_seek, NULL, NULL
};

void fwp_memory_init (fwp_memory *mem, const unsigned char *data, size_t size)
{
    mem->data = data;
    mem->size = size;
    mem->pos = 0;
    mem->pushed = 0;
    mem->has_pushback = 0;
}

////////////////////////////////// block rebuild ///////////////////////////////

// Four bytes of zero padding follow every handcrafted sideband so the
// production 16-bit Bitstream getbit/getbits macros always have a full word
// available without tripping the end-of-stream wrap. Padding is never part of
// the valid bit count checked by the model, and truncated-stream behavior is
// unchanged because the padding is only a read reserve.

#define FWP_PHYS_PADDING 4

void fwp_builder_init_from (fwp_builder *b, unsigned char *storage, size_t cap,
                            const unsigned char *src_block)
{
    b->buf = storage;
    b->cap = cap;
    b->len = sizeof (WavpackHeader);
    memcpy (b->buf, src_block, sizeof (WavpackHeader));
    WavpackLittleEndianToNative ((WavpackHeader *) b->buf, WavpackHeaderFormat);
}

void fwp_builder_add_chunk (fwp_builder *b, unsigned char id,
                            const void *data, int byte_length)
{
    unsigned char *dp;
    int padded = byte_length + (byte_length & 1);
    int header = (byte_length > 510) ? 4 : 2;

    if (b->len + (size_t) (header + padded) > b->cap)
        return;

    dp = b->buf + b->len;

    if (byte_length > 510) {
        *dp++ = (unsigned char) (id | ID_LARGE | (byte_length & 1 ? ID_ODD_SIZE : 0));
        *dp++ = (unsigned char) ((byte_length + 1) >> 1);
        *dp++ = (unsigned char) ((byte_length + 1) >> 9);
        *dp++ = (unsigned char) ((byte_length + 1) >> 17);
    }
    else {
        *dp++ = (unsigned char) (id | (byte_length & 1 ? ID_ODD_SIZE : 0));
        *dp++ = (unsigned char) ((byte_length + 1) >> 1);
    }

    if (byte_length && data)
        memcpy (dp, data, byte_length);

    b->len += (size_t) (header + padded);
}

void fwp_builder_copy_chunk (fwp_builder *b, const fwp_chunk *chunk)
{
    int span = chunk->byte_length + (chunk->byte_length & 1);
    int header = (chunk->raw_id & ID_LARGE) ? 4 : 2;
    size_t total = (size_t) (header + span);

    if (b->len + total > b->cap)
        return;

    memcpy (b->buf + b->len, chunk->raw, total);
    b->len += total;
}

// LSB-first bit writer over a plain byte buffer, matching the Wavpack
// bitstream convention. Used to pack the new-format WVX preamble and
// sideband into one contiguous bit field rather than byte-aligned pieces.

static void put_bit (unsigned char *buf, int *bitpos, int bit)
{
    if (bit)
        buf [*bitpos >> 3] |= (unsigned char) (1u << (*bitpos & 7));

    (*bitpos)++;
}

// Build one WVX chunk. "preamble_bits" (0 for the legacy layout, 10 for the
// new float layout) are packed LSB-first first, followed immediately by the
// sideband bits with no byte realignment. The decoder's getbits() calls read
// the preamble and sideband as one contiguous bitstream, so the two must
// share bytes. Four zero bytes of physical padding keep the Bitstream reads
// in reserve; the chunk length stays even as init_wvx_bitstream() requires.

static void fwp_add_wvx (fwp_builder *b, unsigned char id, uint32_t crc_x,
                         int preamble_bits, uint16_t preamble,
                         const unsigned char *sideband, int valid_bits)
{
    unsigned char payload [256];
    int total_bits = preamble_bits + valid_bits;
    int stream_bytes = (total_bits + 7) >> 3;
    int data_len = 4 + stream_bytes + FWP_PHYS_PADDING;
    int bitpos = 0, i;

    if (data_len & 1)
        data_len++;

    if (data_len > (int) sizeof (payload))
        return;

    memset (payload, 0, (size_t) data_len);
    wr32 (payload, crc_x);

    for (i = 0; i < preamble_bits; i++)
        put_bit (payload + 4, &bitpos, (preamble >> i) & 1);

    for (i = 0; i < valid_bits; i++)
        put_bit (payload + 4, &bitpos, (sideband [i >> 3] >> (i & 7)) & 1);

    fwp_builder_add_chunk (b, id, payload, data_len);
}

void fwp_builder_wvx_legacy (fwp_builder *b, uint32_t crc_x,
                             const unsigned char *sideband, int valid_bits)
{
    fwp_add_wvx (b, ID_WVX_BITSTREAM, crc_x, 0, 0, sideband, valid_bits);
}

void fwp_builder_wvx_new (fwp_builder *b, uint32_t crc_x,
                          int min_shifted_zeros, int max_shifted_ones,
                          const unsigned char *sideband, int valid_bits)
{
    // two 5-bit fields, min_shifted_zeros first in the LSB-first stream
    uint16_t preamble = (uint16_t) ((min_shifted_zeros & 0x1f) |
        ((max_shifted_ones & 0x1f) << 5));

    fwp_add_wvx (b, ID_WVX_NEW_BITSTREAM, crc_x, 10, preamble,
        sideband, valid_bits);
}

static uint32_t block_checksum (const unsigned char *block, int word_count)
{
    uint32_t csum = 0xffffffffu;
    int i;

    for (i = 0; i < word_count; i++)
        csum = csum * 3u + (uint32_t) block [i * 2] +
            ((uint32_t) block [i * 2 + 1] << 8);

    return csum;
}

size_t fwp_rebuild_with_wvx (const fwp_block *src, fwp_builder *b,
                             unsigned char wvx_id,
                             int min_shifted_zeros, int max_shifted_ones,
                             const unsigned char *sideband, int valid_bits,
                             uint32_t crc_x)
{
    int i;

    for (i = 0; i < src->nchunks; i++) {
        const fwp_chunk *ch = src->chunks + i;

        if (ch->id == ID_WVX_BITSTREAM)
            continue;                       // skip the real WVX, replaced below

        if (ch->id == ID_BLOCK_CHECKSUM)
            continue;                       // recomputed by fwp_builder_finish()

        fwp_builder_copy_chunk (b, ch);
    }

    if (wvx_id == ID_WVX_NEW_BITSTREAM)
        fwp_builder_wvx_new (b, crc_x, min_shifted_zeros, max_shifted_ones,
            sideband, valid_bits);
    else
        fwp_builder_wvx_legacy (b, crc_x, sideband, valid_bits);

    return fwp_builder_finish (b);
}

size_t fwp_builder_finish (fwp_builder *b)
{
    WavpackHeader *h = (WavpackHeader *) b->buf;
    unsigned char *dp;
    uint32_t csum;
    int word_count;

    // Mirror block_add_checksum(): the checksum chunk itself (6 bytes, an
    // even payload) is included in ckSize before the checksum is calculated,
    // so the checksum covers the final on-disk ckSize and flags values.

    h->ckSize = (uint32_t) b->len - 8 + 6;
    h->flags |= HAS_CHECKSUM;
    WavpackNativeToLittleEndian (h, WavpackHeaderFormat);

    word_count = (int) b->len >> 1;
    csum = block_checksum (b->buf, word_count);

    dp = b->buf + b->len;
    *dp++ = ID_BLOCK_CHECKSUM;
    *dp++ = 2;                    // four payload bytes
    wr32 (dp, csum);
    b->len += 6;

    return b->len;
}
