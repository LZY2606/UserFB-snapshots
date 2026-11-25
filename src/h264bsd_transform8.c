/*
 * H.264 High profile 8x8 luma residual transform support.
 *
 * The CAVLC parser supplies four groups of 16 scan-order levels for every
 * coded 8x8 luma area.  Those groups are interleaved here into natural 8x8
 * frequency coordinates before one integer 8x8 inverse transform.  They are
 * deliberately not transformed as four independent 4x4 blocks.
 */

#include "basetype.h"
#include "h264bsd_transform.h"
#include "h264bsd_util.h"

/* Inverse scaling factors for the flat default 8x8 scaling list.  Columns are
 * indexed by scaleCategory8 below.  QP rows are qp % 6. */
static const i32 levelScale8x8[6][6] = {
    {20, 18, 32, 19, 25, 24},
    {22, 19, 35, 21, 28, 26},
    {26, 23, 42, 24, 33, 31},
    {28, 25, 45, 26, 35, 33},
    {32, 28, 51, 30, 40, 38},
    {36, 32, 58, 34, 46, 42}
};

/* Frequency-coordinate category pair:
 * 0=(even class A, even class A), 1=A/odd, 2=A/even class B,
 * 3=odd/odd, 4=B/B, 5=B/odd. */
static const u8 scaleCategory8[8][8] = {
    {0, 1, 2, 1, 0, 1, 2, 1},
    {1, 3, 5, 3, 1, 3, 5, 3},
    {2, 5, 4, 5, 2, 5, 4, 5},
    {1, 3, 5, 3, 1, 3, 5, 3},
    {0, 1, 2, 1, 0, 1, 2, 1},
    {1, 3, 5, 3, 1, 3, 5, 3},
    {2, 5, 4, 5, 2, 5, 4, 5},
    {1, 3, 5, 3, 1, 3, 5, 3}
};

/* Standard frame 4x4 zig-zag scan, scan index -> {x,y}. */
static const u8 scan4x4[16][2] = {
    {0,0}, {1,0}, {0,1}, {0,2},
    {1,1}, {2,0}, {3,0}, {2,1},
    {1,2}, {0,3}, {1,3}, {2,2},
    {3,1}, {3,2}, {2,3}, {3,3}
};

static void inverse1d8(i32 *v, u32 stride)
{
    i32 x0,x1,x2,x3,x4,x5,x6,x7;
    i32 a0,a1,a2,a3,a4,a5,a6,a7;
    i32 b0,b1,b2,b3,b4,b5,b6,b7;

    a0 = v[0*stride] + v[4*stride];
    a2 = v[0*stride] - v[4*stride];
    a4 = (v[2*stride] >> 1) - v[6*stride];
    a6 = v[2*stride] + (v[6*stride] >> 1);

    b0 = a0 + a6;
    b2 = a2 + a4;
    b4 = a2 - a4;
    b6 = a0 - a6;

    a1 = -v[3*stride] + v[5*stride] - v[7*stride] -
         (v[7*stride] >> 1);
    a3 =  v[1*stride] + v[7*stride] - v[3*stride] -
         (v[3*stride] >> 1);
    a5 = -v[1*stride] + v[7*stride] + v[5*stride] +
         (v[5*stride] >> 1);
    a7 =  v[1*stride] + v[3*stride] + v[5*stride] +
         (v[5*stride] >> 1);

    b1 = (a7 >> 2) + a1;
    b3 = a3 + (a5 >> 2);
    b5 = (a3 >> 2) - a5;
    b7 = a7 - (a1 >> 2);

    x0 = b0 + b7;
    x1 = b2 + b5;
    x2 = b4 + b3;
    x3 = b6 + b1;
    x4 = b6 - b1;
    x5 = b4 - b3;
    x6 = b2 - b5;
    x7 = b0 - b7;

    v[0*stride] = x0;
    v[1*stride] = x1;
    v[2*stride] = x2;
    v[3*stride] = x3;
    v[4*stride] = x4;
    v[5*stride] = x5;
    v[6*stride] = x6;
    v[7*stride] = x7;
}

u32 h264bsdProcessLuma8x8(i32 residual[][16], const i16 *totalCoeff, u32 qp)
{
    u32 block, group, k;
    u32 x, y;
    u32 qpMod, qpDiv, scaleShift;
    i32 c[8][8];

    (void)totalCoeff;

    if (qp > 51)
        return(HANTRO_NOK);

    qpMod = qp % 6;
    qpDiv = qp / 6;
    scaleShift = qpDiv;

    for (block = 0; block < 4; block++)
    {
        u32 base = block * 4;

        for (k = 0; k < 64; k++)
            ((i32 *)c)[k] = 0;

        for (group = 0; group < 4; group++)
        {
            u32 blockIndex = base + group;
            u32 sx = (group & 1U) ? 4 : 0;
            u32 sy = (group & 2U) ? 4 : 0;

            if (!totalCoeff[blockIndex])
                continue;

            for (k = 0; k < 16; k++)
            {
                i32 level = residual[blockIndex][k];
                u32 x, y, cat, scale;

                if (!level)
                    continue;

                x = sx + scan4x4[k][0];
                y = sy + scan4x4[k][1];
                cat = scaleCategory8[y][x];
                scale = (u32)levelScale8x8[qpMod][cat] << scaleShift;
                c[y][x] = level * (i32)scale;
            }
        }

        /* Horizontal and vertical integer transforms. */
        for (y = 0; y < 8; y++)
            inverse1d8(&c[y][0], 1);
        for (x = 0; x < 8; x++)
            inverse1d8(&c[0][x], 8);

        /* Store natural 8x8 residuals as the four raster-order 4x4 arrays
         * consumed by the existing prediction/addition output path. */
        for (group = 0; group < 4; group++)
        {
            u32 blockIndex = base + group;
            u32 qx4 = (group & 1U) ? 4 : 0;
            u32 qy4 = (group & 2U) ? 4 : 0;
            u32 yy, xx;
            u32 allZero = 1;

            for (yy = 0; yy < 4; yy++)
            {
                for (xx = 0; xx < 4; xx++)
                {
                    i32 value = c[qy4 + yy][qx4 + xx];
                    value = (value + 32) >> 6;
                    residual[blockIndex][yy*4 + xx] = value;
                    if (value)
                        allZero = 0;
                }
            }

            /* A quarter output block can be non-zero even when its own CAVLC
             * group had no coefficients: the shared 8x8 transform spreads
             * energy from the other groups into it. */
            if (allZero)
                MARK_RESIDUAL_EMPTY(residual[blockIndex]);
        }
    }

    return(HANTRO_OK);
}
