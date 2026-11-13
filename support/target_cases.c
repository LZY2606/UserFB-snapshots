/*
 * Target cases for the EXR encoder PIZ compression work.
 *
 * The fixed runner in driver.c calls task_cases() once per group. Besides
 * exercising the public avcodec_send_frame/avcodec_receive_packet entry point
 * these cases parse the produced EXR packet on their own (header, offset
 * table, PIZ bitmap and Huffman header) and run an independent inverse
 * wavelet / Huffman implementation, so a correct result does not rely solely
 * on the library being able to open its own output.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/avassert.h"
#include "libavutil/float2half.h"
#include "libavutil/half2float.h"
#include "libavutil/imgutils.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#include "checks.h"

#define PIZ_SCAN_LINES 32

/* ------------------------------------------------------------------ */
/* small EXR packet parser, independent from libavcodec's decoder     */
/* ------------------------------------------------------------------ */

typedef struct ExrAttr {
    int      compression;
    int      pixel_type;
    int      nb_channels;
    int      width, height;
    int      header_end;
} ExrAttr;

static int read_cstr(const uint8_t *p, int size, int *pos, char *out, int outsz)
{
    int start = *pos, len = 0;

    while (*pos < size && p[*pos] != 0)
        (*pos)++;
    if (*pos >= size)
        return -1;
    len = *pos - start;
    if (len >= outsz)
        return -1;
    memcpy(out, p + start, len);
    out[len] = 0;
    (*pos)++;
    return 0;
}

static int parse_exr(const uint8_t *p, int size, ExrAttr *a)
{
    char name[64], type[64];
    int pos = 8; /* magic + version */

    memset(a, 0, sizeof(*a));
    a->compression = -1;

    if (AV_RL32(p) != 20000630)
        return -1;

    while (pos < size && p[pos] != 0) {
        int attr_size;

        if (read_cstr(p, size, &pos, name, sizeof(name)) < 0 ||
            read_cstr(p, size, &pos, type, sizeof(type)) < 0)
            return -1;
        if (pos + 4 > size)
            return -1;
        attr_size = AV_RL32(p + pos);
        pos += 4;
        if (pos + attr_size > size)
            return -1;

        if (!strcmp(name, "compression") && attr_size == 1) {
            a->compression = p[pos];
        } else if (!strcmp(name, "dataWindow") && attr_size == 16) {
            a->width  = AV_RL32(p + pos + 8) - AV_RL32(p + pos) + 1;
            a->height = AV_RL32(p + pos + 12) - AV_RL32(p + pos + 4) + 1;
        } else if (!strcmp(name, "channels")) {
            a->nb_channels = (attr_size - 1) / 18;
            if (attr_size >= 18)
                a->pixel_type = AV_RL32(p + pos + 2);
        }
        pos += attr_size;
    }
    if (pos >= size)
        return -1;
    pos++; /* attribute list terminator */
    a->header_end = pos;
    return 0;
}

typedef struct ExrBlock {
    uint64_t offset;
    int      y;
    int      data_size;
    int      lines;
} ExrBlock;

static int parse_blocks(const uint8_t *p, int size, const ExrAttr *a,
                        ExrBlock *blocks, int max_blocks)
{
    int nb = (a->height + PIZ_SCAN_LINES - 1) / PIZ_SCAN_LINES;
    int pos = a->header_end;

    if (nb > max_blocks || pos + nb * 8 > size)
        return -1;
    for (int i = 0; i < nb; i++)
        blocks[i].offset = AV_RL64(p + pos + i * 8);

    for (int i = 0; i < nb; i++) {
        uint64_t off = blocks[i].offset;

        if (off + 8 > (uint64_t)size)
            return -1;
        blocks[i].y = AV_RL32(p + off);
        blocks[i].data_size = AV_RL32(p + off + 4);
        if (blocks[i].y != i * PIZ_SCAN_LINES)
            return -1;
        if (off + 8 + blocks[i].data_size > (uint64_t)size)
            return -1;
        if (i + 1 < nb &&
            off + 8 + blocks[i].data_size != blocks[i + 1].offset)
            return -1;
        if (i + 1 == nb &&
            off + 8 + blocks[i].data_size != (uint64_t)size)
            return -1;
        blocks[i].lines = FFMIN(PIZ_SCAN_LINES,
                                a->height - blocks[i].y);
    }
    return nb;
}

/* ------------------------------------------------------------------ */
/* independent PIZ inverse: canonical Huffman, wavelet, reverse LUT   */
/* ------------------------------------------------------------------ */

static inline void t_wdec14(uint16_t l, uint16_t h, uint16_t *a, uint16_t *b)
{
    int16_t ls = (int16_t)l;
    int16_t hs = (int16_t)h;
    int hi = hs;
    int ai = ls + (hi & 1) + (hi >> 1);

    *a = (uint16_t)(int16_t)ai;
    *b = (uint16_t)(int16_t)(ai - hi);
}

static inline void t_wdec16(uint16_t l, uint16_t h, uint16_t *a, uint16_t *b)
{
    int m = l, d = h;
    int bb = (m - (d >> 1)) & 0xFFFF;
    int aa = (d + bb - (1 << 15)) & 0xFFFF;

    *b = (uint16_t)bb;
    *a = (uint16_t)aa;
}

static void t_wav_decode(uint16_t *in, int nx, int ox, int ny, int oy,
                         int w14)
{
    int n = FFMIN(nx, ny), p = 1, p2;

    while (p <= n)
        p <<= 1;
    p >>= 1;
    p2 = p;
    p >>= 1;

    while (p >= 1) {
        uint16_t *py = in;
        uint16_t *ey = in + oy * (ny - p2);
        int oy1 = oy * p, oy2 = oy * p2, ox1 = ox * p, ox2 = ox * p2;

        for (; py <= ey; py += oy2) {
            uint16_t *px = py;
            uint16_t *ex = py + ox * (nx - p2);

            for (; px <= ex; px += ox2) {
                uint16_t *p01 = px + ox1, *p10 = px + oy1, *p11 = p10 + ox1;
                if (w14) {
                    uint16_t i00, i01, i10, i11;
                    t_wdec14(*px, *p10, &i00, &i10);
                    t_wdec14(*p01, *p11, &i01, &i11);
                    t_wdec14(i00, i01, px, p01);
                    t_wdec14(i10, i11, p10, p11);
                } else {
                    uint16_t i00, i01, i10, i11;
                    t_wdec16(*px, *p10, &i00, &i10);
                    t_wdec16(*p01, *p11, &i01, &i11);
                    t_wdec16(i00, i01, px, p01);
                    t_wdec16(i10, i11, p10, p11);
                }
            }
            if (nx & p) {
                uint16_t *p10 = px + oy1, i00;
                if (w14)
                    t_wdec14(*px, *p10, &i00, p10);
                else
                    t_wdec16(*px, *p10, &i00, p10);
                *px = i00;
            }
        }
        if (ny & p) {
            uint16_t *px = py;
            uint16_t *ex = py + ox * (nx - p2);

            for (; px <= ex; px += ox2) {
                uint16_t *p01 = px + ox1, i00;
                if (w14)
                    t_wdec14(*px, *p01, &i00, p01);
                else
                    t_wdec16(*px, *p01, &i00, p01);
                *px = i00;
            }
        }
        p2 = p;
        p >>= 1;
    }
}

typedef struct PizInfo {
    int im, iM, nbits;
    int table_bytes;
    int min_nz, max_nz;
} PizInfo;

static int piz_parse_header(const uint8_t *b, int bsize, PizInfo *pi)
{
    int pos = 0;

    if (bsize < 8)
        return -1;
    pi->min_nz = AV_RL16(b);
    pi->max_nz = AV_RL16(b + 2);
    if (pi->max_nz >= 1 << 13 || pi->min_nz > pi->max_nz)
        return -1;
    pos = 4 + (pi->max_nz - pi->min_nz + 1) + 4;
    if (pos + 20 > bsize)
        return -1;
    pi->im = AV_RL32(b + pos);
    pi->iM = AV_RL32(b + pos + 4);
    pi->table_bytes = AV_RL32(b + pos + 8);
    pi->nbits = AV_RL32(b + pos + 12);
    if (pi->im < 0 || pi->iM < pi->im || pi->iM >= 65535)
        return -1;
    if (pos + 20 + pi->table_bytes > bsize)
        return -1;
    if (pi->nbits > (bsize - pos - 20 - pi->table_bytes) * 8)
        return -1;
    return pos + 20;
}

static int get_bits_msb(const uint8_t *p, int *bitpos, int n)
{
    int v = 0;

    for (int i = 0; i < n; i++) {
        int bp = (*bitpos)++;
        v = (v << 1) | ((p[bp >> 3] >> (7 - (bp & 7))) & 1);
    }
    return v;
}

/* Fully decode one PIZ block and compare every 16-bit word with expected. */
static void check_piz_block(const uint8_t *pkt, const ExrBlock *blk,
                            int width,
                            const uint16_t *expect, int nb_words,
                            int nb_channels, int phs)
{
    const uint8_t *b = pkt + blk->offset + 8;
    int bsize = blk->data_size;
    PizInfo pi;
    int hdr = piz_parse_header(b, bsize, &pi);
    uint8_t length[65536];
    uint32_t code[65536];
    uint16_t lut[65536], *out;
    int table_pos, stream_start, bitpos = 0, produced = 0;
    int maxval, w14;

    task_check(hdr >= 0, "piz header parses");
    task_check(pi.im == 0, "huffman range starts at zero");
    task_check(pi.nbits > 0 && pi.nbits % 1 == 0, "valid coded bit count");

    /* unpack the six bit code length table */
    table_pos = hdr;
    memset(length, 0, sizeof(length));
    for (int i = pi.im; i <= pi.iM; i++) {
        if (bitpos + 6 > pi.table_bytes * 8) {
            task_check(0, "length table overruns");
            return;
        }
        length[i] = (uint8_t)get_bits_msb(b + table_pos, &bitpos, 6);
        if (i < pi.iM)
            task_check(length[i] <= 32, "code length in range");
    }
    task_check((bitpos + 7) / 8 == pi.table_bytes, "table byte size exact");
    /* the run symbol slot itself always carries a real codeword */
    task_check(length[pi.iM] >= 1 && length[pi.iM] <= 32,
               "run symbol slot has a codeword");

    /* canonical code assignment, OpenEXR convention (longest length first),
     * matching huf_canonical_code_table() in the library decoder */
    {
        uint64_t cnt[59] = { 0 }, start[59] = { 0 }, cc = 0;
        for (int i = pi.im; i <= pi.iM; i++)
            if (length[i])
                cnt[length[i]]++;
        for (int len = 58; len >= 1; len--) {
            uint64_t nc = (cc + cnt[len]) >> 1;
            start[len] = cc;
            cc = nc;
        }
        for (int i = pi.im; i <= pi.iM; i++)
            if (length[i])
                code[i] = (uint32_t)start[length[i]]++;
    }
    /* Kraft equality for the complete prefix set */
    {
        uint64_t kraft = 0;
        for (int i = pi.im; i <= pi.iM; i++)
            if (length[i])
                kraft += 1ULL << (32 - length[i]);
        task_check(kraft == (1ULL << 32), "kraft balanced");
    }

    /* rebuild the reverse LUT from the bitmap, zero implicit. Only bytes in
     * [min_nz, max_nz] are present in the stream; every other word is zero. */
    maxval = -1;
    lut[++maxval] = 0;
    for (int byte = pi.min_nz; byte <= pi.max_nz; byte++) {
        uint8_t bits = b[4 + byte - pi.min_nz];
        for (int sub = 0; sub < 8; sub++) {
            int v = byte * 8 + sub;
            if (v && (bits & (1 << sub)))
                lut[++maxval] = (uint16_t)v;
        }
    }
    task_check(maxval >= 0 && maxval < 65536, "reverse lut covers the words");
    w14 = maxval < (1 << 14);

    out = av_malloc_array(nb_words, sizeof(*out));
    task_check(out != NULL, "decode alloc");
    stream_start = table_pos + pi.table_bytes;
    bitpos = 0;
    while (bitpos < pi.nbits && produced < nb_words) {
        uint32_t cur = 0;
        int sym = -1;

        for (int len = 1; len <= 32; len++) {
            if (bitpos >= pi.nbits)
                break;
            cur = (cur << 1) | get_bits_msb(b + stream_start, &bitpos, 1);
            for (int i = pi.im; i < pi.iM; i++) {
                if (length[i] == len && code[i] == cur) {
                    sym = i;
                    break;
                }
            }
            if (sym >= 0)
                break;
        }
        task_check(sym >= 0, "huffman symbol decodes");
        task_check(sym < pi.iM, "run symbol codeword never emitted");
        out[produced++] = (uint16_t)sym;
    }
    task_check(produced == nb_words, "exact coefficient word count");
    task_check(bitpos == pi.nbits, "stream consumes exactly nBits");
    for (int i = 0; i < nb_words; i++)
        task_check(out[i] < pi.iM, "huffman coefficients below run slot");

    /* inverse wavelet per channel and per word plane */
    {
        int ptr = 0;
        for (int c = 0; c < nb_channels; c++) {
            for (int j = 0; j < phs; j++)
                t_wav_decode(out + ptr + j, width, phs,
                             blk->lines, width * phs, w14);
            ptr += width * blk->lines * phs;
        }
    }

    /* reverse LUT and word for word comparison with the source pixels */
    for (int i = 0; i < nb_words; i++) {
        task_check(out[i] <= maxval, "coefficient within lut");
        out[i] = lut[out[i]];
    }
    for (int i = 0; i < nb_words; i++)
        task_check(out[i] == expect[i], "independent piz word matches source");

    av_free(out);
}

/* ------------------------------------------------------------------ */
/* frame helpers                                                       */
/* ------------------------------------------------------------------ */

static const uint8_t gbr_order[3] = { 1, 0, 2 };
static const uint8_t gbra_order[4] = { 3, 1, 0, 2 };
static const uint8_t y_order[1] = { 0 };

static AVFrame *make_frame(int w, int h, enum AVPixelFormat fmt)
{
    AVFrame *f = av_frame_alloc();

    task_check(f != NULL, "frame alloc");
    f->width = w;
    f->height = h;
    f->format = fmt;
    task_check(av_frame_get_buffer(f, 32) == 0, "frame buffer");
    return f;
}

static int plane_count(enum AVPixelFormat fmt)
{
    return fmt == AV_PIX_FMT_GRAYF32 ? 1 : fmt == AV_PIX_FMT_GBRPF32 ? 3 : 4;
}

static const uint8_t *channel_order(enum AVPixelFormat fmt, int *planes)
{
    *planes = plane_count(fmt);
    if (fmt == AV_PIX_FMT_GBRAPF32)
        return gbra_order;
    if (fmt == AV_PIX_FMT_GBRPF32)
        return gbr_order;
    return y_order;
}

/* Expected raw 16-bit words of one PIZ block in channel major layout: each
 * channel owns phs interleaved word planes of width * blk_lines words, LE. */
static uint16_t *expected_words(const AVFrame *f, int blk_y, int blk_lines,
                                int phs, const uint8_t *order, int planes,
                                Float2HalfTables *f2h)
{
    int w = f->width;
    int nb = w * blk_lines * planes * phs;
    uint16_t *raw = av_malloc_array(nb, sizeof(*raw));

    task_check(raw != NULL, "raw alloc");
    for (int p = 0; p < planes; p++) {
        int ch = order[p];
        uint16_t *cdst = raw + (int64_t)p * w * blk_lines * phs;
        for (int l = 0; l < blk_lines; l++) {
            const uint8_t *src = f->data[ch] +
                (blk_y + l) * f->linesize[ch];
            uint16_t *line = cdst + (int64_t)l * w * phs;
            for (int x = 0; x < w; x++) {
                if (phs == 2) {
                    uint32_t u = AV_RL32(src + x * 4);
                    line[2 * x]     = (uint16_t)u;
                    line[2 * x + 1] = (uint16_t)(u >> 16);
                } else {
                    line[x] = float2half(AV_RL32(src + x * 4), f2h);
                }
            }
        }
    }
    return raw;
}

static AVCodecContext *open_encoder(int w, int h, enum AVPixelFormat fmt,
                                    const char *compr, const char *pxf)
{
    AVCodecContext *e = avcodec_alloc_context3(
        avcodec_find_encoder(AV_CODEC_ID_EXR));

    task_check(e != NULL, "encoder alloc");
    e->width = w;
    e->height = h;
    e->pix_fmt = fmt;
    e->time_base = (AVRational){ 1, 25 };
    e->thread_count = 1;
    task_check(av_opt_set(e->priv_data, "compression", compr, 0) == 0,
               "set compression option");
    task_check(av_opt_set(e->priv_data, "format", pxf, 0) == 0,
               "set pixel format option");
    task_check(avcodec_open2(e, e->codec, NULL) == 0, "open encoder");
    return e;
}

static AVCodecContext *open_decoder(void)
{
    AVCodecContext *d = avcodec_alloc_context3(
        avcodec_find_decoder(AV_CODEC_ID_EXR));

    task_check(d != NULL, "decoder alloc");
    d->thread_count = 1;
    task_check(avcodec_open2(d, d->codec, NULL) == 0, "open decoder");
    return d;
}

static void encode_frame_pkt(AVCodecContext *e, AVFrame *f, AVPacket *pkt)
{
    task_check(avcodec_send_frame(e, f) == 0, "send frame");
    task_check(avcodec_receive_packet(e, pkt) == 0, "receive packet");
}

static void public_roundtrip(int w, int h, enum AVPixelFormat fmt,
                             const char *pxf, int phs, unsigned seed,
                             int signed_zeros)
{
    AVCodecContext *e = open_encoder(w, h, fmt, "piz", pxf);
    AVCodecContext *d = open_decoder();
    AVFrame *in = make_frame(w, h, fmt), *out = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int planes = plane_count(fmt);
    uint32_t state = seed ? seed : 0x9e3779b9u;

    task_check(out && pkt, "alloc out frame/packet");
    for (int c = 0; c < planes; c++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t u;
                state = state * 1664525u + 1013904223u;
                if (phs == 2) {
                    /* finite floats: random mantissa with a mild exponent */
                    u = 0x3f000000u | (state & 0x007fffffu);
                } else {
                    float v = (float)((state >> 8) % 1024) / 512.f - 1.f;
                    u = av_float2int(v);
                }
                AV_WL32(in->data[c] + y * in->linesize[c] + x * 4, u);
            }
        }
    }
    if (signed_zeros) {
        AV_WL32(in->data[0], 0x00000000);
        AV_WL32(in->data[0] + 4, 0x80000000);
        if (w * h > 2)
            AV_WL32(in->data[0] + 8, 0x80000000);
    }

    encode_frame_pkt(e, in, pkt);
    task_check(avcodec_send_packet(d, pkt) == 0, "send packet");
    task_check(avcodec_receive_frame(d, out) == 0, "receive frame");
    task_check(out->width == w && out->height == h, "decoded geometry");

    if (phs == 2) {
        /* finite floats and signed zeros compare on raw 32-bit words */
        for (int c = 0; c < planes; c++)
            for (int y = 0; y < h; y++)
                task_check(!memcmp(in->data[c] + y * in->linesize[c],
                                   out->data[c] + y * out->linesize[c],
                                   w * 4),
                           "float32 exact word roundtrip");
    } else {
        Float2HalfTables f2h;
        ff_init_float2half_tables(&f2h);
        for (int c = 0; c < planes; c++)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint16_t got = AV_RL16(out->data[c] +
                                           y * out->linesize[c] + x * 2);
                    uint16_t want = float2half(
                        AV_RL32(in->data[c] + y * in->linesize[c] + x * 4),
                        &f2h);
                    task_check(got == want, "half matches float2half rule");
                }
    }

    av_packet_free(&pkt);
    av_frame_free(&in);
    av_frame_free(&out);
    avcodec_free_context(&e);
    avcodec_free_context(&d);
}

/* Fill a frame with a flat, highly compressible pattern. */
static void fill_flat(AVFrame *f, int planes)
{
    for (int c = 0; c < planes; c++)
        for (int y = 0; y < f->height; y++)
            for (int x = 0; x < f->width; x++) {
                float v = (float)((c + 1) * 0.25) +
                          (float)(x == y) * 0.0009765625f;
                AV_WL32(f->data[c] + y * f->linesize[c] + x * 4,
                        av_float2int(v));
            }
}

/* ------------------------------------------------------------------ */
/* groups                                                              */
/* ------------------------------------------------------------------ */

void task_cases(const char *group)
{
    if (!strcmp(group, "normal")) {
        Float2HalfTables f2h;
        ExrAttr a;
        ExrBlock blocks[64];
        int w = 24, h = 40, planes, nb_blocks;
        const uint8_t *order;
        enum AVPixelFormat fmt = AV_PIX_FMT_GBRPF32;
        AVCodecContext *e, *d;
        AVFrame *f, *out;
        AVPacket *pkt;
        int phs, compressed_blocks = 0;

        ff_init_float2half_tables(&f2h);

        /* real PIZ blocks on a flat float image */
        e = open_encoder(w, h, fmt, "piz", "float");
        d = open_decoder();
        f = make_frame(w, h, fmt);
        out = av_frame_alloc();
        pkt = av_packet_alloc();
        task_check(out && pkt, "alloc");
        order = channel_order(fmt, &planes);
        fill_flat(f, planes);
        encode_frame_pkt(e, f, pkt);

        task_check(parse_exr(pkt->data, pkt->size, &a) == 0, "parse exr");
        task_check(a.compression == 4, "compression byte is piz");
        task_check(a.width == w && a.height == h, "header geometry");
        task_check(a.pixel_type == 2, "pixel type float");
        nb_blocks = parse_blocks(pkt->data, pkt->size, &a, blocks, 64);
        task_check(nb_blocks == 2, "32 line block count");
        task_check(blocks[0].lines == 32 && blocks[1].lines == 8,
                   "last block keeps remaining lines");

        for (int i = 0; i < nb_blocks; i++) {
            int raw_size = w * blocks[i].lines * planes * 4;
            uint16_t *exp;

            task_check(blocks[i].data_size > 0, "block has payload");
            task_check(blocks[i].data_size < raw_size,
                       "flat pattern yields a real compressed block");
            compressed_blocks++;
            phs = 2;
            exp = expected_words(f, blocks[i].y, blocks[i].lines, phs,
                                 order, planes, &f2h);
            check_piz_block(pkt->data, &blocks[i], w, exp,
                            w * blocks[i].lines * planes * phs,
                            planes, phs);
            av_free(exp);
        }
        task_check(compressed_blocks == nb_blocks, "all blocks compressed");

        task_check(avcodec_send_packet(d, pkt) == 0, "decode send");
        task_check(avcodec_receive_frame(d, out) == 0, "decode receive");
        for (int c = 0; c < planes; c++)
            for (int y = 0; y < h; y++)
                task_check(!memcmp(f->data[c] + y * f->linesize[c],
                                   out->data[c] + y * out->linesize[c],
                                   w * 4), "public float roundtrip");
        av_packet_free(&pkt);
        av_frame_free(&f);
        av_frame_free(&out);
        avcodec_free_context(&e);
        avcodec_free_context(&d);

        /* public per-channel roundtrips across every supported input */
        public_roundtrip(17, 33, AV_PIX_FMT_GRAYF32,  "float", 2, 11, 1);
        public_roundtrip(19, 31, AV_PIX_FMT_GBRPF32,  "float", 2, 22, 1);
        public_roundtrip(23, 29, AV_PIX_FMT_GBRAPF32, "float", 2, 33, 1);
        public_roundtrip(21, 17, AV_PIX_FMT_GRAYF32,  "half",  1, 44, 1);
        public_roundtrip(15, 27, AV_PIX_FMT_GBRPF32,  "half",  1, 55, 0);
        public_roundtrip(13, 11, AV_PIX_FMT_GBRAPF32, "half",  1, 66, 0);

        /* bitmap byte boundary: word values straddling every 8-value edge */
        {
            AVCodecContext *e2 = open_encoder(64, 32, AV_PIX_FMT_GRAYF32,
                                              "piz", "float");
            AVFrame *f2 = make_frame(64, 32, AV_PIX_FMT_GRAYF32);
            AVPacket *p2 = av_packet_alloc();
            ExrAttr a2;
            ExrBlock b2[4];

            task_check(p2 != NULL, "alloc p2");
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 64; x++) {
                    int k = (y * 64 + x) % 2048;
                    /* low word walks values around byte boundaries */
                    uint32_t lo = (uint32_t)(k * 7) & 0xFFFFu;
                    uint32_t hi = (uint32_t)((k * 13) >> 1) & 0xFFFFu;
                    AV_WL32(f2->data[0] + y * f2->linesize[0] + x * 4,
                            (hi << 16) | lo);
                }
            encode_frame_pkt(e2, f2, p2);
            task_check(parse_exr(p2->data, p2->size, &a2) == 0, "parse 2");
            task_check(parse_blocks(p2->data, p2->size, &a2, b2, 4) == 1,
                       "single block");
            {
                const uint8_t *b = p2->data + b2[0].offset + 8;
                int min_nz = AV_RL16(b), max_nz = AV_RL16(b + 2);
                int saw_low = 0, saw_high = 0;

                task_check(min_nz == 0, "bitmap starts at byte zero");
                task_check(max_nz < 8192, "bitmap byte range valid");
                for (int v = 0; v < 65536; v++) {
                    int bit = b[4 + (v >> 3) - min_nz] & (1 << (v & 7));
                    if (bit && (v & 7) == 0)
                        saw_low++;
                    if (bit && (v & 7) == 7)
                        saw_high++;
                }
                task_check(saw_low > 0 && saw_high > 0,
                           "bitmap marks both sides of byte boundaries");
            }
            av_packet_free(&p2);
            av_frame_free(&f2);
            avcodec_free_context(&e2);
        }

        /* Huffman extremes: a single repeated value and a two value image */
        {
            AVFrame *f3 = make_frame(16, 32, AV_PIX_FMT_GRAYF32);
            AVCodecContext *e3 = open_encoder(16, 32, AV_PIX_FMT_GRAYF32,
                                              "piz", "half");
            AVPacket *p3 = av_packet_alloc();
            ExrAttr a3;
            ExrBlock b3[4];

            task_check(p3 != NULL, "alloc p3");
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 16; x++)
                    AV_WL32(f3->data[0] + y * f3->linesize[0] + x * 4,
                            av_float2int(0.5f));
            encode_frame_pkt(e3, f3, p3);
            task_check(parse_exr(p3->data, p3->size, &a3) == 0, "parse 3");
            task_check(parse_blocks(p3->data, p3->size, &a3, b3, 4) == 1,
                       "one block");
            task_check(b3[0].data_size < 16 * 32 * 2,
                       "constant image compresses");
            {
                const uint8_t *b = p3->data + b3[0].offset + 8;
                PizInfo pi;
                int phr = piz_parse_header(b, b3[0].data_size, &pi);
                task_check(phr > 0,
                           "huf header");
                task_check(pi.iM <= 2, "tiny symbol table");
                /* a constant image concentrates into one DC coefficient and
                 * zeros, so the coded stream stays around one bit per word */
                task_check(pi.nbits > 0 && pi.nbits <= 16 * 32 * 2,
                           "constant image coded at about one bit per word");
            }
            av_packet_free(&p3);
            av_frame_free(&f3);
            avcodec_free_context(&e3);
        }
    } else if (!strcmp(group, "boundary")) {
        /* 32 / 33 / 65 scan lines with odd widths */
        static const int heights[] = { 32, 33, 65 };
        for (unsigned k = 0; k < sizeof(heights) / sizeof(heights[0]); k++) {
            int h = heights[k], w = 13;
            AVCodecContext *e = open_encoder(w, h, AV_PIX_FMT_GBRPF32,
                                             "piz", "float");
            AVCodecContext *d = open_decoder();
            AVFrame *f = make_frame(w, h, AV_PIX_FMT_GBRPF32);
            AVFrame *o = av_frame_alloc();
            AVPacket *p = av_packet_alloc();
            ExrAttr a;
            ExrBlock blk[8];
            int nb;

            task_check(o && p, "alloc");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        AV_WL32(f->data[c] + y * f->linesize[c] + x * 4,
                                av_float2int((float)((x + y + c) % 11) /
                                             11.f));
            encode_frame_pkt(e, f, p);
            task_check(parse_exr(p->data, p->size, &a) == 0, "parse b");
            nb = parse_blocks(p->data, p->size, &a, blk, 8);
            task_check(nb == (h + 31) / 32, "block count by height");
            task_check(blk[nb - 1].lines == h - (nb - 1) * 32,
                       "final block line count");
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "boundary decode");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < h; y++)
                    task_check(!memcmp(f->data[c] + y * f->linesize[c],
                                       o->data[c] + y * o->linesize[c],
                                       w * 4), "boundary float exact");
            av_packet_free(&p);
            av_frame_free(&f);
            av_frame_free(&o);
            avcodec_free_context(&e);
            avcodec_free_context(&d);
        }

        /* odd shapes, single row / column and 1x1, half and float */
        {
            static const struct { int w, h; const char *px; int phs; }
                shapes[] = {
                { 1, 1, "float", 2 }, { 1, 1, "half", 1 },
                { 1, 33, "float", 2 }, { 33, 1, "half", 1 },
                { 7, 5, "float", 2 }, { 5, 7, "half", 1 },
                { 2, 64, "float", 2 },
            };
            for (unsigned k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
                int w = shapes[k].w, h = shapes[k].h;
                public_roundtrip(w, h, AV_PIX_FMT_GRAYF32, shapes[k].px,
                                 shapes[k].phs, 1000u + k, 1);
            }
        }

        /* all zero image */
        {
            int w = 31, h = 33;
            AVCodecContext *e = open_encoder(w, h, AV_PIX_FMT_GBRAPF32,
                                             "piz", "float");
            AVCodecContext *d = open_decoder();
            AVFrame *f = make_frame(w, h, AV_PIX_FMT_GBRAPF32);
            AVFrame *o = av_frame_alloc();
            AVPacket *p = av_packet_alloc();
            ExrAttr a;
            ExrBlock blk[8];

            task_check(o && p, "alloc zero");
            /* build an explicitly all zero frame */
            for (int c = 0; c < 4; c++)
                memset(f->data[c], 0, (size_t)f->linesize[c] * h);
            encode_frame_pkt(e, f, p);
            task_check(parse_exr(p->data, p->size, &a) == 0, "parse zero");
            task_check(parse_blocks(p->data, p->size, &a, blk, 8) == 2,
                       "zero blocks");
            task_check(blk[0].data_size < w * 32 * 4 * 4 &&
                       blk[1].data_size < w * 1 * 4 * 4,
                       "zero image strongly compressed");
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "zero decode");
            for (int c = 0; c < 4; c++)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        task_check(AV_RL32(o->data[c] +
                                           y * o->linesize[c] + x * 4) == 0,
                                   "zero preserved");
            av_packet_free(&p);
            av_frame_free(&f);
            av_frame_free(&o);
            avcodec_free_context(&e);
            avcodec_free_context(&d);
        }

        /* dense word values force the raw block fallback, half and float */
        {
            Half2FloatTables h2f;
            ff_init_half2float_tables(&h2f);

            int w = 256, h = 32;
            AVCodecContext *e = open_encoder(w, h, AV_PIX_FMT_GRAYF32,
                                             "piz", "half");
            AVCodecContext *d = open_decoder();
            AVFrame *f = make_frame(w, h, AV_PIX_FMT_GRAYF32);
            AVFrame *o = av_frame_alloc();
            AVPacket *p = av_packet_alloc();
            ExrAttr a;
            ExrBlock blk[4];
            int raw_size = w * h * 2, saw_fallback = 0;

            task_check(o && p, "alloc dense");
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    /* every normal half word 0x3c00..0x5bff appears once */
                    uint16_t hw = (uint16_t)(0x3c00 + y * w + x);
                    uint32_t fb = half2float(hw, &h2f);
                    AV_WL32(f->data[0] + y * f->linesize[0] + x * 4, fb);
                }
            encode_frame_pkt(e, f, p);
            task_check(parse_exr(p->data, p->size, &a) == 0, "parse dense");
            task_check(parse_blocks(p->data, p->size, &a, blk, 4) == 1,
                       "dense blocks");
            if (blk[0].data_size >= raw_size) {
                const uint8_t *payload = p->data + blk[0].offset + 8;
                Float2HalfTables f2h;
                ff_init_float2half_tables(&f2h);
                saw_fallback = 1;
                task_check(blk[0].data_size == raw_size,
                           "fallback stored at exact raw size");
                /* payload must be raw pixels, not a PIZ entropy stream:
                 * compare directly against the float2half source words */
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        uint16_t got = AV_RL16(payload + (y * w + x) * 2);
                        uint16_t want = float2half(
                            AV_RL32(f->data[0] +
                                    y * f->linesize[0] + x * 4), &f2h);
                        task_check(got == want, "fallback block is raw data");
                    }
            }
            task_check(saw_fallback, "dense words trigger raw fallback");
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "fallback decode");
            task_check(o->width == w && o->height == h, "fallback geometry");
            av_packet_free(&p);
            av_frame_free(&f);
            av_frame_free(&o);
            avcodec_free_context(&e);
            avcodec_free_context(&d);
        }
        {
            /* float dense: both 16-bit planes carry wide random values */
            int w = 96, h = 40;
            AVCodecContext *e = open_encoder(w, h, AV_PIX_FMT_GBRPF32,
                                             "piz", "float");
            AVCodecContext *d = open_decoder();
            AVFrame *f = make_frame(w, h, AV_PIX_FMT_GBRPF32);
            AVFrame *o = av_frame_alloc();
            AVPacket *p = av_packet_alloc();
            ExrAttr a;
            ExrBlock blk[8];
            uint32_t st = 0xdeadbeefu;
            int fallbacks = 0;

            task_check(o && p, "alloc dense float");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        st = st * 1103515245u + 12345u;
                        AV_WL32(f->data[c] + y * f->linesize[c] + x * 4,
                                0x3f800000u | (st & 0x007fffffu));
                    }
            encode_frame_pkt(e, f, p);
            task_check(parse_exr(p->data, p->size, &a) == 0, "parse df");
            task_check(parse_blocks(p->data, p->size, &a, blk, 8) == 2,
                       "dense float blocks");
            for (int i = 0; i < 2; i++) {
                int raw = w * blk[i].lines * 3 * 4;
                task_check(blk[i].data_size <= raw,
                           "block never larger than raw payload");
                if (blk[i].data_size == raw)
                    fallbacks++;
            }
            task_check(fallbacks >= 1, "float raw fallback present");
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "df decode");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < h; y++)
                    task_check(!memcmp(f->data[c] + y * f->linesize[c],
                                       o->data[c] + y * o->linesize[c],
                                       w * 4), "dense float exact");
            av_packet_free(&p);
            av_frame_free(&f);
            av_frame_free(&o);
            avcodec_free_context(&e);
            avcodec_free_context(&d);
        }
    } else if (!strcmp(group, "compat")) {
        static const char *const comprs[] = {
            "none", "rle", "zip1", "zip16"
        };
        for (unsigned k = 0; k < sizeof(comprs) / sizeof(comprs[0]); k++) {
            int w = 17, h = 33;
            AVCodecContext *e = open_encoder(w, h, AV_PIX_FMT_GBRPF32,
                                             comprs[k], "float");
            AVCodecContext *d = open_decoder();
            AVFrame *f = make_frame(w, h, AV_PIX_FMT_GBRPF32);
            AVFrame *o = av_frame_alloc();
            AVPacket *p = av_packet_alloc();
            ExrAttr a;

            task_check(o && p, "alloc compat");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        AV_WL32(f->data[c] + y * f->linesize[c] + x * 4,
                                av_float2int((float)((x * 3 + y + c) % 29) /
                                             29.f));
            encode_frame_pkt(e, f, p);
            task_check(parse_exr(p->data, p->size, &a) == 0, "parse compat");
            task_check(a.compression == (int)k, "legacy compression byte");
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "legacy decode");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < h; y++)
                    task_check(!memcmp(f->data[c] + y * f->linesize[c],
                                       o->data[c] + y * o->linesize[c],
                                       w * 4), "legacy roundtrip exact");
            av_packet_free(&p);
            av_frame_free(&f);
            av_frame_free(&o);
            avcodec_free_context(&e);
            avcodec_free_context(&d);
        }

        /* encoding must not modify the input frame */
        {
            int w = 23, h = 19;
            AVCodecContext *e = open_encoder(w, h, AV_PIX_FMT_GBRAPF32,
                                             "piz", "float");
            AVFrame *f = make_frame(w, h, AV_PIX_FMT_GBRAPF32);
            AVPacket *p = av_packet_alloc();
            uint8_t *snapshot;
            int sizes[4], total = 0;

            task_check(p != NULL, "alloc snapshot");
            for (int c = 0; c < 4; c++) {
                sizes[c] = f->linesize[c] * h;
                total += sizes[c];
            }
            snapshot = av_malloc(total);
            task_check(snapshot != NULL, "snapshot alloc");
            for (int c = 0; c < 4; c++)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        AV_WL32(f->data[c] + y * f->linesize[c] + x * 4,
                                av_float2int((float)(x ^ (y + c))));
            for (int c = 0, off = 0; c < 4; c++) {
                memcpy(snapshot + off, f->data[c], sizes[c]);
                off += sizes[c];
            }
            encode_frame_pkt(e, f, p);
            for (int c = 0, off = 0; c < 4; c++) {
                task_check(!memcmp(snapshot + off, f->data[c], sizes[c]),
                           "input pixels untouched");
                off += sizes[c];
            }
            av_free(snapshot);
            av_packet_free(&p);
            av_frame_free(&f);
            avcodec_free_context(&e);
        }

        /* different sized encoder instances must not share work buffers */
        {
            AVCodecContext *e1 = open_encoder(17, 33, AV_PIX_FMT_GBRPF32,
                                              "piz", "float");
            AVCodecContext *e2 = open_encoder(65, 9, AV_PIX_FMT_GBRPF32,
                                              "piz", "float");
            AVCodecContext *e3 = open_encoder(5, 7, AV_PIX_FMT_GRAYF32,
                                              "piz", "half");
            AVCodecContext *d = open_decoder();
            AVFrame *f1 = make_frame(17, 33, AV_PIX_FMT_GBRPF32);
            AVFrame *f2 = make_frame(65, 9, AV_PIX_FMT_GBRPF32);
            AVFrame *f3 = make_frame(5, 7, AV_PIX_FMT_GRAYF32);
            AVFrame *o = av_frame_alloc();
            AVPacket *p = av_packet_alloc();

            task_check(o && p, "alloc multi");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < 33; y++)
                    for (int x = 0; x < 17; x++)
                        AV_WL32(f1->data[c] + y * f1->linesize[c] + x * 4,
                                av_float2int(0.1f * (x + y + c)));
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < 9; y++)
                    for (int x = 0; x < 65; x++)
                        AV_WL32(f2->data[c] + y * f2->linesize[c] + x * 4,
                                av_float2int(0.2f * (x - y + c)));
            for (int y = 0; y < 7; y++)
                for (int x = 0; x < 5; x++)
                    AV_WL32(f3->data[0] + y * f3->linesize[0] + x * 4,
                            av_float2int(0.3f * x + 0.05f * y));

            encode_frame_pkt(e1, f1, p);
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "multi d1");
            task_check(o->width == 17 && o->height == 33, "size one");
            av_packet_unref(p);
            av_frame_unref(o);

            encode_frame_pkt(e2, f2, p);
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "multi d2");
            task_check(o->width == 65 && o->height == 9, "size two");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < 9; y++)
                    task_check(!memcmp(f2->data[c] + y * f2->linesize[c],
                                       o->data[c] + y * o->linesize[c],
                                       65 * 4), "instance two content");
            av_packet_unref(p);
            av_frame_unref(o);

            encode_frame_pkt(e3, f3, p);
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "multi d3");
            task_check(o->width == 5 && o->height == 7, "size three");
            av_packet_unref(p);

            /* repeat the first instance: its workspace must still be sane */
            encode_frame_pkt(e1, f1, p);
            task_check(avcodec_send_packet(d, p) == 0 &&
                       avcodec_receive_frame(d, o) == 0, "multi d1 again");
            task_check(o->width == 17 && o->height == 33, "size one again");
            for (int c = 0; c < 3; c++)
                for (int y = 0; y < 33; y++)
                    task_check(!memcmp(f1->data[c] + y * f1->linesize[c],
                                       o->data[c] + y * o->linesize[c],
                                       17 * 4), "instance one content");

            av_packet_free(&p);
            av_frame_free(&f1);
            av_frame_free(&f2);
            av_frame_free(&f3);
            av_frame_free(&o);
            avcodec_free_context(&e1);
            avcodec_free_context(&e2);
            avcodec_free_context(&e3);
            avcodec_free_context(&d);
        }
    } else {
        task_check(0, "unknown group");
    }
}
