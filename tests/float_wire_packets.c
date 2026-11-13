////////////////////////////////////////////////////////////////////////////
// float_wire_packets.c
//
// Container-side material for the independent float wire regression. The
// chunk walker below follows the published metadata framing rules and never
// relies on production parsing code, while the round trips exercise the
// real public encode/decode API on complete WavPack files.
////////////////////////////////////////////////////////////////////////////

#include "float_wire_packets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------- little-endian helpers -----------------------------

uint16_t fw_get_le16 (const unsigned char *p)
{
    return (uint16_t)(p [0] | ((uint16_t) p [1] << 8));
}

uint32_t fw_get_le32 (const unsigned char *p)
{
    return (uint32_t) p [0] | ((uint32_t) p [1] << 8) |
        ((uint32_t) p [2] << 16) | ((uint32_t) p [3] << 24);
}

void fw_put_le16 (unsigned char *p, uint16_t v)
{
    p [0] = (unsigned char) v;
    p [1] = (unsigned char)(v >> 8);
}

void fw_put_le32 (unsigned char *p, uint32_t v)
{
    p [0] = (unsigned char) v;
    p [1] = (unsigned char)(v >> 8);
    p [2] = (unsigned char)(v >> 16);
    p [3] = (unsigned char)(v >> 24);
}

// ----------------------------- block / chunk walking -----------------------------

int fw_block_parse (const unsigned char *data, size_t size, fw_block_view *view)
{
    const unsigned char *dp, *end;

    if (size < 32 || memcmp (data, "wvpk", 4))
        return -1;

    memset (view, 0, sizeof (*view));
    view->block = data;
    view->block_size = size;
    view->ck_size = fw_get_le32 (data + 4);
    view->version = (int16_t) fw_get_le16 (data + 8);
    view->total_samples = fw_get_le32 (data + 12);
    view->block_index = fw_get_le32 (data + 16);
    view->block_samples = fw_get_le32 (data + 20);
    view->flags = fw_get_le32 (data + 24);
    view->crc = fw_get_le32 (data + 28);

    if (view->ck_size + 8u > size)
        return -1;

    dp = data + 32;
    end = data + view->ck_size + 8;
    view->nchunks = 0;

    while (end - dp >= 2) {
        unsigned char id = *dp++;
        int32_t byte_length = (int32_t)(*dp++ << 1);
        int odd = 0;

        if (id & FW_ID_LARGE) {
            id &= (unsigned char) ~FW_ID_LARGE;

            if (end - dp < 2)
                return -1;

            byte_length += (int32_t)(*dp++ << 9);
            byte_length += (int32_t)(*dp++ << 17);
        }

        if (id & FW_ID_ODD_SIZE) {
            if (!byte_length)
                return -1;

            id &= (unsigned char) ~FW_ID_ODD_SIZE;
            odd = 1;
            byte_length--;
        }

        if (end - dp < byte_length + (byte_length & 1))
            return -1;

        if (view->nchunks < FW_MAX_CHUNKS) {
            fw_chunk *chunk = &view->chunks [view->nchunks++];
            chunk->id = id;
            chunk->odd_size = odd;
            chunk->byte_length = byte_length;
            chunk->data = byte_length ? dp : NULL;
        }

        dp += byte_length + (byte_length & 1);
    }

    return 0;
}

const fw_chunk *fw_block_find (const fw_block_view *view, unsigned char id)
{
    int i;

    for (i = 0; i < view->nchunks; i++)
        if (view->chunks [i].id == id)
            return &view->chunks [i];

    return NULL;
}

// Locate either the legacy or the new WVX chunk (the two share id 0xc, with
// the new one additionally carrying the optional-data marker 0x20).

const fw_chunk *fw_block_find_wvx (const fw_block_view *view)
{
    const fw_chunk *chunk = fw_block_find (view, FW_ID_WVX_NEW_BITSTREAM);

    return chunk ? chunk : fw_block_find (view, FW_ID_WVX_BITSTREAM);
}

int fw_read_float_info (const fw_chunk *chunk, unsigned *float_flags,
    unsigned *float_shift, unsigned *max_exp, unsigned *norm_exp)
{
    if (!chunk || chunk->id != FW_ID_FLOAT_INFO || chunk->byte_length != 4)
        return -1;

    *float_flags = chunk->data [0];
    *float_shift = chunk->data [1];
    *max_exp = chunk->data [2];
    *norm_exp = chunk->data [3];
    return 0;
}

int fw_open_wvx (const fw_chunk *chunk, int is_float, fw_wvx_view *wvx)
{
    const unsigned char *dp;

    memset (wvx, 0, sizeof (*wvx));

    if (!chunk || (chunk->id != FW_ID_WVX_BITSTREAM &&
        chunk->id != FW_ID_WVX_NEW_BITSTREAM) || chunk->byte_length <= 4 ||
        (chunk->byte_length & 1))
        return -1;

    wvx->crc_wvx = fw_get_le32 (chunk->data);
    dp = chunk->data + 4;
    wvx->payload = dp;
    wvx->payload_bytes = (size_t) chunk->byte_length - 4;

    // The new format marks itself through the optional-data bit on the id,
    // which fw_block_parse keeps distinct from the legacy id.
    if (chunk->id == FW_ID_WVX_NEW_BITSTREAM) {
        wvx->new_format = 1;

        if (is_float) {
            // Two 5-bit fields, LSB first: min_shifted_zeros then
            // max_shifted_ones, packed into the first payload byte(s).
            wvx->min_shifted_zeros = dp [0] & 0x1fu;
            wvx->max_shifted_ones = ((dp [0] >> 5) | (dp [1] << 3)) & 0x1fu;
            wvx->payload += 2;          // the 10 bits occupy two wire bytes
            wvx->payload_bytes -= 2;
        }
    }

    return 0;
}

// ----------------------------- growable blob -----------------------------

void fw_blob_init (fw_blob *blob)
{
    blob->cap = 4096;
    blob->size = 0;
    blob->data = (unsigned char *) malloc (blob->cap);
}

void fw_blob_free (fw_blob *blob)
{
    free (blob->data);
    blob->data = NULL;
    blob->size = blob->cap = 0;
}

int fw_blob_append (fw_blob *blob, const void *data, int32_t n)
{
    if (n < 0)
        return -1;

    while ((size_t) n + blob->size > blob->cap)
        blob->cap *= 2;

    blob->data = (unsigned char *) realloc (blob->data, blob->cap);
    memcpy (blob->data + blob->size, data, (size_t) n);
    blob->size += (size_t) n;
    return 0;
}

static int blob_block_output (void *id, void *data, int32_t n)
{
    return fw_blob_append ((fw_blob *) id, data, n) == 0;
}

// ----------------------------- public in-memory reader -----------------------------

static int32_t mem_read_bytes (void *id, void *data, int32_t bcount)
{
    fw_memory_id *mem = (fw_memory_id *) id;
    unsigned char *out = (unsigned char *) data;
    int32_t done = 0;

    while (bcount) {
        if (mem->ungetc_flag) {
            *out++ = mem->ungetc_char;
            mem->ungetc_flag = 0;
            bcount--;
            done++;
        }
        else {
            size_t avail = mem->size - mem->pos;

            if (!avail)
                break;

            if (avail > (size_t) bcount)
                avail = (size_t) bcount;

            memcpy (out, mem->data + mem->pos, avail);
            mem->pos += avail;
            out += avail;
            done += (int32_t) avail;
            bcount -= (int32_t) avail;
        }
    }

    return done;
}

static int32_t mem_write_bytes (void *id, void *data, int32_t bcount)
{
    (void) id;
    (void) data;
    return bcount;
}

static int64_t mem_get_pos (void *id)
{
    fw_memory_id *mem = (fw_memory_id *) id;
    return (int64_t) mem->pos - (mem->ungetc_flag ? 1 : 0);
}

static int mem_set_pos_abs (void *id, int64_t pos)
{
    fw_memory_id *mem = (fw_memory_id *) id;

    if (pos < 0 || (size_t) pos > mem->size)
        return 1;

    mem->pos = (size_t) pos;
    mem->ungetc_flag = 0;
    return 0;
}

static int mem_set_pos_rel (void *id, int64_t delta, int mode)
{
    fw_memory_id *mem = (fw_memory_id *) id;
    int64_t base;

    if (mode == SEEK_SET)
        base = 0;
    else if (mode == SEEK_END)
        base = (int64_t) mem->size;
    else
        base = (int64_t) mem->pos;

    return mem_set_pos_abs (id, base + delta);
}

static int mem_push_back_byte (void *id, int c)
{
    fw_memory_id *mem = (fw_memory_id *) id;
    mem->ungetc_char = (unsigned char) c;
    mem->ungetc_flag = 1;
    return c;
}

static int64_t mem_get_length (void *id)
{
    return (int64_t) ((fw_memory_id *) id)->size;
}

static int mem_can_seek (void *id)
{
    (void) id;
    return 1;
}

static int mem_close (void *id)
{
    (void) id;
    return 0;
}

WavpackStreamReader64 fw_memory_reader = {
    mem_read_bytes, mem_write_bytes, mem_get_pos, mem_set_pos_abs, mem_set_pos_rel,
    mem_push_back_byte, mem_get_length, mem_can_seek, NULL, mem_close
};

void fw_memory_id_init (fw_memory_id *id, const fw_blob *blob)
{
    id->data = blob->data;
    id->size = blob->size;
    id->pos = 0;
    id->ungetc_flag = 0;
}

// ----------------------------- public API round trips -----------------------------

int fw_pack_float_blob (const uint32_t *patterns, uint32_t frames, int num_channels,
    uint32_t channel_mask, uint32_t block_samples, fw_blob *blob)
{
    WavpackConfig cfg;
    WavpackContext *wpc;
    char err [128];

    memset (&cfg, 0, sizeof (cfg));
    cfg.bytes_per_sample = 4;
    cfg.bits_per_sample = 32;
    cfg.float_norm_exp = 127;
    cfg.num_channels = num_channels;
    cfg.channel_mask = (int32_t) channel_mask;
    cfg.sample_rate = 48000;
    cfg.block_samples = (int32_t) block_samples;

    wpc = WavpackOpenFileOutput (blob_block_output, blob, NULL);

    if (!wpc)
        return -1;

    if (!WavpackSetConfiguration64 (wpc, &cfg, (int64_t) frames, NULL)) {
        strcpy (err, WavpackGetErrorMessage (wpc));
        WavpackCloseFile (wpc);
        fprintf (stderr, "set configuration failed: %s\n", err);
        return -1;
    }

    if (!WavpackPackInit (wpc)) {
        WavpackCloseFile (wpc);
        return -1;
    }

    // Hand every frame to the library in one call; it partitions the stream
    // into fixed blocks internally using the configured block_samples, which
    // keeps the per-block shared exponent calculations aligned. The count is
    // in frames; the library multiplies by the channel count internally.
    if (!WavpackPackSamples (wpc, (int32_t *) patterns, frames)) {
        WavpackCloseFile (wpc);
        return -1;
    }

    if (!WavpackFlushSamples (wpc)) {
        WavpackCloseFile (wpc);
        return -1;
    }

    WavpackCloseFile (wpc);
    return blob->size ? 0 : -1;
}

uint32_t fw_unpack_float_blob (const fw_blob *blob, uint32_t frames, int num_channels,
    uint32_t batch_frames, uint32_t *out_patterns, uint32_t *error_count)
{
    fw_memory_id mem;
    WavpackContext *wpc;
    char err [128];
    uint32_t done = 0;

    fw_memory_id_init (&mem, blob);
    wpc = WavpackOpenFileInputEx64 (&fw_memory_reader, &mem, NULL, err, 0, 0);

    if (!wpc) {
        fprintf (stderr, "open failed: %s\n", err);
        return 0;
    }

    if (!(WavpackGetMode (wpc) & MODE_FLOAT) || WavpackGetNumChannels (wpc) != num_channels) {
        WavpackCloseFile (wpc);
        return 0;
    }

    while (done < frames) {
        uint32_t want = frames - done;
        uint32_t got;

        if (want > batch_frames)
            want = batch_frames;

        got = WavpackUnpackSamples (wpc, (int32_t *)(out_patterns +
            (size_t) done * num_channels), want);

        if (!got)
            break;

        done += got;
    }

    // WavpackGetNumErrors() returns the cumulative count for the whole file.
    *error_count = (uint32_t) WavpackGetNumErrors (wpc);
    WavpackCloseFile (wpc);
    return done;
}

// ----------------------------- container flag reporting -----------------------------

int fw_report_container (const fw_blob *blob, fw_container_report *report)
{
    const unsigned char *dp = blob->data;
    const unsigned char *end = blob->data + blob->size;
    int cap = 16;

    memset (report, 0, sizeof (*report));
    report->report_cap = cap;
    report->reports = (fw_block_report *) calloc ((size_t) cap, sizeof (fw_block_report));

    while ((size_t)(end - dp) >= 32) {
        fw_block_view view;
        fw_block_report *br;
        const fw_chunk *fi, *wvx;
        size_t block_size;

        if (fw_block_parse (dp, (size_t)(end - dp), &view))
            goto fail;

        block_size = view.ck_size + 8;

        if (report->nblocks == cap) {
            cap *= 2;
            report->reports = (fw_block_report *) realloc (report->reports,
                (size_t) cap * sizeof (fw_block_report));
            memset (report->reports + report->nblocks, 0,
                (size_t)(cap - report->nblocks) * sizeof (fw_block_report));
            report->report_cap = cap;
        }

        br = &report->reports [report->nblocks];
        br->block_samples = view.block_samples;
        br->flags = view.flags;
        br->block_index = view.block_index;

        fi = fw_block_find (&view, FW_ID_FLOAT_INFO);
        br->has_float_info = !!fi;

        if (fi) {
            unsigned norm;
            report->float_info_blocks++;

            if (fw_read_float_info (fi, &br->float_flags, &br->float_shift,
                &br->max_exp, &norm))
                goto fail;

            report->float_flag_union |= br->float_flags;
        }

        wvx = fw_block_find_wvx (&view);
        br->has_wvx = !!wvx;

        if (wvx) {
            fw_wvx_view wvx_view;

            report->wvx_blocks++;

            if (fw_open_wvx (wvx, 1, &wvx_view))
                goto fail;

            br->has_new_wvx = wvx_view.new_format;

            if (wvx_view.new_format)
                report->new_wvx_blocks++;
        }

        report->nblocks++;
        dp += block_size;
    }

    if (dp != end)
        goto fail;

    return 0;

fail:
    fw_report_free (report);
    return -1;
}

void fw_report_free (fw_container_report *report)
{
    free (report->reports);
    report->reports = NULL;
    report->report_cap = report->nblocks = 0;
}

// ----------------------------- hand-assembled new WVX material -----------------------------

// Verify that the literal low bits of every SHIFT_SENT value respect the
// elision guarantees of the requested new-format header fields.

static int elision_is_valid (const uint32_t *patterns, int n, unsigned max_exp,
    unsigned min_shifted_zeros, unsigned max_shifted_ones)
{
    int i;

    for (i = 0; i < n; i++) {
        unsigned exponent = FW_EXPONENT (patterns [i]);
        uint32_t mantissa = FW_MANTISSA (patterns [i]);
        int shift_count;

        if (exponent == FW_EXP_MAX || !exponent)
            continue;

        shift_count = (int) max_exp - (int) exponent;

        if (shift_count <= 0)
            continue;

        {
            uint32_t low = mantissa & ((1u << shift_count) - 1);
            int num_zeros = 0;

            if (max_shifted_ones && shift_count > (int) max_shifted_ones)
                num_zeros = shift_count - (int) max_shifted_ones;

            if ((int) min_shifted_zeros > num_zeros)
                num_zeros = ((int) min_shifted_zeros > shift_count) ? shift_count :
                    (int) min_shifted_zeros;

            if (num_zeros && (low & ((1u << num_zeros) - 1u)))
                return 0;

            if (max_shifted_ones && shift_count > (int) max_shifted_ones &&
                (low >> max_shifted_ones))
                return 0;
        }
    }

    return 1;
}

static void put_chunk_header (fw_blob *blob, unsigned char id, int32_t byte_length)
{
    unsigned char hdr [4];
    int large = byte_length > 510;
    int hlen = large ? 4 : 2;

    memset (hdr, 0, sizeof (hdr));
    hdr [0] = (unsigned char)(id | (large ? FW_ID_LARGE : 0));
    hdr [1] = (unsigned char)((byte_length >> 1) & 0xff);

    if (large) {
        hdr [2] = (unsigned char)((byte_length >> 9) & 0xff);
        hdr [3] = (unsigned char)((byte_length >> 17) & 0xff);
    }

    fw_blob_append (blob, hdr, hlen);
}

int fw_build_new_wvx_material (const uint32_t *patterns, int n,
    unsigned min_shifted_zeros, unsigned max_shifted_ones, fw_blob *blob,
    fw_bits *expected_sideband)
{
    fw_scan_result scan;
    int32_t *integers;
    unsigned char header [32];
    unsigned char info [4];
    unsigned char hdr10 [2];
    size_t payload_bytes, chunk_bytes;
    unsigned i;

    if (min_shifted_zeros > 31 || max_shifted_ones > 31 || n <= 0)
        return -1;

    integers = (int32_t *) malloc ((size_t) n * sizeof (*integers));
    fw_scan_float (patterns, n, &scan, integers);
    free (integers);

    if (!(scan.flags & FW_FLAG_SHIFT_SENT))
        return -1;      // the new-format elision fields only matter here

    if (!elision_is_valid (patterns, n, scan.max_exp, min_shifted_zeros, max_shifted_ones))
        return -1;

    fw_bits_init (expected_sideband);
    fw_send_plan_elided (patterns, n, scan.flags, scan.max_exp,
        min_shifted_zeros, max_shifted_ones, expected_sideband);

    // Physical layout of the WVX chunk: 32-bit crc_x, two bytes holding the
    // 10-bit new-format header, the LSB-first payload, then explicit padding
    // so the production Bitstream always has full cells in front of every
    // read it performs (a 23-bit getbits may prefetch two cells ahead). The
    // padding is not part of the valid bit count.
    payload_bytes = fw_bits_padded_bytes (expected_sideband);
    chunk_bytes = 4 + 2 + payload_bytes + 8;       // 8 trailing physical pad bytes

    fw_blob_init (blob);
    memset (header, 0, sizeof (header));
    memcpy (header, "wvpk", 4);

    // ckSize is patched at the end from the real blob length.
    fw_put_le16 (header + 8, 0x410);               // current stream version
    fw_put_le32 (header + 20, (uint32_t) n);       // block_samples
    fw_put_le32 (header + 24, 0x80 | 0x4 | 0x800 | 0x1000); // FLOAT_DATA, mono flag,
                                                            // initial + final block
    fw_blob_append (blob, header, sizeof (header));

    // FLOAT_INFO chunk (id 0x8, four bytes).
    info [0] = (unsigned char) scan.flags;
    info [1] = (unsigned char) scan.float_shift;
    info [2] = (unsigned char) scan.max_exp;
    info [3] = 127;                                // float_norm_exp
    put_chunk_header (blob, FW_ID_FLOAT_INFO, 4);
    fw_blob_append (blob, info, 4);

    // A structurally valid, even-length WV bitstream placeholder. The float
    // decode under test is driven directly through float_values(), so the
    // entropy payload itself is never consulted there.
    put_chunk_header (blob, 0xa, 2);
    {
        unsigned char dummy [2] = { 0, 0 };
        fw_blob_append (blob, dummy, 2);
    }

    // New WVX chunk. Encode the two 5-bit fields LSB-first into two bytes.
    hdr10 [0] = (unsigned char)((min_shifted_zeros & 0x1f) |
        ((max_shifted_ones & 0x1f) << 5));
    hdr10 [1] = (unsigned char)((max_shifted_ones >> 3) & 0x3);
    put_chunk_header (blob, FW_ID_WVX_NEW_BITSTREAM, (int32_t) chunk_bytes);
    {
        unsigned char crc [4];
        fw_put_le32 (crc, scan.crc_x);
        fw_blob_append (blob, crc, 4);
        fw_blob_append (blob, hdr10, 2);
        fw_blob_append (blob, expected_sideband->data, payload_bytes);

        for (i = 0; i < 8; i++) {
            unsigned char pad = 0;
            fw_blob_append (blob, &pad, 1);
        }
    }

    // Patch the block size from the bytes actually emitted; every chunk
    // header is 2 bytes here because no chunk exceeds the 510-byte limit.
    fw_put_le32 (blob->data + 4, (uint32_t)(blob->size - 8));
    return 0;
}
