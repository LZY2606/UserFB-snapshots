/*
 * deblock_adapter.h - test-only input adaptation layer.
 *
 * Turns the coordinate/natural-raster scenario used by the independent
 * reference into a REAL image_t + mbStorage_t grid and invokes the
 * production entry h264bsdFilterPicture().  Boundary strengths are never
 * fed to internal filter routines directly.
 *
 * The H.264 macroblock residual/motion arrays use 4x4 block DECODING order,
 * which differs from the natural raster order.  The translation table below
 * belongs to this adapter; the reference itself only knows natural
 * coordinates.  Production keeps its own static mb4x4Index table, so the
 * two sides do not share an expansion table.
 */
#ifndef DEBLOCK_ADAPTER_H
#define DEBLOCK_ADAPTER_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264bsd_deblocking.h"
#include "deblock_ref.h"

/* Natural raster block n = by*4+bx -> H.264 4x4 luma decoding index.
 * Independently listed from the specification's residual scan ordering:
 *   natural rows  [ 0  1  2  3]     decode [ 0  1  4  5]
 *                 [ 4  5  6  7]            [ 2  3  6  7]
 *                 [ 8  9 10 11]            [ 8  9 12 13]
 *                 [12 13 14 15]            [10 11 14 15]            */
static const unsigned AD_NAT2DEC[16] = {
    0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15
};

#define AD_GUARD 256
#define AD_GUARD_BYTE 0x5A

typedef struct {
    int wMb, hMb;
    dref_mb *mbs;              /* caller-owned wMb*hMb                 */
    /* Packed source planes, strides wMb*16 / wMb*8, caller-owned. */
    uint8_t *y, *cb, *cr;
} ad_scenario;

typedef struct {
    long mismatches;
    int frontGuardBad;
    int rearGuardBad;
    char first[192];           /* human readable first difference        */
} ad_result;

/* Run production + independent reference from the same scenario and
 * byte-compare the complete Y/Cb/Cr planes plus outer sentinels.
 * Returns 1 when everything matches, 0 otherwise (res filled in). */
static int ad_run_and_compare(const ad_scenario *scn, ad_result *res) {
    int W = scn->wMb, H = scn->hMb, nMb = W * H;
    int yBytes = nMb * 256;
    int cBytes = nMb * 64;
    int regionBytes = yBytes + 2 * cBytes;

    uint8_t *storage = (uint8_t *)malloc(AD_GUARD + regionBytes + AD_GUARD);
    mbStorage_t *grid = (mbStorage_t *)calloc((size_t)nMb, sizeof(mbStorage_t));
    dref_mb *refMbs = (dref_mb *)calloc((size_t)nMb, sizeof(dref_mb));
    uint8_t *refY = (uint8_t *)malloc((size_t)yBytes);
    uint8_t *refCb = (uint8_t *)malloc((size_t)cBytes);
    uint8_t *refCr = (uint8_t *)malloc((size_t)cBytes);
    image_t img;
    dref_pic rp;
    uint8_t *region;
    int k, mc, mr, q, n;
    long mism = 0;
    int reported = 0;

    memset(res, 0, sizeof(*res));
    if (!storage || !grid || !refMbs || !refY || !refCb || !refCr) {
        res->mismatches = -1;
        snprintf(res->first, sizeof(res->first), "allocation failure");
        free(storage); free(grid); free(refMbs);
        free(refY); free(refCb); free(refCr);
        return 0;
    }

    memset(storage, AD_GUARD_BYTE, AD_GUARD);
    memset(storage + AD_GUARD + regionBytes, AD_GUARD_BYTE, AD_GUARD);
    region = storage + AD_GUARD;

    /* Production image layout (see h264bsdFilterPicture):
     * Y nMb*256, Cb nMb*64, Cr nMb*64, packed raster strides. */
    memcpy(region, scn->y, (size_t)yBytes);
    memcpy(region + yBytes, scn->cb, (size_t)cBytes);
    memcpy(region + yBytes + cBytes, scn->cr, (size_t)cBytes);

    /* ---- adapt descriptors to real mbStorage_t grid ---- */
    for (mr = 0; mr < H; ++mr) {
        for (mc = 0; mc < W; ++mc) {
            int idx = mr * W + mc;
            const dref_mb *s = &scn->mbs[idx];
            mbStorage_t *m = &grid[idx];

            memset(m, 0, sizeof(*m));
            if (s->intra)
                m->mbType = I_16x16_2_0_0;   /* any value > 5 is intra */
            else if (s->part == DREF_PART_16X8)
                m->mbType = P_L0_L0_16x8;
            else if (s->part == DREF_PART_8X16)
                m->mbType = P_L0_L0_8x16;
            else if (s->part == DREF_PART_8X8)
                m->mbType = P_8x8;
            else
                m->mbType = P_L0_16x16;

            m->sliceId = s->sliceId;
            m->disableDeblockingFilterIdc = s->disableIdc;
            m->filterOffsetA = s->offA;
            m->filterOffsetB = s->offB;
            m->qpY = s->qpY;
            m->chromaQpIndexOffset = s->chromaQpIndexOffset;

            for (n = 0; n < 16; ++n) {
                unsigned d = AD_NAT2DEC[n];
                m->totalCoeff[d] = s->coeff[n] ? 1 : 0;
                m->mv[d].hor = (int16_t)s->mvx[n];
                m->mv[d].ver = (int16_t)s->mvy[n];
            }
            /* Decode refAddr groups 0..3 are the natural TL/TR/BL/BR
             * 8x8 quadrants.  refPic is held identical on purpose: the
             * production strength test compares refAddr pointers. */
            for (q = 0; q < 4; ++q) {
                m->refAddr[q] = (u8 *)s->ref[q];
                m->refPic[q] = 7;
            }

            if (mc > 0) m->mbA = &grid[idx - 1];
            if (mr > 0) m->mbB = &grid[idx - W];
        }
    }

    img.data = region;
    img.width = (u32)W;       /* image_t width/height are in MB units */
    img.height = (u32)H;
    img.luma = img.cb = img.cr = NULL;

    /* ---- independent reference side ---- */
    memcpy(refMbs, scn->mbs, (size_t)nMb * sizeof(dref_mb));
    memcpy(refY, scn->y, (size_t)yBytes);
    memcpy(refCb, scn->cb, (size_t)cBytes);
    memcpy(refCr, scn->cr, (size_t)cBytes);
    rp.wMb = W; rp.hMb = H; rp.mb = refMbs;
    rp.y = refY; rp.cb = refCb; rp.cr = refCr;
    dref_filter(&rp);

    /* ---- call the production entry on the real grid ---- */
    h264bsdFilterPicture(&img, grid);

    /* ---- sentinels ---- */
    for (k = 0; k < AD_GUARD; ++k) {
        if (storage[k] != AD_GUARD_BYTE) { res->frontGuardBad = 1; break; }
    }
    for (k = 0; k < AD_GUARD; ++k) {
        if (storage[AD_GUARD + regionBytes + k] != AD_GUARD_BYTE) {
            res->rearGuardBad = 1; break;
        }
    }

    /* ---- whole-plane byte comparison ---- */
    {
        const char *pname[3] = {"Y", "Cb", "Cr"};
        const uint8_t *pprod[3] = {
            region, region + yBytes, region + yBytes + cBytes
        };
        const uint8_t *pref[3] = { refY, refCb, refCr };
        const int pw[3] = { W * 16, W * 8, W * 8 };
        const int ph[3] = { H * 16, H * 8, H * 8 };
        int pl;
        for (pl = 0; pl < 3; ++pl) {
            int sz = pw[pl] * ph[pl];
            int z;
            for (z = 0; z < sz; ++z) {
                if (pprod[pl][z] != pref[pl][z]) {
                    ++mism;
                    if (!reported) {
                        int x = z % pw[pl], yy = z / pw[pl];
                        snprintf(res->first, sizeof(res->first),
                            "FIRST_DIFF plane=%s x=%d y=%d lib=%u ref=%u",
                            pname[pl], x, yy,
                            (unsigned)pprod[pl][z],
                            (unsigned)pref[pl][z]);
                        reported = 1;
                    }
                }
            }
        }
    }

    res->mismatches = mism;
    free(storage); free(grid); free(refMbs);
    free(refY); free(refCb); free(refCr);
    return mism == 0 && !res->frontGuardBad && !res->rearGuardBad;
}

#endif /* DEBLOCK_ADAPTER_H */
