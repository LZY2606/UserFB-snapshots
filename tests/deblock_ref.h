/*
 * deblock_ref.h - independent whole-picture deblocking reference.
 *
 * This module describes a 4:2:0 picture purely in pixel coordinates and
 * natural 4x4 block raster coordinates.  It derives boundary strengths,
 * thresholds and filtered samples itself, following the normative
 * deblocking process of ITU-T H.264 / ISO/IEC 14496-10 (clause 8.7).
 *
 * It deliberately does NOT share any expansion index table, threshold
 * array or filter macro with src/h264bsd_deblocking.c.  Macroblock decode
 * order <-> natural raster order translation belongs to the test input
 * adapter (deblock_adapter.h), never to this reference.
 *
 * C99 and the standard library only.
 */
#ifndef DEBLOCK_REF_H
#define DEBLOCK_REF_H

#include <stdint.h>

#define DREF_MAX_REFS 8

/* Inter macroblock partition shapes, coordinate level. */
enum {
    DREF_PART_16X16 = 0,
    DREF_PART_16X8  = 1,
    DREF_PART_8X16  = 2,
    DREF_PART_8X8   = 3
};

/* Natural 4x4 block grid inside one macroblock: index = by*4 + bx. */
typedef struct {
    int intra;                  /* mbType > 5 in the library           */
    int part;                   /* DREF_PART_* for inter MBs            */

    unsigned qpY;               /* luma QP 0..51                        */
    int chromaQpIndexOffset;    /* chroma_qp_index_offset (PPS/slice)    */
    int offA;                   /* slice alpha/c0 offset, already x2    */
    int offB;                   /* slice beta offset, already x2         */

    unsigned sliceId;
    unsigned disableIdc;        /* disable_deblocking_filter_idc 0..2    */

    unsigned coeff[16];         /* non-zero residual coefficient count?  */
    int mvx[16];                /* MV horizontal, quarter-luma units    */
    int mvy[16];                /* MV vertical, quarter-luma units      */

    /* Reference picture identity per natural 8x8 quadrant:
     * q = qy*2 + qx, i.e. TL, TR, BL, BR.  Pointers, not refIdx values,
     * are the identity, matching refAddr comparison in the library. */
    const void *ref[4];
} dref_mb;

typedef struct {
    int wMb, hMb;
    dref_mb *mb;                /* wMb*hMb, raster order                */
    uint8_t *y;                 /* stride wMb*16, hMb*16 rows           */
    uint8_t *cb;                /* stride wMb*8,  hMb*8 rows            */
    uint8_t *cr;
} dref_pic;

/* Filter the whole picture in place, MB raster order, each 4-row band
 * vertical edges first then horizontal edges. */
void dref_filter(dref_pic *pic);

/* Normative tables, exposed so cases can pin specific threshold indexes. */
unsigned dref_alpha(unsigned index);
unsigned dref_beta(unsigned index);
unsigned dref_tc0(unsigned indexA, unsigned bSminus1);
unsigned dref_qpc(unsigned qpY);

#endif /* DEBLOCK_REF_H */
