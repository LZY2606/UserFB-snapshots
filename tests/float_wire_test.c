////////////////////////////////////////////////////////////////////////////
// float_wire_test.c
//
// Bit-level compatibility regression for the WavPack 32-bit float sideband.
//
// The independent reference (float_wire_model.*) never uses float math or
// the production getbits/putbits/float conversion code to calculate an
// expectation. This file drives that reference against the real production
// scan_float_data(), send_float_data() and float_values() so that a format
// change on either side is pinpointed to the shared exponent, the main
// integers, or the exact sideband bit position where the two first diverge.
//
// Public-container behavior is exercised separately through
// WavpackOpenFileOutput(), WavpackPackSamples() and the public reader.
////////////////////////////////////////////////////////////////////////////

#include "check.h"

#include "wavpack_local.h"
#include "float_wire_model.h"
#include "float_wire_packets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW_MAX_N 64

#ifdef BITSTREAM_SHORTS
typedef uint16_t fw_bs_cell;
#else
typedef unsigned char fw_bs_cell;
#endif

static void fw_bs_wrap (Bitstream *bs)
{
    bs->error = 1;
}

// Open a production Bitstream for writing over our own scratch buffer.

static void fw_bs_open_write_local (Bitstream *bs, fw_bs_cell *cells, size_t ncells)
{
    memset (bs, 0, sizeof (*bs));
    bs->buf = cells;
    bs->ptr = bs->buf;
    bs->end = cells + ncells;
    bs->wrap = fw_bs_wrap;
}

// Mirror of the production bs_open_read() layout, which cannot be linked
// directly because it is file-local in open_utils.c.

static void fw_bs_open_read_local (Bitstream *bs, const fw_bs_cell *cells, size_t ncells)
{
    memset (bs, 0, sizeof (*bs));
    bs->buf = (fw_bs_cell *) (void *) cells;
    bs->ptr = bs->buf - 1;
    bs->end = (fw_bs_cell *) (void *) (cells + ncells);
    bs->wrap = fw_bs_wrap;
}

// Copy a byte buffer into an LSB-first fw_bits instance.

static void fw_load_bytes (fw_bits *bits, const unsigned char *bytes, size_t nbytes)
{
    size_t i;

    fw_bits_init (bits);

    for (i = 0; i < nbytes; i++)
        fw_bits_putn (bits, bytes [i], 8);
}

// Capture the bits a production write Bitstream has emitted so far. The
// production macros keep a partially filled cell in the shift register
// (sr/bc) and only flush whole cells to the buffer, so the valid bits are
// the completed cells followed by the bc low bits of sr. This reads the
// Bitstream fields but never calls the production getbits/putbits macros.

static void fw_capture_written (const Bitstream *bs, fw_bits *bits)
{
    size_t completed_cells = (size_t)(bs->ptr - bs->buf);
    size_t i;
    int cell_bits = (int) sizeof (fw_bs_cell) * 8;

    fw_bits_init (bits);

    for (i = 0; i < completed_cells; i++)
        fw_bits_putn (bits, bs->buf [i], cell_bits);

    fw_bits_putn (bits, bs->sr, bs->bc);
}

// Number of bits a production read Bitstream has consumed. A freshly opened
// read stream has ptr = buf - 1 and bc = 0; every cell load advances ptr by
// one, so the consumed count is the cells crossed minus the bits buffered.

static size_t fw_reader_consumed_bits (const Bitstream *bs)
{
    int cell_bits = (int) sizeof (fw_bs_cell) * 8;
    return (size_t)(bs->ptr - bs->buf + 1) * (size_t) cell_bits - (size_t) bs->bc;
}

// ----------------------------- production vs. model: scan -----------------------------

static void compare_scan (const char *name, const uint32_t *patterns, int n)
{
    fw_scan_result ref;
    int32_t ref_ints [FW_MAX_N], prod_ints [FW_MAX_N];
    f32 prod_vals [FW_MAX_N];
    WavpackStream wps;
    int prod_needs_wvx, ref_needs_wvx, i;

    TASK_CHECK (n <= FW_MAX_N);

    fw_scan_float (patterns, n, &ref, ref_ints);

    memset (&wps, 0, sizeof (wps));
    memcpy (prod_vals, patterns, (size_t) n * sizeof (uint32_t));
    prod_needs_wvx = scan_float_data (&wps, prod_vals, n);
    memcpy (prod_ints, prod_vals, (size_t) n * sizeof (int32_t));

    if (wps.float_flags != ref.flags) {
        fprintf (stderr, "first divergence [%s]: float_flags ref=%u prod=%u\n",
            name, ref.flags, (unsigned) wps.float_flags);
        TASK_CHECK (0);
    }

    TASK_CHECK (wps.float_max_exp == ref.max_exp);
    TASK_CHECK (wps.float_shift == ref.float_shift);
    TASK_CHECK (wps.crc_x == ref.crc_x);

    ref_needs_wvx = (int)(ref.flags & (FW_FLAG_EXCEPTIONS | FW_FLAG_ZEROS_SENT |
        FW_FLAG_SHIFT_SENT | FW_FLAG_SHIFT_SAME));
    TASK_CHECK (prod_needs_wvx == ref_needs_wvx);

    for (i = 0; i < n; i++) {
        if (prod_ints [i] != ref_ints [i]) {
            fprintf (stderr, "first divergence [%s]: main integer[%d] ref=0x%08x prod=0x%08x\n",
                name, i, (unsigned) ref_ints [i], (unsigned) prod_ints [i]);
            TASK_CHECK (0);
        }
    }

    // The MAG magnitude steps derived independently must match the flags
    // field the production scanner programmed.
    TASK_CHECK (((wps.wphdr.flags & MAG_MASK) >> MAG_LSB) == ref.mag_steps);
}

// ----------------------------- production vs. model: sender -----------------------------

static void compare_send (const char *name, const uint32_t *patterns, int n)
{
    fw_scan_result ref;
    int32_t ref_ints [FW_MAX_N];
    f32 send_vals [FW_MAX_N];
    WavpackStream wps;
    fw_bits planned, got;
    fw_bs_cell scratch [1024];
    size_t i;

    fw_scan_float (patterns, n, &ref, ref_ints);

    // Reproduce the production per-block state, then send the original
    // patterns through the real production sender.
    memset (&wps, 0, sizeof (wps));
    wps.float_flags = (unsigned char) ref.flags;
    wps.float_max_exp = (unsigned char) ref.max_exp;
    wps.float_shift = (unsigned char) ref.float_shift;
    memcpy (send_vals, patterns, (size_t) n * sizeof (uint32_t));

    fw_bs_open_write_local (&wps.wvxbits, scratch,
        sizeof (scratch) / sizeof (scratch [0]));
    send_float_data (&wps, send_vals, n);
    TASK_CHECK (!wps.wvxbits.error);

    fw_bits_init (&planned);
    fw_send_plan (patterns, n, ref.flags, ref.max_exp, &planned);
    fw_capture_written (&wps.wvxbits, &got);

    // A pure SHIFT_ONES block (or any block without a sideband trigger) must
    // emit zero bits on both sides; the implied ones are restored on decode.
    if (!(ref.flags & (FW_FLAG_EXCEPTIONS | FW_FLAG_ZEROS_SENT |
        FW_FLAG_SHIFT_SENT | FW_FLAG_SHIFT_SAME))) {
        TASK_CHECK (fw_bits_nbits (&planned) == 0);
        TASK_CHECK (fw_bits_nbits (&got) == 0);
        fw_bits_free (&planned);
        fw_bits_free (&got);
        return;
    }

    if (fw_bits_nbits (&got) != fw_bits_nbits (&planned)) {
        fprintf (stderr, "first divergence [%s]: sideband length ref=%u prod=%u bits\n",
            name, (unsigned) fw_bits_nbits (&planned), (unsigned) fw_bits_nbits (&got));
        TASK_CHECK (0);
    }

    for (i = 0; i < fw_bits_nbits (&planned); i++) {
        int want = fw_bits_get (&planned);
        int have = fw_bits_get (&got);

        if (want != have) {
            fprintf (stderr, "first divergence [%s]: sideband bit %zu ref=%d prod=%d\n",
                name, i, want, have);
            TASK_CHECK (0);
        }
    }

    fw_bits_free (&planned);
    fw_bits_free (&got);
}

// ----------------------------- production vs. model: receiver -----------------------------

static void compare_restore (const char *name, const uint32_t *patterns, int n,
    unsigned min_shifted_zeros, unsigned max_shifted_ones, int new_style)
{
    fw_scan_result ref;
    int32_t ref_ints [FW_MAX_N], work_ints [FW_MAX_N];
    WavpackStream wps;
    fw_bits sideband;
    fw_bs_cell wire [1024];
    size_t padded_bytes, padded_cells;
    int i, total_consumed = 0;

    fw_scan_float (patterns, n, &ref, ref_ints);

    // Independent receiver first: one shared sideband for the whole block,
    // consumed value after value with no per-sample realignment.
    fw_bits_init (&sideband);

    if (new_style)
        fw_send_plan_elided (patterns, n, ref.flags, ref.max_exp,
            min_shifted_zeros, max_shifted_ones, &sideband);
    else
        fw_send_plan (patterns, n, ref.flags, ref.max_exp, &sideband);

    for (i = 0; i < n; i++) {
        uint32_t restored = 0;
        int used = fw_restore_wvx (ref_ints [i], ref.flags, ref.float_shift, ref.max_exp,
            min_shifted_zeros, max_shifted_ones, &sideband, &restored);

        TASK_CHECK (used >= 0);
        total_consumed += used;

        if (restored != patterns [i]) {
            fprintf (stderr, "first divergence [%s]: model restore sample %d "
                "want=0x%08x got=0x%08x\n", name, i, patterns [i], restored);
            TASK_CHECK (0);
        }
    }

    TASK_CHECK ((size_t) total_consumed == fw_bits_nbits (&sideband));

    // Materialize the wire cells with explicit physical padding for the
    // production Bitstream read convention.
    {
        size_t valid_bits = fw_bits_nbits (&sideband);

        padded_bytes = fw_bits_padded_bytes (&sideband);
        padded_cells = (padded_bytes + 8) / sizeof (fw_bs_cell);
        TASK_CHECK (padded_cells <= sizeof (wire) / sizeof (wire [0]));
        memset (wire, 0, sizeof (wire));
        memcpy (wire, sideband.data, padded_bytes);
        fw_bits_free (&sideband);

        // Production receiver over the same self-assembled integers and sideband.
        memset (&wps, 0, sizeof (wps));
        wps.float_flags = (unsigned char) ref.flags;
        wps.float_shift = (unsigned char) ref.float_shift;
        wps.float_max_exp = (unsigned char) ref.max_exp;
        wps.float_min_shifted_zeros = (unsigned char) min_shifted_zeros;
        wps.float_max_shifted_ones = (unsigned char) max_shifted_ones;
        wps.crc_x = 0xffffffffu;
        memcpy (work_ints, ref_ints, (size_t) n * sizeof (int32_t));

        fw_bs_open_read_local (&wps.wvxbits, wire, padded_cells);
        float_values (&wps, work_ints, n);

        for (i = 0; i < n; i++) {
            uint32_t produced = (uint32_t) work_ints [i];

            if (produced != patterns [i]) {
                fprintf (stderr, "first divergence [%s]: production float_values sample %d "
                    "want=0x%08x got=0x%08x\n", name, i, patterns [i], produced);
                TASK_CHECK (0);
            }
        }

        TASK_CHECK (wps.crc_x == fw_crc_x (patterns, n));
        TASK_CHECK (!wps.wvxbits.error);

        // The production reader must consume exactly the planned valid bit
        // count; the padding cells remain buffered and are not sideband bits.
        TASK_CHECK (fw_reader_consumed_bits (&wps.wvxbits) == valid_bits);
        bs_close_read (&wps.wvxbits);
    }
}

// No-WVX path: integer-precision floats decode through float_values_nowvx().

static void compare_nowvx (const char *name, const uint32_t *patterns, int n)
{
    fw_scan_result ref;
    int32_t ref_ints [FW_MAX_N], work_ints [FW_MAX_N];
    WavpackStream wps;
    int i;

    fw_scan_float (patterns, n, &ref, ref_ints);
    TASK_CHECK (ref.flags == 0);

    for (i = 0; i < n; i++)
        TASK_CHECK (fw_restore_nowvx (ref_ints [i], ref.flags, ref.float_shift,
            ref.max_exp) == patterns [i]);

    memset (&wps, 0, sizeof (wps));
    wps.float_max_exp = (unsigned char) ref.max_exp;
    wps.float_shift = (unsigned char) ref.float_shift;
    TASK_CHECK (!bs_is_open (&wps.wvxbits));
    memcpy (work_ints, ref_ints, (size_t) n * sizeof (int32_t));
    float_values (&wps, work_ints, n);

    for (i = 0; i < n; i++) {
        if ((uint32_t) work_ints [i] != patterns [i]) {
            fprintf (stderr, "first divergence [%s]: nowvx sample %d want=0x%08x got=0x%08x\n",
                name, i, patterns [i], (uint32_t) work_ints [i]);
            TASK_CHECK (0);
        }
    }
}

// ----------------------------- hand-assembled new WVX format -----------------------------

static void compare_new_wvx_blob (const char *name, const uint32_t *patterns, int n,
    unsigned min_shifted_zeros, unsigned max_shifted_ones)
{
    fw_blob blob;
    fw_bits expected;
    fw_block_view view;
    const fw_chunk *wvx_chunk;
    fw_wvx_view wvx;
    fw_scan_result ref;
    int32_t ref_ints [FW_MAX_N], work_ints [FW_MAX_N];
    WavpackStream wps;
    fw_bs_cell wire [1024];
    size_t wire_len, wire_cells;
    int i;

    TASK_CHECK (fw_build_new_wvx_material (patterns, n, min_shifted_zeros,
        max_shifted_ones, &blob, &expected) == 0);

    TASK_CHECK (fw_block_parse (blob.data, blob.size, &view) == 0);
    wvx_chunk = fw_block_find (&view, FW_ID_WVX_NEW_BITSTREAM);
    TASK_CHECK (wvx_chunk != NULL);
    TASK_CHECK (fw_open_wvx (wvx_chunk, 1, &wvx) == 0);
    TASK_CHECK (wvx.new_format);
    TASK_CHECK (wvx.min_shifted_zeros == min_shifted_zeros);
    TASK_CHECK (wvx.max_shifted_ones == max_shifted_ones);
    TASK_CHECK (wvx.crc_wvx == fw_crc_x (patterns, n));

    // Independent receiver against the physically elided payload. The view's
    // payload bytes include the explicit physical padding, but the bit count
    // of the expected plan marks where valid information ends.
    {
        fw_bits physical;

        fw_load_bytes (&physical, wvx.payload, wvx.payload_bytes);
        fw_scan_float (patterns, n, &ref, ref_ints);

        for (i = 0; i < n; i++) {
            uint32_t restored = 0;
            int used = fw_restore_wvx (ref_ints [i], ref.flags, ref.float_shift, ref.max_exp,
                min_shifted_zeros, max_shifted_ones, &physical, &restored);

            TASK_CHECK (used >= 0);

            if (restored != patterns [i]) {
                fprintf (stderr, "first divergence [%s]: elided restore sample %d "
                    "want=0x%08x got=0x%08x\n", name, i, patterns [i], restored);
                TASK_CHECK (0);
            }
        }

        TASK_CHECK (physical.pos == fw_bits_nbits (&expected));
        TASK_CHECK (fw_bits_padded_bytes (&expected) <= wvx.payload_bytes);
        fw_bits_free (&physical);
    }

    // Production float_values() on the same hand-assembled container bytes.
    // The view already spans payload plus the explicit pad bytes.
    wire_len = wvx.payload_bytes;
    wire_cells = (wire_len + sizeof (fw_bs_cell) - 1) / sizeof (fw_bs_cell);
    TASK_CHECK (wire_cells <= sizeof (wire) / sizeof (wire [0]));
    memset (wire, 0, sizeof (wire));
    memcpy (wire, wvx.payload, wire_len);

    memset (&wps, 0, sizeof (wps));
    wps.float_flags = (unsigned char) ref.flags;
    wps.float_shift = (unsigned char) ref.float_shift;
    wps.float_max_exp = (unsigned char) ref.max_exp;
    wps.float_min_shifted_zeros = (unsigned char) min_shifted_zeros;
    wps.float_max_shifted_ones = (unsigned char) max_shifted_ones;
    wps.crc_x = 0xffffffffu;
    memcpy (work_ints, ref_ints, (size_t) n * sizeof (int32_t));

    fw_bs_open_read_local (&wps.wvxbits, wire, wire_cells);
    float_values (&wps, work_ints, n);

    for (i = 0; i < n; i++) {
        if ((uint32_t) work_ints [i] != patterns [i]) {
            fprintf (stderr, "first divergence [%s]: production elided restore sample %d "
                "want=0x%08x got=0x%08x\n", name, i, patterns [i], (uint32_t) work_ints [i]);
            TASK_CHECK (0);
        }
    }

    TASK_CHECK (wps.crc_x == fw_crc_x (patterns, n));
    TASK_CHECK (!wps.wvxbits.error);

    fw_bits_free (&expected);
    fw_blob_free (&blob);
}

// ----------------------------- public API round trips -----------------------------

static void api_roundtrip (const char *name, const uint32_t *patterns, uint32_t frames,
    int num_channels, uint32_t channel_mask, uint32_t block_samples, uint32_t batch_frames)
{
    fw_blob blob;
    fw_container_report report;
    uint32_t *decoded, errors = 0, got_frames;
    size_t total_words, i;

    fw_blob_init (&blob);
    TASK_CHECK (fw_pack_float_blob (patterns, frames, num_channels, channel_mask,
        block_samples, &blob) == 0);

    TASK_CHECK (fw_report_container (&blob, &report) == 0);
    TASK_CHECK (report.nblocks >= 2);                       // material spans blocks
    TASK_CHECK (report.float_info_blocks >= 1);
    TASK_CHECK (report.wvx_blocks >= 1);
    TASK_CHECK (report.new_wvx_blocks == 0);               // encoder writes legacy float WVX
    TASK_CHECK (report.float_flag_union & FW_FLAG_EXCEPTIONS);
    TASK_CHECK (report.float_flag_union & FW_FLAG_NEG_ZEROS);
    TASK_CHECK (report.float_flag_union & FW_FLAG_ZEROS_SENT);
    TASK_CHECK (report.float_flag_union & FW_FLAG_SHIFT_SENT);

    total_words = (size_t) frames * (size_t) num_channels;
    decoded = (uint32_t *) calloc (total_words, sizeof (uint32_t));
    got_frames = fw_unpack_float_blob (&blob, frames, num_channels, batch_frames,
        decoded, &errors);

    TASK_CHECK (got_frames == frames);
    TASK_CHECK (errors == 0);

    for (i = 0; i < total_words; i++) {
        if (decoded [i] != patterns [i]) {
            fprintf (stderr, "first divergence [%s]: public API word %zu "
                "want=0x%08x got=0x%08x\n", name, i, patterns [i], decoded [i]);
            TASK_CHECK (0);
        }
    }

    free (decoded);
    fw_report_free (&report);
    fw_blob_free (&blob);
}

// ----------------------------- bit-pattern catalog -----------------------------

static uint32_t pat (unsigned sign, unsigned exponent, uint32_t mantissa)
{
    return fw_make_pattern (sign, exponent, mantissa);
}

void task_cases (void)
{
    // A large finite anchor forces every other value in the same block to
    // shift down to line up with its exponent.
    const uint32_t anchor_big = pat (0, 157, 0x7fffff);    // large finite normal
    const uint32_t anchor_mid = pat (0, 129, 0);           // exponent 129 anchor
    const uint32_t anchor_128 = pat (0, 134, 0);           // 128.0 scale anchor

    // Ordinary signed audio, including values that lose low bits.
    const uint32_t audio [] = {
        pat (0, 128, 0x400000), pat (1, 128, 0x200000),
        pat (0, 127, 0x000003), pat (1, 127, 0x7fffff),
        pat (0, 126, 0x123456), pat (1, 130, 0x000001),
        anchor_mid
    };

    // SHIFT_SENT: a mixed low-bit field is shifted out (bits 1:0 = 0b01).
    const uint32_t shift_sent [] = {
        anchor_mid, pat (0, 127, 0x000001), pat (1, 127, 0x123456),
        pat (0, 128, 0x000002)
    };

    // SHIFT_ONES: every shifted-out field is all ones, no shifted zeros.
    const uint32_t shift_ones [] = {
        anchor_mid, pat (0, 127, 0x7fffff), pat (1, 127, 0x3fffff),
        pat (0, 128, 0x000001)
    };

    // SHIFT_SAME: one all-ones field and one all-zeros field, nothing mixed.
    const uint32_t shift_same [] = {
        anchor_mid, pat (0, 127, 0x7fffff), pat (1, 127, 0x000000),
        pat (0, 128, 0x000000)
    };

    // Integer-precision floats: no information lands in a sideband.
    const uint32_t integer_only [] = {
        pat (0, 127, 0),           // 1.0
        pat (0, 128, 0),           // 2.0
        pat (0, 126, 0),           // 0.5
        pat (1, 128, 0),           // -2.0
        pat (0, 130, 0),           // 4.0
        0                          // plain positive zero
    };

    // Zeros: an all-zero block, then a block mixing positive and negative
    // zeros (the latter is the only one needing a sideband sign bit).
    const uint32_t all_zeros [] = {
        0, 0, 0, 0, 0, 0, 0, 0
    };
    const uint32_t zeros [] = {
        0, 0, pat (1, 0, 0), 0, pat (1, 0, 0), pat (0, 0, 0)
    };

    // Denormals at both extremes, plus finite values whose main integer is
    // shifted all the way to zero (false zeros) beside the big anchor.
    const uint32_t denormals [] = {
        anchor_big,
        pat (0, 0, 1),                 // smallest positive denormal
        pat (1, 0, 1),                 // smallest negative denormal
        pat (0, 0, 0x7fffff),          // largest positive denormal
        pat (1, 0, 0x7fffff),          // largest negative denormal
        pat (0, 1, 1),                 // tiny finite normal -> false zero
        pat (1, 2, 0x200000)           // tiny finite normal -> false zero
    };

    // Infinities and NaNs with both signs and distinct payloads.
    const uint32_t exceptions [] = {
        pat (0, 255, 0),               // +infinity
        pat (1, 255, 0),               // -infinity
        pat (0, 255, 0x400001),        // quiet NaN, positive, payload A
        pat (1, 255, 0x012345),        // NaN, negative, payload B
        pat (0, 255, 1),               // smallest signaling-style NaN
        pat (1, 255, 0x7fffff),        // largest negative NaN payload
        pat (0, 128, 0)                // ordinary neighbor
    };

    // Combined block exercising every category at once.
    const uint32_t combined [] = {
        anchor_big,
        pat (0, 128, 0x400000), pat (1, 127, 0x000003),
        pat (0, 0, 1), pat (1, 0, 0x7fffff),
        0, pat (1, 0, 0),
        pat (0, 255, 0), pat (1, 255, 0x7fc12345 & 0x7fffff),
        pat (0, 255, 0x412345),
        pat (0, 1, 1),
        anchor_mid
    };

    // Hand-assembled new-format material, exponents chosen so shifts are 6
    // and 3 against the 128.0 anchor (max_exp = 134).
    const uint32_t elided_zeros [] = {
        anchor_128,
        pat (0, 128, 0x000008), pat (1, 128, 0x000010),
        pat (0, 128, 0x000018), pat (0, 131, 0x000008)
    };
    const uint32_t elided_ones [] = {
        anchor_128,
        pat (0, 128, 0x000004), pat (1, 128, 0x000008),
        pat (0, 131, 0x000005)
    };

    // ---- direct scan / send / restore comparisons, legacy sideband ----
    compare_scan ("audio", audio, (int)(sizeof audio / sizeof audio [0]));
    compare_scan ("shift-sent", shift_sent, (int)(sizeof shift_sent / sizeof shift_sent [0]));
    compare_scan ("shift-ones", shift_ones, (int)(sizeof shift_ones / sizeof shift_ones [0]));
    compare_scan ("shift-same", shift_same, (int)(sizeof shift_same / sizeof shift_same [0]));
    compare_scan ("integer-only", integer_only,
        (int)(sizeof integer_only / sizeof integer_only [0]));
    compare_scan ("all-zeros", all_zeros, (int)(sizeof all_zeros / sizeof all_zeros [0]));
    compare_scan ("zeros", zeros, (int)(sizeof zeros / sizeof zeros [0]));
    compare_scan ("denormals", denormals, (int)(sizeof denormals / sizeof denormals [0]));
    compare_scan ("exceptions", exceptions, (int)(sizeof exceptions / sizeof exceptions [0]));
    compare_scan ("combined", combined, (int)(sizeof combined / sizeof combined [0]));

    compare_send ("audio", audio, (int)(sizeof audio / sizeof audio [0]));
    compare_send ("shift-sent", shift_sent,
        (int)(sizeof shift_sent / sizeof shift_sent [0]));
    compare_send ("shift-ones", shift_ones,
        (int)(sizeof shift_ones / sizeof shift_ones [0]));
    compare_send ("shift-same", shift_same,
        (int)(sizeof shift_same / sizeof shift_same [0]));
    compare_send ("all-zeros", all_zeros, (int)(sizeof all_zeros / sizeof all_zeros [0]));
    compare_send ("zeros", zeros, (int)(sizeof zeros / sizeof zeros [0]));
    compare_send ("denormals", denormals,
        (int)(sizeof denormals / sizeof denormals [0]));
    compare_send ("exceptions", exceptions,
        (int)(sizeof exceptions / sizeof exceptions [0]));
    compare_send ("combined", combined, (int)(sizeof combined / sizeof combined [0]));

    compare_restore ("audio", audio, (int)(sizeof audio / sizeof audio [0]), 0, 0, 0);
    compare_restore ("shift-sent", shift_sent,
        (int)(sizeof shift_sent / sizeof shift_sent [0]), 0, 0, 0);
    compare_restore ("shift-ones", shift_ones,
        (int)(sizeof shift_ones / sizeof shift_ones [0]), 0, 0, 0);
    compare_restore ("shift-same", shift_same,
        (int)(sizeof shift_same / sizeof shift_same [0]), 0, 0, 0);
    compare_restore ("zeros", zeros, (int)(sizeof zeros / sizeof zeros [0]), 0, 0, 0);
    compare_restore ("denormals", denormals,
        (int)(sizeof denormals / sizeof denormals [0]), 0, 0, 0);
    compare_restore ("exceptions", exceptions,
        (int)(sizeof exceptions / sizeof exceptions [0]), 0, 0, 0);
    compare_restore ("combined", combined,
        (int)(sizeof combined / sizeof combined [0]), 0, 0, 0);

    compare_nowvx ("integer-only", integer_only,
        (int)(sizeof integer_only / sizeof integer_only [0]));
    compare_nowvx ("all-zeros", all_zeros, (int)(sizeof all_zeros / sizeof all_zeros [0]));

    // ---- new WVX bitstream format with elided shifted bits ----
    // The production sender does not emit the new layout, so its bits are
    // checked against the legacy plan while the elided layout is validated
    // through the hand-assembled container below.
    compare_send ("elided-zeros", elided_zeros,
        (int)(sizeof elided_zeros / sizeof elided_zeros [0]));
    compare_restore ("elided-zeros", elided_zeros,
        (int)(sizeof elided_zeros / sizeof elided_zeros [0]), 3, 0, 1);
    compare_new_wvx_blob ("elided-zeros", elided_zeros,
        (int)(sizeof elided_zeros / sizeof elided_zeros [0]), 3, 0);

    compare_send ("elided-ones", elided_ones,
        (int)(sizeof elided_ones / sizeof elided_ones [0]));
    compare_restore ("elided-ones", elided_ones,
        (int)(sizeof elided_ones / sizeof elided_ones [0]), 0, 4, 1);
    compare_new_wvx_blob ("elided-ones", elided_ones,
        (int)(sizeof elided_ones / sizeof elided_ones [0]), 0, 4);

    // ---- public API: stereo, multiple blocks, small-batch reads ----
    // Every 16-frame block carries its own large finite anchor plus each
    // special category, so the per-block shared exponent changes the other
    // samples' representation in every block.
    {
        enum { TILE = 16, STEREO_FRAMES = 37 };
        const uint32_t left_tile [TILE] = {
            anchor_big,                          // shared-exponent anchor
            pat (1, 0, 0),                       // negative zero
            pat (0, 0, 1),                       // smallest denormal
            pat (0, 255, 0x400001),              // quiet NaN with payload
            pat (0, 255, 0),                     // positive infinity
            pat (1, 255, 0),                     // negative infinity
            pat (0, 150, 0x000001),              // mixed low bits -> SHIFT_SENT
            pat (0, 1, 1),                       // false zero against the anchor
            pat (0, 0, 0x7fffff),                // largest denormal
            0,                                   // positive zero
            pat (1, 128, 0x200000), pat (0, 127, 0x000003),
            pat (1, 127, 0x7fffff), pat (0, 126, 0x123456),
            pat (1, 130, 0x000001), anchor_mid
        };
        const uint32_t right_tile [TILE] = {
            anchor_big,
            pat (0, 0, 1),
            pat (1, 0, 0x7fffff),                // negative largest denormal
            pat (1, 255, 0x012345),              // negative NaN, other payload
            pat (1, 255, 0),
            pat (0, 255, 1),                     // smallest NaN payload
            pat (1, 150, 0x000003),              // different mixed low bits
            pat (1, 2, 0x200000),                // false zero, negative
            pat (1, 0, 1),
            pat (1, 0, 0),                       // negative zero
            pat (0, 128, 0x400000), pat (1, 128, 0x200000),
            pat (0, 127, 0x7fffff), pat (1, 126, 0x654321),
            pat (0, 130, 0), pat (1, 129, 0)
        };
        uint32_t stereo [STEREO_FRAMES * 2];
        int f;

        for (f = 0; f < STEREO_FRAMES; f++) {
            stereo [f * 2] = left_tile [f % TILE];
            stereo [f * 2 + 1] = right_tile [f % TILE];
        }

        api_roundtrip ("stereo", stereo, STEREO_FRAMES, 2, 0x3, 16, 7);
    }

    // ---- public API: six channels, several blocks, tiny reads ----
    // Each channel follows a different bit-pattern plan; all six plans are
    // tiled into 16-frame blocks so the categories recur in every block.
    {
        enum { TILE = 16, SIX_FRAMES = 33 };
        uint32_t tile [TILE][6];
        uint32_t six [SIX_FRAMES * 6];
        int f, c;

        for (f = 0; f < TILE; f++)
            for (c = 0; c < 6; c++)
                tile [f][c] = anchor_big;        // every block has its own anchor

        for (f = 1; f < TILE; f++) {
            tile [f][0] = audio [(f - 1) % (int)(sizeof audio / sizeof audio [0])];
            tile [f][1] = shift_sent [(f - 1) % (int)(sizeof shift_sent / sizeof shift_sent [0])];
            tile [f][2] = zeros [(f - 1) % (int)(sizeof zeros / sizeof zeros [0])];
            tile [f][3] = denormals [(f - 1) % (int)(sizeof denormals / sizeof denormals [0])];
            tile [f][4] = exceptions [(f - 1) % (int)(sizeof exceptions / sizeof exceptions [0])];
            tile [f][5] = shift_same [(f - 1) % (int)(sizeof shift_same / sizeof shift_same [0])];
        }

        // Pin the required categories at fixed positions in every tile.
        tile [1][1] = pat (0, 150, 0x000001);    // SHIFT_SENT low bits
        tile [2][2] = pat (1, 0, 0);             // negative zero
        tile [3][3] = pat (0, 0, 1);             // minimum denormal
        tile [4][4] = pat (0, 255, 0x7fc12345 & 0x7fffff);  // NaN payload
        tile [5][4] = pat (1, 255, 0);           // negative infinity
        tile [6][3] = pat (0, 0, 0x7fffff);      // maximum denormal

        for (f = 0; f < SIX_FRAMES; f++)
            for (c = 0; c < 6; c++)
                six [f * 6 + c] = tile [f % TILE][c];

        api_roundtrip ("six-channel", six, SIX_FRAMES, 6, 0x3f, 16, 5);
    }

    // ---- public API: a pure integer-precision file has no WVX chunk ----
    {
        enum { TILE = 16, INT_FRAMES = 33 };
        uint32_t ints [INT_FRAMES * 2];
        fw_blob blob;
        fw_container_report report;
        int f;

        for (f = 0; f < INT_FRAMES; f++) {
            ints [f * 2] = integer_only [f % (int)(sizeof integer_only / sizeof integer_only [0])];
            ints [f * 2 + 1] = integer_only [(f + 2) %
                (int)(sizeof integer_only / sizeof integer_only [0])];
        }

        fw_blob_init (&blob);
        TASK_CHECK (fw_pack_float_blob (ints, INT_FRAMES, 2, 0x3, TILE, &blob) == 0);
        TASK_CHECK (fw_report_container (&blob, &report) == 0);
        TASK_CHECK (report.nblocks >= 2);
        TASK_CHECK (report.wvx_blocks == 0);
        TASK_CHECK (report.float_flag_union == 0);
        fw_report_free (&report);
        fw_blob_free (&blob);
    }
}
