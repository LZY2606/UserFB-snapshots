#ifndef BCDEC_TRANSFORM_H
#define BCDEC_TRANSFORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Geometric pixel transforms for a single 4x4 BC7 block.
 *
 * Coordinates follow the usual top-left origin.  For a source texel (x, y),
 * BCDEC_ROTATE_90 maps it to (3 - y, x), BCDEC_FLIP_X maps it to (3 - x, y),
 * and BCDEC_TRANSPOSE maps it to (y, x).
 */
typedef enum bcdec_transform_op
{
    BCDEC_IDENTITY = 0,
    BCDEC_ROTATE_90,
    BCDEC_ROTATE_180,
    BCDEC_ROTATE_270,
    BCDEC_FLIP_X,
    BCDEC_FLIP_Y,
    BCDEC_TRANSPOSE,
    BCDEC_ANTI_TRANSPOSE
} bcdec_transform_op;

typedef enum bcdec_transform_status
{
    BCDEC_TRANSFORM_OK = 0,
    BCDEC_TRANSFORM_INVALID,
    BCDEC_TRANSFORM_UNREPRESENTABLE
} bcdec_transform_status;

/*
 * Rearrange a 16-byte BC7 block without decoding and recompressing it.
 *
 * src16 and dst16 may be exactly the same object.  Other overlap is the
 * caller's responsibility.  On BCDEC_TRANSFORM_INVALID or
 * BCDEC_TRANSFORM_UNREPRESENTABLE, dst16 is not modified.
 */
bcdec_transform_status bcdec_bc7_transform(const void* src16,
                                           void* dst16,
                                           bcdec_transform_op op);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BCDEC_TRANSFORM_H */
