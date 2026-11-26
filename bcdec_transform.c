#include "bcdec_transform.h"

#include "bcdec.h"

#include <string.h>

/*
 * The implementation parses the same LSB-first 128-bit layout as bcdec_bc7.
 * Endpoint values are kept as their packed quantized bit fields; only their
 * order and the index bits are rearranged.
 */

typedef struct bcdec__tr_reader
{
    const unsigned char* data;
    unsigned pos;
} bcdec__tr_reader;

typedef struct bcdec__tr_writer
{
    unsigned char data[16];
    unsigned pos;
} bcdec__tr_writer;

typedef struct bcdec__tr_block
{
    int mode;
    int subset_count;
    int partition;
    int rotation;
    int selection;

    int color_bits;
    int alpha_bits;
    int index_bits;
    int index_bits2;

    unsigned int ep[6][4];
    unsigned int pbit[6];
    unsigned int shared_pbit[3];
    unsigned int idx[2][4][4];
} bcdec__tr_block;

static unsigned int bcdec__tr_get_bits(bcdec__tr_reader* r, int n)
{
    unsigned int value = 0;
    int k;

    for (k = 0; k < n; ++k) {
        unsigned int bit = (r->data[(r->pos + (unsigned)k) >> 3]
                            >> ((r->pos + (unsigned)k) & 7u)) & 1u;
        value |= bit << k;
    }

    r->pos += (unsigned)n;
    return value;
}

static void bcdec__tr_put_bits(bcdec__tr_writer* w, unsigned int value, int n)
{
    int k;

    for (k = 0; k < n; ++k) {
        unsigned int bit = (value >> k) & 1u;
        unsigned int pos = w->pos++;
        w->data[pos >> 3] = (unsigned char)(w->data[pos >> 3]
                                            | (bit << (pos & 7u)));
    }
}

static void bcdec__tr_map_xy(int x, int y, bcdec_transform_op op,
                             int* out_x, int* out_y)
{
    switch (op) {
        case BCDEC_ROTATE_90:
            *out_x = 3 - y;
            *out_y = x;
            break;
        case BCDEC_ROTATE_180:
            *out_x = 3 - x;
            *out_y = 3 - y;
            break;
        case BCDEC_ROTATE_270:
            *out_x = y;
            *out_y = 3 - x;
            break;
        case BCDEC_FLIP_X:
            *out_x = 3 - x;
            *out_y = y;
            break;
        case BCDEC_FLIP_Y:
            *out_x = x;
            *out_y = 3 - y;
            break;
        case BCDEC_TRANSPOSE:
            *out_x = y;
            *out_y = x;
            break;
        case BCDEC_ANTI_TRANSPOSE:
            *out_x = 3 - y;
            *out_y = 3 - x;
            break;
        case BCDEC_IDENTITY:
        default:
            *out_x = x;
            *out_y = y;
            break;
    }
}

static int bcdec__tr_subset_count(int mode)
{
    static const int counts[8] = { 3, 2, 3, 2, 1, 1, 1, 2 };
    return counts[mode];
}

static int bcdec__tr_partition_bits(int mode)
{
    if (mode == 0) {
        return 4;
    }
    if (mode == 1 || mode == 2 || mode == 3 || mode == 7) {
        return 6;
    }
    return 0;
}

static int bcdec__tr_is_anchor(const bcdec__tr_block* b, int y, int x)
{
    if (b->subset_count == 1) {
        return x == 0 && y == 0;
    }

    return (bcdec_bc7_partition_sets[b->subset_count - 2][b->partition][y][x]
            & 0x80) != 0;
}

static void bcdec__tr_anchor_xy(int subset_count, int partition, int subset,
                                int* anchor_x, int* anchor_y)
{
    int y, x;

    if (subset_count == 1) {
        *anchor_x = 0;
        *anchor_y = 0;
        return;
    }

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            unsigned char label =
                bcdec_bc7_partition_sets[subset_count - 2][partition][y][x];
            if ((label & 0x80) && (label & 3) == subset) {
                *anchor_x = x;
                *anchor_y = y;
                return;
            }
        }
    }

    /* Every standard BC7 partition in the shared table has one anchor/subset. */
    *anchor_x = 0;
    *anchor_y = 0;
}

/*
 * Find the smallest standard partition and lexicographically smallest
 * target-subset -> source-subset map whose labels equal the transformed
 * source layout after renaming.
 */
static bcdec_transform_status bcdec__tr_find_layout(
    const bcdec__tr_block* b,
    bcdec_transform_op op,
    int* target_partition,
    int subset_map[3])
{
    int transformed[4][4];
    int y, x, p;
    int max_partition = (b->mode == 0) ? 16 : 64;

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int source_subset = 0;
            int tx, ty;

            if (b->subset_count > 1) {
                source_subset =
                    bcdec_bc7_partition_sets[b->subset_count - 2]
                                           [b->partition][y][x] & 3;
            }

            bcdec__tr_map_xy(x, y, op, &tx, &ty);
            transformed[ty][tx] = source_subset;
        }
    }

    if (b->subset_count == 1) {
        /* A single subset is geometrically invariant and needs no rename. */
        *target_partition = 0;
        subset_map[0] = 0;
        subset_map[1] = 0;
        subset_map[2] = 0;
        return BCDEC_TRANSFORM_OK;
    }

    for (p = 0; p < max_partition; ++p) {
        if (b->subset_count == 2) {
            static const int perms[2][2] = { { 0, 1 }, { 1, 0 } };
            int pi;

            for (pi = 0; pi < 2; ++pi) {
                int ok = 1;
                for (y = 0; y < 4 && ok; ++y) {
                    for (x = 0; x < 4; ++x) {
                        int target_subset =
                            bcdec_bc7_partition_sets[0][p][y][x] & 3;
                        if (transformed[y][x] != perms[pi][target_subset]) {
                            ok = 0;
                            break;
                        }
                    }
                }

                if (ok) {
                    *target_partition = p;
                    subset_map[0] = perms[pi][0];
                    subset_map[1] = perms[pi][1];
                    subset_map[2] = 0;
                    return BCDEC_TRANSFORM_OK;
                }
            }
        } else {
            /* Lexicographic order of target-subset -> source-subset maps. */
            static const int perms[6][3] = {
                { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
                { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 }
            };
            int pi;

            for (pi = 0; pi < 6; ++pi) {
                int ok = 1;
                for (y = 0; y < 4 && ok; ++y) {
                    for (x = 0; x < 4; ++x) {
                        int target_subset =
                            bcdec_bc7_partition_sets[1][p][y][x] & 3;
                        if (transformed[y][x] != perms[pi][target_subset]) {
                            ok = 0;
                            break;
                        }
                    }
                }

                if (ok) {
                    *target_partition = p;
                    subset_map[0] = perms[pi][0];
                    subset_map[1] = perms[pi][1];
                    subset_map[2] = perms[pi][2];
                    return BCDEC_TRANSFORM_OK;
                }
            }
        }
    }

    return BCDEC_TRANSFORM_UNREPRESENTABLE;
}

bcdec_transform_status bcdec_bc7_transform(const void* src16,
                                           void* dst16,
                                           bcdec_transform_op op)
{
    static const int color_bits[8] = { 4, 6, 5, 7, 5, 7, 7, 5 };
    static const int alpha_bits[8] = { 0, 0, 0, 0, 6, 8, 7, 5 };
    static const int index_bits[8] = { 3, 3, 2, 2, 2, 2, 4, 2 };
    static const int index_bits2[8] = { 0, 0, 0, 0, 3, 2, 0, 0 };

    bcdec__tr_reader reader;
    bcdec__tr_block in, out;
    bcdec__tr_writer writer;
    unsigned char* dst = (unsigned char*)dst16;
    const unsigned char* src = (const unsigned char*)src16;
    int mode, y, x, c, e, stream;
    int subset_map[3] = { 0, 0, 0 };
    int target_partition = 0;
    int partition_bits_count;
    bcdec_transform_status layout_status;

    if (src == NULL || dst == NULL) {
        return BCDEC_TRANSFORM_INVALID;
    }
    if ((int)op < (int)BCDEC_IDENTITY || (int)op > (int)BCDEC_ANTI_TRANSPOSE) {
        return BCDEC_TRANSFORM_INVALID;
    }

    memset(&in, 0, sizeof(in));
    reader.data = src;
    reader.pos = 0;

    for (mode = 0; mode < 8; ++mode) {
        if (bcdec__tr_get_bits(&reader, 1)) {
            break;
        }
    }
    if (mode == 8) {
        return BCDEC_TRANSFORM_INVALID;
    }

    in.mode = mode;
    in.subset_count = bcdec__tr_subset_count(mode);
    in.color_bits = color_bits[mode];
    in.alpha_bits = alpha_bits[mode];
    in.index_bits = index_bits[mode];
    in.index_bits2 = index_bits2[mode];

    partition_bits_count = bcdec__tr_partition_bits(mode);
    if (partition_bits_count) {
        in.partition = (int)bcdec__tr_get_bits(&reader, partition_bits_count);
    }

    if (mode == 4 || mode == 5) {
        in.rotation = (int)bcdec__tr_get_bits(&reader, 2);
        if (mode == 4) {
            in.selection = (int)bcdec__tr_get_bits(&reader, 1);
        }
    }

    for (c = 0; c < 3; ++c) {
        for (e = 0; e < 2 * in.subset_count; ++e) {
            in.ep[e][c] = bcdec__tr_get_bits(&reader, in.color_bits);
        }
    }
    if (in.alpha_bits) {
        for (e = 0; e < 2 * in.subset_count; ++e) {
            in.ep[e][3] = bcdec__tr_get_bits(&reader, in.alpha_bits);
        }
    }

    if (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7) {
        if (mode == 1) {
            in.shared_pbit[0] = bcdec__tr_get_bits(&reader, 1);
            in.shared_pbit[1] = bcdec__tr_get_bits(&reader, 1);
        } else {
            for (e = 0; e < 2 * in.subset_count; ++e) {
                in.pbit[e] = bcdec__tr_get_bits(&reader, 1);
            }
        }
    }

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int reduced = bcdec__tr_is_anchor(&in, y, x);
            in.idx[0][y][x] =
                bcdec__tr_get_bits(&reader, in.index_bits - reduced);
        }
    }

    if (in.index_bits2) {
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                int reduced = bcdec__tr_is_anchor(&in, y, x);
                in.idx[1][y][x] =
                    bcdec__tr_get_bits(&reader, in.index_bits2 - reduced);
            }
        }
    }

    if (reader.pos != 128) {
        return BCDEC_TRANSFORM_INVALID;
    }

    layout_status = bcdec__tr_find_layout(&in, op, &target_partition,
                                          subset_map);
    if (layout_status != BCDEC_TRANSFORM_OK) {
        return layout_status;
    }

    out = in;
    out.partition = target_partition;

    /* Move both independent index planes through the same pixel permutation. */
    for (stream = 0; stream < 2; ++stream) {
        int stream_bits = (stream == 0) ? in.index_bits : in.index_bits2;
        if (!stream_bits) {
            continue;
        }
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                int tx, ty;
                bcdec__tr_map_xy(x, y, op, &tx, &ty);
                out.idx[stream][ty][tx] = in.idx[stream][y][x];
            }
        }
    }

    if (!in.index_bits2) {
        int t;

        /* One index stream controls the whole RGBA endpoint pair. */
        for (t = 0; t < in.subset_count; ++t) {
            int s = subset_map[t];
            int ax, ay;
            int swap_endpoints;
            unsigned int mask = (1u << in.index_bits) - 1u;

            bcdec__tr_anchor_xy(in.subset_count, target_partition, t,
                                &ax, &ay);
            swap_endpoints =
                (int)((out.idx[0][ay][ax] >> (in.index_bits - 1)) & 1u);

            if (swap_endpoints) {
                for (y = 0; y < 4; ++y) {
                    for (x = 0; x < 4; ++x) {
                        int label = 0;
                        if (in.subset_count > 1) {
                            label = bcdec_bc7_partition_sets
                                [in.subset_count - 2][target_partition][y][x] & 3;
                        }
                        if (label == t) {
                            out.idx[0][y][x] ^= mask;
                        }
                    }
                }
            }

            for (c = 0; c < 4; ++c) {
                out.ep[2 * t][c] =
                    in.ep[2 * s + (swap_endpoints ? 1 : 0)][c];
                out.ep[2 * t + 1][c] =
                    in.ep[2 * s + (swap_endpoints ? 0 : 1)][c];
            }

            out.pbit[2 * t] =
                in.pbit[2 * s + (swap_endpoints ? 1 : 0)];
            out.pbit[2 * t + 1] =
                in.pbit[2 * s + (swap_endpoints ? 0 : 1)];

            /* Mode 1 has one p-bit shared by both endpoints of a subset. */
            out.shared_pbit[t] = in.shared_pbit[s];
        }
    } else {
        /*
         * Modes 4 and 5 have separate color and alpha interpolation planes.
         * Mode 4's selection bit decides which physical plane controls RGB.
         * RGB and alpha endpoint order can therefore be normalized separately
         * when placing the omitted anchor MSB back at zero.  These modes have
         * no p-bits, so no shared endpoint bit obstructs that.
         */
        int color_stream = (in.mode == 4 && in.selection == 1) ? 1 : 0;
        int alpha_stream = color_stream ^ 1;
        int stream_swap[2] = { 0, 0 };
        int ax = 0, ay = 0;
        int color_bits_for_stream =
            (color_stream == 0) ? in.index_bits : in.index_bits2;
        int alpha_bits_for_stream =
            (alpha_stream == 0) ? in.index_bits : in.index_bits2;
        int color_swap;
        int alpha_swap;

        bcdec__tr_anchor_xy(1, 0, 0, &ax, &ay);
        color_swap = (int)((out.idx[color_stream][ay][ax]
                            >> (color_bits_for_stream - 1)) & 1u);
        alpha_swap = (int)((out.idx[alpha_stream][ay][ax]
                            >> (alpha_bits_for_stream - 1)) & 1u);

        stream_swap[color_stream] = color_swap;
        stream_swap[alpha_stream] = alpha_swap;

        for (stream = 0; stream < 2; ++stream) {
            int bits = (stream == 0) ? in.index_bits : in.index_bits2;
            unsigned int mask = (1u << bits) - 1u;
            if (!stream_swap[stream]) {
                continue;
            }
            for (y = 0; y < 4; ++y) {
                for (x = 0; x < 4; ++x) {
                    out.idx[stream][y][x] ^= mask;
                }
            }
        }

        for (c = 0; c < 3; ++c) {
            out.ep[0][c] = in.ep[color_swap ? 1 : 0][c];
            out.ep[1][c] = in.ep[color_swap ? 0 : 1][c];
        }
        if (in.alpha_bits) {
            out.ep[0][3] = in.ep[alpha_swap ? 1 : 0][3];
            out.ep[1][3] = in.ep[alpha_swap ? 0 : 1][3];
        }
    }

    memset(&writer, 0, sizeof(writer));

    for (mode = 0; mode < out.mode; ++mode) {
        bcdec__tr_put_bits(&writer, 0, 1);
    }
    bcdec__tr_put_bits(&writer, 1, 1);

    partition_bits_count = bcdec__tr_partition_bits(out.mode);
    if (partition_bits_count) {
        bcdec__tr_put_bits(&writer, (unsigned int)out.partition,
                           partition_bits_count);
    }

    if (out.mode == 4 || out.mode == 5) {
        bcdec__tr_put_bits(&writer, (unsigned int)out.rotation, 2);
        if (out.mode == 4) {
            bcdec__tr_put_bits(&writer, (unsigned int)out.selection, 1);
        }
    }

    for (c = 0; c < 3; ++c) {
        for (e = 0; e < 2 * out.subset_count; ++e) {
            bcdec__tr_put_bits(&writer, out.ep[e][c], out.color_bits);
        }
    }
    if (out.alpha_bits) {
        for (e = 0; e < 2 * out.subset_count; ++e) {
            bcdec__tr_put_bits(&writer, out.ep[e][3], out.alpha_bits);
        }
    }

    if (out.mode == 1) {
        bcdec__tr_put_bits(&writer, out.shared_pbit[0], 1);
        bcdec__tr_put_bits(&writer, out.shared_pbit[1], 1);
    } else if (out.mode == 0 || out.mode == 3 ||
               out.mode == 6 || out.mode == 7) {
        for (e = 0; e < 2 * out.subset_count; ++e) {
            bcdec__tr_put_bits(&writer, out.pbit[e], 1);
        }
    }

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int reduced = bcdec__tr_is_anchor(&out, y, x);
            bcdec__tr_put_bits(&writer, out.idx[0][y][x],
                               out.index_bits - reduced);
        }
    }

    if (out.index_bits2) {
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                int reduced = bcdec__tr_is_anchor(&out, y, x);
                bcdec__tr_put_bits(&writer, out.idx[1][y][x],
                                   out.index_bits2 - reduced);
            }
        }
    }

    if (writer.pos != 128) {
        return BCDEC_TRANSFORM_INVALID;
    }

    memcpy(dst, writer.data, 16);
    return BCDEC_TRANSFORM_OK;
}
