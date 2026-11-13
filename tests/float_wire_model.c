////////////////////////////////////////////////////////////////////////////
// float_wire_model.c
//
// Independent bit-level reference for the WavPack float sideband. Every
// rule here is re-derived from the on-disk format description and operates
// on explicit IEEE-754 fields plus an LSB-first bit array. It deliberately
// does not include any production header, bitstream macro or float cast.
////////////////////////////////////////////////////////////////////////////

#include "float_wire_model.h"

#include <stdlib.h>
#include <string.h>

// ----------------------------- LSB-first bit array -----------------------------

void fw_bits_init (fw_bits *bits)
{
    bits->cap = 64;
    bits->data = (unsigned char *) malloc (bits->cap);
    bits->nbits = bits->pos = 0;
    memset (bits->data, 0, bits->cap);
}

void fw_bits_free (fw_bits *bits)
{
    free (bits->data);
    bits->data = NULL;
    bits->cap = bits->nbits = bits->pos = 0;
}

static void fw_bits_ensure (fw_bits *bits, size_t need_bytes)
{
    size_t old_cap;

    if (need_bytes <= bits->cap)
        return;

    old_cap = bits->cap;

    while (bits->cap < need_bytes)
        bits->cap *= 2;

    bits->data = (unsigned char *) realloc (bits->data, bits->cap);
    memset (bits->data + old_cap, 0, bits->cap - old_cap);
}

void fw_bits_put (fw_bits *bits, int bit)
{
    size_t byte_index = bits->nbits / 8;
    unsigned bit_index = (unsigned)(bits->nbits % 8);

    fw_bits_ensure (bits, byte_index + 1);

    if (bit)
        bits->data [byte_index] |= (unsigned char)(1u << bit_index);
    else if (bit_index == 0)
        bits->data [byte_index] = 0;

    bits->nbits++;
}

void fw_bits_putn (fw_bits *bits, uint32_t value, int nbits)
{
    int i;

    for (i = 0; i < nbits; i++)
        fw_bits_put (bits, (int)((value >> i) & 1u));
}

int fw_bits_get (fw_bits *bits)
{
    size_t byte_index;
    unsigned bit_index;

    if (bits->pos >= bits->nbits)
        return -1;

    byte_index = bits->pos / 8;
    bit_index = (unsigned)(bits->pos % 8);
    bits->pos++;
    return (bits->data [byte_index] >> bit_index) & 1;
}

uint32_t fw_bits_getn (fw_bits *bits, int nbits)
{
    uint32_t value = 0;
    int i;

    for (i = 0; i < nbits; i++) {
        int bit = fw_bits_get (bits);

        if (bit < 0)
            break;

        value |= (uint32_t) bit << i;
    }

    return value;
}

size_t fw_bits_nbits (const fw_bits *bits)
{
    return bits->nbits;
}

size_t fw_bits_padded_bytes (const fw_bits *bits)
{
    size_t bytes = (bits->nbits + 7) / 8;

    if (bytes & 1)       // the wire stores an even byte count
        bytes++;

    return bytes;
}

// ----------------------------- shared block CRC -----------------------------

uint32_t fw_crc_x (const uint32_t *patterns, int n)
{
    uint32_t crc = 0xffffffffu;
    int i;

    for (i = 0; i < n; i++) {
        uint32_t p = patterns [i];
        crc = crc * 27u + FW_MANTISSA (p) * 9u + FW_EXPONENT (p) * 3u + FW_SIGN (p);
    }

    return crc;
}

// Per-value integer projection, re-derived from the field layout.
// Returns the unsigned magnitude before the sign is applied, and reports
// the nominal shift count plus whether the value is an IEEE exception.

static uint32_t fw_project (uint32_t pattern, unsigned max_exp, int *shift_count, int *is_exception)
{
    unsigned exponent = FW_EXPONENT (pattern);
    uint32_t mantissa = FW_MANTISSA (pattern);
    uint32_t value;

    *is_exception = (exponent == FW_EXP_MAX);

    if (*is_exception) {
        // Infinity / NaN are marked with one more than 24 unsigned bits.
        *shift_count = 0;
        return 0x1000000u;
    }

    if (exponent) {
        // Normal value: restore the implied '1' at bit 23 and align to the
        // largest finite magnitude in the block.
        *shift_count = (int) max_exp - (int) exponent;
        value = 0x800000u + mantissa;
    }
    else {
        // Zero exponent: +/- zero or a denormal, which has no implied '1'.
        *shift_count = max_exp ? (int) max_exp - 1 : 0;
        value = mantissa;
    }

    if (*shift_count < 25)
        value >>= *shift_count;
    else
        value = 0;

    return value;
}

// ----------------------------- independent scan -----------------------------

void fw_scan_float (const uint32_t *patterns, int n, fw_scan_result *result, int32_t *integers)
{
    int shifted_ones = 0, shifted_zeros = 0, shifted_both = 0;
    int false_zeros = 0, neg_zeros = 0;
    uint32_t ordata = 0;
    uint32_t max_mag = 0;
    unsigned max_exp = 0;
    int i;

    memset (result, 0, sizeof (*result));

    // First pass: CRC plus the largest finite magnitude (exponent 255 is
    // reserved for inf / NaN and never participates).
    for (i = 0; i < n; i++) {
        uint32_t mag = FW_MAGNITUDE (patterns [i]);

        if (FW_EXPONENT (patterns [i]) < FW_EXP_MAX && mag > max_mag)
            max_mag = mag;
    }

    result->crc_x = fw_crc_x (patterns, n);

    // Round the magnitude up so the projected integers stay within 24 bits.
    if (FW_EXPONENT (max_mag))
        max_exp = FW_EXPONENT (max_mag + 0x7f0000u);

    for (i = 0; i < n; i++) {
        uint32_t pattern = patterns [i];
        unsigned exponent = FW_EXPONENT (pattern);
        uint32_t mantissa = FW_MANTISSA (pattern);
        int shift_count, is_exception;
        uint32_t value = fw_project (pattern, max_exp, &shift_count, &is_exception);

        if (is_exception)
            result->flags |= FW_FLAG_EXCEPTIONS;

        if (!value) {
            // An integer zero is a "false" zero when the pattern held real
            // bits that the alignment shifted out, a negative zero when the
            // pattern is exactly -0.0, and a plain zero otherwise.
            if (exponent || mantissa)
                false_zeros++;
            else if (FW_SIGN (pattern))
                neg_zeros++;
        }
        else if (shift_count) {
            uint32_t mask = (1u << shift_count) - 1;
            uint32_t low = mantissa & mask;

            if (!low)
                shifted_zeros++;
            else if (low == mask)
                shifted_ones++;
            else
                shifted_both++;
        }

        ordata |= value;
        integers [i] = FW_SIGN (pattern) ? -(int32_t) value : (int32_t) value;
    }

    result->max_exp = max_exp;

    // Select the scheme for the bits shifted out of the main integers.
    if (shifted_both)
        result->flags |= FW_FLAG_SHIFT_SENT;
    else if (shifted_ones && !shifted_zeros)
        result->flags |= FW_FLAG_SHIFT_ONES;
    else if (shifted_ones && shifted_zeros)
        result->flags |= FW_FLAG_SHIFT_SAME;
    else if (ordata && !(ordata & 1u)) {
        // Only zero bits were lost: shrink the integers themselves.
        unsigned float_shift = 0;

        while (!(ordata & 1u)) {
            float_shift++;
            ordata >>= 1;
        }

        result->float_shift = float_shift;

        for (i = 0; i < n; i++)
            integers [i] >>= float_shift;
    }

    // The MAG field records the actual integer width after the reduction.
    {
        uint32_t mag = ordata;
        int steps = 0;

        while (mag) {
            steps++;
            mag >>= 1;
        }

        result->mag_steps = steps;
    }

    if (false_zeros || neg_zeros)
        result->flags |= FW_FLAG_ZEROS_SENT;

    if (neg_zeros)
        result->flags |= FW_FLAG_NEG_ZEROS;

    result->shifted_ones = shifted_ones;
    result->shifted_zeros = shifted_zeros;
    result->shifted_both = shifted_both;
    result->false_zeros = false_zeros;
    result->neg_zeros = neg_zeros;
}

// ----------------------------- independent sender plan -----------------------------

void fw_send_plan_elided (const uint32_t *patterns, int n, unsigned flags, unsigned max_exp,
    unsigned min_shifted_zeros, unsigned max_shifted_ones, fw_bits *out)
{
    int i;

    for (i = 0; i < n; i++) {
        uint32_t pattern = patterns [i];
        unsigned exponent = FW_EXPONENT (pattern);
        uint32_t mantissa = FW_MANTISSA (pattern);
        unsigned sign = FW_SIGN (pattern);
        int shift_count, is_exception;
        uint32_t value = fw_project (pattern, max_exp, &shift_count, &is_exception);

        if (is_exception) {
            if (mantissa) {
                fw_bits_put (out, 1);                 // NaN: full mantissa follows
                fw_bits_putn (out, mantissa, FW_MANT_BITS);
            }
            else
                fw_bits_put (out, 0);                 // infinity: marker bit only
        }

        if (!value) {
            if (flags & FW_FLAG_ZEROS_SENT) {
                if (exponent || mantissa) {
                    // False zero: the significant fields travel literally.
                    fw_bits_put (out, 1);
                    fw_bits_putn (out, mantissa, FW_MANT_BITS);

                    if (max_exp >= 25)
                        fw_bits_putn (out, exponent, FW_EXP_BITS);

                    fw_bits_put (out, (int) sign);
                }
                else {
                    // Real zero: one marker bit, plus the sign only when the
                    // block contains at least one negative zero.
                    fw_bits_put (out, 0);

                    if (flags & FW_FLAG_NEG_ZEROS)
                        fw_bits_put (out, (int) sign);
                }
            }
        }
        else if (shift_count) {
            if (flags & FW_FLAG_SHIFT_SENT) {
                uint32_t low = mantissa & ((1u << shift_count) - 1);
                int num_zeros = 0;

                // Mirror the receiver's elision arithmetic exactly. With the
                // legacy header values (0, 0) nothing is removed.
                if (max_shifted_ones && shift_count > (int) max_shifted_ones)
                    num_zeros = shift_count - (int) max_shifted_ones;

                if ((int) min_shifted_zeros > num_zeros)
                    num_zeros = ((int) min_shifted_zeros > shift_count) ? shift_count :
                        (int) min_shifted_zeros;

                fw_bits_putn (out, low >> num_zeros, shift_count - num_zeros);
            }
            else if (flags & FW_FLAG_SHIFT_SAME)
                fw_bits_put (out, (int)(mantissa & 1u));
        }
    }
}

void fw_send_plan (const uint32_t *patterns, int n, unsigned flags, unsigned max_exp, fw_bits *out)
{
    fw_send_plan_elided (patterns, n, flags, max_exp, 0, 0, out);
}

// ----------------------------- independent receiver -----------------------------
int fw_restore_wvx (int32_t signed_value, unsigned flags, unsigned float_shift, unsigned max_exp,
    unsigned min_shifted_zeros, unsigned max_shifted_ones, fw_bits *in, uint32_t *out_pattern)
{
    size_t start = in->pos;
    uint32_t value = (uint32_t) signed_value;
    uint32_t pattern = 0;
    int shift_count = 0;
    unsigned exponent = max_exp;
    unsigned sign = 0;

    if (!value) {
        if (flags & FW_FLAG_ZEROS_SENT) {
            int marker = fw_bits_get (in);

            if (marker > 0) {
                uint32_t mantissa = fw_bits_getn (in, FW_MANT_BITS);
                unsigned restored_exp;

                pattern |= mantissa;

                if (max_exp >= 25)
                    restored_exp = fw_bits_getn (in, FW_EXP_BITS);
                else
                    restored_exp = 0;

                pattern |= (uint32_t) restored_exp << 23;
                sign = (unsigned) fw_bits_get (in);
                pattern |= (uint32_t) sign << 31;
                *out_pattern = pattern;
                return (int)(in->pos - start);
            }

            if (flags & FW_FLAG_NEG_ZEROS) {
                sign = (unsigned) fw_bits_get (in);
                pattern |= (uint32_t) sign << 31;
            }
        }

        *out_pattern = pattern;       // positive or negative zero
        return (int)(in->pos - start);
    }

    // Undo the integer-width reduction first.
    value <<= (float_shift & 0x1f);

    if ((int32_t) value < 0) {
        value = (uint32_t)(-(int32_t) value);
        sign = 1;
    }

    if (value == 0x1000000u) {
        // Exception marker: infinity, or NaN with its payload following.
        int is_nan = fw_bits_get (in);

        if (is_nan > 0) {
            uint32_t mantissa = fw_bits_getn (in, FW_MANT_BITS);
            pattern |= mantissa;
        }

        pattern |= (uint32_t) FW_EXP_MAX << 23;
        pattern |= (uint32_t) sign << 31;
        *out_pattern = pattern;
        return (int)(in->pos - start);
    }

    // Recover the exponent by re-aligning the implied '1' to bit 23.
    if (exponent)
        while (!(value & 0x800000u) && --exponent) {
            shift_count++;
            value <<= 1;
        }

    shift_count &= 0x1f;

    if (shift_count) {
        if ((flags & FW_FLAG_SHIFT_ONES) ||
            ((flags & FW_FLAG_SHIFT_SAME) && fw_bits_get (in) > 0))
            value |= (1u << shift_count) - 1;
        else if (flags & FW_FLAG_SHIFT_SENT) {
            uint32_t mask = (1u << shift_count) - 1;
            int num_zeros = 0;
            int bits_to_read;

            // New WVX format: runs of known low zeros / high ones are elided.
            if (max_shifted_ones && shift_count > (int) max_shifted_ones)
                num_zeros = shift_count - (int) max_shifted_ones;

            if ((int) min_shifted_zeros > num_zeros)
                num_zeros = ((int) min_shifted_zeros > shift_count) ? shift_count : (int) min_shifted_zeros;

            bits_to_read = shift_count - num_zeros;

            if (bits_to_read > 0) {
                uint32_t sent = fw_bits_getn (in, bits_to_read);
                value |= (sent << num_zeros) & mask;
            }
        }
    }

    pattern |= value & 0x7fffffu;
    pattern |= (uint32_t) exponent << 23;
    pattern |= (uint32_t) sign << 31;
    *out_pattern = pattern;
    return (int)(in->pos - start);
}

uint32_t fw_restore_nowvx (int32_t signed_value, unsigned flags, unsigned float_shift, unsigned max_exp)
{
    uint32_t value = (uint32_t) signed_value;
    uint32_t pattern = 0;
    int shift_count = 0;
    unsigned exponent = max_exp;
    unsigned sign = 0;

    if (!value)
        return 0;

    value <<= (float_shift & 0x1f);

    if ((int32_t) value < 0) {
        value = (uint32_t)(-(int32_t) value);
        sign = 1;
    }

    if (value >= 0x1000000u) {
        // No sideband available: an exception marker degrades by consuming
        // the extra magnitude through exponent steps.
        while (value & 0xf000000u) {
            value >>= 1;
            exponent++;
        }
    }
    else if (exponent) {
        while (!(value & 0x800000u) && --exponent) {
            shift_count++;
            value <<= 1;
        }

        shift_count &= 0x1f;

        if (shift_count && (flags & FW_FLAG_SHIFT_ONES))
            value |= (1u << shift_count) - 1;
    }

    pattern |= value & 0x7fffffu;
    pattern |= (uint32_t) exponent << 23;
    pattern |= (uint32_t) sign << 31;
    return pattern;
}
