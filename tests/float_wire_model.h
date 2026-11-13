////////////////////////////////////////////////////////////////////////////
// float_wire_model.h
//
// Independent bit-level reference model for the WavPack 32-bit float
// sideband (the "wvx" extension stream and the FLOAT_INFO metadata).
//
// This module knows nothing about WavpackStream, production bitstreams or
// floating-point arithmetic: every input and output value is an IEEE-754
// binary32 *bit pattern* held in a uint32_t, and every sideband is a
// self-contained LSB-first bit array. The flag values match the on-disk
// format constants; they are documented here as literal values so that this
// header does not depend on the production sources.
////////////////////////////////////////////////////////////////////////////

#ifndef FLOAT_WIRE_MODEL_H
#define FLOAT_WIRE_MODEL_H

#include <stddef.h>
#include <stdint.h>

// IEEE-754 binary32 field widths and biases (bit positions only, no math).

#define FW_SIGN_BITS    1
#define FW_EXP_BITS     8
#define FW_MANT_BITS    23
#define FW_EXP_BIAS     127
#define FW_EXP_MAX      255     // reserved exponent for infinity and NaN

// On-disk float_flags byte values, identical to the production format.

#define FW_FLAG_SHIFT_ONES     1u    // shifted-out low bits are all ones
#define FW_FLAG_SHIFT_SAME     2u    // shifted-out bits are identical, 1 bit sent
#define FW_FLAG_SHIFT_SENT     4u    // shifted-out bits are sent literally
#define FW_FLAG_ZEROS_SENT     8u    // integer zeros carry sideband information
#define FW_FLAG_NEG_ZEROS      0x10u // at least one negative zero is present
#define FW_FLAG_EXCEPTIONS     0x20u // infinity / NaN values are present

// Metadata chunk id bytes (see wavpack.h ID_*).

#define FW_ID_FLOAT_INFO       0x8
#define FW_ID_WVX_BITSTREAM    0xc
#define FW_ID_WVX_NEW_BITSTREAM 0x2c  // ID_OPTIONAL_DATA (0x20) | 0xc
#define FW_ID_LARGE            0x80
#define FW_ID_ODD_SIZE         0x40

// Extract the three IEEE-754 fields from a binary32 bit pattern.

#define FW_SIGN(p)      ((unsigned)(((p) >> 31) & 1u))
#define FW_EXPONENT(p)  ((unsigned)(((p) >> 23) & 0xffu))
#define FW_MANTISSA(p)  ((uint32_t)((p) & 0x7fffffu))
#define FW_MAGNITUDE(p) ((uint32_t)((p) & 0x7fffffffu))

// Build a binary32 bit pattern from explicit fields (never uses float math).

static inline uint32_t fw_make_pattern (unsigned sign, unsigned exponent, uint32_t mantissa)
{
    return ((uint32_t)(sign & 1u) << 31) | ((uint32_t)(exponent & 0xffu) << 23) |
        (mantissa & 0x7fffffu);
}

// Growable LSB-first bit array. Bits are packed into bytes with the first
// emitted bit in the least-significant bit of the first byte, which is the
// same wire ordering the production bitstream uses.

typedef struct {
    unsigned char *data;
    size_t cap;         // allocated bytes
    size_t nbits;       // valid bits written
    size_t pos;         // read cursor in bits
} fw_bits;

void fw_bits_init (fw_bits *bits);
void fw_bits_free (fw_bits *bits);
void fw_bits_put (fw_bits *bits, int bit);
void fw_bits_putn (fw_bits *bits, uint32_t value, int nbits);
int fw_bits_get (fw_bits *bits);                 // -1 when exhausted
uint32_t fw_bits_getn (fw_bits *bits, int nbits);
size_t fw_bits_nbits (const fw_bits *bits);
size_t fw_bits_padded_bytes (const fw_bits *bits); // even byte count on the wire

// Result of independently scanning one block of binary32 bit patterns.

typedef struct {
    unsigned flags;         // FW_FLAG_* combination actually selected
    unsigned float_shift;   // trailing-zero reduction of the main integers
    unsigned max_exp;       // shared block exponent reference
    uint32_t crc_x;         // independently rolled sideband CRC
    int shifted_ones, shifted_zeros, shifted_both;
    int false_zeros, neg_zeros;
    int mag_steps;          // MAG_MASK/LSB magnitude steps of the integer data
} fw_scan_result;

// The CRC formula used for crc_x, rolled over raw bit patterns only.

uint32_t fw_crc_x (const uint32_t *patterns, int n);

// Independent equivalent of the production scan: fills "result" and writes
// the signed main integers (after every shift reduction) into "integers",
// which must hold at least n entries.

void fw_scan_float (const uint32_t *patterns, int n, fw_scan_result *result, int32_t *integers);

// Independently derives the exact sideband bit sequence that the production
// sender must emit for these patterns, given the shared scan parameters.

void fw_send_plan (const uint32_t *patterns, int n, unsigned flags, unsigned max_exp, fw_bits *out);

// Derives the sideband for the new WVX bitstream format, where the lowest
// min_shifted_zeros bits and any bits above max_shifted_ones are physically
// omitted. The caller guarantees the elision preconditions per value.

void fw_send_plan_elided (const uint32_t *patterns, int n, unsigned flags, unsigned max_exp,
    unsigned min_shifted_zeros, unsigned max_shifted_ones, fw_bits *out);

// Restores one binary32 pattern from its main integer through an open sideband
// reader, independently mirroring the production wvx decode rules (including
// the new-format min_shifted_zeros / max_shifted_ones elision). Returns the
// number of sideband bits consumed for this value.

int fw_restore_wvx (int32_t value, unsigned flags, unsigned float_shift, unsigned max_exp,
    unsigned min_shifted_zeros, unsigned max_shifted_ones, fw_bits *in, uint32_t *out_pattern);

// Restores one value for the no-sideband path (integer-only block or a block
// whose sender wrote no wvx chunk), mirroring the production nowvx rules.

uint32_t fw_restore_nowvx (int32_t value, unsigned flags, unsigned float_shift, unsigned max_exp);

#endif
