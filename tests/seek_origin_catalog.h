// seek_origin_catalog.h
//
// Independent physical-block catalog for the seek-origin regression.  Every
// value here is derived by walking the raw WavPack bytes ourselves; the
// library's GET_BLOCK_INDEX / GET_TOTAL_SAMPLES macros and any decoder output
// are deliberately never used as a source of truth.
//
// A WavPack block header is 32 little-endian bytes:
//
//   0..3   ckID "wvpk"
//   4..7   ckSize (payload following the 8-byte chunk preamble)
//   8..9   version (int16)
//   10     block_index high 8 bits  (40-bit block index, bits 32..39)
//   11     total_samples high 8 bits
//   12..15 total_samples low 32 bits (all ones == unknown)
//   16..19 block_index low 32 bits
//   20..23 block_samples
//   24..27 flags
//   28..31 crc
//
// A physical block occupies ckSize + 8 bytes.  Multichannel segments are
// groups of consecutive blocks sharing one block_index; only the first block
// of a segment carries INITIAL_BLOCK (0x800) and only the last carries
// FINAL_BLOCK (0x1000).

#ifndef SEEK_ORIGIN_CATALOG_H
#define SEEK_ORIGIN_CATALOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PV_FLAG_INITIAL 0x800u
#define PV_FLAG_FINAL   0x1000u
#define PV_LANE_WV      0
#define PV_LANE_WVC     1

struct pv_block {
    size_t   offset;              // byte position of the "wvpk" magic
    size_t   length;              // full physical block length (ckSize + 8)
    int      lane;                // PV_LANE_WV or PV_LANE_WVC
    int      group;               // ordinal of the multichannel segment
    uint32_t flags;
    uint32_t block_samples;
    int64_t  raw_index;           // 40-bit block index exactly as stored
    int64_t  total_stored;        // 40-bit total, or -1 when unknown
    int64_t  rel_start;           // playback-relative first frame
    int64_t  rel_end;             // playback-relative one-past-last frame
};

struct pv_catalog {
    struct pv_block *items;
    size_t           count;
    int              lane;
    int64_t          origin;      // raw index of the first initial block
    int64_t          total_frames;
};

static inline uint32_t pv_rd32le(const unsigned char *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// 40-bit block index, assembled strictly from the documented byte positions.
static inline int64_t pv_rd_block_index(const unsigned char *p)
{
    return (int64_t) pv_rd32le(p + 16) + ((int64_t) p[10] << 32);
}

// 40-bit total samples; 0xffffffff in the low 32 bits means "unknown".
static inline int64_t pv_rd_total_samples(const unsigned char *p)
{
    uint32_t lo = pv_rd32le(p + 12);

    if (lo == 0xffffffffu)
        return -1;

    // mirrors the on-disk bias of the high byte without using the macro
    return (int64_t) lo + ((int64_t) p[11] << 32) - (int64_t) p[11];
}

// Walk every physical block of one lane's raw bytes and build a catalog.
// Returns 1 on success, 0 on a malformed stream.
static inline int pv_catalog_build(struct pv_catalog *cat, int lane,
                                   const unsigned char *data, size_t size)
{
    size_t pos = 0;
    int group = -1;

    memset(cat, 0, sizeof(*cat));
    cat->lane = lane;
    cat->origin = -1;
    cat->total_frames = -1;

    while (pos + 32 <= size) {
        const unsigned char *blk = data + pos;
        uint32_t ckSize, flags, samples;
        size_t blen;
        struct pv_block *grown;

        if (memcmp(blk, "wvpk", 4))
            return 0;

        ckSize = pv_rd32le(blk + 4);

        if (ckSize < 24 || (ckSize & 1u) || (size_t) ckSize + 8 > size - pos)
            return 0;

        blen = (size_t) ckSize + 8;
        flags = pv_rd32le(blk + 24);
        samples = pv_rd32le(blk + 20);

        if (flags & PV_FLAG_INITIAL)
            group++;

        grown = (struct pv_block *) realloc(cat->items,
                    (cat->count + 1) * sizeof(cat->items[0]));
        if (!grown)
            return 0;
        cat->items = grown;

        struct pv_block *rec = &cat->items[cat->count++];
        rec->offset = pos;
        rec->length = blen;
        rec->lane = lane;
        rec->group = group < 0 ? 0 : group;
        rec->flags = flags;
        rec->block_samples = samples;
        rec->raw_index = pv_rd_block_index(blk);
        rec->total_stored = pv_rd_total_samples(blk);

        if ((flags & PV_FLAG_INITIAL) && cat->origin < 0)
            cat->origin = rec->raw_index;

        pos += blen;
    }

    if (pos != size || !cat->count || cat->origin < 0)
        return 0;

    // Relative playback coordinates are derived from the catalog's own origin.
    for (size_t i = 0; i < cat->count; ++i) {
        cat->items[i].rel_start = cat->items[i].raw_index - cat->origin;
        cat->items[i].rel_end = cat->items[i].rel_start +
                                (int64_t) cat->items[i].block_samples;
    }

    // Total frame count: sum of block_samples of segment-leading blocks,
    // independently of any header total field.
    cat->total_frames = 0;
    for (size_t i = 0; i < cat->count; ++i)
        if (cat->items[i].flags & PV_FLAG_INITIAL)
            cat->total_frames += cat->items[i].block_samples;

    return 1;
}

static inline void pv_catalog_free(struct pv_catalog *cat)
{
    free(cat->items);
    memset(cat, 0, sizeof(*cat));
}

// Return the segment-leading (INITIAL) physical block whose relative frame
// interval contains target, or NULL.  This is the block a correct seek must
// load first for that lane.
static inline const struct pv_block *
pv_catalog_find_initial(const struct pv_catalog *cat, int64_t target)
{
    for (size_t i = 0; i < cat->count; ++i) {
        const struct pv_block *b = &cat->items[i];

        if ((b->flags & PV_FLAG_INITIAL) &&
            target >= b->rel_start && target < b->rel_end)
            return b;
    }

    return NULL;
}

// Memory lane with its own position ledger and a trace of every absolute
// positioning call and every physical header read performed through it.  WV
// and WVC lanes never share state.
struct pv_event {
    int64_t at;
    int     kind;       // 0 = set_pos_abs, 1 = header read
};

struct pv_lane {
    const unsigned char *data;
    size_t               size;
    size_t               pos;
    const char          *name;
    struct pv_event     *trace;
    size_t               ntrace;
};

static inline void pv_lane_init(struct pv_lane *lane, const char *name,
                                const unsigned char *data, size_t size)
{
    memset(lane, 0, sizeof(*lane));
    lane->name = name;
    lane->data = data;
    lane->size = size;
}

static inline void pv_lane_free(struct pv_lane *lane)
{
    free(lane->trace);
    memset(lane, 0, sizeof(*lane));
}

static inline void pv_lane_clear_trace(struct pv_lane *lane)
{
    lane->ntrace = 0;
}

static inline void pv_lane_record(struct pv_lane *lane, int kind, int64_t at)
{
    struct pv_event *grown = (struct pv_event *) realloc(lane->trace,
        (lane->ntrace + 1) * sizeof(lane->trace[0]));

    if (grown) {
        lane->trace = grown;
        lane->trace[lane->ntrace].at = at;
        lane->trace[lane->ntrace].kind = kind;
        lane->ntrace++;
    }
}

static inline int32_t pv_lane_read(void *id, void *out, int32_t want)
{
    struct pv_lane *lane = (struct pv_lane *) id;
    size_t n = (size_t) want;

    if (n > lane->size - lane->pos)
        n = lane->size - lane->pos;

    if (want >= 32 && lane->pos + 32 <= lane->size &&
        !memcmp(lane->data + lane->pos, "wvpk", 4))
        pv_lane_record(lane, 1, (int64_t) lane->pos);

    memcpy(out, lane->data + lane->pos, n);
    lane->pos += n;
    return (int32_t) n;
}

static inline int64_t pv_lane_get_pos(void *id)
{
    return (int64_t) ((struct pv_lane *) id)->pos;
}

static inline int pv_lane_set_abs(void *id, int64_t p)
{
    struct pv_lane *lane = (struct pv_lane *) id;

    if (p < 0 || (size_t) p > lane->size)
        return -1;

    pv_lane_record(lane, 0, p);
    lane->pos = (size_t) p;
    return 0;
}

static inline int pv_lane_set_rel(void *id, int64_t delta, int mode)
{
    struct pv_lane *lane = (struct pv_lane *) id;
    int64_t next;

    if (mode == 0)       next = delta;                /* SEEK_SET */
    else if (mode == 1)  next = (int64_t) lane->pos + delta;
    else                 next = (int64_t) lane->size + delta;

    if (next < 0 || (size_t) next > lane->size)
        return -1;

    lane->pos = (size_t) next;
    return 0;
}

static inline int pv_lane_push(void *id, int c)
{
    struct pv_lane *lane = (struct pv_lane *) id;

    if (!lane->pos)
        return -1;

    lane->pos--;
    return c;
}

static inline int64_t pv_lane_length(void *id)
{
    return (int64_t) ((struct pv_lane *) id)->size;
}

static inline int pv_lane_can_seek(void *id)
{
    (void) id;
    return 1;
}

static inline int pv_lane_write(void *id, void *data, int32_t n)
{
    (void) id; (void) data; (void) n;
    return 0;
}

static inline int pv_lane_close(void *id)
{
    (void) id;
    return 0;
}

// Last header-read event in the trace, or -1 if none happened.
static inline int64_t pv_lane_last_header_read(const struct pv_lane *lane)
{
    for (size_t i = lane->ntrace; i > 0; --i)
        if (lane->trace[i - 1].kind == 1)
            return lane->trace[i - 1].at;

    return -1;
}

#endif // SEEK_ORIGIN_CATALOG_H
