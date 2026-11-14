/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** The "appropriate copyright message" mentioned in section 2c of the GPLv2
** must read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com"
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**
** $Id: drc.c,v 1.29 2015/02/22 10:09:29 knik Exp $
**/

#include "common.h"
#include "structs.h"

#include <stdlib.h>
#include "syntax.h"
#include "drc.h"

drc_info *drc_init(real_t cut, real_t boost)
{
    drc_info *drc = (drc_info*)faad_malloc(sizeof(drc_info));
    memset(drc, 0, sizeof(drc_info));

    drc->ctrl1 = cut;
    drc->ctrl2 = boost;

    /* defaults, same as for a freshly received dynamic_range_info()
       payload in which the corresponding fields are omitted */
    drc->num_bands = 1;
    drc->band_top[0] = 1024/4 - 1;
    drc->prog_ref_level = DRC_REF_LEVEL;
    drc->dyn_rng_sgn[0] = 1;
    drc->dyn_rng_ctl[0] = 0;

    return drc;
}

void drc_end(drc_info *drc)
{
    if (drc) faad_free(drc);
}

#ifdef FIXED_POINT
/* 2^(k/256) for k = 0..256 in COEF (Q28) scale, used to raise 2 to the
   fractional part of the DRC gain exponent */
static const real_t drc_pow2_table[257] =
{
    COEF_CONST(1.0000000000), COEF_CONST(1.0027112751), COEF_CONST(1.0054299011), COEF_CONST(1.0081558981),
    COEF_CONST(1.0108892861), COEF_CONST(1.0136300850), COEF_CONST(1.0163783149), COEF_CONST(1.0191339961),
    COEF_CONST(1.0218971487), COEF_CONST(1.0246677929), COEF_CONST(1.0274459491), COEF_CONST(1.0302316377),
    COEF_CONST(1.0330248790), COEF_CONST(1.0358256936), COEF_CONST(1.0386341020), COEF_CONST(1.0414501247),
    COEF_CONST(1.0442737824), COEF_CONST(1.0471050959), COEF_CONST(1.0499440858), COEF_CONST(1.0527907730),
    COEF_CONST(1.0556451784), COEF_CONST(1.0585073228), COEF_CONST(1.0613772273), COEF_CONST(1.0642549129),
    COEF_CONST(1.0671404007), COEF_CONST(1.0700337118), COEF_CONST(1.0729348675), COEF_CONST(1.0758438891),
    COEF_CONST(1.0787607978), COEF_CONST(1.0816856150), COEF_CONST(1.0846183622), COEF_CONST(1.0875590609),
    COEF_CONST(1.0905077327), COEF_CONST(1.0934643991), COEF_CONST(1.0964290818), COEF_CONST(1.0994018026),
    COEF_CONST(1.1023825833), COEF_CONST(1.1053714457), COEF_CONST(1.1083684117), COEF_CONST(1.1113735033),
    COEF_CONST(1.1143867426), COEF_CONST(1.1174081516), COEF_CONST(1.1204377524), COEF_CONST(1.1234755673),
    COEF_CONST(1.1265216186), COEF_CONST(1.1295759286), COEF_CONST(1.1326385196), COEF_CONST(1.1357094142),
    COEF_CONST(1.1387886348), COEF_CONST(1.1418762040), COEF_CONST(1.1449721444), COEF_CONST(1.1480764788),
    COEF_CONST(1.1511892300), COEF_CONST(1.1543104206), COEF_CONST(1.1574400736), COEF_CONST(1.1605782120),
    COEF_CONST(1.1637248588), COEF_CONST(1.1668800370), COEF_CONST(1.1700437697), COEF_CONST(1.1732160802),
    COEF_CONST(1.1763969917), COEF_CONST(1.1795865275), COEF_CONST(1.1827847110), COEF_CONST(1.1859915657),
    COEF_CONST(1.1892071150), COEF_CONST(1.1924313826), COEF_CONST(1.1956643920), COEF_CONST(1.1989061671),
    COEF_CONST(1.2021567315), COEF_CONST(1.2054161090), COEF_CONST(1.2086843236), COEF_CONST(1.2119613993),
    COEF_CONST(1.2152473600), COEF_CONST(1.2185422298), COEF_CONST(1.2218460330), COEF_CONST(1.2251587936),
    COEF_CONST(1.2284805361), COEF_CONST(1.2318112847), COEF_CONST(1.2351510639), COEF_CONST(1.2384998982),
    COEF_CONST(1.2418578121), COEF_CONST(1.2452248302), COEF_CONST(1.2486009772), COEF_CONST(1.2519862779),
    COEF_CONST(1.2553807570), COEF_CONST(1.2587844395), COEF_CONST(1.2621973504), COEF_CONST(1.2656195146),
    COEF_CONST(1.2690509572), COEF_CONST(1.2724917034), COEF_CONST(1.2759417784), COEF_CONST(1.2794012075),
    COEF_CONST(1.2828700161), COEF_CONST(1.2863482295), COEF_CONST(1.2898358734), COEF_CONST(1.2933329732),
    COEF_CONST(1.2968395547), COEF_CONST(1.3003556434), COEF_CONST(1.3038812652), COEF_CONST(1.3074164459),
    COEF_CONST(1.3109612115), COEF_CONST(1.3145155879), COEF_CONST(1.3180796013), COEF_CONST(1.3216532776),
    COEF_CONST(1.3252366432), COEF_CONST(1.3288297242), COEF_CONST(1.3324325471), COEF_CONST(1.3360451382),
    COEF_CONST(1.3396675241), COEF_CONST(1.3432997312), COEF_CONST(1.3469417862), COEF_CONST(1.3505937159),
    COEF_CONST(1.3542555469), COEF_CONST(1.3579273062), COEF_CONST(1.3616090206), COEF_CONST(1.3653007172),
    COEF_CONST(1.3690024230), COEF_CONST(1.3727141651), COEF_CONST(1.3764359708), COEF_CONST(1.3801678673),
    COEF_CONST(1.3839098820), COEF_CONST(1.3876620423), COEF_CONST(1.3914243758), COEF_CONST(1.3951969100),
    COEF_CONST(1.3989796725), COEF_CONST(1.4027726912), COEF_CONST(1.4065759938), COEF_CONST(1.4103896082),
    COEF_CONST(1.4142135624), COEF_CONST(1.4180478843), COEF_CONST(1.4218926022), COEF_CONST(1.4257477441),
    COEF_CONST(1.4296133384), COEF_CONST(1.4334894134), COEF_CONST(1.4373759974), COEF_CONST(1.4412731191),
    COEF_CONST(1.4451808070), COEF_CONST(1.4490990896), COEF_CONST(1.4530279958), COEF_CONST(1.4569675544),
    COEF_CONST(1.4609177942), COEF_CONST(1.4648787441), COEF_CONST(1.4688504333), COEF_CONST(1.4728328909),
    COEF_CONST(1.4768261459), COEF_CONST(1.4808302278), COEF_CONST(1.4848451659), COEF_CONST(1.4888709895),
    COEF_CONST(1.4929077283), COEF_CONST(1.4969554118), COEF_CONST(1.5010140696), COEF_CONST(1.5050837316),
    COEF_CONST(1.5091644276), COEF_CONST(1.5132561875), COEF_CONST(1.5173590412), COEF_CONST(1.5214730189),
    COEF_CONST(1.5255981507), COEF_CONST(1.5297344669), COEF_CONST(1.5338819978), COEF_CONST(1.5380407738),
    COEF_CONST(1.5422108254), COEF_CONST(1.5463921831), COEF_CONST(1.5505848777), COEF_CONST(1.5547889398),
    COEF_CONST(1.5590044002), COEF_CONST(1.5632312900), COEF_CONST(1.5674696400), COEF_CONST(1.5717194813),
    COEF_CONST(1.5759808451), COEF_CONST(1.5802537627), COEF_CONST(1.5845382653), COEF_CONST(1.5888343843),
    COEF_CONST(1.5931421513), COEF_CONST(1.5974615979), COEF_CONST(1.6017927557), COEF_CONST(1.6061356564),
    COEF_CONST(1.6104903319), COEF_CONST(1.6148568142), COEF_CONST(1.6192351352), COEF_CONST(1.6236253270),
    COEF_CONST(1.6280274219), COEF_CONST(1.6324414520), COEF_CONST(1.6368674498), COEF_CONST(1.6413054476),
    COEF_CONST(1.6457554782), COEF_CONST(1.6502175739), COEF_CONST(1.6546917677), COEF_CONST(1.6591780922),
    COEF_CONST(1.6636765803), COEF_CONST(1.6681872651), COEF_CONST(1.6727101796), COEF_CONST(1.6772453570),
    COEF_CONST(1.6817928305), COEF_CONST(1.6863526334), COEF_CONST(1.6909247993), COEF_CONST(1.6955093615),
    COEF_CONST(1.7001063537), COEF_CONST(1.7047158097), COEF_CONST(1.7093377631), COEF_CONST(1.7139722479),
    COEF_CONST(1.7186192981), COEF_CONST(1.7232789477), COEF_CONST(1.7279512310), COEF_CONST(1.7326361820),
    COEF_CONST(1.7373338353), COEF_CONST(1.7420442252), COEF_CONST(1.7467673862), COEF_CONST(1.7515033530),
    COEF_CONST(1.7562521604), COEF_CONST(1.7610138430), COEF_CONST(1.7657884359), COEF_CONST(1.7705759741),
    COEF_CONST(1.7753764925), COEF_CONST(1.7801900265), COEF_CONST(1.7850166113), COEF_CONST(1.7898562823),
    COEF_CONST(1.7947090750), COEF_CONST(1.7995750249), COEF_CONST(1.8044541678), COEF_CONST(1.8093465394),
    COEF_CONST(1.8142521755), COEF_CONST(1.8191711122), COEF_CONST(1.8241033854), COEF_CONST(1.8290490314),
    COEF_CONST(1.8340080864), COEF_CONST(1.8389805868), COEF_CONST(1.8439665690), COEF_CONST(1.8489660695),
    COEF_CONST(1.8539791251), COEF_CONST(1.8590057724), COEF_CONST(1.8640460484), COEF_CONST(1.8690999899),
    COEF_CONST(1.8741676341), COEF_CONST(1.8792490181), COEF_CONST(1.8843441790), COEF_CONST(1.8894531544),
    COEF_CONST(1.8945759816), COEF_CONST(1.8997126982), COEF_CONST(1.9048633418), COEF_CONST(1.9100279503),
    COEF_CONST(1.9152065614), COEF_CONST(1.9203992132), COEF_CONST(1.9256059436), COEF_CONST(1.9308267910),
    COEF_CONST(1.9360617935), COEF_CONST(1.9413109895), COEF_CONST(1.9465744176), COEF_CONST(1.9518521162),
    COEF_CONST(1.9571441242), COEF_CONST(1.9624504802), COEF_CONST(1.9677712232), COEF_CONST(1.9731063923),
    COEF_CONST(1.9784560264), COEF_CONST(1.9838201649), COEF_CONST(1.9891988470), COEF_CONST(1.9945921122),
    COEF_CONST(2.0000000000)
};

/* DRC gain exponent quantum: the gain is 2^(t/24) with t expressed in
   REAL (Q14) scale here */
#define DRC_EXP_STEP (24 * REAL_PRECISION)

/* Compute the fixed point DRC gain for one band, split into an integer
   octave part *n_out and a fractional part *g_out (Q28, in [1,2)), such
   that the gain equals 2^(*n_out) * (*g_out / 2^28). Pure integer math,
   follows the same formula as the floating point branch. */
static void drc_gain_fixed(const drc_info *drc, uint8_t bd,
                           int32_t *n_out, int32_t *g_out)
{
    int64_t t;
    int32_t n, idx;
    uint32_t r, rem;
    int64_t g;

    /* t = (+/-ctrl * ctl - (DRC_REF_LEVEL - prog_ref_level)) in Q14 */
    if (drc->dyn_rng_sgn[bd]) /* compress */
        t = -(int64_t)drc->ctrl1 * drc->dyn_rng_ctl[bd];
    else /* boost */
        t = (int64_t)drc->ctrl2 * drc->dyn_rng_ctl[bd];
    t -= (int64_t)(DRC_REF_LEVEL - drc->prog_ref_level) * REAL_PRECISION;

    /* exponent in octaves is t / DRC_EXP_STEP, split into integer and
       fractional part (fraction always in [0,1)) */
    n = (int32_t)(t / DRC_EXP_STEP);
    r = (uint32_t)(t - (int64_t)n * DRC_EXP_STEP);
    if (t < 0 && r != 0)
    {
        n -= 1;
        r = (uint32_t)(t - (int64_t)n * DRC_EXP_STEP);
    }

    /* 2^(r / DRC_EXP_STEP): table lookup at 1/256 octave spacing plus
       linear interpolation; DRC_EXP_STEP = 1536 * 256 */
    idx = (int32_t)(r / 1536);
    rem = r % 1536;
    g = drc_pow2_table[idx];
    g += ((int64_t)(drc_pow2_table[idx + 1] - drc_pow2_table[idx]) * rem) / 1536;

    *n_out = n;
    *g_out = (int32_t)g;
}

/* Scale one Q14 spectral value by 2^n * (g_q28 / 2^28), with rounding
   that stays correct for negative values and saturation to int32 */
static real_t drc_scale_fixed(real_t v, int32_t n, int32_t g_q28)
{
    int64_t p = (int64_t)v * g_q28;
    int64_t r;
    int shift = 28 - n;

    if (shift <= 0)
    {
        /* gain far too large for int32, saturate */
        if (p == 0)
            return 0;
        return (p > 0) ? (real_t)0x7FFFFFFF : (real_t)(int32_t)0x80000000;
    }

    /* round to nearest, half away from zero (symmetric for v < 0) */
    if (p >= 0)
        r = (p + ((int64_t)1 << (shift - 1))) >> shift;
    else
        r = -((-p + ((int64_t)1 << (shift - 1))) >> shift);

    if (r > 0x7FFFFFFF)
        r = 0x7FFFFFFF;
    else if (r < -(int64_t)0x80000000)
        r = -(int64_t)0x80000000;

    return (real_t)r;
}
#endif

/* Apply the dynamic range info to one frame of spectral data.
   frame_len must be 960 or 1024. Returns 0 on success; returns non-zero
   (leaving the spectrum untouched) on invalid arguments or invalid band
   descriptions. */
int drc_decode_frame(drc_info *drc, real_t *spec, uint16_t frame_len)
{
    uint16_t i, bd, top;
    uint16_t bottom = 0;
    uint8_t num_bands;

    if (drc == NULL || spec == NULL)
        return 1;
    if (frame_len != 960 && frame_len != 1024)
        return 1;

    num_bands = drc->num_bands;
    if (num_bands < 1 || num_bands > 17)
        return 1;

    if (num_bands > 1)
    {
        /* raw band endpoints must be strictly increasing; clamping them
           to the frame length must not be used to hide a reversed order */
        for (bd = 1; bd < num_bands; bd++)
        {
            if (drc->band_top[bd] <= drc->band_top[bd-1])
                return 1;
        }
    }

    for (bd = 0; bd < num_bands; bd++)
    {
        if (num_bands == 1)
        {
            /* a single band always covers the whole frame */
            top = frame_len;
        } else {
            uint32_t t = 4 * ((uint32_t)drc->band_top[bd] + 1);
            top = (t < frame_len) ? (uint16_t)t : frame_len;
        }

#ifndef FIXED_POINT
        {
            real_t factor, exp;

            /* Decode DRC gain factor */
            if (drc->dyn_rng_sgn[bd])  /* compress */
                exp = ((-drc->ctrl1 * drc->dyn_rng_ctl[bd]) - (DRC_REF_LEVEL - drc->prog_ref_level))/REAL_CONST(24.0);
            else /* boost */
                exp = ((drc->ctrl2 * drc->dyn_rng_ctl[bd]) - (DRC_REF_LEVEL - drc->prog_ref_level))/REAL_CONST(24.0);
            factor = (real_t)pow(2.0, exp);

            /* Apply gain factor */
            for (i = bottom; i < top; i++)
                spec[i] *= factor;
        }
#else
        {
            int32_t n, g_q28;

            /* Decode DRC gain factor */
            drc_gain_fixed(drc, bd, &n, &g_q28);

            /* Apply gain factor */
            for (i = bottom; i < top; i++)
                spec[i] = drc_scale_fixed(spec[i], n, g_q28);
        }
#endif

        bottom = top;
    }

    return 0;
}

/* compatibility wrapper for 1024 line frames */
void drc_decode(drc_info *drc, real_t *spec)
{
    (void)drc_decode_frame(drc, spec, 1024);
}
