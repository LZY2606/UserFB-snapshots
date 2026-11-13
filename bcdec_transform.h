/* bcdec_transform.h - v0.1
   direct (compressed block to compressed block) rearrangements for BC7, built
   on top of bcdec.h

   These functions never decompress to RGBA and never re-compress. The result
   of a transform is still an ordinary 16 byte BC7 block: decoding it with
   bcdec_bc7() yields exactly the same 4x4 pixels as decoding the source block
   and then applying the requested pixel permutation.

   Coordinate system: the origin is the top-left pixel of the 4x4 block, x
   grows to the right and y grows downwards. Each operation maps a source
   pixel position to a destination position:

       BCDEC_IDENTITY         (x, y) -> (x, y)
       BCDEC_ROTATE_90        (x, y) -> (3 - y, x)
       BCDEC_ROTATE_180       (x, y) -> (3 - x, 3 - y)
       BCDEC_ROTATE_270       (x, y) -> (y, 3 - x)
       BCDEC_FLIP_X           (x, y) -> (3 - x, y)
       BCDEC_FLIP_Y           (x, y) -> (x, 3 - y)
       BCDEC_TRANSPOSE        (x, y) -> (y, x)
       BCDEC_ANTI_TRANSPOSE   (x, y) -> (3 - y, 3 - x)

   BCDEC_TRANSFORM_OK              destination written
   BCDEC_TRANSFORM_INVALID         NULL pointer, unknown operation, or the
                                   source block has no valid BC7 mode bits
   BCDEC_TRANSFORM_UNREPRESENTABLE the block is valid BC7 but the permuted
                                   pixel layout cannot be described by any
                                   standard partition of its mode (see README)

   On failure dst16 is never touched. src16 and dst16 may be equal (in-place
   operation is supported); any other overlap is the caller's responsibility.

   This header can be included from C and C++.

   LICENSE: see bcdec.h.
*/

#ifndef BCDEC_TRANSFORM_HEADER_INCLUDED
#define BCDEC_TRANSFORM_HEADER_INCLUDED

#include "bcdec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bcdec_transform_op {
    BCDEC_IDENTITY = 0,
    BCDEC_ROTATE_90,
    BCDEC_ROTATE_180,
    BCDEC_ROTATE_270,
    BCDEC_FLIP_X,
    BCDEC_FLIP_Y,
    BCDEC_TRANSPOSE,
    BCDEC_ANTI_TRANSPOSE
} bcdec_transform_op;

typedef enum bcdec_transform_status {
    BCDEC_TRANSFORM_OK = 0,
    BCDEC_TRANSFORM_INVALID,
    BCDEC_TRANSFORM_UNREPRESENTABLE
} bcdec_transform_status;

bcdec_transform_status bcdec_bc7_transform(const void* src16, void* dst16, bcdec_transform_op op);

#ifdef __cplusplus
}
#endif

#endif /* BCDEC_TRANSFORM_HEADER_INCLUDED */

#ifdef BCDEC_TRANSFORM_IMPLEMENTATION

#ifndef BCDEC_PARTITION_TABLE_ONLY
#define BCDEC_PARTITION_TABLE_ONLY
#define BCDEC_TR_PARTITION_TABLE_MACRO_ADDED
#endif
#include "bcdec.h"         /* compiles the shared bcdec__partition_sets table only */
#ifdef BCDEC_TR_PARTITION_TABLE_MACRO_ADDED
#undef BCDEC_TR_PARTITION_TABLE_MACRO_ADDED
#undef BCDEC_PARTITION_TABLE_ONLY
#endif

/* Mode tables, indexed by BC7 mode number. They mirror the tables used by
   bcdec_bc7() so the two pieces of code always agree on the layout. */
static const unsigned char bcdec__tr_rgba_bits[8]  = { 4, 6, 5, 7, 5, 7, 7, 5 };
static const unsigned char bcdec__tr_alpha_bits[8] = { 0, 0, 0, 0, 6, 8, 7, 5 };
static const unsigned char bcdec__tr_has_pbits[8]  = { 1, 1, 0, 1, 0, 0, 1, 1 };
static const int bcdec__tr_primary_bits[8]   = { 3, 3, 2, 2, 2, 2, 4, 2 };
static const int bcdec__tr_secondary_bits[8] = { 0, 0, 0, 0, 3, 2, 0, 0 };

static int bcdec__tr_subset_count(int mode) {
    if (mode == 0 || mode == 2) {
        return 3;
    }
    if (mode == 1 || mode == 3 || mode == 7) {
        return 2;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                   */

static void bcdec__tr_src_of(bcdec_transform_op op, int x, int y, int* sx, int* sy) {
    switch (op) {
        case BCDEC_IDENTITY:       *sx = x;     *sy = y;     break;
        case BCDEC_ROTATE_90:      *sx = y;     *sy = 3 - x; break;
        case BCDEC_ROTATE_180:     *sx = 3 - x; *sy = 3 - y; break;
        case BCDEC_ROTATE_270:     *sx = 3 - y; *sy = x;     break;
        case BCDEC_FLIP_X:         *sx = 3 - x; *sy = y;     break;
        case BCDEC_FLIP_Y:         *sx = x;     *sy = 3 - y; break;
        case BCDEC_TRANSPOSE:      *sx = y;     *sy = x;     break;
        case BCDEC_ANTI_TRANSPOSE: *sx = 3 - y; *sy = 3 - x; break;
        default:                   *sx = -1;    *sy = -1;    break;
    }
}

/* ------------------------------------------------------------------------- */
/* Endian independent bit io                                                  */

typedef struct bcdec__tr_reader {
    unsigned long long low;
    unsigned long long high;
} bcdec__tr_reader;

typedef struct bcdec__tr_writer {
    unsigned char* dst;
    int pos;
} bcdec__tr_writer;

static void bcdec__tr_reader_init(bcdec__tr_reader* r, const unsigned char* src) {
    int i;
    r->low = 0;
    r->high = 0;
    for (i = 0; i < 8; ++i) {
        r->low  |= (unsigned long long)src[i] << (i * 8);
        r->high |= (unsigned long long)src[i + 8] << (i * 8);
    }
}

static int bcdec__tr_read(bcdec__tr_reader* r, int count) {
    unsigned long long mask = (1ull << count) - 1ull;
    unsigned long long value = r->low & mask;
    r->low >>= count;
    r->low |= (r->high & mask) << (64 - count);
    r->high >>= count;
    return (int)value;
}

static void bcdec__tr_writer_init(bcdec__tr_writer* w, unsigned char* dst) {
    int i;
    for (i = 0; i < 16; ++i) {
        dst[i] = 0;
    }
    w->dst = dst;
    w->pos = 0;
}

static void bcdec__tr_write(bcdec__tr_writer* w, unsigned value, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        if ((value >> i) & 1u) {
            w->dst[w->pos >> 3] = (unsigned char)(w->dst[w->pos >> 3] | (1u << (w->pos & 7)));
        }
        ++w->pos;
    }
}

/* ------------------------------------------------------------------------- */
/* Subset layout matching                                                     */

/* Number of standard partitions a mode may use. Mode 0 stores its partition
   index in 4 bits, so only partitions 0..15 exist for it. */
static int bcdec__tr_partition_limit(int mode) {
    return (mode == 0) ? 16 : 64;
}

/* Does the permuted source subset labelling equal the labelling of `partition`?
   On success out_map[o] = source subset feeding output subset o. The map has to
   be a bijection (each output subset takes exactly one source subset and every
   source subset is used once), which is what allows subsets to be renamed. */
static int bcdec__tr_match_partition(int subsets, int partition, const int* src_label,
                                     bcdec_transform_op op, int* out_map) {
    const unsigned char(*table)[4] = bcdec__partition_sets[subsets - 2][partition];
    int map[3] = { -1, -1, -1 };
    int used[3] = { 0, 0, 0 };
    int x, y, t, s;

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int sx, sy;
            bcdec__tr_src_of(op, x, y, &sx, &sy);
            t = table[y][x] & 0x03;
            s = src_label[sy * 4 + sx];
            if (t >= subsets || s >= subsets) {
                return 0;
            }
            if (map[t] < 0) {
                if (used[s]) {
                    return 0;
                }
                map[t] = s;
                used[s] = 1;
            } else if (map[t] != s) {
                return 0;
            }
        }
    }

    for (t = 0; t < subsets; ++t) {
        if (map[t] < 0) {
            return 0;
        }
    }

    out_map[0] = map[0];
    out_map[1] = map[1];
    out_map[2] = map[2];
    return 1;
}

/* Searches the partitions the mode allows in ascending numeric order and
   returns the first one whose labelling matches. For a fixed partition the
   output-to-source subset map is forced (each output pixel fixes which source
   subset its output subset must take) and, being a bijection, is unique; it is
   therefore also the lexicographically smallest map for that partition. Scanning
   partitions in order thus implements the documented tie-break exactly: lowest
   partition number first, then the smallest output-subset-to-source-subset map. */
static int bcdec__tr_find_layout(int mode, const int* src_label, bcdec_transform_op op,
                                 int* subset_map) {
    int subsets = bcdec__tr_subset_count(mode);
    int limit = bcdec__tr_partition_limit(mode);
    int partition;

    for (partition = 0; partition < limit; ++partition) {
        int map[3];
        if (bcdec__tr_match_partition(subsets, partition, src_label, op, map)) {
            subset_map[0] = map[0];
            subset_map[1] = map[1];
            subset_map[2] = map[2];
            return partition;
        }
    }
    return -1;
}

/* Output subset label of pixel (x, y) for a candidate partition. */
static int bcdec__tr_out_label(int subsets, int partition, int x, int y) {
    return bcdec__partition_sets[subsets - 2][partition][y][x] & 0x03;
}

/* Is (x, y) the fix-up anchor of its subset in that partition? */
static int bcdec__tr_out_is_anchor(int subsets, int partition, int x, int y) {
    return (bcdec__partition_sets[subsets - 2][partition][y][x] & 0x80) != 0;
}

/* ------------------------------------------------------------------------- */
/* Multi subset modes (0, 1, 2, 3, 7)                                         */

/* The fix-up anchor selector of every subset is stored with one bit less, so
   the value that lands on the output anchor must fit in that narrower field.
   When it does not, the subset's endpoint pair is swapped and all of its selectors
   are complemented: with the standard weight tables (which are symmetric,
   w[i] + w[N-1-i] == 64) this decodes to exactly the same pixels, and it moves
   the anchor's value into the low half. Endpoint order, p-bits and the selector
   interpretation of the subset always travel together. */
static bcdec_transform_status bcdec__tr_multi_subset(const unsigned char* src, unsigned char* dst,
                                                     int mode, bcdec_transform_op op) {
    bcdec__tr_reader r;
    bcdec__tr_writer w;
    int subsets = bcdec__tr_subset_count(mode);
    int endpoints = subsets * 2;
    int partition_bits = (mode == 0) ? 4 : 6;
    int index_bits = bcdec__tr_primary_bits[mode];
    int rgb[8][3];
    int alpha[8];
    int pbit[8];
    int subset_pbit[3];
    int src_label[16];
    int selector[16];
    int subset_map[3];
    int flip[3];
    int partition, out_partition;
    int i, x, y, j, subset;
    int have_alpha = (bcdec__tr_alpha_bits[mode] > 0);

    for (i = 0; i < 8; ++i) {
        alpha[i] = 0;
        pbit[i] = 0;
    }
    for (i = 0; i < 3; ++i) {
        subset_pbit[i] = 0;
        flip[i] = 0;
    }

    bcdec__tr_reader_init(&r, src);
    /* mode unary prefix */
    for (i = 0; i < mode + 1; ++i) {
        (void)bcdec__tr_read(&r, 1);
    }
    partition = bcdec__tr_read(&r, partition_bits);

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            src_label[y * 4 + x] = bcdec__partition_sets[subsets - 2][partition][y][x] & 0x03;
        }
    }

    for (i = 0; i < 3; ++i) {
        for (j = 0; j < endpoints; ++j) {
            rgb[j][i] = bcdec__tr_read(&r, bcdec__tr_rgba_bits[mode]);
        }
    }
    if (have_alpha) {
        for (j = 0; j < endpoints; ++j) {
            alpha[j] = bcdec__tr_read(&r, bcdec__tr_alpha_bits[mode]);
        }
    }
    if (mode == 1) {
        for (subset = 0; subset < subsets; ++subset) {
            subset_pbit[subset] = bcdec__tr_read(&r, 1);
        }
    } else if (bcdec__tr_has_pbits[mode]) {
        for (j = 0; j < endpoints; ++j) {
            pbit[j] = bcdec__tr_read(&r, 1);
        }
    }

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int bits = index_bits - (bcdec__tr_out_is_anchor(subsets, partition, x, y) ? 1 : 0);
            selector[y * 4 + x] = bcdec__tr_read(&r, bits);
        }
    }

    out_partition = bcdec__tr_find_layout(mode, src_label, op, subset_map);
    if (out_partition < 0) {
        return BCDEC_TRANSFORM_UNREPRESENTABLE;
    }

    /* Decide per output subset whether the arriving anchor needs its MSB cleared. */
    for (subset = 0; subset < subsets; ++subset) {
        int ax = 0, ay = 0, found = 0;
        for (y = 0; y < 4 && !found; ++y) {
            for (x = 0; x < 4 && !found; ++x) {
                if (bcdec__tr_out_is_anchor(subsets, out_partition, x, y)
                    && bcdec__tr_out_label(subsets, out_partition, x, y) == subset) {
                    ax = x;
                    ay = y;
                    found = 1;
                }
            }
        }
        {
            int sx, sy, value;
            bcdec__tr_src_of(op, ax, ay, &sx, &sy);
            value = selector[sy * 4 + sx];
            flip[subset] = (value >= (1 << (index_bits - 1)));
        }
    }

    bcdec__tr_writer_init(&w, dst);
    bcdec__tr_write(&w, 1u << mode, mode + 1);
    bcdec__tr_write(&w, (unsigned)out_partition, partition_bits);

    for (i = 0; i < 3; ++i) {
        for (j = 0; j < endpoints; ++j) {
            int src_endpoint = 2 * subset_map[j / 2] + ((j & 1) ^ flip[j / 2]);
            bcdec__tr_write(&w, (unsigned)rgb[src_endpoint][i], bcdec__tr_rgba_bits[mode]);
        }
    }
    if (have_alpha) {
        for (j = 0; j < endpoints; ++j) {
            int src_endpoint = 2 * subset_map[j / 2] + ((j & 1) ^ flip[j / 2]);
            bcdec__tr_write(&w, (unsigned)alpha[src_endpoint], bcdec__tr_alpha_bits[mode]);
        }
    }
    if (mode == 1) {
        for (subset = 0; subset < subsets; ++subset) {
            bcdec__tr_write(&w, (unsigned)subset_pbit[subset_map[subset]], 1);
        }
    } else if (bcdec__tr_has_pbits[mode]) {
        for (j = 0; j < endpoints; ++j) {
            int src_endpoint = 2 * subset_map[j / 2] + ((j & 1) ^ flip[j / 2]);
            bcdec__tr_write(&w, (unsigned)pbit[src_endpoint], 1);
        }
    }

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int sx, sy, value, label, bits;
            bcdec__tr_src_of(op, x, y, &sx, &sy);
            label = bcdec__tr_out_label(subsets, out_partition, x, y);
            value = selector[sy * 4 + sx];
            if (flip[label]) {
                value = ((1 << index_bits) - 1) - value;
            }
            bits = index_bits - (bcdec__tr_out_is_anchor(subsets, out_partition, x, y) ? 1 : 0);
            if (value >= (1 << bits)) {
                return BCDEC_TRANSFORM_UNREPRESENTABLE;
            }
            bcdec__tr_write(&w, (unsigned)value, bits);
        }
    }

    return BCDEC_TRANSFORM_OK;
}

/* ------------------------------------------------------------------------- */
/* Single subset modes (4, 5, 6)                                              */

/* Single subset blocks have no partition, every pixel uses the same endpoint
   pair. The anchor is always (0, 0). The source pixel that lands on the output
   anchor may be any of the 16 pixels, and its selector must fit the truncated
   anchor field; if it does not, the relevant endpoint pair is swapped and that
   selector stream is complemented (same symmetry argument as above). Mode 4/5
   carry two independent selector streams, so color and alpha are handled
   separately and never cross; mode 6 has one stream driving RGB and alpha
   together, so its whole endpoint pair travels as a unit. This is why single
   subset modes can express every operation without re-fitting colors. */
static bcdec_transform_status bcdec__tr_single_subset(const unsigned char* src, unsigned char* dst,
                                                      int mode, bcdec_transform_op op) {
    bcdec__tr_reader r;
    bcdec__tr_writer w;
    int primary_bits = bcdec__tr_primary_bits[mode];
    int secondary_bits = bcdec__tr_secondary_bits[mode];
    int rgb[2][3];
    int alpha[2];
    int pbit[2];
    int primary[16];
    int secondary[16];
    int rotation = 0;
    int index_selection = 0;
    int flip_color = 0, flip_alpha = 0, flip_all = 0;
    int i, j, x, y;

    for (i = 0; i < 2; ++i) {
        alpha[i] = 0;
        pbit[i] = 0;
    }

    bcdec__tr_reader_init(&r, src);
    for (i = 0; i < mode + 1; ++i) {
        (void)bcdec__tr_read(&r, 1);
    }
    if (mode == 4 || mode == 5) {
        rotation = bcdec__tr_read(&r, 2);
        if (mode == 4) {
            index_selection = bcdec__tr_read(&r, 1);
        }
    }

    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 2; ++j) {
            rgb[j][i] = bcdec__tr_read(&r, bcdec__tr_rgba_bits[mode]);
        }
    }
    if (bcdec__tr_alpha_bits[mode] > 0) {
        for (j = 0; j < 2; ++j) {
            alpha[j] = bcdec__tr_read(&r, bcdec__tr_alpha_bits[mode]);
        }
    }
    if (bcdec__tr_has_pbits[mode]) {
        for (j = 0; j < 2; ++j) {
            pbit[j] = bcdec__tr_read(&r, 1);
        }
    }

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            primary[y * 4 + x] = bcdec__tr_read(&r, primary_bits - ((x == 0 && y == 0) ? 1 : 0));
        }
    }
    if (secondary_bits > 0) {
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                secondary[y * 4 + x] = bcdec__tr_read(&r, secondary_bits - ((x == 0 && y == 0) ? 1 : 0));
            }
        }
    }

    {
        int sx, sy, anchor_primary, anchor_secondary;
        bcdec__tr_src_of(op, 0, 0, &sx, &sy);
        anchor_primary = primary[sy * 4 + sx];
        anchor_secondary = (secondary_bits > 0) ? secondary[sy * 4 + sx] : 0;

        if (mode == 6) {
            flip_all = (anchor_primary >= (1 << (primary_bits - 1)));
            if (flip_all) {
                for (y = 0; y < 4; ++y) {
                    for (x = 0; x < 4; ++x) {
                        primary[y * 4 + x] = ((1 << primary_bits) - 1) - primary[y * 4 + x];
                    }
                }
            }
        } else {
            int color_is_primary = (mode == 5) || (index_selection == 0);
            int color_bits = color_is_primary ? primary_bits : secondary_bits;
            int alpha_stream_bits = color_is_primary ? secondary_bits : primary_bits;
            int color_anchor = color_is_primary ? anchor_primary : anchor_secondary;
            int alpha_anchor = color_is_primary ? anchor_secondary : anchor_primary;
            int* color_stream = color_is_primary ? primary : secondary;
            int* alpha_stream = color_is_primary ? secondary : primary;

            flip_color = (color_anchor >= (1 << (color_bits - 1)));
            flip_alpha = (alpha_anchor >= (1 << (alpha_stream_bits - 1)));
            if (flip_color) {
                for (y = 0; y < 4; ++y) {
                    for (x = 0; x < 4; ++x) {
                        color_stream[y * 4 + x] = ((1 << color_bits) - 1) - color_stream[y * 4 + x];
                    }
                }
            }
            if (flip_alpha) {
                for (y = 0; y < 4; ++y) {
                    for (x = 0; x < 4; ++x) {
                        alpha_stream[y * 4 + x] = ((1 << alpha_stream_bits) - 1) - alpha_stream[y * 4 + x];
                    }
                }
            }
        }
    }

    bcdec__tr_writer_init(&w, dst);
    bcdec__tr_write(&w, 1u << mode, mode + 1);
    if (mode == 4 || mode == 5) {
        bcdec__tr_write(&w, (unsigned)rotation, 2);
        if (mode == 4) {
            bcdec__tr_write(&w, (unsigned)index_selection, 1);
        }
    }

    /* Mode 6 has no p-bit split and one shared stream: both RGB and alpha
       endpoints plus their p-bits follow the single flip. Modes 4/5 swap only
       the endpoint pair that belongs to the stream being complemented. */
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 2; ++j) {
            int src_endpoint = (mode == 6) ? (j ^ flip_all) : (j ^ flip_color);
            bcdec__tr_write(&w, (unsigned)rgb[src_endpoint][i], bcdec__tr_rgba_bits[mode]);
        }
    }
    if (bcdec__tr_alpha_bits[mode] > 0) {
        for (j = 0; j < 2; ++j) {
            int src_endpoint = (mode == 6) ? (j ^ flip_all) : (j ^ flip_alpha);
            bcdec__tr_write(&w, (unsigned)alpha[src_endpoint], bcdec__tr_alpha_bits[mode]);
        }
    }
    if (bcdec__tr_has_pbits[mode]) {
        for (j = 0; j < 2; ++j) {
            bcdec__tr_write(&w, (unsigned)pbit[j ^ flip_all], 1);
        }
    }

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int sx, sy, value, bits;
            bcdec__tr_src_of(op, x, y, &sx, &sy);
            value = primary[sy * 4 + sx];
            bits = primary_bits - ((x == 0 && y == 0) ? 1 : 0);
            if (value >= (1 << bits)) {
                return BCDEC_TRANSFORM_UNREPRESENTABLE;
            }
            bcdec__tr_write(&w, (unsigned)value, bits);
        }
    }
    if (secondary_bits > 0) {
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                int sx, sy, value, bits;
                bcdec__tr_src_of(op, x, y, &sx, &sy);
                value = secondary[sy * 4 + sx];
                bits = secondary_bits - ((x == 0 && y == 0) ? 1 : 0);
                if (value >= (1 << bits)) {
                    return BCDEC_TRANSFORM_UNREPRESENTABLE;
                }
                bcdec__tr_write(&w, (unsigned)value, bits);
            }
        }
    }

    return BCDEC_TRANSFORM_OK;
}

/* ------------------------------------------------------------------------- */
/* Public entry point                                                         */

bcdec_transform_status bcdec_bc7_transform(const void* src16, void* dst16, bcdec_transform_op op) {
    const unsigned char* src = (const unsigned char*)src16;
    unsigned char out[16];
    int mode;

    if (src == 0 || dst16 == 0) {
        return BCDEC_TRANSFORM_INVALID;
    }
    if ((int)op < (int)BCDEC_IDENTITY || (int)op > (int)BCDEC_ANTI_TRANSPOSE) {
        return BCDEC_TRANSFORM_INVALID;
    }

    /* A valid BC7 block starts with a unary mode prefix: mode zeros then a one.
       No set bit in the first eight bits means there is no valid mode. */
    for (mode = 0; mode < 8 && (src[0] & (1u << mode)) == 0; ++mode) {
    }
    if (mode >= 8) {
        return BCDEC_TRANSFORM_INVALID;
    }

    if (op == BCDEC_IDENTITY) {
        /* Identity is byte-for-byte, including bits the decoder ignores. */
        int i;
        unsigned char* dst = (unsigned char*)dst16;
        for (i = 0; i < 16; ++i) {
            dst[i] = src[i];
        }
        return BCDEC_TRANSFORM_OK;
    }

    {
        bcdec_transform_status status;
        if (bcdec__tr_subset_count(mode) == 1) {
            status = bcdec__tr_single_subset(src, out, mode, op);
        } else {
            status = bcdec__tr_multi_subset(src, out, mode, op);
        }
        if (status == BCDEC_TRANSFORM_OK) {
            int i;
            unsigned char* dst = (unsigned char*)dst16;
            for (i = 0; i < 16; ++i) {
                dst[i] = out[i];
            }
        }
        return status;
    }
}

#endif /* BCDEC_TRANSFORM_IMPLEMENTATION */
