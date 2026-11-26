/*
 * Floor 1 numerical contract for stb_vorbis.
 *
 * This file deliberately contains three independent reference models:
 *
 *   points  - declaration-order neighbor search, prediction, symmetric
 *             difference recovery and post-step-2 activity;
 *   curve   - the integer Bresenham line state and dB gain expansion;
 *   packets  - spectral floor/residue product followed by a direct cosine
 *             IMDCT and explicit Vorbis window overlap.
 *
 * The models are written from the Vorbis I specification's Floor 1 and
 * transform/window equations.  They do not call predict_point(), draw_line(),
 * inverse_mdct() or any decoding entry point to manufacture expected values.
 * The only production entry points used as the system under test are the
 * internal do_floor() helper and stb_vorbis_open_memory()/get_frame_float().
 */
#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_vorbis.c"

#define FC_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Small LSB-first bit writer and Ogg page writer                     */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *data;
    size_t cap;
    size_t pos;
    int overflow;
} fc_bits;

static void fc_bw_init(fc_bits *b, unsigned char *data, size_t cap)
{
    memset(data, 0, cap);
    b->data = data;
    b->cap = cap;
    b->pos = 0;
    b->overflow = 0;
}

static void fc_bw_put(fc_bits *b, uint32_t value, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        size_t byte = b->pos >> 3;
        unsigned bit = (unsigned)(b->pos & 7);
        if (byte >= b->cap) {
            b->overflow = 1;
        } else {
            if ((value >> i) & 1u)
                b->data[byte] |= (unsigned char)(1u << bit);
            else
                b->data[byte] &= (unsigned char)~(1u << bit);
        }
        ++b->pos;
    }
}

static void fc_bw_bytes(fc_bits *b, unsigned char *out, size_t *len)
{
    *len = (b->pos + 7) / 8;
    if (out && *len) memcpy(out, b->data, *len);
}

static uint32_t fc_ogg_crc(const unsigned char *p, size_t n)
{
    uint32_t crc = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        unsigned k;
        crc ^= (uint32_t)p[i] << 24;
        for (k = 0; k < 8; ++k)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : (crc << 1);
    }
    return crc;
}

static size_t fc_ogg_page(unsigned char *out, size_t cap,
                          const unsigned char *packet, size_t len,
                          uint32_t seq, unsigned flags,
                          uint64_t granule, uint32_t serial)
{
    size_t full = len / 255;
    size_t nseg = full + 1;
    size_t hdr = 27 + nseg;
    size_t total = hdr + len;
    size_t i;
    uint32_t crc;

    if (nseg > 255 || total > cap) return 0;
    memset(out, 0, total);
    memcpy(out, "OggS", 4);
    out[4] = 0;
    out[5] = (unsigned char)flags;
    for (i = 0; i < 8; ++i)
        out[6 + i] = (unsigned char)((granule >> (8 * i)) & 255u);
    for (i = 0; i < 4; ++i)
        out[14 + i] = (unsigned char)((serial >> (8 * i)) & 255u);
    for (i = 0; i < 4; ++i)
        out[18 + i] = (unsigned char)((seq >> (8 * i)) & 255u);
    out[26] = (unsigned char)nseg;
    for (i = 0; i < full; ++i) out[27 + i] = 255;
    out[27 + full] = (unsigned char)(len % 255);
    memcpy(out + hdr, packet, len);
    crc = fc_ogg_crc(out, total);
    for (i = 0; i < 4; ++i)
        out[22 + i] = (unsigned char)((crc >> (8 * i)) & 255u);
    return total;
}

static void fc_magic(fc_bits *b, unsigned type)
{
    static const char sig[6] = {'v','o','r','b','i','s'};
    unsigned i;
    fc_bw_put(b, type, 8);
    for (i = 0; i < 6; ++i) fc_bw_put(b, (unsigned char)sig[i], 8);
}

/* Equal-length codebook codewords are the bit reversal of the symbol. */
static uint32_t fc_rev_bits(unsigned symbol, unsigned len)
{
    uint32_t code = 0;
    unsigned i;
    for (i = 0; i < len; ++i)
        if ((symbol >> i) & 1u)
            code |= 1u << (len - 1 - i);
    return code;
}

static void fc_symbol7(fc_bits *b, unsigned symbol)
{
    fc_bw_put(b, fc_rev_bits(symbol, 7), 7);
}

/* ------------------------------------------------------------------ */
/* Shared Floor 1 fixture                                              */
/* ------------------------------------------------------------------ */

#define FC_VALUES 8
#define FC_INTERIOR 6

/*
 * Declaration order is intentionally not X-sorted:
 *   id 2 X=40, id 3 X=16, id 4 X=24, id 5 X=32, id 6 X=56, id 7 X=8.
 * Sorted draw order is 0,7,3,4,5,2,6,1.
 */
static const int fc_x[FC_VALUES] = {0, 64, 40, 16, 24, 32, 56, 8};

static int fc_range_for_mult(int mult)
{
    static const int ranges[4] = {256, 128, 86, 64};
    return ranges[mult - 1];
}

static int fc_y_bits(int mult)
{
    int range = fc_range_for_mult(mult);
    if (range == 256) return 8;
    if (range == 128) return 7;
    return 6;
}

/* Independent declaration-order nearest-neighbor search. */
static void fc_neighbors(int id, int *low, int *high)
{
    int lo_id = 0, hi_id = 1;
    int lo_x = -1, hi_x = 1000000;
    int j;
    for (j = 0; j < id; ++j) {
        if (fc_x[j] < fc_x[id] && fc_x[j] > lo_x) {
            lo_x = fc_x[j];
            lo_id = j;
        }
        if (fc_x[j] > fc_x[id] && fc_x[j] < hi_x) {
            hi_x = fc_x[j];
            hi_id = j;
        }
    }
    *low = lo_id;
    *high = hi_id;
}

/* Spec integer prediction: absolute slope, truncating C division. */
static int fc_predict(int x, int x0, int x1, int y0, int y1)
{
    int dy = y1 - y0;
    int adx = x1 - x0;
    int off = (int)((unsigned)abs(dy) * (unsigned)(x - x0) / (unsigned)adx);
    return dy < 0 ? y0 - off : y0 + off;
}

/*
 * Recover all Y values and the post-step-2 draw activity.
 * encoded[0..1] are endpoint Ys; encoded[2..7] are raw difference symbols.
 * On return y[] is the recovered prediction-domain Y for every point and
 * active[] is the flag after a later nonzero point reactivates neighbors.
 */
static void fc_recover(int mult, const int encoded[FC_VALUES],
                       int y[FC_VALUES], int active[FC_VALUES])
{
    int range = fc_range_for_mult(mult);
    int j;
    for (j = 0; j < FC_VALUES; ++j) {
        y[j] = encoded[j];
        active[j] = (j < 2);
    }
    for (j = 2; j < FC_VALUES; ++j) {
        int low, high, pred, highroom, lowroom, room, val;
        fc_neighbors(j, &low, &high);
        pred = fc_predict(fc_x[j], fc_x[low], fc_x[high], y[low], y[high]);
        val = encoded[j];
        highroom = range - pred;
        lowroom = pred;
        room = (highroom < lowroom ? highroom * 2 : lowroom * 2);
        active[j] = 0;
        if (val) {
            active[j] = active[low] = active[high] = 1;
            if (val >= room) {
                if (highroom > lowroom)
                    y[j] = val - lowroom + pred;
                else
                    y[j] = pred - val + highroom - 1;
            } else if (val & 1) {
                y[j] = pred - ((val + 1) >> 1);
            } else {
                y[j] = pred + (val >> 1);
            }
        } else {
            y[j] = pred;
        }
    }
}

static void fc_sorted_ids(int ids[FC_VALUES])
{
    int used[FC_VALUES] = {0};
    int n, j, best;
    ids[0] = 0; used[0] = 1;
    for (n = 1; n < FC_VALUES; ++n) {
        best = -1;
        for (j = 0; j < FC_VALUES; ++j) {
            if (used[j]) continue;
            if (best < 0 || fc_x[j] < fc_x[best]) best = j;
        }
        ids[n] = best;
        used[best] = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Stream construction                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int mult;
    int block_log;
    int audio_count;
    const int *present;       /* audio_count */
    const int (*endpoint)[2]; /* audio_count, may be NULL for defaults */
    const int (*raw)[FC_INTERIOR]; /* audio_count */
} fc_stream_cfg;

static void fc_write_id(fc_bits *b, int block_log)
{
    fc_magic(b, 1);
    fc_bw_put(b, 0, 32);             /* vorbis version */
    fc_bw_put(b, 1, 8);              /* channels */
    fc_bw_put(b, 8000, 32);
    fc_bw_put(b, 0, 32);             /* max bitrate */
    fc_bw_put(b, 0, 32);             /* nominal bitrate */
    fc_bw_put(b, 0, 32);             /* min bitrate */
    fc_bw_put(b, (uint32_t)((block_log << 4) | block_log), 8);
    fc_bw_put(b, 1, 8);              /* framing */
}

static void fc_write_comment(fc_bits *b)
{
    fc_magic(b, 3);
    fc_bw_put(b, 0, 32);             /* vendor length */
    fc_bw_put(b, 0, 32);             /* comment count */
    fc_bw_put(b, 1, 8);              /* framing */
}

static const int fc_residue_values[4] = {1, 2, 4, 3};

static void fc_write_setup(fc_bits *b, int mult)
{
    int i;
    fc_magic(b, 5);

    /* Two codebooks. */
    fc_bw_put(b, 1, 8);

    /* CB0: 128 scalar seven-bit symbols, identity lookup. */
    fc_bw_put(b, 0x42, 8);
    fc_bw_put(b, 0x43, 8);
    fc_bw_put(b, 0x56, 8);
    fc_bw_put(b, 1, 16);             /* dimensions */
    fc_bw_put(b, 128, 24);           /* entries */
    fc_bw_put(b, 0, 1);              /* unordered */
    fc_bw_put(b, 0, 1);              /* not sparse */
    for (i = 0; i < 128; ++i) fc_bw_put(b, 6, 5); /* length = 7 */
    fc_bw_put(b, 1, 4);              /* lookup type 1 */
    fc_bw_put(b, 0, 32);             /* minimum */
    fc_bw_put(b, 0x60100000u, 32);   /* delta = 1.0 in Vorbis float format */
    fc_bw_put(b, 7, 4);              /* value_bits = 8 */
    fc_bw_put(b, 0, 1);              /* no sequence continuation */
    for (i = 0; i < 128; ++i) fc_bw_put(b, (uint32_t)i, 8);

    /* CB1: one four-dimensional residue vector, direct lookup type 2. */
    fc_bw_put(b, 0x42, 8);
    fc_bw_put(b, 0x43, 8);
    fc_bw_put(b, 0x56, 8);
    fc_bw_put(b, 4, 16);
    fc_bw_put(b, 1, 24);
    fc_bw_put(b, 0, 1);
    fc_bw_put(b, 0, 1);
    fc_bw_put(b, 0, 5);              /* code length 1 */
    fc_bw_put(b, 2, 4);              /* direct lookup */
    fc_bw_put(b, 0, 32);
    fc_bw_put(b, 0x60100000u, 32);
    fc_bw_put(b, 7, 4);              /* 8-bit multiplicands */
    fc_bw_put(b, 0, 1);
    for (i = 0; i < 4; ++i)
        fc_bw_put(b, (uint32_t)fc_residue_values[i], 8);

    /* One time-domain transform. */
    fc_bw_put(b, 0, 6);
    fc_bw_put(b, 0, 16);

    /* Floor 1: one partition/class of six interior points. */
    fc_bw_put(b, 0, 6);              /* floor count - 1 */
    fc_bw_put(b, 1, 16);             /* type 1 */
    fc_bw_put(b, 1, 5);              /* partitions */
    fc_bw_put(b, 0, 4);              /* partition 0 -> class 0 */
    fc_bw_put(b, FC_INTERIOR - 1, 3);
    fc_bw_put(b, 0, 2);              /* no subclasses */
    fc_bw_put(b, 1, 8);              /* subclass book 0 (stored + 1) */
    fc_bw_put(b, (uint32_t)(mult - 1), 2);
    fc_bw_put(b, 6, 4);              /* rangebits: X max 64 */
    for (i = 2; i < FC_VALUES; ++i)
        fc_bw_put(b, (uint32_t)fc_x[i], 6);

    /* Residue 0: type 1, one four-sample partition. */
    fc_bw_put(b, 0, 6);
    fc_bw_put(b, 1, 16);
    fc_bw_put(b, 0, 24);             /* begin */
    fc_bw_put(b, 4, 24);             /* end */
    fc_bw_put(b, 3, 24);             /* part_size - 1 */
    fc_bw_put(b, 0, 6);              /* classifications - 1 */
    fc_bw_put(b, 0, 8);              /* classbook CB0 */
    fc_bw_put(b, 1, 3);              /* cascade low bits: pass 0 */
    fc_bw_put(b, 0, 1);              /* no high cascade byte */
    fc_bw_put(b, 1, 8);              /* residue book CB1 */

    /* Mapping 0: mono, one submap. */
    fc_bw_put(b, 0, 6);
    fc_bw_put(b, 0, 16);             /* mapping type */
    fc_bw_put(b, 0, 1);              /* one submap */
    fc_bw_put(b, 0, 1);              /* no coupling */
    fc_bw_put(b, 0, 2);              /* reserved */
    fc_bw_put(b, 0, 8);              /* unused time config */
    fc_bw_put(b, 0, 8);              /* floor 0 */
    fc_bw_put(b, 0, 8);              /* residue 0 */

    /* Mode 0: fixed block size. */
    fc_bw_put(b, 0, 6);
    fc_bw_put(b, 0, 1);              /* blockflag = short/fixed block */
    fc_bw_put(b, 0, 16);             /* window type */
    fc_bw_put(b, 0, 16);             /* transform type */
    fc_bw_put(b, 0, 8);              /* mapping 0 */
    fc_bw_put(b, 1, 8);              /* setup framing */
}

static void fc_write_audio(fc_bits *b, int mult, int present,
                           const int endpoint[2], const int raw[FC_INTERIOR])
{
    int w = fc_y_bits(mult);
    int i;
    fc_bw_put(b, 0, 1);              /* audio packet type */
    fc_bw_put(b, present ? 1u : 0u, 1);
    if (!present) return;

    fc_bw_put(b, (uint32_t)endpoint[0], (unsigned)w);
    fc_bw_put(b, (uint32_t)endpoint[1], (unsigned)w);
    for (i = 0; i < FC_INTERIOR; ++i)
        fc_symbol7(b, (unsigned)raw[i]);

    /* One residue class symbol zero, then the one CB1 codeword zero. */
    fc_bw_put(b, 0, 7);
    fc_bw_put(b, 0, 1);
}

typedef struct {
    unsigned char bytes[16384];
    size_t size;
} fc_stream;

static int fc_build_stream(fc_stream *s, const fc_stream_cfg *cfg)
{
    static unsigned char pkt[4096];
    unsigned char page[8192];
    fc_bits b;
    size_t len, at = 0, page_len;
    uint32_t serial = 0x07;
    int j;

    memset(s, 0, sizeof(*s));

    fc_bw_init(&b, pkt, sizeof pkt);
    fc_write_id(&b, cfg->block_log);
    fc_bw_bytes(&b, NULL, &len);
    page_len = fc_ogg_page(page, sizeof page, pkt, len, 0, 0x02, 0, serial);
    if (!page_len || at + page_len > sizeof s->bytes) return 0;
    memcpy(s->bytes + at, page, page_len); at += page_len;

    fc_bw_init(&b, pkt, sizeof pkt);
    fc_write_comment(&b);
    fc_bw_bytes(&b, NULL, &len);
    page_len = fc_ogg_page(page, sizeof page, pkt, len, 1, 0, 0, serial);
    if (!page_len || at + page_len > sizeof s->bytes) return 0;
    memcpy(s->bytes + at, page, page_len); at += page_len;

    fc_bw_init(&b, pkt, sizeof pkt);
    fc_write_setup(&b, cfg->mult);
    fc_bw_bytes(&b, NULL, &len);
    if (b.overflow) return 0;
    page_len = fc_ogg_page(page, sizeof page, pkt, len, 2, 0, 0, serial);
    if (!page_len || at + page_len > sizeof s->bytes) return 0;
    memcpy(s->bytes + at, page, page_len); at += page_len;

    for (j = 0; j < cfg->audio_count; ++j) {
        int ep_default[2] = {48, 18};
        const int *ep = cfg->endpoint ? cfg->endpoint[j] : ep_default;
        fc_bw_init(&b, pkt, sizeof pkt);
        fc_write_audio(&b, cfg->mult, cfg->present[j], ep, cfg->raw[j]);
        fc_bw_bytes(&b, NULL, &len);
        if (b.overflow) return 0;
        page_len = fc_ogg_page(page, sizeof page, pkt, len,
                               (uint32_t)(3 + j),
                               j == cfg->audio_count - 1 ? 0x04 : 0x00,
                               (uint64_t)j * (1u << (cfg->block_log - 1)),
                               serial);
        if (!page_len || at + page_len > sizeof s->bytes) return 0;
        memcpy(s->bytes + at, page, page_len); at += page_len;
    }

    s->size = at;
    return 1;
}

/* ------------------------------------------------------------------ */
/* points group                                                        */
/* ------------------------------------------------------------------ */

static int points_one_multiplier(int mult)
{
    static const int unwrap_raw[4] = {100, 100, 82, 40};
    int encoded[FC_VALUES];
    int want_y[FC_VALUES], want_active[FC_VALUES];
    int sorted[FC_VALUES];
    int present[1] = {1};
    int raw[1][FC_INTERIOR];
    int ep[1][2] = {{48, 18}};
    fc_stream_cfg cfg;
    fc_stream stream;
    stb_vorbis *z;
    int err = 0, j;
    vorb *f;

    encoded[0] = 48;
    encoded[1] = 18;
    encoded[2] = 5;
    encoded[3] = 6;
    encoded[4] = 0;
    encoded[5] = 3;
    encoded[6] = 0;
    encoded[7] = unwrap_raw[mult - 1];
    for (j = 0; j < FC_INTERIOR; ++j) raw[0][j] = encoded[j + 2];

    fc_recover(mult, encoded, want_y, want_active);
    /* The shipping deferred-floor state stores -1 for inactive points. */
    for (j = 0; j < FC_VALUES; ++j)
        if (!want_active[j]) want_y[j] = -1;

    memset(&cfg, 0, sizeof cfg);
    cfg.mult = mult;
    cfg.block_log = 8;
    cfg.audio_count = 1;
    cfg.present = present;
    cfg.endpoint = ep;
    cfg.raw = raw;
    TASK_CHECK(fc_build_stream(&stream, &cfg));

    z = stb_vorbis_open_memory(stream.bytes, (int)stream.size, &err, NULL);
    if (!z) fprintf(stderr, "points mult=%d open_error=%d size=%zu\n", mult, err, stream.size);
    TASK_CHECK(z != NULL);
    f = (vorb *)z;

    TASK_CHECK(f->floor_count == 1);
    TASK_CHECK((int)f->floor_config[0].floor1.values == FC_VALUES);
    TASK_CHECK((int)f->floor_config[0].floor1.floor1_multiplier == mult);
    TASK_CHECK((int)f->floor_config[0].floor1.rangebits == 6);

    for (j = 0; j < FC_VALUES; ++j)
        TASK_CHECK((int)f->floor_config[0].floor1.Xlist[j] == fc_x[j]);

    fc_sorted_ids(sorted);
    for (j = 0; j < FC_VALUES; ++j)
        TASK_CHECK((int)f->floor_config[0].floor1.sorted_order[j] == sorted[j]);

    for (j = 2; j < FC_VALUES; ++j) {
        int low, high;
        fc_neighbors(j, &low, &high);
        TASK_CHECK((int)f->floor_config[0].floor1.neighbors[j][0] == low);
        TASK_CHECK((int)f->floor_config[0].floor1.neighbors[j][1] == high);
    }

    /* open_memory has already decoded and pumped the first audio packet. */
    for (j = 0; j < FC_VALUES; ++j)
        TASK_CHECK((int)f->finalY[0][j] == want_y[j]);

    /* Explicit identity split: id 4 was reactivated; id 6 remains absent. */
    TASK_CHECK(want_active[4] && f->finalY[0][4] >= 0);
    TASK_CHECK(!want_active[6] && f->finalY[0][6] == -1);

    stb_vorbis_close(z);
    return 0;
}

static int case_points_contract(void)
{
    int mult;
    for (mult = 1; mult <= 4; ++mult)
        if (points_one_multiplier(mult)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* curve group                                                         */
/* ------------------------------------------------------------------ */

#define FC_CURVE_MAX 256

typedef struct {
    int x0, y0, x1, y1;
} fc_seg;

/*
 * Independent integer line state.  Returns the Y used at integer frequency x.
 * The first point is included; x1 itself belongs to the following segment.
 */
static int fc_line_y(const fc_seg *s, int x)
{
    int dy = s->y1 - s->y0;
    int adx = s->x1 - s->x0;
    int ady = abs(dy);
    int base = dy / adx;                 /* C99 truncation toward zero */
    int sy = dy < 0 ? base - 1 : base + 1;
    int rem = ady - abs(base) * adx;
    int xx = s->x0, y = s->y0, err = 0;
    if (x == s->x0) return y;
    for (xx = s->x0 + 1; xx <= x; ++xx) {
        err += rem;
        if (err >= adx) {               /* spec threshold is inclusive */
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
    }
    return y;
}

static int fc_find_db_index(float value)
{
    int i;
    for (i = 0; i < 256; ++i)
        if (inverse_db_table[i] == value) return i;
    return -1;
}

static int curve_one_layout(int n2, int npoints, const int *xs,
                            const int *ys, unsigned seed)
{
    Floor floors[1];
    Mapping map;
    MappingChannel chan;
    vorb f;
    short final_y[FC_VALUES];
    float unit_storage[FC_CURVE_MAX + 2];
    float prod_storage[FC_CURVE_MAX + 2];
    float *unit = unit_storage + 1;
    float *prod = prod_storage + 1;
    int order[FC_VALUES];
    int expected_y[FC_CURVE_MAX];
    int k, q, last, rc = 0;

    memset(&f, 0, sizeof f);
    memset(floors, 0, sizeof floors);
    memset(&map, 0, sizeof map);
    memset(&chan, 0, sizeof chan);
    memset(order, 0, sizeof order);

    f.floor_config = floors;
    f.floor_types[0] = 1;
    map.chan = &chan;
    chan.mux = 0;
    map.submap_floor[0] = 0;

    floors[0].floor1.values = (uint8)npoints;
    floors[0].floor1.floor1_multiplier = 1;
    for (k = 0; k < npoints; ++k) {
        floors[0].floor1.Xlist[k] = (uint16)xs[k];
        final_y[k] = (short)ys[k];
    }

    /* Independent insertion sort by X for do_floor's traversal. */
    for (q = 0; q < npoints; ++q) order[q] = q;
    for (q = 1; q < npoints; ++q) {
        int key = order[q], p = q - 1;
        while (p >= 0 && xs[order[p]] > xs[key]) {
            order[p + 1] = order[p];
            --p;
        }
        order[p + 1] = key;
    }
    for (q = 0; q < npoints; ++q)
        floors[0].floor1.sorted_order[q] = (uint8)order[q];

    for (k = 0; k < n2; ++k) expected_y[k] = 0;

    /* Complete gain curve over sorted active segments. */
    q = 0;
    while (q < npoints && xs[order[q]] <= 0) ++q;
    /* First sorted point is X=0 in all layouts below. */
    last = order[0];
    for (q = 1; q < npoints; ++q) {
        int id = order[q];
        int xa = xs[last], xb = xs[id];
        fc_seg seg;
        if (xb <= xa) continue;
        seg.x0 = xa; seg.y0 = ys[last]; seg.x1 = xb; seg.y1 = ys[id];
        for (k = xa; k < xb && k < n2; ++k)
            expected_y[k] = fc_line_y(&seg, k);
        last = id;
    }
    for (k = xs[last]; k < n2; ++k) expected_y[k] = ys[last];

    /* Run 1: unit residue makes the exact dB table index observable. */
    unit_storage[0] = -1234.5f;
    unit_storage[n2 + 1] = -6789.0f;
    for (k = 0; k < n2; ++k) unit[k] = 1.0f;
    TASK_CHECK(do_floor(&f, &map, 0, 2 * n2, unit, final_y, NULL));
    TASK_CHECK(unit_storage[0] == -1234.5f);
    TASK_CHECK(unit_storage[n2 + 1] == -6789.0f);
    for (k = 0; k < n2; ++k) {
        int idx = fc_find_db_index(unit[k]);
        TASK_CHECK(idx >= 0);
        TASK_CHECK(idx == (expected_y[k] & 255));
        TASK_CHECK(unit[k] == inverse_db_table[expected_y[k] & 255]);
    }

    /* Run 2: nonconstant, nonzero residue multiplied in place by do_floor. */
    prod_storage[0] = 4321.25f;
    prod_storage[n2 + 1] = -9876.5f;
    for (k = 0; k < n2; ++k) {
        int v = 1 + (int)((seed + (unsigned)k * 17u) % 5u);
        prod[k] = (k & 1) ? -(0.75f + 0.25f * (float)v)
                          :  (0.75f + 0.25f * (float)v);
        TASK_CHECK(prod[k] != 0.0f);
    }
    TASK_CHECK(do_floor(&f, &map, 0, 2 * n2, prod, final_y, NULL));
    TASK_CHECK(prod_storage[0] == 4321.25f);
    TASK_CHECK(prod_storage[n2 + 1] == -9876.5f);
    for (k = 0; k < n2; ++k) {
        float residue = (k & 1) ? -(0.75f + 0.25f *
            (float)(1 + (int)((seed + (unsigned)k * 17u) % 5u)))
                                 :  (0.75f + 0.25f *
            (float)(1 + (int)((seed + (unsigned)k * 17u) % 5u)));
        double expected = (double)residue *
                          (double)inverse_db_table[expected_y[k] & 255];
        double tol = 2e-6 * (1.0 + fabs(expected));
        double diff = fabs((double)prod[k] - expected);
        int ok = diff <= tol;
        if (!ok)
            fprintf(stderr, "curve product mismatch n2=%d k=%d got=%.9g want=%.9g diff=%.3g\n",
                    n2, k, prod[k], expected, diff);
        TASK_CHECK(ok);
    }
    TASK_CHECK(rc == 0);
    return 0;
}

static int case_curve_contract(void)
{
    /* Hand-built edge segments: zero quotient, signs and exact threshold. */
    {
        const int xs[2] = {0, 4};
        const int ys[2] = {100, 102};
        int want[5] = {100, 100, 101, 101, 102};
        fc_seg s = {0, 100, 4, 102};
        int x;
        for (x = 0; x <= 4; ++x) TASK_CHECK(fc_line_y(&s, x) == want[x]);
        if (curve_one_layout(16, 2, xs, ys, 11)) return 1;
    }
    {
        const int xs[2] = {0, 3};
        const int ys[2] = {200, 195};
        int want[3] = {200, 199, 197};
        fc_seg s = {0, 200, 3, 195};
        int x;
        for (x = 0; x < 3; ++x) TASK_CHECK(fc_line_y(&s, x) == want[x]);
        if (curve_one_layout(32, 2, xs, ys, 23)) return 1;
    }
    {
        const int xs[2] = {0, 3};
        const int ys[2] = {100, 107};
        int want[3] = {100, 102, 104};
        fc_seg s = {0, 100, 3, 107};
        int x;
        for (x = 0; x < 3; ++x) TASK_CHECK(fc_line_y(&s, x) == want[x]);
        if (curve_one_layout(64, 2, xs, ys, 37)) return 1;
    }
    {
        /* Negative exact threshold at x=2: >= steps, a strict > would not. */
        const int xs[2] = {0, 4};
        const int ys[2] = {160, 158};
        int want[4] = {160, 160, 159, 159};
        fc_seg s = {0, 160, 4, 158};
        int x;
        for (x = 0; x < 4; ++x) TASK_CHECK(fc_line_y(&s, x) == want[x]);
        if (curve_one_layout(32, 2, xs, ys, 41)) return 1;
    }

    /*
     * A dependent active chain.  X=4 is an interior endpoint handled by the
     * next segment; X=40 lies beyond n2=32 and must be clipped.
     */
    {
        const int xs[5] = {0, 40, 4, 12, 24};
        const int ys[5] = {210, 170, 205, 190, 180};
        if (curve_one_layout(32, 5, xs, ys, 53)) return 1;
        if (curve_one_layout(64, 5, xs, ys, 67)) return 1;
        if (curve_one_layout(128, 5, xs, ys, 71)) return 1;
    }

    /* Deterministic varied layouts, including points past the half block. */
    {
        unsigned rng = 0x1234abcdU;
        int it;
        for (it = 0; it < 24; ++it) {
            int n2 = 16 + (int)(rng % 113);
            int np = 2 + (int)(rng >> 3) % 5;
            int xs[8], ys[8];
            int j;
            rng = rng * 1664525U + 1013904223U;
            xs[0] = 0;
            xs[1] = 4 + (int)(rng % (unsigned)(n2 + 18));
            if (xs[1] <= 0) xs[1] = 1;
            for (j = 2; j < np; ++j) {
                rng = rng * 1664525U + 1013904223U;
                xs[j] = (int)(rng % (unsigned)(n2 + 24));
                rng = rng * 1664525U + 1013904223U;
                ys[j] = 20 + (int)(rng % 211);
            }
            ys[0] = 30 + (int)(rng % 200);
            rng = rng * 1664525U + 1013904223U;
            ys[1] = 30 + (int)(rng % 200);
            /* Reject duplicate X; duplicate setup points are not legal. */
            {
                int unique = 1, a, b;
                for (a = 0; a < np; ++a)
                    for (b = a + 1; b < np; ++b)
                        if (xs[a] == xs[b]) unique = 0;
                if (!unique) continue;
            }
            if (curve_one_layout(n2, np, xs, ys, rng)) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* packets group                                                       */
/* ------------------------------------------------------------------ */

static void packet_window(int n2, double w[256])
{
    int i;
    for (i = 0; i < n2; ++i) {
        double inner = ((double)i + 0.5) / (double)n2 * 0.5 * FC_PI;
        float s = (float)sin(inner);
        float sq = s * s;
        w[i] = sin(0.5 * FC_PI * (double)sq);
    }
}

/* Direct unnormalized cosine IMDCT identified from the Vorbis transform. */
static void packet_imdct(const float spec[256], int n, double raw[512])
{
    int n2 = n / 2, i, k;
    for (i = 0; i < n; ++i) {
        double sum = 0.0;
        double A = (double)i + (double)n / 4.0 + 0.5;
        for (k = 0; k < n2; ++k) {
            double angle = 2.0 * FC_PI / (double)n * A * ((double)k + 0.5);
            sum += (double)spec[k] * cos(angle);
        }
        raw[i] = sum;
    }
}

static void packet_spectrum(const int endpoint[2],
                            const int rawvals[FC_INTERIOR],
                            float spec[256])
{
    int encoded[FC_VALUES];
    int y[FC_VALUES], active[FC_VALUES], sorted[FC_VALUES];
    int gain_y[256];
    int k, q;
    for (k = 0; k < 256; ++k) { spec[k] = 0; gain_y[k] = 0; }
    encoded[0] = endpoint[0];
    encoded[1] = endpoint[1];
    for (k = 0; k < FC_INTERIOR; ++k) encoded[k + 2] = rawvals[k];
    fc_recover(1, encoded, y, active);
    fc_sorted_ids(sorted);

    /* Build integer gain Y only for active sorted points. */
    {
        int last = -1;
        for (q = 0; q < FC_VALUES; ++q) {
            int id = sorted[q];
            if (!active[id]) continue;
            if (last < 0) {
                last = id;
                for (k = 0; k < fc_x[id] && k < 256; ++k) gain_y[k] = y[id];
            } else {
                fc_seg seg;
                int xa = fc_x[last], xb = fc_x[id];
                seg.x0 = xa; seg.y0 = y[last]; seg.x1 = xb; seg.y1 = y[id];
                for (k = xa; k < xb && k < 256; ++k)
                    gain_y[k] = fc_line_y(&seg, k);
                last = id;
            }
        }
        if (last >= 0)
            for (k = fc_x[last]; k < 256; ++k) gain_y[k] = y[last];
    }

    for (k = 0; k < 4; ++k)
        spec[k] = (float)fc_residue_values[k] *
                  inverse_db_table[gain_y[k] & 255];
}

static int packets_one_block(int block_log)
{
    int n = 1 << block_log, n2 = n / 2;
    static const int present[4] = {1, 0, 1, 1};
    static const int ep[4][2] = {
        {210, 150}, {0, 0}, {210, 150}, {210, 150}
    };
    static const int rawv[4][FC_INTERIOR] = {
        {5, 6, 0, 3, 0, 11},
        {0, 0, 0, 0, 0, 0},
        {5, 6, 0, 3, 0, 11},
        {5, 6, 0, 3, 0, 11}
    };
    fc_stream_cfg cfg;
    fc_stream stream;
    stb_vorbis *z;
    vorb *vf;
    stb_vorbis_info info;
    int err = 0, frame;
    double w[256];
    float spectrum[4][256];
    double raw[4][512];
    double saved_tail[256];

    memset(&cfg, 0, sizeof cfg);
    cfg.mult = 1;
    cfg.block_log = block_log;
    cfg.audio_count = 4;
    cfg.present = present;
    cfg.endpoint = ep;
    cfg.raw = rawv;
    TASK_CHECK(fc_build_stream(&stream, &cfg));

    z = stb_vorbis_open_memory(stream.bytes, (int)stream.size, &err, NULL);
    if (!z) fprintf(stderr, "packets block=%d open_error=%d size=%zu\n", n, err, stream.size);
    TASK_CHECK(z != NULL);
    vf = (vorb *)z;
    info = stb_vorbis_get_info(z);
    TASK_CHECK(info.channels == 1);
    TASK_CHECK(info.sample_rate == 8000u);
    TASK_CHECK(vf->blocksize[0] == n);
    TASK_CHECK(vf->blocksize[1] == n);
    TASK_CHECK((int)vf->floor_config[0].floor1.values == FC_VALUES);
    TASK_CHECK((int)vf->floor_config[0].floor1.Xlist[3] == 16);
    TASK_CHECK(fc_residue_values[0] != 0 && fc_residue_values[1] != 0 &&
               fc_residue_values[2] != 0 && fc_residue_values[3] != 0);

    packet_window(n2, w);
    for (frame = 0; frame < 4; ++frame) {
        int k;
        if (present[frame])
            packet_spectrum(ep[frame], rawv[frame], spectrum[frame]);
        else
            memset(spectrum[frame], 0, sizeof spectrum[frame]);
        packet_imdct(spectrum[frame], n, raw[frame]);
        for (k = 0; k < n2; ++k)
            saved_tail[k] = raw[frame][n2 + k]; /* updated after each output */
    }

    /*
     * open_memory pumps packet 0 as preroll.  Its saved second half is the
     * first previous_window used by the first returned frame.
     */
    memcpy(saved_tail, raw[0] + n2, n2 * sizeof saved_tail[0]);

    for (frame = 1; frame < 4; ++frame) {
        float **pcm = NULL;
        int chans = 99, got, k;
        got = stb_vorbis_get_frame_float(z, &chans, &pcm);
        TASK_CHECK(chans == 1);
        TASK_CHECK(got == n2);
        TASK_CHECK(pcm != NULL && pcm[0] != NULL);
        for (k = 0; k < n2; ++k) {
            double expected = raw[frame][k] * w[k] +
                              saved_tail[k] * w[n2 - 1 - k];
            float actual = pcm[0][k];
            double tol = 2e-5 * (1.0 + fabs(expected));
            double diff = fabs((double)actual - expected);
            int ok = diff <= tol;
            if (!ok)
                fprintf(stderr,
                        "pcm mismatch block=%d output_frame=%d sample=%d "
                        "got=%.9g want=%.9g diff=%.3g tol=%.3g\n",
                        n, frame - 1, k, (double)actual, expected,
                        diff, tol);
            TASK_CHECK(ok);
        }
        if (!present[frame]) {
            double overlap_energy = 0.0;
            for (k = 0; k < n2; ++k) {
                /* The missing packet's own transform tail is zero. */
                TASK_CHECK(vf->channel_buffers[0][n2 + k] == 0.0f);
                overlap_energy += (double)pcm[0][k] * (double)pcm[0][k];
            }
            /* The returned frame can still be preroll overlap from packet 0. */
            TASK_CHECK(overlap_energy > 1.0e-12);
        }
        /* After mixing, packet frame's second half is saved for next call. */
        memcpy(saved_tail, raw[frame] + n2, n2 * sizeof saved_tail[0]);
    }

    stb_vorbis_close(z);
    return 0;
}

static int case_packets_contract(void)
{
    if (packets_one_block(8)) return 1;
    if (packets_one_block(9)) return 1;
    return 0;
}

/*
 * Why these cases are shaped this way:
 *
 * The provided flat two-endpoint stream only proves Ogg framing plus a
 * nonzero decode.  It never exercises declaration-order neighbors, symmetric
 * difference unwrapping, step-2 reactivation, the inclusive Bresenham
 * threshold, or overlap of a floor-absent packet.  PCM energy alone also
 * cannot identify which frequency bin or control point moved.
 *
 * The first localized divergences for the two seeded production mutations are:
 *   - wrong descending prediction sign: multiplier 1, control id 2 (X=40),
 *     expected recovered Y 27; the mutated path stores 63;
 *   - strict line threshold: segment (X,Y)=(0,100)->(4,102), frequency bin 2,
 *     where the inclusive remainder rule gives Y=101 but "err > adx" keeps 100.
 */
const struct task_case task_cases[] = {
    {"points", "floor1_point_recovery", case_points_contract},
    {"curve", "integer_lines_and_dofloor", case_curve_contract},
    {"packets", "ogg_cosine_overlap", case_packets_contract},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
