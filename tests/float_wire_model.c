////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Independent float sideband reference model             //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// float_wire_model.c
//
// Independent, bit-exact reference for the WavPack 32-bit float sideband.
// Everything here operates on explicit IEEE-754 fields and on the model's
// own LSB-first bit array. There is no floating point arithmetic and no call
// into the production bitstream or float code; the production source is only
// read by humans to derive the rules encoded below.

#include "float_wire_model.h"

#include <string.h>

//////////////////////////////////// bit array ////////////////////////////////

void fwm_bits_init_read (fwm_bits *bs, const unsigned char *data, size_t nbytes)
{
    bs->data = (unsigned char *) data;
    bs->nbytes = nbytes;
    bs->capacity = nbytes;
    bs->bitpos = 0;
    bs->error = 0;
}

void fwm_bits_init_write (fwm_bits *bs, unsigned char *data, size_t capacity)
{
    memset (data, 0, capacity);
    bs->data = data;
    bs->nbytes = 0;
    bs->capacity = capacity;
    bs->bitpos = 0;
    bs->error = 0;
}

void fwm_put_bit (fwm_bits *bs, int bit)
{
    size_t byte = (size_t) bs->bitpos >> 3;
    unsigned mask = 1u << (bs->bitpos & 7);

    if (byte >= bs->capacity) {
        bs->error = 1;
        return;
    }

    if (bit)
        bs->data [byte] |= (unsigned char) mask;

    bs->bitpos++;
    bs->nbytes = (size_t) ((bs->bitpos + 7) >> 3);
}

void fwm_put_bits (fwm_bits *bs, uint32_t value, int nbits)
{
    int i;

    for (i = 0; i < nbits; i++)
        fwm_put_bit (bs, (int) ((value >> i) & 1u));
}

int fwm_get_bit (fwm_bits *bs)
{
    size_t byte = (size_t) bs->bitpos >> 3;
    int bit;

    if (byte >= bs->nbytes) {
        bs->error = 1;
        return 0;
    }

    bit = (bs->data [byte] >> (bs->bitpos & 7)) & 1;
    bs->bitpos++;
    return bit;
}

uint32_t fwm_get_bits (fwm_bits *bs, int nbits)
{
    uint32_t value = 0;
    int i;

    for (i = 0; i < nbits; i++)
        value |= (uint32_t) fwm_get_bit (bs) << i;

    return value;
}

uint32_t fwm_make (unsigned sign, unsigned exp, uint32_t mantissa)
{
    return ((sign & 1u) << 31) | ((exp & 0xffu) << 23) | (mantissa & 0x7fffffu);
}

//////////////////////////////////// scanner //////////////////////////////////

// Reproduces scan_float_data() from pack_floats.c using only the explicit
// field helpers. See the production comments for the rationale of each step.

int fwm_scan (const uint32_t *patterns, int num_values,
              int32_t *integers_out, fwm_item *items, fwm_plan *plan)
{
    int32_t shifted_ones = 0, shifted_zeros = 0, shifted_both = 0;
    int32_t false_zeros = 0, neg_zeros = 0;
    uint32_t ordata = 0, crc = 0xffffffffu;
    uint32_t max_mag = 0;
    int max_exp = 0, i;

    memset (plan, 0, sizeof (*plan));

    for (i = 0; i < num_values; i++) {
        uint32_t p = patterns [i];
        crc = crc * 27u + FWM_MANT (p) * 9u + FWM_EXP (p) * 3u + FWM_SIGN (p);

        if (FWM_EXP (p) < FWM_EXP_INF_NAN && FWM_MAGNITUDE (p) > max_mag)
            max_mag = FWM_MAGNITUDE (p);
    }

    plan->crc = crc;

    if (FWM_EXP (max_mag))
        max_exp = FWM_EXP (max_mag + 0x7f0000u);

    plan->max_exp = max_exp;

    for (i = 0; i < num_values; i++) {
        uint32_t p = patterns [i];
        unsigned exp = FWM_EXP (p), mant = FWM_MANT (p), sign = FWM_SIGN (p);
        int shift_count;
        uint32_t value;

        items [i].pattern = p;
        items [i].bit_offset = 0;
        items [i].bit_count = 0;

        if (exp == FWM_EXP_INF_NAN) {
            plan->flags |= FWM_EXCEPTIONS;
            items [i].kind = mant ? FWM_ITEM_NAN : FWM_ITEM_INFINITY;
            value = FWM_EXC_INTEGER;
            shift_count = 0;
        }
        else if (exp) {
            shift_count = max_exp - (int) exp;
            value = FWM_IMPLIED_ONE + mant;
            items [i].kind = FWM_ITEM_NORMAL;
        }
        else {
            shift_count = max_exp ? max_exp - 1 : 0;
            value = mant;
            items [i].kind = FWM_ITEM_NORMAL;
        }

        items [i].shift_count = shift_count;

        if (shift_count < 25)
            value >>= shift_count;
        else
            value = 0;

        if (!value) {
            if (exp || mant) {
                items [i].kind = FWM_ITEM_FALSE_ZERO;
                ++false_zeros;
            }
            else if (sign) {
                items [i].kind = FWM_ITEM_NEG_ZERO;
                ++neg_zeros;
            }
            else
                items [i].kind = FWM_ITEM_POS_ZERO;
        }
        else if (shift_count) {
            uint32_t mask = (1u << shift_count) - 1;
            uint32_t low = mant & mask;

            if (!low) {
                shifted_zeros++;
                items [i].kind = FWM_ITEM_NORMAL;      // scheme decided block-wide
            }
            else if (low == mask) {
                shifted_ones++;
                items [i].kind = FWM_ITEM_SHIFT_ONES;
            }
            else {
                shifted_both++;
                items [i].kind = FWM_ITEM_SHIFT_SENT;
            }
        }

        ordata |= value;
        integers_out [i] = sign ? -(int32_t) value : (int32_t) value;
        items [i].integer = integers_out [i];
    }

    if (shifted_both)
        plan->flags |= FWM_SHIFT_SENT;
    else if (shifted_ones && !shifted_zeros)
        plan->flags |= FWM_SHIFT_ONES;
    else if (shifted_ones && shifted_zeros)
        plan->flags |= FWM_SHIFT_SAME;
    else if (ordata && !(ordata & 1u)) {
        while (!(ordata & 1u)) {
            plan->float_shift++;
            ordata >>= 1;
        }

        for (i = 0; i < num_values; i++) {
            integers_out [i] >>= plan->float_shift;
            items [i].integer = integers_out [i];
        }
    }

    if (false_zeros || neg_zeros)
        plan->flags |= FWM_ZEROS_SENT;

    if (neg_zeros)
        plan->flags |= FWM_NEG_ZEROS;

    // The shift scheme is a block-wide decision, so reclassify every shifted
    // nonzero item to the scheme actually selected. Under SHIFT_SENT the
    // all-zero low parts are still written literally, under SHIFT_SAME every
    // item contributes one bit, and under SHIFT_ONES nothing is written.

    if (plan->flags & (FWM_SHIFT_SENT | FWM_SHIFT_SAME | FWM_SHIFT_ONES)) {
        int scheme_kind = (plan->flags & FWM_SHIFT_SENT) ? FWM_ITEM_SHIFT_SENT :
            (plan->flags & FWM_SHIFT_SAME) ? FWM_ITEM_SHIFT_SAME : FWM_ITEM_SHIFT_ONES;

        for (i = 0; i < num_values; i++)
            if (items [i].shift_count && items [i].integer != 0 &&
                (items [i].kind == FWM_ITEM_NORMAL || items [i].kind == FWM_ITEM_SHIFT_ONES))
                items [i].kind = scheme_kind;
    }

    // Per-item sideband widths, matching send_float_data(). Zero entries only
    // consume bits when FLOAT_ZEROS_SENT is selected; the zero marker carries
    // a sign bit for every zero entry when the block has negative zeros, not
    // just for the negative ones.

    for (i = 0; i < num_values; i++) {
        switch (items [i].kind) {
            case FWM_ITEM_NAN:
                items [i].bit_count = 1 + FWM_MANT_BITS;
                break;
            case FWM_ITEM_INFINITY:
                items [i].bit_count = 1;
                break;
            case FWM_ITEM_FALSE_ZERO:
                items [i].bit_count = (plan->flags & FWM_ZEROS_SENT) ?
                    1 + FWM_MANT_BITS + (max_exp >= 25 ? 8 : 0) + 1 : 0;
                break;
            case FWM_ITEM_NEG_ZERO:
            case FWM_ITEM_POS_ZERO:
                items [i].bit_count = (plan->flags & FWM_ZEROS_SENT) ?
                    1 + ((plan->flags & FWM_NEG_ZEROS) ? 1 : 0) : 0;
                break;
            case FWM_ITEM_SHIFT_SENT:
                items [i].bit_count = items [i].shift_count;
                break;
            case FWM_ITEM_SHIFT_SAME:
                items [i].bit_count = 1;
                break;
            default:
                items [i].bit_count = 0;
                break;
        }
    }

    plan->wvx_needed = !!(plan->flags &
        (FWM_EXCEPTIONS | FWM_ZEROS_SENT | FWM_SHIFT_SENT | FWM_SHIFT_SAME));

    {
        int total = 0;

        for (i = 0; i < num_values; i++)
            total += items [i].bit_count;

        return total;
    }
}

//////////////////////////////////// sideband writer //////////////////////////

void fwm_send (const uint32_t *patterns, int num_values,
               fwm_item *items, const fwm_plan *plan, fwm_bits *out)
{
    int max_exp = plan->max_exp;
    int i;

    for (i = 0; i < num_values; i++) {
        uint32_t p = patterns [i];
        unsigned exp = FWM_EXP (p), mant = FWM_MANT (p), sign = FWM_SIGN (p);

        items [i].bit_offset = out->bitpos;     // shared stream, no realignment

        if (exp == FWM_EXP_INF_NAN) {
            if (mant) {
                fwm_put_bit (out, 1);
                fwm_put_bits (out, mant, FWM_MANT_BITS);
            }
            else
                fwm_put_bit (out, 0);
        }
        else {
            int shift_count, value;

            if (exp) {
                shift_count = max_exp - (int) exp;
                value = (int) (FWM_IMPLIED_ONE + mant);
            }
            else {
                shift_count = max_exp ? max_exp - 1 : 0;
                value = (int) mant;
            }

            value = (shift_count < 25) ? value >> shift_count : 0;

            if (!value) {
                if (plan->flags & FWM_ZEROS_SENT) {
                    if (exp || mant) {
                        fwm_put_bit (out, 1);
                        fwm_put_bits (out, mant, FWM_MANT_BITS);

                        if (max_exp >= 25)
                            fwm_put_bits (out, exp, 8);

                        fwm_put_bit (out, (int) sign);
                    }
                    else {
                        fwm_put_bit (out, 0);

                        if (plan->flags & FWM_NEG_ZEROS)
                            fwm_put_bit (out, (int) sign);
                    }
                }
            }
            else if (shift_count) {
                if (plan->flags & FWM_SHIFT_SENT)
                    fwm_put_bits (out, mant & ((1u << shift_count) - 1), shift_count);
                else if (plan->flags & FWM_SHIFT_SAME)
                    fwm_put_bit (out, (int) (mant & 1u));
            }
        }
    }
}

// Windowed variant of fwm_send() for the handcrafted new-format materials.
// The zero/one omission windows are applied exactly as float_values() inverts
// them, so the bits actually written are the middle portion of each shifted
// mantissa only. Every other item uses the identical encoding as fwm_send().

int fwm_send_windowed (const uint32_t *patterns, int num_values,
                       const fwm_item *items, const fwm_plan *plan,
                       int min_zeros, int max_ones, fwm_bits *out)
{
    int max_exp = plan->max_exp;
    int i;

    for (i = 0; i < num_values; i++) {
        uint32_t p = patterns [i];
        unsigned exp = FWM_EXP (p), mant = FWM_MANT (p), sign = FWM_SIGN (p);

        if (exp == FWM_EXP_INF_NAN) {
            if (mant) {
                fwm_put_bit (out, 1);
                fwm_put_bits (out, mant, FWM_MANT_BITS);
            }
            else
                fwm_put_bit (out, 0);
        }
        else {
            int shift_count, value;

            if (exp) {
                shift_count = max_exp - (int) exp;
                value = (int) (FWM_IMPLIED_ONE + mant);
            }
            else {
                shift_count = max_exp ? max_exp - 1 : 0;
                value = (int) mant;
            }

            value = (shift_count < 25) ? value >> shift_count : 0;

            if (!value) {
                if (plan->flags & FWM_ZEROS_SENT) {
                    if (exp || mant) {
                        fwm_put_bit (out, 1);
                        fwm_put_bits (out, mant, FWM_MANT_BITS);

                        if (max_exp >= 25)
                            fwm_put_bits (out, exp, 8);

                        fwm_put_bit (out, (int) sign);
                    }
                    else {
                        fwm_put_bit (out, 0);

                        if (plan->flags & FWM_NEG_ZEROS)
                            fwm_put_bit (out, (int) sign);
                    }
                }
            }
            else if (shift_count && (plan->flags & FWM_SHIFT_SENT)) {
                uint32_t low = mant & ((1u << shift_count) - 1);
                int num_zeros = 0, bits_to_read;

                if (max_ones && shift_count > max_ones)
                    num_zeros = shift_count - max_ones;

                if (min_zeros > num_zeros)
                    num_zeros = (min_zeros > shift_count) ? shift_count : min_zeros;

                bits_to_read = shift_count - num_zeros;

                if (bits_to_read > 0)
                    fwm_put_bits (out, low >> num_zeros, bits_to_read);
            }
            else if (shift_count && (plan->flags & FWM_SHIFT_SAME))
                fwm_put_bit (out, (int) (mant & 1u));
        }
    }

    (void) items;
    return out->bitpos;
}

//////////////////////////////////// sideband reader //////////////////////////

// Reader rules of float_values() in unpack_floats.c, including the new-format
// min_shifted_zeros / max_shifted_ones omission windows.

void fwm_restore (const int32_t *integers, int num_values,
                  const fwm_plan *plan, int min_zeros, int max_ones,
                  fwm_bits *in, uint32_t *patterns_out, uint32_t *crc_io)
{
    uint32_t crc = *crc_io;
    int i;

    for (i = 0; i < num_values; i++) {
        int32_t raw = integers [i];
        uint32_t v, pattern = 0;
        unsigned sign = 0, exp = 0, mant = 0;

        if (raw == 0) {
            if (plan->flags & FWM_ZEROS_SENT) {
                if (fwm_get_bit (in)) {
                    mant = fwm_get_bits (in, FWM_MANT_BITS);

                    if (plan->max_exp >= 25)
                        exp = fwm_get_bits (in, 8);

                    sign = (unsigned) fwm_get_bit (in);
                    pattern = fwm_make (sign, exp, mant);
                }
                else if (plan->flags & FWM_NEG_ZEROS) {
                    sign = (unsigned) fwm_get_bit (in);
                    pattern = fwm_make (sign, 0, 0);
                }
            }
        }
        else {
            int shift_count = 0;

            v = (uint32_t) raw << (plan->float_shift & 0x1f);

            if ((int32_t) v < 0) {
                v = (uint32_t) (-(int32_t) v);
                sign = 1;
            }

            if (v == FWM_EXC_INTEGER) {
                if (fwm_get_bit (in))
                    mant = fwm_get_bits (in, FWM_MANT_BITS);

                pattern = fwm_make (sign, FWM_EXP_INF_NAN, mant);
            }
            else {
                exp = (unsigned) plan->max_exp;

                if (exp)
                    while (!(v & FWM_IMPLIED_ONE) && --exp) {
                        shift_count++;
                        v <<= 1;
                    }

                if ((shift_count &= 0x1f)) {
                    if ((plan->flags & FWM_SHIFT_ONES) ||
                        ((plan->flags & FWM_SHIFT_SAME) && fwm_get_bit (in)))
                        v |= (1u << shift_count) - 1;
                    else if (plan->flags & FWM_SHIFT_SENT) {
                        uint32_t mask = (1u << shift_count) - 1;
                        int num_zeros = 0;

                        if (max_ones && shift_count > max_ones)
                            num_zeros = shift_count - max_ones;

                        if (min_zeros > num_zeros)
                            num_zeros = (min_zeros > shift_count) ? shift_count : min_zeros;

                        shift_count -= num_zeros;

                        if (shift_count > 0) {
                            uint32_t temp = fwm_get_bits (in, shift_count);
                            v |= (temp << num_zeros) & mask;
                        }
                    }
                }

                pattern = fwm_make (sign, exp, v & 0x7fffffu);
            }
        }

        patterns_out [i] = pattern;
        crc = crc * 27u + FWM_MANT (pattern) * 9u +
            FWM_EXP (pattern) * 3u + FWM_SIGN (pattern);
    }

    *crc_io = crc;
}

// Reader rules of float_values_nowvx(): integer-only float blocks and the
// implicit SHIFT_ONES fill, with no sideband present.

void fwm_restore_nowvx (const int32_t *integers, int num_values,
                        const fwm_plan *plan, uint32_t *patterns_out, uint32_t *crc_io)
{
    uint32_t crc = *crc_io;
    int i;

    for (i = 0; i < num_values; i++) {
        int32_t raw = integers [i];
        uint32_t v, pattern = 0;
        unsigned sign = 0, exp = (unsigned) plan->max_exp;
        int shift_count = 0;

        if (raw) {
            v = (uint32_t) raw << (plan->float_shift & 0x1f);

            if ((int32_t) v < 0) {
                v = (uint32_t) (-(int32_t) v);
                sign = 1;
            }

            if (v >= FWM_EXC_INTEGER) {
                while (v & 0xf000000u) {
                    v >>= 1;
                    ++exp;
                }
            }
            else if (exp) {
                while (!(v & FWM_IMPLIED_ONE) && --exp) {
                    shift_count++;
                    v <<= 1;
                }

                if ((shift_count &= 0x1f) && (plan->flags & FWM_SHIFT_ONES))
                    v |= (1u << shift_count) - 1;
            }

            pattern = fwm_make (sign, exp, v & 0x7fffffu);
        }

        patterns_out [i] = pattern;
        crc = crc * 27u + FWM_MANT (pattern) * 9u +
            FWM_EXP (pattern) * 3u + FWM_SIGN (pattern);
    }

    *crc_io = crc;
}
