////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Independent float sideband reference model             //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// float_wire_model.h
//
// This header declares an independent model of the WavPack 32-bit float
// "sideband" (the WVX bitstream plus the FLOAT_INFO metadata). The model is
// written only in terms of explicit IEEE-754 sign/exponent/mantissa bit
// fields and its own LSB-first bit array. It never performs floating point
// arithmetic, never builds special values through the native float type, and
// never calls the production getbits/putbits macros or float conversion
// routines. Only numeric format constants (field widths, flag values) are
// shared with the production code.
//
// The model takes raw 32-bit bit patterns (uint32_t), so -0.0, NaN payloads
// and subnormals are always handled as exact patterns rather than numeric
// values. The same model drives both directions of the regression test:
//
//   - the writer side predicts what scan_float_data()/send_float_data() must
//     produce, bit by bit;
//   - the reader side predicts what float_values() must reconstruct from a
//     hand-built integer stream and WVX sideband.

#ifndef FLOAT_WIRE_MODEL_H
#define FLOAT_WIRE_MODEL_H

#include <stddef.h>
#include <stdint.h>

// Explicit IEEE-754 binary32 field extraction, expressed as bit operations.
// These mirror the production field macros but are defined locally so the
// reference stays independent of the implementation under test.

#define FWM_SIGN(p)         ((unsigned) ((p) >> 31) & 1u)
#define FWM_EXP(p)          ((unsigned) ((p) >> 23) & 0xffu)
#define FWM_MANT(p)         ((uint32_t) (p) & 0x7fffffu)
#define FWM_MAGNITUDE(p)    ((uint32_t) (p) & 0x7fffffffu)

#define FWM_EXP_INF_NAN     255     // reserved exponent for +/-infinity and NaN
#define FWM_MANT_BITS       23      // stored mantissa width
#define FWM_EXC_INTEGER     0x1000000u      // integer marker for inf/NaN (25th bit)
#define FWM_IMPLIED_ONE     0x800000u       // implied leading 1 of a normal value

// Sideband encoding scheme identifiers used by the model's plan. The numeric
// values intentionally match the production FLOAT_* flag bits so that model
// output can be compared byte-for-byte with the ID_FLOAT_INFO metadata.

#define FWM_SHIFT_ONES      1u
#define FWM_SHIFT_SAME      2u
#define FWM_SHIFT_SENT      4u
#define FWM_ZEROS_SENT      8u
#define FWM_NEG_ZEROS       0x10u
#define FWM_EXCEPTIONS      0x20u

// One sample's place in the shared sideband. Adjacent samples share one
// contiguous bit array, so each entry records the [bit_offset, bit_offset +
// bit_count) slice it consumes rather than realigning per sample.

typedef struct {
    uint32_t pattern;       // original IEEE-754 bit pattern
    int32_t  integer;       // signed 24-bit-ish main integer sent in the WV stream
    int      shift_count;   // bits aligned out against the block maximum
    int      bit_offset;    // offset of this sample's sideband, in bits
    int      bit_count;     // number of sideband bits this sample consumes
    int      kind;          // FWM_ITEM_* classification below
} fwm_item;

#define FWM_ITEM_NORMAL         0   // regular finite value, no sideband or shift bits
#define FWM_ITEM_SHIFT_SENT     1   // literal shifted mantissa bits
#define FWM_ITEM_SHIFT_SAME     2   // one "all shifted bits equal" bit
#define FWM_ITEM_SHIFT_ONES     3   // shifted bits implicitly all ones, no bits
#define FWM_ITEM_FALSE_ZERO     4   // integer vanished: full mantissa/exp/sign sent
#define FWM_ITEM_NEG_ZERO       5   // negative zero: marker + sign bit
#define FWM_ITEM_POS_ZERO       6   // positive zero (only a marker when ZEROS_SENT)
#define FWM_ITEM_INFINITY       7   // +/-infinity: single 0 marker
#define FWM_ITEM_NAN            8   // NaN: 1 marker + 23 mantissa payload bits

// The block-level plan, equivalent to the fields the production scanner
// stores in the WavpackStream and the ID_FLOAT_INFO chunk.

typedef struct {
    unsigned flags;             // FWM_* or-combination, matches float_flags
    int      max_exp;           // rounded exponent of the block maximum magnitude
    int      float_shift;       // trailing zero bits stripped from the integers
    uint32_t crc;               // independently computed crc_x seed/result
    int      wvx_needed;        // nonzero if a WVX sideband must be written
} fwm_plan;

// An LSB-first bit array. Bit 0 is the least significant bit of data[0],
// matching the Wavpack bitstream convention, but the implementation here is
// self-contained.

typedef struct {
    unsigned char *data;
    size_t         nbytes;      // physical bytes available (includes padding)
    size_t         capacity;
    int            bitpos;      // next bit to read or write
    int            error;       // set on overrun so callers can TASK_CHECK it
} fwm_bits;

void fwm_bits_init_read (fwm_bits *bs, const unsigned char *data, size_t nbytes);
void fwm_bits_init_write (fwm_bits *bs, unsigned char *data, size_t capacity);
void fwm_put_bit (fwm_bits *bs, int bit);
void fwm_put_bits (fwm_bits *bs, uint32_t value, int nbits);
int  fwm_get_bit (fwm_bits *bs);
uint32_t fwm_get_bits (fwm_bits *bs, int nbits);

// Build the complete block plan for "num_values" raw bit patterns, fill the
// per-sample items (including the signed main integers, written in-place into
// integers_out which must have num_values entries) and return the sideband
// bit count that send_float_data() is expected to produce. The returned value
// excludes byte-alignment padding.

int fwm_scan (const uint32_t *patterns, int num_values,
              int32_t *integers_out, fwm_item *items, fwm_plan *plan);

// Emit the predicted sideband for a planned block into a fresh bit array.
// Mirrors send_float_data() exactly, including the conditional exponent byte
// (present only when max_exp >= 25) and per-sample slice bookkeeping.

void fwm_send (const uint32_t *patterns, int num_values,
               fwm_item *items, const fwm_plan *plan, fwm_bits *out);

// Emit a sideband in the new WVX format convention, where the reader can
// recover runs of low zero bits (min_shifted_zeros) and high one bits
// (max_shifted_ones) without them appearing on the wire. Used only for the
// handcrafted materials; the production encoder does not write this layout
// for float data. Returns the number of valid bits written.

int fwm_send_windowed (const uint32_t *patterns, int num_values,
                       const fwm_item *items, const fwm_plan *plan,
                       int min_zeros, int max_ones, fwm_bits *out);

// Reconstruct bit patterns from main integers plus a sideband using the
// reader rules of float_values(). "min_zeros"/"max_ones" are the new-format
// WVX fields (zero for the legacy ID_WVX_BITSTREAM layout). The running CRC is
// updated exactly as the production decoder does and returned via crc_io.

void fwm_restore (const int32_t *integers, int num_values,
                  const fwm_plan *plan, int min_zeros, int max_ones,
                  fwm_bits *in, uint32_t *patterns_out, uint32_t *crc_io);

// Integer-only variant of fwm_restore() for blocks that carry no WVX
// sideband at all (equivalent to float_values_nowvx()).

void fwm_restore_nowvx (const int32_t *integers, int num_values,
                        const fwm_plan *plan, uint32_t *patterns_out, uint32_t *crc_io);

// Convenience: assemble an IEEE-754 pattern from explicit fields without any
// floating point arithmetic.

uint32_t fwm_make (unsigned sign, unsigned exp, uint32_t mantissa);

#endif
