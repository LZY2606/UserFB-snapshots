#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checks.h"

#include "libavcodec/avcodec.h"
#include "libavutil/float2half.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#define CHECK(cond, msg) task_check(!!(cond), msg)

#define SHORT_ZEROCODE_RUN 59
#define LONG_ZEROCODE_RUN  63
#define SHORTEST_LONG_RUN  (2 + LONG_ZEROCODE_RUN - SHORT_ZEROCODE_RUN)

enum Pattern {
    PAT_ZERO = 0,
    PAT_ONE,
    PAT_GRADIENT,
    PAT_CHECKER,
    PAT_SPECIAL,
    PAT_RANDOM,
    PAT_DENSE_14,
    PAT_DENSE_FULL
};

typedef struct CaseSpec {
    int width;
    int height;
    enum AVPixelFormat pix_fmt;
    int half;
    int pattern;
    uint32_t seed;
    double gamma;
} CaseSpec;

typedef struct BlockInfo {
    int y;
    int chunk_size;
    int raw_size;
    int piz;
    int fallback;

    unsigned min_nonzero;
    unsigned max_nonzero;
    unsigned bitmap_length;

    uint32_t im;
    uint32_t iM;
    uint32_t table_length;
    uint32_t n_bits;
    uint32_t huf_length;
    int table_symbols;
} BlockInfo;

typedef struct ExrInfo {
    int header_size;
    int blocks;
    int piz_blocks;
    int raw_blocks;
    int compression;
    uint32_t gamma_bits;
    int has_gamma;
    BlockInfo block[16];
} ExrInfo;

static int planes_for(enum AVPixelFormat fmt)
{
    if (fmt == AV_PIX_FMT_GRAYF32)
        return 1;
    if (fmt == AV_PIX_FMT_GBRAPF32)
        return 4;
    return 3;
}

static uint32_t next_u32(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static uint32_t pattern_bits(const CaseSpec *cs, int plane, int x, int y)
{
    uint32_t state = cs->seed + (uint32_t)(x * 7919 + y * 104729 + plane * 1299709);
    int idx = y * cs->width + x;

    switch (cs->pattern) {
    case PAT_ZERO:
        return 0;
    case PAT_ONE:
        return 0x3f800000u;
    case PAT_GRADIENT: {
        float v = (float)((x + 3 * y + 5 * plane) % 41) / 41.0f;
        return av_float2int(v);
    }
    case PAT_CHECKER:
        if (((x / 2) + (y / 2) + plane) & 1)
            return av_float2int(0.25f + 0.01f * (x & 7));
        return av_float2int(0.75f - 0.01f * (y & 7));
    case PAT_SPECIAL: {
        static const uint32_t bits[] = {
            0x00000000u, 0x80000000u, /* +0 and -0 */
            0x3f800000u, 0xbf800000u,
            0x7f7fffffu, 0xff7fffffu, /* finite extrema */
            0x00800000u, 0x80800000u  /* smallest normals, one signed */
        };
        return bits[(idx + plane * 3) % (int)(sizeof(bits) / sizeof(bits[0]))];
    }
    case PAT_RANDOM: {
        uint32_t v = next_u32(&state);
        /* Keep random values finite while retaining dense word patterns. */
        if ((v & 0x7f800000u) == 0x7f800000u)
            v ^= 0x40000000u;
        return v;
    }
    case PAT_DENSE_14: {
        int count = cs->width * 32;
        if (idx < count - 1)
            return (uint32_t)(idx & 0x3fff);
        return 0x40000000u; /* high word 16384 crosses the w14 condition */
    }
    case PAT_DENSE_FULL:
        return (uint32_t)(idx & 0xffff);
    }
    return 0;
}

static void fill_frame(AVFrame *frame, const CaseSpec *cs)
{
    int planes = planes_for(cs->pix_fmt);

    for (int p = 0; p < planes; p++) {
        for (int y = 0; y < cs->height; y++) {
            uint32_t *row = (uint32_t *)(frame->data[p] + y * frame->linesize[p]);
            for (int x = 0; x < cs->width; x++)
                row[x] = pattern_bits(cs, p, x, y);
        }
    }
}

typedef struct BitReader {
    const uint8_t *data;
    size_t size;
    size_t pos;
    int error;
} BitReader;

static unsigned br_bits(BitReader *b, int n)
{
    unsigned value = 0;

    for (int i = 0; i < n; i++) {
        size_t bit = b->pos++;
        size_t byte = bit / 8;
        int offset = 7 - (int)(bit & 7);

        if (byte >= b->size) {
            b->error = 1;
            return 0;
        }
        value = (value << 1) | ((b->data[byte] >> offset) & 1);
    }
    return value;
}

static void parse_huffman_table(const uint8_t *huf, uint32_t huf_size,
                                BlockInfo *bi)
{
    uint32_t im = bi->im, iM = bi->iM;
    uint32_t table_size = bi->table_length;
    BitReader br = { huf + 20, table_size, 0, 0 };
    uint32_t i = im;
    int positive = 0;

    CHECK(huf_size >= 20, "Huffman header fits");
    CHECK(im <= iM && iM <= 65536, "Huffman symbol range");
    CHECK(20u + table_size <= huf_size, "Huffman table range");

    while (i <= iM && !br.error) {
        unsigned len = br_bits(&br, 6);

        if (len == LONG_ZEROCODE_RUN) {
            unsigned run = br_bits(&br, 8) + SHORTEST_LONG_RUN;
            CHECK(i + run <= iM + 1, "long zero run stays in table range");
            i += run;
        } else if (len >= SHORT_ZEROCODE_RUN) {
            unsigned run = len - SHORT_ZEROCODE_RUN + 2;
            CHECK(i + run <= iM + 1, "short zero run stays in table range");
            i += run;
        } else {
            CHECK(len <= 32, "decoder-supported Huffman length");
            if (len > 0)
                positive++;
            i++;
        }
    }

    CHECK(!br.error, "Huffman table bits are present");
    CHECK(i == iM + 1, "Huffman table covers exactly im..iM");
    bi->table_symbols = positive;

    size_t used_bytes = (br.pos + 7) / 8;
    CHECK(used_bytes <= table_size, "packed table byte boundary");
    CHECK((uint32_t)used_bytes == table_size, "packed table length is exact");
    CHECK(positive >= 2, "Huffman table has symbol and run code");
}

static void parse_piz_block(const uint8_t *packet, size_t packet_size,
                            uint64_t offset, int raw_size, BlockInfo *bi)
{
    const uint8_t *chunk = packet + offset;
    const uint8_t *piz;
    unsigned min_b, max_b;
    uint32_t huf_length;

    bi->chunk_size = AV_RL32(chunk + 4);
    bi->raw_size = raw_size;
    CHECK(offset + 8 + (uint64_t)bi->chunk_size <= packet_size,
          "chunk data lies inside packet");
    CHECK(bi->chunk_size > 0 && bi->chunk_size <= raw_size,
          "PIZ chunk length is at most raw block size");

    if (bi->chunk_size == raw_size) {
        bi->fallback = 1;
        return;
    }

    bi->piz = 1;
    piz = chunk + 8;

    min_b = AV_RL16(piz);
    max_b = AV_RL16(piz + 2);
    bi->min_nonzero = min_b;
    bi->max_nonzero = max_b;

    CHECK(max_b < 8192, "PIZ bitmap maximum byte is in bounds");
    CHECK(min_b <= 8191, "PIZ bitmap minimum byte is in bounds");

    if (min_b <= max_b) {
        bi->bitmap_length = max_b - min_b + 1;
        CHECK(bi->bitmap_length >= 1 && bi->bitmap_length <= 8192,
              "PIZ bitmap extrema enclose a valid bitmap");
    } else {
        /* The format's all-zero convention is min=8191, max=0. */
        bi->bitmap_length = 0;
        CHECK(min_b == 8191 && max_b == 0, "empty bitmap extrema convention");
    }

    CHECK(4u + bi->bitmap_length + 4u <= (unsigned)bi->chunk_size,
          "bitmap and PIZ Huffman length prefix fit");

    huf_length = AV_RL32(piz + 4 + bi->bitmap_length);
    bi->huf_length = huf_length;
    CHECK((unsigned)bi->chunk_size == 8u + bi->bitmap_length + huf_length,
          "PIZ Huffman length matches chunk boundary");
    CHECK(huf_length >= 20, "PIZ Huffman stream has a header");

    const uint8_t *huf = piz + 8 + bi->bitmap_length;
    bi->im = AV_RL32(huf + 0);
    bi->iM = AV_RL32(huf + 4);
    bi->table_length = AV_RL32(huf + 8);
    bi->n_bits = AV_RL32(huf + 12);
    CHECK(AV_RL32(huf + 16) == 0, "reserved Huffman header word is zero");

    uint32_t data_bytes = (bi->n_bits + 7) / 8;
    CHECK(bi->n_bits > 0, "Huffman data has valid bits");
    CHECK(20u + bi->table_length + data_bytes == huf_length,
          "Huffman header/table/data lengths are exact");
    CHECK(bi->n_bits <= data_bytes * 8, "nBits does not exceed data bytes");
    CHECK(bi->n_bits > (data_bytes - 1) * 8,
          "nBits identifies the final partial byte");

    parse_huffman_table(huf, huf_length, bi);
}

static void parse_exr(const AVPacket *pkt, const CaseSpec *cs, ExrInfo *info)
{
    const uint8_t *d = pkt->data;
    size_t n = pkt->size;
    size_t pos = 8;
    int planes = planes_for(cs->pix_fmt);
    int bytes_per_sample = cs->half ? 2 : 4;

    memset(info, 0, sizeof(*info));
    CHECK(n > 32 && AV_RL32(d) == 20000630u, "EXR magic");

    while (pos < n && d[pos] != 0) {
        const uint8_t *name = d + pos;
        const uint8_t *name_nul = memchr(name, 0, n - pos);
        const uint8_t *type;
        const uint8_t *type_nul;
        uint32_t value_size;

        CHECK(name_nul != NULL, "attribute name is NUL terminated");
        pos = (size_t)(name_nul - d) + 1;
        type = d + pos;
        type_nul = memchr(type, 0, n - pos);
        CHECK(type_nul != NULL, "attribute type is NUL terminated");
        pos = (size_t)(type_nul - d) + 1;
        CHECK(pos + 4 <= n, "attribute size fits");
        value_size = AV_RL32(d + pos);
        pos += 4;
        CHECK(pos + value_size <= n, "attribute value fits");

        if (!strcmp((const char *)name, "compression")) {
            CHECK(value_size == 1, "compression attribute size");
            info->compression = d[pos];
        } else if (!strcmp((const char *)name, "gamma")) {
            CHECK(value_size == 4, "gamma attribute size");
            info->gamma_bits = AV_RL32(d + pos);
            info->has_gamma = 1;
        }

        pos += value_size;
    }

    CHECK(pos < n && d[pos] == 0, "EXR header terminator");
    info->header_size = (int)++pos;

    info->blocks = (cs->height + 31) / 32;
    CHECK(info->blocks <= 16, "test parser block capacity");
    CHECK(pos + 8u * info->blocks <= n, "offset table fits");

    uint64_t first_offset = AV_RL64(d + pos);
    CHECK(first_offset == pos + 8u * info->blocks,
          "first chunk offset follows offset table");

    for (int i = 0; i < info->blocks; i++) {
        uint64_t offset = AV_RL64(d + pos + 8u * i);
        int rows = FFMIN(32, cs->height - i * 32);
        int raw_size = cs->width * rows * planes * bytes_per_sample;

        CHECK(offset < n && offset + 8 <= n, "chunk offset is in packet");
        BlockInfo *bi = &info->block[i];
        bi->y = AV_RL32(d + offset);
        CHECK(bi->y == i * 32, "PIZ chunk y coordinate");

        parse_piz_block(d, n, offset, raw_size, bi);
        if (bi->piz)
            info->piz_blocks++;
        if (bi->fallback)
            info->raw_blocks++;

        if (i + 1 < info->blocks) {
            uint64_t next = AV_RL64(d + pos + 8u * (i + 1));
            CHECK(next == offset + 8u + bi->chunk_size,
                  "offset table walks contiguous chunk boundaries");
        } else {
            CHECK(n == offset + 8u + bi->chunk_size,
                  "last chunk ends at packet end");
        }
    }

    CHECK(info->compression == 4, "header advertises PIZ compression byte 4");
    if (cs->gamma > 0) {
        CHECK(info->has_gamma, "gamma metadata is present");
        CHECK(info->gamma_bits == av_float2int((float)cs->gamma),
              "gamma metadata is unchanged");
    }
}

static void compare_half_frame(const AVFrame *in, const AVFrame *out,
                               const CaseSpec *cs)
{
    static Float2HalfTables f2h;
    static int initialized;
    int planes = planes_for(cs->pix_fmt);

    if (!initialized) {
        ff_init_float2half_tables(&f2h);
        initialized = 1;
    }

    for (int p = 0; p < planes; p++) {
        for (int y = 0; y < cs->height; y++) {
            const uint8_t *in_row = in->data[p] + y * in->linesize[p];
            const uint16_t *out_row =
                (const uint16_t *)(out->data[p] + y * out->linesize[p]);

            for (int x = 0; x < cs->width; x++) {
                uint32_t bits;
                uint16_t expected;

                memcpy(&bits, in_row + x * 4, sizeof(bits));
                expected = float2half(bits, &f2h);
                CHECK(out_row[x] == expected,
                      "half output matches encoder float-to-half words");
            }
        }
    }
}

static void compare_float_frame(const AVFrame *in, const AVFrame *out,
                                const CaseSpec *cs)
{
    int planes = planes_for(cs->pix_fmt);

    for (int p = 0; p < planes; p++) {
        for (int y = 0; y < cs->height; y++) {
            const uint8_t *a = in->data[p] + y * in->linesize[p];
            const uint8_t *b = out->data[p] + y * out->linesize[p];
            CHECK(!memcmp(a, b, (size_t)cs->width * 4),
                  "float words round-trip exactly");
        }
    }
}

static void check_input_unchanged(const AVFrame *in,
                                  const uint8_t *before[4],
                                  const CaseSpec *cs)
{
    int planes = planes_for(cs->pix_fmt);

    for (int p = 0; p < planes; p++) {
        for (int y = 0; y < cs->height; y++) {
            CHECK(!memcmp(before[p] + (size_t)y * cs->width * 4,
                          in->data[p] + y * in->linesize[p],
                          (size_t)cs->width * 4),
                  "encoding does not modify input pixels");
        }
    }
}

static ExrInfo run_piz_case(const CaseSpec *cs)
{
    const AVCodec *enc_codec = avcodec_find_encoder(AV_CODEC_ID_EXR);
    const AVCodec *dec_codec = avcodec_find_decoder(AV_CODEC_ID_EXR);
    AVCodecContext *enc = avcodec_alloc_context3(enc_codec);
    AVCodecContext *dec = avcodec_alloc_context3(dec_codec);
    AVFrame *in = av_frame_alloc();
    AVFrame *out = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    const uint8_t *before[4] = { 0 };
    ExrInfo info;
    int planes = planes_for(cs->pix_fmt);

    memset(&info, 0, sizeof(info));
    CHECK(enc && dec && in && out && pkt, "allocate codecs and frames");
    enc->width = cs->width;
    enc->height = cs->height;
    enc->pix_fmt = cs->pix_fmt;
    enc->time_base = (AVRational) { 1, 25 };
    enc->thread_count = 1;
    dec->thread_count = 1;

    CHECK(av_opt_set(enc->priv_data, "compression", "piz", 0) == 0,
          "set compression=piz");
    CHECK(av_opt_set(enc->priv_data, "format", cs->half ? "half" : "float", 0) == 0,
          "set EXR sample format");
    if (cs->gamma > 0)
        CHECK(av_opt_set_double(enc->priv_data, "gamma", cs->gamma, 0) == 0,
              "set gamma option");

    CHECK(avcodec_open2(enc, enc->codec, NULL) == 0, "open PIZ encoder");
    CHECK(avcodec_open2(dec, dec->codec, NULL) == 0, "open EXR decoder");

    in->width = cs->width;
    in->height = cs->height;
    in->format = cs->pix_fmt;
    CHECK(av_frame_get_buffer(in, 32) == 0, "allocate input frame");
    fill_frame(in, cs);

    for (int p = 0; p < planes; p++) {
        before[p] = malloc((size_t)cs->height * cs->width * 4);
        CHECK(before[p] != NULL, "allocate input snapshot");
        for (int y = 0; y < cs->height; y++)
            memcpy((uint8_t *)before[p] + (size_t)y * cs->width * 4,
                   in->data[p] + y * in->linesize[p],
                   (size_t)cs->width * 4);
    }

    CHECK(avcodec_send_frame(enc, in) == 0, "send frame to PIZ encoder");
    CHECK(avcodec_receive_packet(enc, pkt) == 0, "receive PIZ packet");

    check_input_unchanged(in, before, cs);
    parse_exr(pkt, cs, &info);

    CHECK(avcodec_send_packet(dec, pkt) == 0, "send PIZ packet to decoder");
    int decoded = avcodec_receive_frame(dec, out);
    CHECK(decoded == 0, "decode PIZ packet through libavcodec EXR decoder");

    if (decoded == 0) {
        if (cs->half)
            compare_half_frame(in, out, cs);
        else
            compare_float_frame(in, out, cs);
    }

    for (int p = 0; p < planes; p++)
        free((void *)before[p]);
    av_packet_free(&pkt);
    av_frame_free(&in);
    av_frame_free(&out);
    avcodec_free_context(&enc);
    avcodec_free_context(&dec);
    return info;
}

static void run_legacy_case(const char *compression)
{
    CaseSpec cs = {
        .width = 17, .height = 33, .pix_fmt = AV_PIX_FMT_GBRPF32,
        .half = 0, .pattern = PAT_GRADIENT, .seed = 7
    };
    const AVCodec *ec = avcodec_find_encoder(AV_CODEC_ID_EXR);
    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_EXR);
    AVCodecContext *e = avcodec_alloc_context3(ec);
    AVCodecContext *d = avcodec_alloc_context3(dc);
    AVFrame *in = av_frame_alloc(), *out = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();

    e->width = cs.width;
    e->height = cs.height;
    e->pix_fmt = cs.pix_fmt;
    e->time_base = (AVRational) {1, 25};
    e->thread_count = d->thread_count = 1;
    CHECK(av_opt_set(e->priv_data, "compression", compression, 0) == 0,
          "set legacy compression");
    CHECK(av_opt_set(e->priv_data, "format", "float", 0) == 0,
          "set legacy float format");
    CHECK(avcodec_open2(e, e->codec, NULL) == 0, "open legacy encoder");
    CHECK(avcodec_open2(d, d->codec, NULL) == 0, "open legacy decoder");

    in->width = cs.width;
    in->height = cs.height;
    in->format = cs.pix_fmt;
    CHECK(av_frame_get_buffer(in, 32) == 0, "legacy frame buffer");
    fill_frame(in, &cs);
    CHECK(avcodec_send_frame(e, in) == 0, "legacy send frame");
    CHECK(avcodec_receive_packet(e, pkt) == 0, "legacy packet");
    CHECK(avcodec_send_packet(d, pkt) == 0, "legacy decoder packet");
    CHECK(avcodec_receive_frame(d, out) == 0, "legacy decoded frame");
    compare_float_frame(in, out, &cs);

    av_packet_free(&pkt);
    av_frame_free(&in);
    av_frame_free(&out);
    avcodec_free_context(&e);
    avcodec_free_context(&d);
}

static void normal_cases(void)
{
    CaseSpec flat = {
        .width = 33, .height = 64, .pix_fmt = AV_PIX_FMT_GBRPF32,
        .half = 0, .pattern = PAT_ONE, .seed = 1, .gamma = 2.5
    };
    ExrInfo info = run_piz_case(&flat);
    CHECK(info.piz_blocks == info.blocks && info.blocks == 2,
          "flat image produces real PIZ blocks in every 32-line block");
    CHECK(info.block[0].piz && info.block[0].chunk_size < info.block[0].raw_size / 4,
          "flat PIZ block is substantially compressed");
    CHECK(info.block[0].min_nonzero == 0x3f80 >> 3,
          "bitmap minimum tracks the high word of 1.0f");
    CHECK(info.block[0].max_nonzero == info.block[0].min_nonzero,
          "constant image has one nonzero bitmap byte");

    CaseSpec gray = {
        .width = 31, .height = 32, .pix_fmt = AV_PIX_FMT_GRAYF32,
        .half = 0, .pattern = PAT_GRADIENT, .seed = 2
    };
    run_piz_case(&gray);

    CaseSpec rgb = {
        .width = 17, .height = 33, .pix_fmt = AV_PIX_FMT_GBRPF32,
        .half = 0, .pattern = PAT_CHECKER, .seed = 3
    };
    ExrInfo rgb_info = run_piz_case(&rgb);
    CHECK(rgb_info.piz_blocks + rgb_info.raw_blocks == rgb_info.blocks,
          "every RGB block is either PIZ or a correct fallback");

    CaseSpec alpha = {
        .width = 19, .height = 35, .pix_fmt = AV_PIX_FMT_GBRAPF32,
        .half = 0, .pattern = PAT_SPECIAL, .seed = 4
    };
    run_piz_case(&alpha);
}

static void boundary_cases(void)
{
    const int heights[] = { 1, 31, 32, 33, 64, 65 };
    const int widths[] = { 1, 2, 3, 17, 33 };

    for (unsigned hi = 0; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
        for (unsigned wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
            /* Focus the exhaustive odd-size loop on the requested edges. */
            if ((heights[hi] != 32 && heights[hi] != 33 && heights[hi] != 65) &&
                widths[wi] != 1)
                continue;
            if (wi > 2 && hi % 2)
                continue;

            CaseSpec cs = {
                .width = widths[wi],
                .height = heights[hi],
                .pix_fmt = (wi & 1) ? AV_PIX_FMT_GBRPF32 : AV_PIX_FMT_GBRAPF32,
                .half = hi & 1,
                .pattern = (wi & 1) ? PAT_GRADIENT : PAT_CHECKER,
                .seed = 100u + hi * 17u + wi
            };
            ExrInfo info = run_piz_case(&cs);
            CHECK(info.blocks == (cs.height + 31) / 32,
                  "boundary block count follows 32-line grouping");
            CHECK(info.piz_blocks + info.raw_blocks == info.blocks,
                  "all boundary blocks are accounted for");
        }
    }

    CaseSpec zero_rgb = {
        .width = 33, .height = 65, .pix_fmt = AV_PIX_FMT_GBRPF32,
        .half = 0, .pattern = PAT_ZERO, .seed = 11
    };
    ExrInfo zero_info = run_piz_case(&zero_rgb);
    CHECK(zero_info.piz_blocks >= 2, "zero blocks compress as real PIZ");
    CHECK(zero_info.block[0].min_nonzero == 8191 &&
          zero_info.block[0].max_nonzero == 0,
          "all-zero bitmap uses the empty extrema convention");

    CaseSpec zero_half = {
        .width = 37, .height = 33, .pix_fmt = AV_PIX_FMT_GBRAPF32,
        .half = 1, .pattern = PAT_ZERO, .seed = 12
    };
    run_piz_case(&zero_half);

    CaseSpec special_half = {
        .width = 41, .height = 65, .pix_fmt = AV_PIX_FMT_GBRAPF32,
        .half = 1, .pattern = PAT_SPECIAL, .seed = 13
    };
    run_piz_case(&special_half);

    CaseSpec dense14 = {
        .width = 512, .height = 32, .pix_fmt = AV_PIX_FMT_GRAYF32,
        .half = 0, .pattern = PAT_DENSE_14, .seed = 14
    };
    ExrInfo dense_info = run_piz_case(&dense14);
    CHECK(dense_info.blocks == 1, "dense threshold case has one full block");
    CHECK(dense_info.block[0].piz || dense_info.block[0].fallback,
          "dense threshold block is valid PIZ or raw fallback");

    CaseSpec dense_full = {
        .width = 2048, .height = 32, .pix_fmt = AV_PIX_FMT_GRAYF32,
        .half = 0, .pattern = PAT_DENSE_FULL, .seed = 15
    };
    ExrInfo full_info = run_piz_case(&dense_full);
    CHECK(full_info.block[0].min_nonzero == 0 &&
          full_info.block[0].max_nonzero == 8191 &&
          full_info.block[0].bitmap_length == 8192,
          "dense word values exercise the full 8 KiB bitmap");
    CHECK(full_info.block[0].piz, "dense ramp still produces a PIZ block");

    CaseSpec tiny_float = {
        .width = 1, .height = 1, .pix_fmt = AV_PIX_FMT_GRAYF32,
        .half = 0, .pattern = PAT_SPECIAL, .seed = 16
    };
    ExrInfo tiny_info = run_piz_case(&tiny_float);
    CHECK(tiny_info.raw_blocks == 1 && tiny_info.piz_blocks == 0,
          "tiny float block uses the EXR raw fallback");

    CaseSpec tiny_half = {
        .width = 1, .height = 1, .pix_fmt = AV_PIX_FMT_GRAYF32,
        .half = 1, .pattern = PAT_ONE, .seed = 17
    };
    ExrInfo tiny_half_info = run_piz_case(&tiny_half);
    CHECK(tiny_half_info.raw_blocks == 1, "tiny half block uses raw fallback");

    CaseSpec odd_col = {
        .width = 1, .height = 33, .pix_fmt = AV_PIX_FMT_GBRPF32,
        .half = 0, .pattern = PAT_CHECKER, .seed = 18
    };
    run_piz_case(&odd_col);

    CaseSpec odd_row = {
        .width = 33, .height = 1, .pix_fmt = AV_PIX_FMT_GBRPF32,
        .half = 1, .pattern = PAT_GRADIENT, .seed = 19
    };
    run_piz_case(&odd_row);

    CaseSpec half_constant_row = {
        .width = 257, .height = 1, .pix_fmt = AV_PIX_FMT_GRAYF32,
        .half = 1, .pattern = PAT_ONE, .seed = 20
    };
    ExrInfo half_row_info = run_piz_case(&half_constant_row);
    CHECK(half_row_info.block[0].piz, "wide constant half row uses PIZ");
    CHECK(half_row_info.block[0].im > 0,
          "Huffman range can start after the reserved zero run symbol");
}

static void compat_cases(void)
{
    static const char *legacy[] = { "none", "rle", "zip1", "zip16" };

    for (unsigned i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++)
        run_legacy_case(legacy[i]);

    CaseSpec sizes[] = {
        { 1,  1, AV_PIX_FMT_GRAYF32, 1, PAT_ONE,      21, 0},
        {33, 65, AV_PIX_FMT_GBRAPF32, 1, PAT_CHECKER,  22, 0},
        {17,  1, AV_PIX_FMT_GBRPF32, 0, PAT_SPECIAL,   23, 0},
        {65, 33, AV_PIX_FMT_GRAYF32, 0, PAT_RANDOM,    24, 0},
        { 3, 97, AV_PIX_FMT_GBRAPF32, 0, PAT_GRADIENT, 25, 0},
        {33, 65, AV_PIX_FMT_GBRAPF32, 1, PAT_ZERO,     26, 0}
    };

    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ExrInfo info = run_piz_case(&sizes[i]);
        CHECK(info.piz_blocks + info.raw_blocks == info.blocks,
              "different-sized encoder workspaces remain independent");
    }
}

void task_cases(const char *group)
{
    if (!strcmp(group, "normal"))
        normal_cases();
    else if (!strcmp(group, "boundary"))
        boundary_cases();
    else if (!strcmp(group, "compat"))
        compat_cases();
}
