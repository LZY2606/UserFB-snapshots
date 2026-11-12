/*
 * Independent 64-bit integer reference for the JPEG2000 reversible
 * 5/3 wavelet transform (lifting definition, ISO/IEC 15444-1 annex F).
 *
 * This implementation is written from the standard's predict/update
 * definitions only: it never calls into the OpenJPEG DWT, lifting or
 * deinterleave code and does not copy any production loop. All arithmetic
 * runs on int64_t and every division that may meet a negative numerator is
 * performed by an explicit floor-towards-negative-infinity helper.
 */

#ifndef DWT_REFERENCE_H
#define DWT_REFERENCE_H

#include <stdint.h>

/* Rectangle in global component coordinates, half-open: x0 <= x < x1. */
typedef struct {
    int64_t x0;
    int64_t y0;
    int64_t x1;
    int64_t y1;
} ref_rect64;

/* Subband identifiers, matching the band numbering used by the standard. */
enum {
    REF_BAND_LL = 0,
    REF_BAND_HL = 1,
    REF_BAND_LH = 2,
    REF_BAND_HH = 3
};

typedef struct {
    int level;          /* decomposition level, 1..numres-1 */
    int band;           /* REF_BAND_* */
    ref_rect64 rect;    /* global coordinates of the band grid */
    int64_t *coef;      /* row-major, (x1-x0) * (y1-y0) entries */
} ref_band64;

typedef struct {
    unsigned numres;        /* number of resolution levels (>= 1) */
    ref_rect64 comp;        /* component rectangle on the finest grid */
    ref_rect64 ll_rect;     /* rectangle of the lowest resolution LL grid */
    int64_t *ll;            /* lowest resolution coefficients, row major */
    ref_band64 *bands;      /* HL/LH/HH of every level, coarsest first */
    unsigned nb_bands;
} ref_dwt64;

/* Exact integer divisions. The denominator is always strictly positive. */
int64_t ref_floor_div(int64_t a, int64_t b);
int64_t ref_ceil_div(int64_t a, int64_t b);

/* Floor / ceil division by an exact power of two (shift < 63). */
int64_t ref_floor_div_pow2(int64_t a, unsigned shift);
int64_t ref_ceil_div_pow2(int64_t a, unsigned shift);

/* Rectangle of resolution level r for a component rectangle. Level 0 is the
 * coarsest resolution and level numres-1 the full resolution, matching the
 * JPEG2000 convention: ceil(p / 2^(numres-1-r)). */
ref_rect64 ref_resolution_rect(ref_rect64 comp, unsigned numres, unsigned r);

/* Global band rectangle for a subband produced at decomposition level level,
 * where level runs from 1 (coarsest split) to numres-1 (finest split). */
ref_rect64 ref_band_rect(ref_rect64 comp, unsigned numres, int level, int band);

/*
 * Forward multilevel 2-D reversible 5/3 transform of an image.
 * img has (comp.x1-comp.x0) * (comp.y1-comp.y0) row-major samples whose
 * global coordinate is comp.x0 + column / comp.y0 + row. Returns NULL on
 * allocation failure. The returned object owns all its arrays.
 */
ref_dwt64 *ref_dwt64_forward(const int64_t *img, ref_rect64 comp,
                             unsigned numres);

/*
 * Inverse transform. Writes (comp.x1-comp.x0) * (comp.y1-comp.y0) samples
 * to out in the same layout as the forward input. Returns 1 on success.
 */
int ref_dwt64_inverse(const ref_dwt64 *d, int64_t *out);

void ref_dwt64_destroy(ref_dwt64 *d);

/* Band coefficient access by global band-grid coordinate. */
int64_t ref_band64_get(const ref_band64 *b, int64_t gx, int64_t gy);

/* Number of legal resolution levels for a component rectangle. */
unsigned ref_max_numres(ref_rect64 comp);

#endif /* DWT_REFERENCE_H */
