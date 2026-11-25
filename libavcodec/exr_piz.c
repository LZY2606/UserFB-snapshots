/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * OpenEXR PIZ compression helpers: used-value remapping, Haar wavelets and
 * canonical Huffman coding.
 *
 * The byte stream produced here follows the PIZ layout understood by
 * libavcodec's EXR decoder.
 */

#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "exr_piz.h"

#define HUF_BITS              16
#define HUF_ENCSIZE           EXR_PIZ_HUF_ENCSIZE
#define SHORT_ZEROCODE_RUN    59
#define LONG_ZEROCODE_RUN     63
#define SHORTEST_LONG_RUN     (2 + LONG_ZEROCODE_RUN - SHORT_ZEROCODE_RUN)
#define LONGEST_LONG_RUN      (255 + SHORTEST_LONG_RUN)

struct EXRPizWork {
    uint16_t *words;
    size_t    words_capacity;

    uint8_t  bitmap[EXR_PIZ_BITMAP_SIZE];
    uint16_t lut[EXR_PIZ_USHORT_RANGE];

    /* freq owns the Huffman heap keys; codes becomes length|(code << 6). */
    uint64_t freq[HUF_ENCSIZE];
    uint64_t codes[HUF_ENCSIZE];
    int32_t  link[HUF_ENCSIZE];
    int      heap[HUF_ENCSIZE];
    int      heap_size;

    /* Largest secondary VLC width required by each 12-bit primary prefix. */
    uint8_t  vlc_subtable_bits[4096];
};

EXRPizWork *ff_exr_piz_work_alloc(void)
{
    return av_mallocz(sizeof(EXRPizWork));
}

void ff_exr_piz_work_free(EXRPizWork **work)
{
    if (!work || !*work)
        return;
    av_freep(&(*work)->words);
    av_freep(work);
}

size_t ff_exr_piz_max_block_size(size_t raw_size)
{
    /*
     * Huffman output is close to entropy-sized and is bounded by the
     * encoder's length limiting; the large fixed allowance covers both
     * bitmap extrema, the bitmap itself, the PIZ Huffman length prefix,
     * 20 bytes of Huffman header and the packed code-length table.
     */
    return raw_size + raw_size / 16 +
           EXR_PIZ_BITMAP_SIZE + 65536 + 64;
}

/* Bit writer using the same MSB-first packing as the EXR Huffman stream. */
typedef struct PizBits {
    uint8_t *out;
    uint8_t *end;
    uint64_t acc;
    int      left;
    int      total_bits;
    int      overflow;
} PizBits;

static void piz_put_bits(PizBits *b, int n, uint64_t value)
{
    if (b->overflow)
        return;

    av_assert2(n > 0 && n <= 58);
    b->acc = (b->acc << n) | (value & ((((uint64_t)1 << (n - 1)) << 1) - 1));
    b->left += n;
    b->total_bits += n;

    while (b->left >= 8) {
        if (b->out >= b->end) {
            b->overflow = 1;
            return;
        }
        b->left -= 8;
        *b->out++ = (uint8_t)((b->acc >> b->left) & 0xff);
    }
}

static void piz_put_code(PizBits *b, uint64_t code)
{
    piz_put_bits(b, (int)(code & 63), code >> 6);
}

/* Flush a partial byte.  The stream pads the final byte with zero bits. */
static int piz_flush_byte(PizBits *b)
{
    if (b->overflow || b->out >= b->end)
        return AVERROR_BUFFER_TOO_SMALL;

    if (b->left > 0) {
        *b->out++ = (uint8_t)((b->acc << (8 - b->left)) & 0xff);
        b->acc = 0;
        b->left = 0;
    }

    return 0;
}

static void heap_sift_down(EXRPizWork *w, int root, int n)
{
    int *heap = w->heap;

    for (;;) {
        int left = root * 2 + 1;
        int right = left + 1;
        int best = root;

        if (left < n) {
            if (w->freq[heap[left]] < w->freq[heap[best]] ||
                (w->freq[heap[left]] == w->freq[heap[best]] &&
                 heap[left] < heap[best]))
                best = left;
        }
        if (right < n) {
            if (w->freq[heap[right]] < w->freq[heap[best]] ||
                (w->freq[heap[right]] == w->freq[heap[best]] &&
                 heap[right] < heap[best]))
                best = right;
        }

        if (best == root)
            break;

        FFSWAP(int, heap[root], heap[best]);
        root = best;
    }
}

static void heap_sift_up(EXRPizWork *w, int node)
{
    int *heap = w->heap;

    while (node > 0) {
        int parent = (node - 1) / 2;
        int a = heap[node];
        int b = heap[parent];

        if (w->freq[a] > w->freq[b] ||
            (w->freq[a] == w->freq[b] && a >= b))
            break;

        FFSWAP(int, heap[node], heap[parent]);
        node = parent;
    }
}

static void heap_push(EXRPizWork *w, int symbol)
{
    w->heap[w->heap_size] = symbol;
    heap_sift_up(w, w->heap_size);
    w->heap_size++;
}

static int heap_pop(EXRPizWork *w)
{
    int symbol = w->heap[0];

    w->heap[0] = w->heap[--w->heap_size];
    if (w->heap_size > 1)
        heap_sift_down(w, 0, w->heap_size);

    return symbol;
}

/* Assign the canonical code numbers expected by the in-tree EXR decoder. */
static void huf_canonical_table(uint64_t *table)
{
    uint64_t count[59] = { 0 };
    uint64_t code = 0;

    for (int i = 0; i < HUF_ENCSIZE; i++)
        count[table[i]]++;

    for (int len = 58; len > 0; len--) {
        uint64_t next = (code + count[len]) >> 1;
        count[len] = code;
        code = next;
    }

    for (int i = 0; i < HUF_ENCSIZE; i++) {
        unsigned len = table[i];

        if (len > 0)
            table[i] = len | (count[len]++ << 6);
    }
}

/*
 * Extremely skewed input can produce a deeper tree than EXR's FFmpeg VLC
 * accepts.  Replace the lengths with a short equal-length canonical code in
 * that rare case.  It remains a valid prefix code and still benefits common
 * and run-length symbols.
 */
static void limit_or_equal_lengths(EXRPizWork *w, int im, int iM)
{
    int used = 0;
    int bits = 0;
    int deepest = 0;

    for (int i = im; i <= iM; i++) {
        int len = (int)w->codes[i];
        if (len > deepest)
            deepest = len;
        if (w->freq[i] > 0)
            used++;
    }

    if (deepest <= 20)
        return;

    while ((1 << bits) < used)
        bits++;

    memset(w->codes, 0, sizeof(w->codes));
    for (int i = im; i <= iM; i++) {
        if (w->freq[i] > 0)
            w->codes[i] = bits;
    }
}

static int huf_build_table(EXRPizWork *w, int symbols, int *im_out, int *iM_out)
{
    int im = 0, iM, first = -1, last = -1;

    memset(w->freq, 0, sizeof(w->freq));
    memset(w->codes, 0, sizeof(w->codes));

    for (int i = 0; i < symbols; i++)
        w->freq[w->words[i]]++;

    for (int i = 0; i < EXR_PIZ_USHORT_RANGE; i++) {
        if (w->freq[i]) {
            if (first < 0)
                first = i;
            last = i;
        }
    }

    /* A block always contains words; the pseudo-symbol is added below. */
    if (first < 0)
        return AVERROR_INVALIDDATA;

    im = first;
    iM = last + 1;
    if (iM >= HUF_ENCSIZE)
        return AVERROR_INVALIDDATA;

    /* Pseudo-symbol used by the EXR run-length Huffman directive. */
    w->freq[iM] = 1;

    for (int i = im; i <= iM; i++)
        w->link[i] = i;

    w->heap_size = 0;
    for (int i = im; i <= iM; i++) {
        if (w->freq[i])
            heap_push(w, i);
    }

    while (w->heap_size > 1) {
        int mm = heap_pop(w);
        int m  = heap_pop(w);

        w->freq[m] += w->freq[mm];

        /* Add one bit to every leaf in m's linked descendant list. */
        for (int j = m;; j = w->link[j]) {
            w->codes[j]++;
            if (w->link[j] == j) {
                w->link[j] = mm;
                break;
            }
        }

        for (int j = mm;; j = w->link[j]) {
            w->codes[j]++;
            if (w->link[j] == j)
                break;
        }

        heap_push(w, m);
    }

    limit_or_equal_lengths(w, im, iM);
    huf_canonical_table(w->codes);

    *im_out = im;
    *iM_out = iM;
    return 0;
}

/*
 * Estimate whether the EXR decoder's 12-bit VLC can represent this table.
 * Our length limiter keeps all codes at <= 20 bits, so every overlong code
 * needs at most one secondary table of 2^(length - 12) entries.  If the
 * cumulative table index would not fit the decoder's int16_t symbol field,
 * the caller falls back to a raw PIZ block.
 */
static int huf_vlc_compatible(EXRPizWork *w, int im, int iM)
{
    long table_entries = 1 << 12;

    memset(w->vlc_subtable_bits, 0, sizeof(w->vlc_subtable_bits));

    for (int i = im; i <= iM; i++) {
        int len = (int)(w->codes[i] & 63);
        if (len > 12) {
            uint32_t code = (uint32_t)(w->codes[i] >> 6);
            uint32_t primary = code >> (len - 12);
            w->vlc_subtable_bits[primary] =
                FFMAX(w->vlc_subtable_bits[primary], len - 12);
        }
    }

    for (int p = 0; p < 1 << 12; p++) {
        if (w->vlc_subtable_bits[p])
            table_entries += 1L << w->vlc_subtable_bits[p];
    }

    /* Keep every returned secondary-table index below INT16_MAX. */
    return table_entries <= 32767;
}

static int huf_pack_table(const EXRPizWork *w, uint8_t *table, uint8_t *table_end,
                          int im, int iM, size_t *table_size)
{
    PizBits b = {
        .out = table, .end = table_end, .acc = 0, .left = 0,
        .total_bits = 0, .overflow = 0
    };

    for (int i = im; i <= iM;) {
        int len = (int)(w->codes[i] & 63);

        if (len == 0) {
            int run = 1;

            while (i + run <= iM && run < LONGEST_LONG_RUN &&
                   (w->codes[i + run] & 63) == 0)
                run++;

            if (run >= 2) {
                if (run >= SHORTEST_LONG_RUN) {
                    piz_put_bits(&b, 6, LONG_ZEROCODE_RUN);
                    piz_put_bits(&b, 8, run - SHORTEST_LONG_RUN);
                } else {
                    piz_put_bits(&b, 6, SHORT_ZEROCODE_RUN + run - 2);
                }
                i += run;
                continue;
            }
        }

        piz_put_bits(&b, 6, len);
        i++;
    }

    if (b.overflow)
        return AVERROR_BUFFER_TOO_SMALL;

    if (piz_flush_byte(&b) < 0)
        return AVERROR_BUFFER_TOO_SMALL;

    *table_size = b.out - table;
    return 0;
}

static void huf_send_code(PizBits *b, unsigned symbol, int run_count,
                          const uint64_t *codes, int run_symbol)
{
    int symbol_len = (int)(codes[symbol] & 63);
    int run_len    = (int)(codes[run_symbol] & 63);

    /* run_count is the number of extra copies after the first symbol. */
    if (symbol_len + run_len + 8 < symbol_len * run_count) {
        piz_put_code(b, codes[symbol]);
        piz_put_code(b, codes[run_symbol]);
        piz_put_bits(b, 8, (unsigned)run_count);
    } else {
        for (int i = 0; i <= run_count; i++)
            piz_put_code(b, codes[symbol]);
    }
}

static int huf_encode_data(const EXRPizWork *w, int symbols, int run_symbol,
                           uint8_t *data, uint8_t *end, int *n_bits,
                           size_t *data_size)
{
    PizBits b = {
        .out = data, .end = end, .acc = 0, .left = 0,
        .total_bits = 0, .overflow = 0
    };
    unsigned current = w->words[0];
    int run = 0;

    for (int i = 1; i < symbols; i++) {
        if (w->words[i] == current && run < 255) {
            run++;
        } else {
            huf_send_code(&b, current, run, w->codes, run_symbol);
            current = w->words[i];
            run = 0;
        }
    }
    huf_send_code(&b, current, run, w->codes, run_symbol);

    if (b.overflow)
        return AVERROR_BUFFER_TOO_SMALL;

    *n_bits = b.total_bits;
    if (piz_flush_byte(&b) < 0)
        return AVERROR_BUFFER_TOO_SMALL;

    *data_size = b.out - data;
    return 0;
}

/* Inverse of the in-tree wdec14(). */
static inline void wenc14(uint16_t a, uint16_t b,
                          uint16_t *low, uint16_t *high)
{
    int16_t as = a;
    int16_t bs = b;
    int16_t ms = (int16_t)((as + bs) >> 1);
    int16_t ds = (int16_t)(as - bs);

    *low  = (uint16_t)ms;
    *high = (uint16_t)ds;
}

/* Inverse of the in-tree wdec16(). */
static inline void wenc16(uint16_t a, uint16_t b,
                          uint16_t *low, uint16_t *high)
{
    const int mask = 0xffff;
    int ao = (a + 0x8000) & mask;
    int m  = (ao + b) >> 1;
    int d  = ao - b;

    if (d < 0)
        m = (m + 0x8000) & mask;

    d &= mask;
    *low = (uint16_t)m;
    *high = (uint16_t)d;
}

static void wav2_encode(uint16_t *in, int nx, int ox, int ny, int oy,
                        uint16_t max_value)
{
    int use14 = max_value < (1 << 14);
    int n = FFMIN(nx, ny);
    int p = 1;
    int p2 = 2;

    while (p2 <= n) {
        uint16_t *py = in;
        uint16_t *ey = in + oy * (ny - p2);
        int oy1 = oy * p;
        int oy2 = oy * p2;
        int ox1 = ox * p;
        int ox2 = ox * p2;
        uint16_t i00, i01, i10, i11;

        for (; py <= ey; py += oy2) {
            uint16_t *px = py;
            uint16_t *ex = py + ox * (nx - p2);

            for (; px <= ex; px += ox2) {
                uint16_t *p01 = px + ox1;
                uint16_t *p10 = px + oy1;
                uint16_t *p11 = p10 + ox1;

                if (use14) {
                    wenc14(*px, *p01, &i00, &i01);
                    wenc14(*p10, *p11, &i10, &i11);
                    wenc14(i00, i10, px, p10);
                    wenc14(i01, i11, p01, p11);
                } else {
                    wenc16(*px, *p01, &i00, &i01);
                    wenc16(*p10, *p11, &i10, &i11);
                    wenc16(i00, i10, px, p10);
                    wenc16(i01, i11, p01, p11);
                }
            }

            if (nx & p) {
                uint16_t *p10 = px + oy1;

                if (use14)
                    wenc14(*px, *p10, &i00, p10);
                else
                    wenc16(*px, *p10, &i00, p10);
                *px = i00;
            }
        }

        if (ny & p) {
            uint16_t *px = py;
            uint16_t *ex = py + ox * (nx - p2);

            for (; px <= ex; px += ox2) {
                uint16_t *p01 = px + ox1;

                if (use14)
                    wenc14(*px, *p01, &i00, p01);
                else
                    wenc16(*px, *p01, &i00, p01);
                *px = i00;
            }
        }

        p = p2;
        p2 <<= 1;
    }
}

int ff_exr_piz_encode(EXRPizWork *work,
                      uint8_t *dst, size_t dst_capacity,
                      const uint8_t *packed, size_t packed_size,
                      int width, int height,
                      int channels, int word_pairs)
{
    const size_t row_words = (size_t)width * word_pairs;
    const size_t channel_words = row_words * height;
    const size_t total_words = channel_words * channels;
    uint8_t *p = dst;
    uint8_t *end = dst + dst_capacity;
    uint16_t max_value;
    uint16_t min_nonzero, max_nonzero;
    int im, iM, n_bits, ret;
    size_t bitmap_length, table_size = 0, data_size = 0;
    size_t huf_size;
    uint8_t *bitmap_dst;
    uint8_t *length_pos;
    uint8_t *huf_start;
    uint8_t *table_start;
    uint8_t *data_start;

    if (!work || !dst || !packed || width <= 0 || height <= 0 ||
        channels <= 0 || (word_pairs != 1 && word_pairs != 2) ||
        packed_size != total_words * 2 ||
        dst_capacity < ff_exr_piz_max_block_size(packed_size))
        return AVERROR(EINVAL);

    if (work->words_capacity < total_words) {
        uint16_t *new_words = av_realloc_array(work->words, total_words,
                                               sizeof(*work->words));
        if (!new_words)
            return AVERROR(ENOMEM);
        work->words = new_words;
        work->words_capacity = total_words;
    }

    /* Convert packed little-endian words to host integer symbol values. */
    for (int c = 0; c < channels; c++) {
        for (int y = 0; y < height; y++) {
            size_t packed_offset =
                ((size_t)y * channels + c) * row_words * 2;
            uint16_t *dst_row =
                work->words + (size_t)c * channel_words + y * row_words;

            for (size_t x = 0; x < row_words; x++)
                dst_row[x] = AV_RL16(packed + packed_offset + x * 2);
        }
    }

    memset(work->bitmap, 0, sizeof(work->bitmap));
    for (size_t i = 0; i < total_words; i++) {
        uint16_t value = work->words[i];
        work->bitmap[value >> 3] |= (uint8_t)(1u << (value & 7));
    }

    /* Value zero is always implied by the format, not stored in bitmap[0]. */
    work->bitmap[0] &= (uint8_t)~1u;

    min_nonzero = EXR_PIZ_BITMAP_SIZE - 1;
    max_nonzero = 0;
    for (int i = 0; i < EXR_PIZ_BITMAP_SIZE; i++) {
        if (work->bitmap[i]) {
            if (i < min_nonzero)
                min_nonzero = (uint16_t)i;
            if (i > max_nonzero)
                max_nonzero = (uint16_t)i;
        }
    }

    /* Build the forward rank LUT and remap pixels into compact symbols. */
    int rank = 0;
    for (int value = 0; value < EXR_PIZ_USHORT_RANGE; value++) {
        if (value == 0 ||
            (work->bitmap[value >> 3] & (1u << (value & 7))))
            work->lut[value] = (uint16_t)rank++;
        else
            work->lut[value] = 0;
    }
    max_value = (uint16_t)(rank - 1);

    for (size_t i = 0; i < total_words; i++)
        work->words[i] = work->lut[work->words[i]];

    /* Each float channel has two interleaved word planes. */
    for (int c = 0; c < channels; c++) {
        uint16_t *channel = work->words + (size_t)c * channel_words;

        for (int plane = 0; plane < word_pairs; plane++) {
            wav2_encode(channel + plane,
                        width, word_pairs,
                        height, width * word_pairs,
                        max_value);
        }
    }

    ret = huf_build_table(work, (int)total_words, &im, &iM);
    if (ret < 0)
        return ret;

    /*
     * FFmpeg's PIZ decoder needs an unused literal slot to host its run
     * symbol in the two widest table ranges.  A completely dense table has
     * no such slot and is rejected before entropy decoding; tell the EXR
     * encoder to use the format's raw-block fallback instead.
     */
    if (im == 0 && iM >= 65535) {
        int has_unused_slot = 0;
        for (int symbol = 0; symbol < iM; symbol++) {
            if ((work->codes[symbol] & 63) == 0) {
                has_unused_slot = 1;
                break;
            }
        }
        if (!has_unused_slot)
            return 0;
    }

    if (!huf_vlc_compatible(work, im, iM))
        return 0;

    /* min/max nonzero bitmap byte indices. */
    if (end - p < 4)
        return AVERROR_BUFFER_TOO_SMALL;
    AV_WL16(p, min_nonzero);
    AV_WL16(p + 2, max_nonzero);
    p += 4;

    if (min_nonzero <= max_nonzero) {
        bitmap_length = (size_t)max_nonzero - min_nonzero + 1;
        if (end - p < (ptrdiff_t)bitmap_length)
            return AVERROR_BUFFER_TOO_SMALL;
        bitmap_dst = p;
        memcpy(bitmap_dst, work->bitmap + min_nonzero, bitmap_length);
        p += bitmap_length;
    } else {
        bitmap_length = 0;
    }

    /* Total Huffman stream length, filled once known. */
    if (end - p < 4 + 20)
        return AVERROR_BUFFER_TOO_SMALL;
    length_pos = p;
    AV_WL32(length_pos, 0);
    p += 4;

    huf_start = p;
    memset(huf_start, 0, 20);
    p += 20;

    table_start = p;
    ret = huf_pack_table(work, table_start, end, im, iM, &table_size);
    if (ret < 0)
        return ret;
    p += table_size;

    data_start = p;
    ret = huf_encode_data(work, (int)total_words, iM,
                          data_start, end, &n_bits, &data_size);
    if (ret < 0)
        return ret;
    p += data_size;

    huf_size = (size_t)(p - huf_start);
    AV_WL32(huf_start + 0, im);
    AV_WL32(huf_start + 4, iM);
    AV_WL32(huf_start + 8, (unsigned)table_size);
    AV_WL32(huf_start + 12, (unsigned)n_bits);
    AV_WL32(huf_start + 16, 0);
    AV_WL32(length_pos, (unsigned)huf_size);

    return (int)(p - dst);
}
