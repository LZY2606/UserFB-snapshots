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
 * OpenEXR PIZ compression, encoder side. See exrpiz.h for the pipeline
 * overview; every transform here is the inverse of the decoding code in
 * exr.c so the two share the on-disk format.
 */

#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "exrpiz.h"

#define USHORT_RANGE  (1 << 16)
#define HUF_ENCBITS   16
#define HUF_ENCSIZE   ((1 << HUF_ENCBITS) + 1)
#define HUF_MAX_LEN   32

#define NBITS      16
#define A_OFFSET   (1 << (NBITS - 1))

int ff_exr_piz_init(EXRPizEnc *s)
{
    s->bitmap  = av_malloc(FF_EXR_PIZ_BITMAP_SIZE);
    s->lut     = av_malloc_array(USHORT_RANGE, sizeof(*s->lut));
    s->map     = av_malloc_array(USHORT_RANGE, sizeof(*s->map));
    s->freq    = av_malloc_array(HUF_ENCSIZE, sizeof(*s->freq));
    s->code    = av_malloc_array(HUF_ENCSIZE, sizeof(*s->code));
    s->length  = av_malloc_array(HUF_ENCSIZE, sizeof(*s->length));
    if (!s->bitmap || !s->lut || !s->map || !s->freq || !s->code ||
        !s->length) {
        ff_exr_piz_uninit(s);
        return AVERROR(ENOMEM);
    }

    s->mapped     = NULL;
    s->mapped_cap = 0;

    return 0;
}

void ff_exr_piz_uninit(EXRPizEnc *s)
{
    av_freep(&s->bitmap);
    av_freep(&s->lut);
    av_freep(&s->map);
    av_freep(&s->mapped);
    av_freep(&s->freq);
    av_freep(&s->code);
    av_freep(&s->length);
    s->mapped_cap = 0;
}

/* Reverse of wdec14(): 14-bit signed arithmetic lifting.
 * decode: a = l + r(h), b = a - h with r(h) = (h & 1) + (h >> 1). */
static inline void wenc14(uint16_t a, uint16_t b, uint16_t *l, uint16_t *h)
{
    int16_t as = (int16_t)a;
    int16_t bs = (int16_t)b;
    int     hi = (int16_t)(as - bs);
    int     li = as - ((hi & 1) + (hi >> 1));

    *l = (uint16_t)(int16_t)li;
    *h = (uint16_t)(int16_t)hi;
}

/* Reverse of wdec16(): modular arithmetic on the full 16-bit range. */
static inline void wenc16(uint16_t a, uint16_t b, uint16_t *l, uint16_t *h)
{
    int aa = a;
    int bb = b;
    int d  = (aa - bb + A_OFFSET) & 0xFFFF;
    int m  = (bb + (d >> 1)) & 0xFFFF;

    *l = (uint16_t)m;
    *h = (uint16_t)d;
}

/* Inverse of wav_decode() in exr.c. The decoder walks the scales coarse to
 * fine, so the encoder walks them fine to coarse; the traversal geometry and
 * the four lifting pairs per block are otherwise identical. nx/ny may be odd
 * and ox/oy describe the strides of one 16-bit word plane. */
static void wav_encode(uint16_t *in, int nx, int ox, int ny, int oy,
                       int w14)
{
    int n = FFMIN(nx, ny);
    int p, p2, pmax;

    pmax = 1;
    while ((pmax << 1) <= n)
        pmax <<= 1;

    /* The decoder's first scale has p2 = pmax, so the encoded scales run
     * from p = 1 up to pmax / 2, fine to coarse. */
    for (p = 1; (p << 1) <= pmax; p <<= 1) {
        p2 = p << 1;
        uint16_t *py = in;
        uint16_t *ey = in + oy * (ny - p2);
        int oy1 = oy * p;
        int oy2 = oy * p2;
        int ox1 = ox * p;
        int ox2 = ox * p2;

        for (; py <= ey; py += oy2) {
            uint16_t *px = py;
            uint16_t *ex = py + ox * (nx - p2);

            for (; px <= ex; px += ox2) {
                uint16_t *p01 = px + ox1;
                uint16_t *p10 = px + oy1;
                uint16_t *p11 = p10 + ox1;

                if (w14) {
                    uint16_t i00, i01, i10, i11;
                    wenc14(*px, *p01, &i00, &i01);
                    wenc14(*p10, *p11, &i10, &i11);
                    wenc14(i00, i10, px, p10);
                    wenc14(i01, i11, p01, p11);
                } else {
                    uint16_t i00, i01, i10, i11;
                    wenc16(*px, *p01, &i00, &i01);
                    wenc16(*p10, *p11, &i10, &i11);
                    wenc16(i00, i10, px, p10);
                    wenc16(i01, i11, p01, p11);
                }
            }

            if (nx & p) {
                uint16_t *p10 = px + oy1;
                uint16_t i00;

                if (w14)
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
                uint16_t i00;

                if (w14)
                    wenc14(*px, *p01, &i00, p01);
                else
                    wenc16(*px, *p01, &i00, p01);
                *px = i00;
            }
        }
    }
}

/* Build the value usage bitmap, the sorted reverse LUT and the word to dense
 * index map. The bitmap is byte addressed, one byte covering eight 16-bit
 * words, so it spans the whole 65536 value range exactly like reverse_lut()
 * in the decoder; zero is always the first LUT entry. */
static int build_bitmap(EXRPizEnc *s, const uint16_t *src, int nb,
                        uint16_t *min_non_zero, uint16_t *max_non_zero,
                        uint16_t *maxval)
{
    int count = 0;
    int lo = -1, hi = -1;

    /* reuse the dense map as the temporary "value present" marker: zero means
     * absent while building, 0xFFFF marks a value that occurs in the block */
    memset(s->map, 0, USHORT_RANGE * sizeof(*s->map));
    s->map[0] = 0xFFFF; /* reverse_lut() in the decoder always lists zero first */
    for (int i = 0; i < nb; i++)
        s->map[src[i]] = 0xFFFF;

    memset(s->bitmap, 0, FF_EXR_PIZ_BITMAP_SIZE);
    for (int v = 0; v < USHORT_RANGE; v++) {
        if (s->map[v] != 0xFFFF)
            continue;
        if (v) {
            s->bitmap[v >> 3] |= (uint8_t)(1u << (v & 7));
            if (lo < 0)
                lo = v;
            hi = v;
        }
        s->lut[count] = (uint16_t)v;
        s->map[v]     = (uint16_t)count;
        count++;
    }

    if (lo < 0) {
        *min_non_zero = 0;
        *max_non_zero = 0;
    } else {
        *min_non_zero = (uint16_t)(lo >> 3);
        *max_non_zero = (uint16_t)(hi >> 3);
    }

    *maxval = (uint16_t)(count - 1);
    return count;
}

/* Min-heap of Huffman node weights. */
typedef struct HufNode {
    uint32_t weight;
    int      parent;
} HufNode;

static void heap_push(const HufNode *nodes, int *heap, int *heap_n, int id)
{
    int i = (*heap_n)++;

    while (i > 0) {
        int parent = (i - 1) >> 1;
        if (nodes[heap[parent]].weight <= nodes[id].weight)
            break;
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i] = id;
}

static int heap_pop(const HufNode *nodes, int *heap, int *heap_n)
{
    int root = heap[0];
    int last = heap[--(*heap_n)];
    int i = 0;

    while (*heap_n > 0) {
        int l = 2 * i + 1;
        int r = l + 1;
        int smallest = i;

        if (l < *heap_n && nodes[heap[l]].weight < nodes[heap[smallest]].weight)
            smallest = l;
        if (r < *heap_n && nodes[heap[r]].weight < nodes[heap[smallest]].weight)
            smallest = r;
        if (smallest == i)
            break;
        heap[i] = heap[smallest];
        i = smallest;
    }
    if (*heap_n > 0)
        heap[i] = last;

    return root;
}

/* Length limited canonical Huffman codes. Active coefficient symbols live in
 * freq[im..iM-1]; slot iM carries a zero weight leaf whose codeword the
 * decoder treats as its run symbol, so it must never be emitted. */
static int build_huffman(EXRPizEnc *s, int im, int iM)
{
    uint32_t *freq   = s->freq;
    uint32_t *code   = s->code;
    uint8_t  *length = s->length;
    int leaves = 0, total_nodes, heap_n = 0, nid;
    HufNode *nodes;
    int *heap, *leaf_node, *leaf_sym, *depth;
    /* per length symbol buckets for linear time Kraft rebalancing */
    int head[HUF_MAX_LEN + 1], next[HUF_ENCSIZE];
    uint64_t kraft, target;

    for (int i = im; i < iM; i++) {
        if (freq[i] > 0)
            leaves++;
    }
    if (leaves == 0)
        return AVERROR_INVALIDDATA;
    leaves++; /* the run symbol slot at iM */

    total_nodes = 2 * leaves - 1;
    nodes     = av_calloc(total_nodes, sizeof(*nodes));
    heap      = av_calloc(total_nodes, sizeof(*heap));
    leaf_node = av_calloc(leaves, sizeof(*leaf_node));
    leaf_sym  = av_calloc(leaves, sizeof(*leaf_sym));
    depth     = av_calloc(total_nodes, sizeof(*depth));
    if (!nodes || !heap || !leaf_node || !leaf_sym || !depth) {
        av_free(nodes);
        av_free(heap);
        av_free(leaf_node);
        av_free(leaf_sym);
        av_free(depth);
        return AVERROR(ENOMEM);
    }

    nid = 0;
    for (int i = im; i <= iM; i++) {
        if (i < iM && freq[i] == 0)
            continue; /* gaps inside the range carry no codeword */
        nodes[nid].weight = (i == iM) ? 0 : freq[i];
        nodes[nid].parent = -1;
        heap_push(nodes, heap, &heap_n, nid);
        leaf_node[nid] = nid;
        leaf_sym[nid]  = i;
        nid++;
    }
    av_assert0(nid == leaves);

    while (heap_n > 1) {
        int a = heap_pop(nodes, heap, &heap_n);
        int b = heap_pop(nodes, heap, &heap_n);

        nodes[nid].weight = nodes[a].weight + nodes[b].weight;
        nodes[nid].parent = -1;
        nodes[a].parent = nid;
        nodes[b].parent = nid;
        heap_push(nodes, heap, &heap_n, nid);
        nid++;
    }

    /* Node ids increase along every parent edge, so one backward pass gives
     * every depth in linear time even for pathologically skewed trees. */
    depth[total_nodes - 1] = 0;
    for (int i = total_nodes - 2; i >= 0; i--)
        depth[i] = depth[nodes[i].parent] + 1;

    memset(length, 0, HUF_ENCSIZE);
    memset(head, -1, sizeof(head));
    for (int i = 0; i < leaves; i++) {
        int sym = leaf_sym[i];
        int len = FFMIN(depth[leaf_node[i]], HUF_MAX_LEN);
        length[sym]  = (uint8_t)len;
        next[sym]    = head[len];
        head[len]    = sym;
    }

    /* Capping can over- or under-subscribe the code space. Rebalance with
     * integer Kraft sums at the HUF_MAX_LEN scale; bucket lists make every
     * pick O(1) so the whole repair stays linear in leaves * HUF_MAX_LEN. */
    target = 1ULL << HUF_MAX_LEN;
    kraft  = 0;
    for (int i = im; i <= iM; i++)
        if (length[i])
            kraft += 1ULL << (HUF_MAX_LEN - length[i]);

    while (kraft > target) {
        int pick = -1;
        for (int len = 1; len < HUF_MAX_LEN && pick < 0; len++)
            pick = head[len];
        if (pick < 0) {
            av_free(nodes);
            av_free(heap);
            av_free(leaf_node);
            av_free(leaf_sym);
            av_free(depth);
            return AVERROR(ENOSPC);
        }
        int old = length[pick];
        head[old] = next[pick];
        length[pick] = old + 1;
        next[pick] = head[old + 1];
        head[old + 1] = pick;
        kraft -= 1ULL << (HUF_MAX_LEN - old - 1);
    }
    while (kraft < target) {
        int pick = -1;
        for (int len = HUF_MAX_LEN; len > 1 && pick < 0; len--)
            pick = head[len];
        if (pick < 0)
            break;
        int old = length[pick];
        uint64_t add = 1ULL << (HUF_MAX_LEN - old);
        if (kraft + add > target)
            break;
        head[old] = next[pick];
        length[pick] = old - 1;
        next[pick] = head[old - 1];
        head[old - 1] = pick;
        kraft += add;
    }

    /* Canonical code assignment identical to huf_canonical_code_table() in
     * the decoder: starting codes are derived from the longest length down.
     * The decoder recomputes the codes from the six bit lengths alone, so the
     * values have to match this convention bit for bit. */
    {
        uint64_t cnt[59] = { 0 }, start[59] = { 0 }, c = 0;

        for (int i = im; i <= iM; i++)
            if (length[i])
                cnt[length[i]]++;
        for (int len = 58; len >= 1; len--) {
            uint64_t nc = (c + cnt[len]) >> 1;
            start[len] = c;
            c = nc;
        }
        for (int i = im; i <= iM; i++) {
            int len = length[i];
            if (len)
                code[i] = (uint32_t)start[len]++;
        }
    }

    av_free(nodes);
    av_free(heap);
    av_free(leaf_node);
    av_free(leaf_sym);
    av_free(depth);
    return 0;
}

/* Append bits MSB first, matching the decoder's big endian GetBitContext. */
static int put_bits(uint8_t *dst, int dst_bytes, int *bitpos,
                    uint32_t value, int nbits)
{
    for (int b = nbits - 1; b >= 0; b--) {
        int pos  = (*bitpos)++;
        int byte = pos >> 3;
        if (byte >= dst_bytes)
            return AVERROR(ENOSPC);
        if ((value >> b) & 1)
            dst[byte] |= (uint8_t)(1u << (7 - (pos & 7)));
    }
    return 0;
}

int ff_exr_piz_encode_block(EXRPizEnc *s, uint16_t *words, int nb,
                            int nb_channels, int pixel_half_size,
                            int xsize, int ysize,
                            uint8_t *dst, int dst_size)
{
    uint16_t min_nz, max_nz, maxval;
    int w14, ret;
    int pos = 0, im, iM, cmax;
    int table_bits, table_bytes;
    uint64_t nbits;
    uint16_t *mapped;

    if (nb <= 0 || nb_channels <= 0 || xsize <= 0 || ysize <= 0)
        return AVERROR(EINVAL);

    if (nb > s->mapped_cap) {
        uint16_t *nm = av_realloc_array(s->mapped, nb, sizeof(*nm));
        if (!nm)
            return AVERROR(ENOMEM);
        s->mapped     = nm;
        s->mapped_cap = nb;
    }
    mapped = s->mapped;

    /* 1. bitmap, reverse LUT and dense index map over the raw words */
    build_bitmap(s, words, nb, &min_nz, &max_nz, &maxval);

    /* 2. remap in place, then run the reversible wavelet on the dense
     * indices per channel and per 16-bit word plane. */
    for (int i = 0; i < nb; i++)
        words[i] = s->map[words[i]];

    w14 = maxval < (1 << 14);
    {
        int ptr = 0;
        for (int c = 0; c < nb_channels; c++) {
            for (int j = 0; j < pixel_half_size; j++)
                wav_encode(words + ptr + j, xsize, pixel_half_size,
                           ysize, xsize * pixel_half_size, w14);
            ptr += xsize * ysize * pixel_half_size;
        }
    }

    /* 3. coefficient frequencies and the active symbol range. The decoder's
     * huf_build_dec_table() always gives table slot iM a codeword, so the
     * emitted coefficients have to stay strictly below iM. We therefore set
     * iM to one past the largest coefficient and give that slot a zero
     * weight leaf. A w14 coefficient of -1 is the uint16 word 65535, which
     * cannot get a smaller iM; such a block falls back to raw storage. */
    im = cmax = words[0];
    memset(s->freq, 0, HUF_ENCSIZE * sizeof(*s->freq));
    for (int i = 0; i < nb; i++) {
        uint16_t coeff = words[i];
        mapped[i] = coeff;
        s->freq[coeff]++;
        if (coeff < im)
            im = coeff;
        if (coeff > cmax)
            cmax = coeff;
    }
    if (cmax == 65535)
        return AVERROR(ENOSPC);
    iM = cmax + 1;

    ret = build_huffman(s, im, iM);
    if (ret < 0)
        return ret;

    /* 4. serialise the block in the layout piz_uncompress() parses */
    memset(dst, 0, dst_size);
    AV_WL16(dst + pos, min_nz);
    AV_WL16(dst + pos + 2, max_nz);
    pos += 4;
    {
        int span = max_nz - min_nz + 1;
        if (pos + span > dst_size)
            return AVERROR(ENOSPC);
        memcpy(dst + pos, s->bitmap + min_nz, span);
        pos += span;
    }

    /* four bytes the decoder skips between bitmap and the Huffman header */
    if (pos + 4 > dst_size)
        return AVERROR(ENOSPC);
    AV_WL32(dst + pos, 0);
    pos += 4;

    table_bits  = 6 * (iM + 1);
    table_bytes = (table_bits + 7) >> 3;
    nbits = 0;
    for (int i = 0; i < nb; i++)
        nbits += s->length[mapped[i]];
    if (nbits > UINT32_MAX)
        return AVERROR(ENOSPC);

    if (pos + 20 + table_bytes > dst_size)
        return AVERROR(ENOSPC);
    AV_WL32(dst + pos,      0);                  /* im   */
    AV_WL32(dst + pos + 4,  iM);                 /* iM   */
    AV_WL32(dst + pos + 8,  table_bytes);        /* packed table size */
    AV_WL32(dst + pos + 12, (uint32_t)nbits);    /* coded bit count */
    AV_WL32(dst + pos + 16, table_bytes + 20);   /* skipped offset */
    pos += 20;

    /* packed six bit code lengths for slots 0..iM, MSB first; unused slots
     * carry zero and active lengths stay in 1..32, so the decoder's 59..63
     * zero run markers never occur */
    {
        int bitpos = 0;
        for (int i = 0; i <= iM; i++) {
            ret = put_bits(dst + pos, table_bytes, &bitpos,
                           s->length[i], 6);
            if (ret < 0)
                return ret;
        }
    }
    pos += table_bytes;

    {
        int stream_bytes = (nbits + 7) >> 3;
        int bitpos = 0;
        if (pos + stream_bytes > dst_size)
            return AVERROR(ENOSPC);
        for (int i = 0; i < nb; i++) {
            uint16_t sym = mapped[i];
            ret = put_bits(dst + pos, stream_bytes, &bitpos,
                           s->code[sym], s->length[sym]);
            if (ret < 0)
                return ret;
        }
        pos += stream_bytes;
    }

    if (pos >= nb * 2)
        return AVERROR(ENOSPC);

    return pos;
}
