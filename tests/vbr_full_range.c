/*
 *  Tests for full-range VBR (twolame_set_VBR_full_range).
 *
 *  Four cases, driven by devcheck/target_main.c:
 *    case_selection      - candidate order / exact-capacity, controlled SMR+scfsi
 *    case_syntax         - API contract, illegal configs, CRC/ancillary, frame parsing
 *    case_table_switch   - public-entry encode with low->high->low table switching,
 *                          joint + stereo output, byte-identical planar/interleaved
 *                          and whole/chunked feeding
 *    case_compatibility  - old mode byte/behaviour preservation, bounded fallback
 *
 *  The frame parser below is an independent re-implementation: it walks the
 *  header, the per-table bit allocation fields, scfsi, scalefactors and the
 *  (grouped) sample codewords, and checks every frame's exact length, mode
 *  and joint stereo boundary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "twolame.h"
#include "common.h"
#include "bitbuffer.h"
#include "encode.h"


/* ------------------------------------------------------------------ */
/* assertion bookkeeping                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int count;
    int failed;
} case_t;

#define CHECK(c, cond) do { \
        (c)->count++; \
        if (!(cond)) { \
            (c)->failed++; \
            fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
} while (0)

#define CASE_RESULT(c) (((c).failed || (c).count == 0) ? 0 : (c).count)


/* ------------------------------------------------------------------ */
/* independent copies of the MPEG-1 Layer II allocation tables         */
/* ------------------------------------------------------------------ */

static const int p_bitrates[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };

static const int p_step_index[9][16] = {
    {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17},
    {0, 1, 2, 3, 4, 5, 6, 17, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 2, 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
    {0, 1, 2, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 2, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
};

static const int p_nbal[9] = { 4, 4, 3, 2, 4, 3, 4, 3, 2 };

static const int p_line[5][32] = {
    {0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, -1, -1, -1,
     -1, -1},
    {0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3,
     -1, -1},
    {4, 4, 5, 5, 5, 5, 5, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1, -1, -1, -1, -1},
    {4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1, -1, -1, -1},
    {6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8}
};

static const int p_table_sblimit[5] = { 27, 30, 8, 12, 30 };

static const int p_bits[18] = { 0, 5, 7, 3, 10, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const int p_group[18] = { 0, 1, 1, 3, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
static const int p_jsbound[4] = { 4, 8, 12, 16 };
static const int p_sfs_per_scfsi[4] = { 3, 2, 1, 2 };

/* the MPEG-1 table selection rules for a 2ch stream, re-implemented */
static int p_tablenum(int bitrate, int samplerate)
{
    int br_per_ch = bitrate / 2;
    int sfrq = samplerate / 1000;

    if ((sfrq == 48 && br_per_ch >= 56) || (br_per_ch >= 56 && br_per_ch <= 80))
        return 0;
    else if (sfrq != 48 && br_per_ch >= 96)
        return 1;
    else if (sfrq != 32 && br_per_ch <= 48)
        return 2;
    else
        return 3;
}

static int p_legal_candidate(int bitrate)
{
    switch (bitrate) {
    case 64:
    case 96:
    case 112:
    case 128:
    case 160:
    case 192:
    case 224:
    case 256:
    case 320:
    case 384:
        return 1;
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* bit reader + independent frame parser                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *p;
    long bitpos;
} bitrd;

static int rd_bits(bitrd * b, int n)
{
    int v = 0, i;
    for (i = 0; i < n; i++) {
        v = (v << 1) | ((b->p[b->bitpos >> 3] >> (7 - (b->bitpos & 7))) & 1);
        b->bitpos++;
    }
    return v;
}

typedef struct {
    int bitrate_index;
    int bitrate;
    int samplerate;
    int padding;
    int mode;
    int mode_ext;
    int tablenum;
    int sblimit;
    int jsbound;
    int frame_bytes;
    int crc_present;
    unsigned int crc_stored;
    unsigned int crc_calc;
    int alloc[2][32];
    int scfsi[2][32];
    long used_bits;         /* header .. end of samples */
    long zero_bits;         /* trailing zero bits up to the frame end */
} frame_info;

/* independent CRC-16 (poly 0x8005) over header bytes 2-3 and the protected bits */
static unsigned int crc_upd(unsigned int value, unsigned int crc, int nbit)
{
    int i;
    value <<= 8;
    for (i = 0; i < nbit; i++) {
        value <<= 1;
        crc <<= 1;
        if (((crc ^ value) & 0x10000))
            crc ^= 0x8005;
    }
    return crc;
}

/* returns 0 on success, -1 on structural error */
static int parse_frame(const unsigned char *buf, int avail, frame_info * fi)
{
    static const int sr_table[3] = { 44100, 48000, 32000 };
    int sr_idx, crc_bits = 0;
    int sb, ch, gr, j;
    bitrd b;
    long i, remaining;
#define PARSE_FAIL(why) do { fprintf(stderr, "parse_frame fail: %s (bridx=%d mode=%d mext=%d)\n", why, fi->bitrate_index, fi->mode, fi->mode_ext); return -1; } while (0)

    memset(fi, 0, sizeof(*fi));

    if (avail < 4)
        PARSE_FAIL("short");
    if (buf[0] != 0xff || (buf[1] & 0xfe) != 0xfc)
        PARSE_FAIL("sync");     /* sync + MPEG-1 + Layer II */
    fi->crc_present = !(buf[1] & 1);
    fi->bitrate_index = buf[2] >> 4;
    if (fi->bitrate_index < 1 || fi->bitrate_index > 14)
        PARSE_FAIL("bitrate_index");
    sr_idx = (buf[2] >> 2) & 3;
    if (sr_idx > 2)
        PARSE_FAIL("sr_idx");
    fi->samplerate = sr_table[sr_idx];
    fi->padding = (buf[2] >> 1) & 1;
    fi->mode = (buf[3] >> 6) & 3;
    fi->mode_ext = (buf[3] >> 4) & 3;
    fi->bitrate = p_bitrates[fi->bitrate_index];
    fi->frame_bytes = 144000 * fi->bitrate / fi->samplerate + fi->padding;
    if (fi->frame_bytes > avail)
        PARSE_FAIL("frame_bytes>avail");

    fi->tablenum = p_tablenum(fi->bitrate, fi->samplerate);
    fi->sblimit = p_table_sblimit[fi->tablenum];
    fi->jsbound = (fi->mode == 1) ? p_jsbound[fi->mode_ext] : fi->sblimit;

    b.p = buf;
    b.bitpos = 32;              /* skip header */
    if (fi->crc_present) {
        fi->crc_stored = ((unsigned int) buf[4] << 8) | buf[5];
        b.bitpos += 16;
    }

    /* bit allocation */
    for (sb = 0; sb < fi->sblimit; sb++) {
        int nch_here = (sb < fi->jsbound) ? 2 : 1;
        int ln = p_line[fi->tablenum][sb];
        for (ch = 0; ch < nch_here; ch++) {
            fi->alloc[ch][sb] = rd_bits(&b, p_nbal[ln]);
            crc_bits += p_nbal[ln];
        }
        if (sb >= fi->jsbound)
            fi->alloc[1][sb] = fi->alloc[0][sb];
    }
    /* scfsi */
    for (sb = 0; sb < fi->sblimit; sb++)
        for (ch = 0; ch < 2; ch++)
            if (fi->alloc[ch][sb]) {
                fi->scfsi[ch][sb] = rd_bits(&b, 2);
                crc_bits += 2;
            }
    /* scalefactors */
    for (sb = 0; sb < fi->sblimit; sb++)
        for (ch = 0; ch < 2; ch++)
            if (fi->alloc[ch][sb])
                rd_bits(&b, 6 * p_sfs_per_scfsi[fi->scfsi[ch][sb]]);
    /* (possibly grouped) subband samples */
    for (gr = 0; gr < 3; gr++)
        for (j = 0; j < 12; j += 3)
            for (sb = 0; sb < fi->sblimit; sb++) {
                int nch_here = (sb < fi->jsbound) ? 2 : 1;
                for (ch = 0; ch < nch_here; ch++)
                    if (fi->alloc[ch][sb]) {
                        int si = p_step_index[p_line[fi->tablenum][sb]][fi->alloc[ch][sb]];
                        if (p_group[si] == 3)
                            rd_bits(&b, 3 * p_bits[si]);
                        else
                            rd_bits(&b, p_bits[si]);
                    }
            }

    fi->used_bits = b.bitpos;
    remaining = (long) fi->frame_bytes * 8 - fi->used_bits;
    if (remaining < 0)
        PARSE_FAIL("overrun");

    /* everything after the samples (padding + reserved ancillary) is zero */
    fi->zero_bits = 0;
    for (i = fi->used_bits; i < (long) fi->frame_bytes * 8; i++) {
        if ((buf[i >> 3] >> (7 - (i & 7))) & 1)
            PARSE_FAIL("nonzero trailing");
        fi->zero_bits++;
    }

    /* verify the CRC over header bytes 2-3 and the counted protected bits */
    if (fi->crc_present) {
        unsigned int crc = 0xffff;
        int whole = crc_bits >> 3, byte;
        crc = crc_upd(buf[2], crc, 8);
        crc = crc_upd(buf[3], crc, 8);
        for (byte = 6; byte < 6 + whole; byte++)
            crc = crc_upd(buf[byte], crc, 8);
        if (crc_bits & 7)
            crc = crc_upd(buf[byte], crc, crc_bits & 7);
        fi->crc_calc = crc;
    }

    return 0;
#undef PARSE_FAIL
}

static int crc_matches(const frame_info * fi)
{
    return (((fi->crc_calc >> 8) & 0xff) == (fi->crc_stored >> 8))
           && ((fi->crc_calc & 0xff) == (fi->crc_stored & 0xff));
}


/* ------------------------------------------------------------------ */
/* shared helpers                                                      */
/* ------------------------------------------------------------------ */

#define NFRAMES 10
#define NSAMP (1152 * NFRAMES)
#define OUTCAP (1 << 20)

static short pcmL[NSAMP], pcmR[NSAMP], pcmIL[2 * NSAMP];
static unsigned char outbuf[OUTCAP];

/* non-zero, left != right, quiet -> loud -> quiet continuous material */
static void gen_signals(void)
{
    int i;
    for (i = 0; i < NSAMP; i++) {
        double t = i / 44100.0;
        int loud = (i >= 1152 * 3 && i < 1152 * 7);
        double l, r;
        if (loud) {
            l = 25000 * sin(2 * M_PI * 1000 * t) + 20000 * sin(2 * M_PI * 3000 * t)
                + 15000 * sin(2 * M_PI * 7000 * t) + 12000 * sin(2 * M_PI * 11000 * t);
            r = 25000 * sin(2 * M_PI * 1300 * t) + 20000 * sin(2 * M_PI * 4200 * t)
                + 15000 * sin(2 * M_PI * 8300 * t) + 12000 * sin(2 * M_PI * 12500 * t);
        } else {
            l = 2 * ((i * 137) % 7 - 3);
            r = 2 * ((i * 251) % 5 - 2);
        }
        pcmL[i] = (short) l;
        pcmR[i] = (short) r;
        pcmIL[2 * i] = (short) l;
        pcmIL[2 * i + 1] = (short) r;
    }
}

/* loud everywhere: no frame can ever fit a small bitrate */
static void gen_loud(void)
{
    int i;
    for (i = 0; i < NSAMP; i++) {
        double t = i / 44100.0;
        double l = 25000 * sin(2 * M_PI * 1000 * t) + 20000 * sin(2 * M_PI * 3000 * t)
            + 15000 * sin(2 * M_PI * 7000 * t) + 12000 * sin(2 * M_PI * 11000 * t);
        double r = 25000 * sin(2 * M_PI * 1300 * t) + 20000 * sin(2 * M_PI * 4200 * t)
            + 15000 * sin(2 * M_PI * 8300 * t) + 12000 * sin(2 * M_PI * 12500 * t);
        pcmL[i] = (short) l;
        pcmR[i] = (short) r;
        pcmIL[2 * i] = (short) l;
        pcmIL[2 * i + 1] = (short) r;
    }
}

/* chunk <= 0 feeds everything in one call */
static int encode_short(twolame_options * o, int nsamp, int chunk, int planar,
                        unsigned char * out, int cap)
{
    int total = 0, pos = 0, z;
    while (pos < nsamp) {
        int n = (chunk > 0) ? chunk : nsamp;
        if (n > nsamp - pos)
            n = nsamp - pos;
        if (planar)
            z = twolame_encode_buffer(o, pcmL + pos, pcmR + pos, n, out + total, cap - total);
        else
            z = twolame_encode_buffer_interleaved(o, pcmIL + 2 * pos, n, out + total, cap - total);
        if (z < 0)
            return -1;
        total += z;
        pos += n;
    }
    z = twolame_encode_flush(o, out + total, cap - total);
    if (z < 0)
        return -1;
    return total + z;
}

static twolame_options *make_full_range(int sr, TWOLAME_MPEG_mode mode, float level,
                                        int bitrate, int max_bitrate)
{
    twolame_options * o = twolame_init();
    if (!o)
        return NULL;
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, sr);
    twolame_set_out_samplerate(o, sr);
    twolame_set_mode(o, mode);
    twolame_set_VBR(o, 1);
    twolame_set_VBR_level(o, level);
    if (bitrate > 0)
        twolame_set_bitrate(o, bitrate);
    if (max_bitrate > 0)
        twolame_set_VBR_max_bitrate_kbps(o, max_bitrate);
    twolame_set_VBR_full_range(o, 1);
    return o;
}

/* walk a whole stream, parsing every frame; returns frame count or -1 */
static int walk_stream(const unsigned char * buf, int total, frame_info * fi, int max_frames)
{
    int pos = 0, n = 0;
    while (pos < total) {
        if (n >= max_frames)
            return -1;
        if (parse_frame(buf + pos, total - pos, &fi[n]) < 0)
            return -1;
        pos += fi[n].frame_bytes;
        n++;
    }
    if (pos != total)
        return -1;
    return n;
}


/* ------------------------------------------------------------------ */
/* case_selection: candidate order and exact capacity, driven by       */
/* controlled SMR and scfsi through the internal selection entry point */
/* ------------------------------------------------------------------ */

int case_selection(void)
{
    case_t c = { 0, 0 };
    twolame_options * o;
    static FLOAT smr[2][SBLIMIT];
    static unsigned int scfsi[2][SBLIMIT];
    unsigned int bit_alloc[2][SBLIMIT];
    unsigned int expect_alloc[2][SBLIMIT];
    int adb, ret, sb, ch, req;

    /* --- 1: lowest fitting candidate wins; plain stereo tried first --- */
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 0.0f, 64, 384);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    CHECK(&c, o->lower_index == 4);     /* 64 kbps */
    CHECK(&c, o->upper_index == 14);    /* 384 kbps */
    CHECK(&c, o->vbr_full_sblimit == 30);   /* 192+ kbps use the 30-subband table */

    for (ch = 0; ch < 2; ch++)
        for (sb = 0; sb < SBLIMIT; sb++) {
            smr[ch][sb] = 5.0;
            scfsi[ch][sb] = 0;
        }
    adb = -1;
    ret = twolame_vbr_full_range_pick(o, smr, scfsi, bit_alloc, &adb);
    CHECK(&c, ret == 0);
    CHECK(&c, o->header.bitrate_index == 4);    /* 64 kbps is enough */
    CHECK(&c, o->header.mode == TWOLAME_STEREO);    /* stereo fits, joint not needed */
    CHECK(&c, o->tablenum == 2);
    CHECK(&c, o->sblimit == 8);
    CHECK(&c, o->jsbound == 8);
    /* the winning allocation is exactly what bits_for_nonoise dictates */
    o->tablenum = 2;
    o->sblimit = 8;
    o->jsbound = 8;
    memset(expect_alloc, 0, sizeof(expect_alloc));
    req = twolame_bits_for_nonoise(o, smr, scfsi, 0.0, expect_alloc);
    CHECK(&c, memcmp(bit_alloc, expect_alloc, sizeof(bit_alloc)) == 0);
    CHECK(&c, adb == 8 * (144000 * 64 / 44100) - req);

    /* --- 2: a candidate that exactly fills the frame counts as fitting;
       one byte less and the next bitrate candidate must be used --- */
    {
        twolame_options * os = make_full_range(44100, TWOLAME_STEREO, 0.0f, 64, 384);
        CHECK(&c, os != NULL);
        CHECK(&c, twolame_init_params(os) == 0);
        os->tablenum = 2;       /* 64 kbps stereo -> table 2, 8 subbands */
        os->sblimit = 8;
        os->jsbound = 8;
        memset(expect_alloc, 0, sizeof(expect_alloc));
        req = twolame_bits_for_nonoise(os, smr, scfsi, 0.0, expect_alloc);
        CHECK(&c, req < 8 * (144000 * 64 / 44100)); /* scenario sanity check */
        twolame_set_num_ancillary_bits(os, 8 * (144000 * 64 / 44100) - req);
        adb = -1;
        ret = twolame_vbr_full_range_pick(os, smr, scfsi, bit_alloc, &adb);
        CHECK(&c, ret == 0);
        CHECK(&c, os->header.bitrate_index == 4);   /* exact fit -> still 64 kbps */
        CHECK(&c, adb == 0);        /* not one bit spare */
        /* one byte less room and the next candidate (96 kbps) must be used */
        twolame_set_num_ancillary_bits(os, 8 * (144000 * 64 / 44100) - req + 8);
        adb = -1;
        ret = twolame_vbr_full_range_pick(os, smr, scfsi, bit_alloc, &adb);
        CHECK(&c, ret == 0);
        CHECK(&c, os->header.bitrate_index == 6);   /* 96 kbps */
        CHECK(&c, os->bitrate == 96);
        twolame_close(&os);
    }

    /* --- 3: joint boundaries beyond the sblimit are skipped; the 64 kbps
       table only has 8 subbands, so boundary 4 is the first usable one --- */
    for (ch = 0; ch < 2; ch++)
        for (sb = 0; sb < SBLIMIT; sb++) {
            smr[ch][sb] = (sb < 4) ? -50.0 : 5.0;
            scfsi[ch][sb] = 0;
        }
    /* independent requirements: stereo vs joint with boundary 4 */
    o->tablenum = 2;
    o->sblimit = 8;
    o->jsbound = 8;
    req = twolame_bits_for_nonoise(o, smr, scfsi, 0.0, expect_alloc);   /* stereo */
    o->jsbound = 4;
    {
        int req_j4 = twolame_bits_for_nonoise(o, smr, scfsi, 0.0, expect_alloc);
        CHECK(&c, req_j4 < req);    /* sharing subbands 4-7 must save bits */
        /* room for joint(4) exactly, but not for stereo */
        twolame_set_num_ancillary_bits(o, 8 * (144000 * 64 / 44100) - req_j4);
    }
    adb = -1;
    ret = twolame_vbr_full_range_pick(o, smr, scfsi, bit_alloc, &adb);
    CHECK(&c, ret == 0);
    CHECK(&c, o->header.bitrate_index == 4);
    CHECK(&c, o->header.mode == TWOLAME_JOINT_STEREO);
    CHECK(&c, o->header.mode_ext == 0); /* boundary 4 (16/12/8 are not below sblimit 8) */
    CHECK(&c, o->jsbound == 4);
    CHECK(&c, o->jsbound <= o->sblimit);
    CHECK(&c, adb == 0);        /* exact fit again */
    twolame_set_num_ancillary_bits(o, 0);
    twolame_close(&o);

    /* --- 4: at the same bitrate the widest fitting boundary wins --- */
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 0.0f, 128, 384);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    CHECK(&c, o->lower_index == 8); /* 128 kbps */
    for (ch = 0; ch < 2; ch++)
        for (sb = 0; sb < SBLIMIT; sb++) {
            smr[ch][sb] = (sb < 16) ? -50.0 : 5.0;
            scfsi[ch][sb] = 0;
        }
    o->tablenum = 0;            /* 128 kbps stereo -> table 0, 27 subbands */
    o->sblimit = 27;
    o->jsbound = 27;
    req = twolame_bits_for_nonoise(o, smr, scfsi, 0.0, expect_alloc);   /* stereo */
    o->jsbound = 16;
    {
        int req_j16 = twolame_bits_for_nonoise(o, smr, scfsi, 0.0, expect_alloc);
        int req_j12, avail = 8 * (144000 * 128 / 44100);
        CHECK(&c, req_j16 < req);   /* stereo alone cannot fit in joint(16)'s room */
        o->jsbound = 12;
        req_j12 = twolame_bits_for_nonoise(o, smr, scfsi, 0.0, expect_alloc);
        /* exact room for boundary 16; boundary 12 would fit too, but 16 comes first */
        twolame_set_num_ancillary_bits(o, avail - req_j16);
        CHECK(&c, req_j12 <= req_j16);
    }
    adb = -1;
    ret = twolame_vbr_full_range_pick(o, smr, scfsi, bit_alloc, &adb);
    CHECK(&c, ret == 0);
    CHECK(&c, o->header.bitrate_index == 8);    /* stays at 128 kbps */
    CHECK(&c, o->header.mode == TWOLAME_JOINT_STEREO);
    CHECK(&c, o->header.mode_ext == 3); /* boundary 16 preferred over 12/8/4 */
    CHECK(&c, o->jsbound == 16);
    CHECK(&c, adb == 0);
    twolame_set_num_ancillary_bits(o, 0);

    /* --- 5: stereo is preferred over joint at the same bitrate --- */
    for (ch = 0; ch < 2; ch++)
        for (sb = 0; sb < SBLIMIT; sb++) {
            smr[ch][sb] = -50.0;    /* next to nothing to encode */
            scfsi[ch][sb] = 0;
        }
    adb = -1;
    ret = twolame_vbr_full_range_pick(o, smr, scfsi, bit_alloc, &adb);
    CHECK(&c, ret == 0);
    CHECK(&c, o->header.bitrate_index == 8);    /* lowest candidate */
    CHECK(&c, o->header.mode == TWOLAME_STEREO);    /* stereo first, joint untouched */
    CHECK(&c, o->header.mode_ext == 0);
    twolame_close(&o);

    /* --- 6: nothing fits -> bounded fallback at the top candidate --- */
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 0.0f, 64, 384);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    for (ch = 0; ch < 2; ch++)
        for (sb = 0; sb < SBLIMIT; sb++) {
            smr[ch][sb] = 90.0; /* demand more than any bitrate offers */
            scfsi[ch][sb] = 0;
        }
    adb = -1;
    ret = twolame_vbr_full_range_pick(o, smr, scfsi, bit_alloc, &adb);
    CHECK(&c, ret == 1);        /* fallback: quality target honestly not met */
    CHECK(&c, o->header.bitrate_index == 14);   /* top candidate 384 kbps */
    CHECK(&c, o->header.mode == TWOLAME_STEREO);
    CHECK(&c, o->tablenum == 1);
    CHECK(&c, o->sblimit == 30);
    CHECK(&c, adb >= 0);        /* bounded allocation never overruns the frame */
    {
        int ok = 1;
        for (sb = 0; sb < 30; sb++)
            for (ch = 0; ch < 2; ch++)
                if (bit_alloc[ch][sb] > (1u << p_nbal[p_line[1][sb]]) - 1)
                    ok = 0;
        for (sb = 30; sb < SBLIMIT; sb++)
            if (bit_alloc[0][sb] || bit_alloc[1][sb])
                ok = 0;
        CHECK(&c, ok);          /* allocation stays inside the table limits */
    }
    twolame_close(&o);

    return CASE_RESULT(c);
}


/* ------------------------------------------------------------------ */
/* case_syntax: API contract, illegal configurations, CRC + ancillary, */
/* full independent frame parsing                                      */
/* ------------------------------------------------------------------ */

/* one illegal configuration: full-range on, everything else valid except
   the single knob varied by the caller. Returns init_params() result. */
static int try_init(int vbr, int channels, TWOLAME_MPEG_mode mode, int sr,
                    int bitrate, int max_bitrate)
{
    twolame_options * o = twolame_init();
    int r;
    if (!o)
        return -99;
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, channels);
    twolame_set_in_samplerate(o, sr);
    twolame_set_out_samplerate(o, sr);
    twolame_set_mode(o, mode);
    twolame_set_VBR(o, vbr);
    if (bitrate > 0)
        twolame_set_bitrate(o, bitrate);
    if (max_bitrate > 0)
        twolame_set_VBR_max_bitrate_kbps(o, max_bitrate);
    twolame_set_VBR_full_range(o, 1);
    r = twolame_init_params(o);
    twolame_close(&o);
    return r;
}

int case_syntax(void)
{
    case_t c = { 0, 0 };
    twolame_options * o;
    int total, nframes, i, pos;

    /* --- setter/getter contract --- */
    o = twolame_init();
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_get_VBR_full_range(o) == 0);  /* default off */
    CHECK(&c, twolame_set_VBR_full_range(o, 2) != 0);   /* only 0/1 accepted */
    CHECK(&c, twolame_get_VBR_full_range(o) == 0);
    CHECK(&c, twolame_set_VBR_full_range(o, -1) != 0);
    CHECK(&c, twolame_set_VBR_full_range(o, 1) == 0);
    CHECK(&c, twolame_get_VBR_full_range(o) == 1);
    CHECK(&c, twolame_set_VBR_full_range(o, 0) == 0);
    CHECK(&c, twolame_get_VBR_full_range(o) == 0);
    twolame_close(&o);

    /* --- illegal combinations must fail at init --- */
    CHECK(&c, try_init(0, 2, TWOLAME_STEREO, 44100, 64, 384) != 0); /* VBR off */
    CHECK(&c, try_init(1, 1, TWOLAME_MONO, 44100, 64, 384) != 0);   /* mono */
    CHECK(&c, try_init(1, 2, TWOLAME_DUAL_CHANNEL, 44100, 64, 384) != 0); /* dual */
    CHECK(&c, try_init(1, 2, TWOLAME_STEREO, 22050, 64, 384) != 0); /* MPEG-2 */
    CHECK(&c, try_init(1, 2, TWOLAME_STEREO, 44100, 64, 48) != 0);  /* no candidate <= 48 */
    CHECK(&c, try_init(1, 2, TWOLAME_STEREO, 44100, 384, 64) != 0); /* empty interval */

    /* --- legal combinations succeed and keep the requested mode --- */
    CHECK(&c, try_init(1, 2, TWOLAME_STEREO, 44100, 64, 384) == 0);
    CHECK(&c, try_init(1, 2, TWOLAME_JOINT_STEREO, 48000, 96, 320) == 0);
    CHECK(&c, try_init(1, 2, TWOLAME_JOINT_STEREO, 32000, 64, 384) == 0);
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 5.0f, 64, 384);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    CHECK(&c, twolame_get_mode(o) == TWOLAME_JOINT_STEREO); /* not forced to stereo */
    twolame_close(&o);

    /* --- end-to-end with CRC and reserved ancillary bits, fully parsed --- */
    gen_signals();
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 5.0f, 64, 384);
    CHECK(&c, o != NULL);
    twolame_set_error_protection(o, 1);
    twolame_set_num_ancillary_bits(o, 64);
    CHECK(&c, twolame_init_params(o) == 0);
    total = encode_short(o, NSAMP, 0, 0, outbuf, OUTCAP);
    CHECK(&c, total > 0);
    twolame_close(&o);

    {
        static frame_info fi[64];
        nframes = walk_stream(outbuf, total, fi, 64);
        CHECK(&c, nframes == NFRAMES);
        pos = 0;
        for (i = 0; i < nframes && i < 64; i++) {
            frame_info * f = &fi[i];
            CHECK(&c, f->padding == 0); /* VBR frames are never padded */
            CHECK(&c, p_legal_candidate(f->bitrate));
            CHECK(&c, f->bitrate >= 64 && f->bitrate <= 384);
            CHECK(&c, f->mode == 0 || f->mode == 1);    /* stereo or joint stereo */
            CHECK(&c, f->jsbound <= f->sblimit);    /* boundary inside the table */
            if (f->mode == 0)
                CHECK(&c, f->mode_ext == 0);
            CHECK(&c, f->used_bits + f->zero_bits == (long) f->frame_bytes * 8);
            CHECK(&c, f->zero_bits >= 64);  /* reserved ancillary bits are there */
            CHECK(&c, f->crc_present);
            CHECK(&c, crc_matches(f));
            pos += f->frame_bytes;
        }
        CHECK(&c, pos == total);    /* frames tile the stream exactly */
    }

    /* --- 48 kHz end-to-end: table rules and frame lengths follow the rate --- */
    o = make_full_range(48000, TWOLAME_JOINT_STEREO, 10.0f, 64, 384);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    total = encode_short(o, NSAMP, 0, 0, outbuf, OUTCAP);
    CHECK(&c, total > 0);
    twolame_close(&o);
    {
        static frame_info fi[64];
        nframes = walk_stream(outbuf, total, fi, 64);
        CHECK(&c, nframes == NFRAMES);
        pos = 0;
        for (i = 0; i < nframes && i < 64; i++) {
            CHECK(&c, fi[i].samplerate == 48000);
            CHECK(&c, p_legal_candidate(fi[i].bitrate));
            CHECK(&c, fi[i].jsbound <= fi[i].sblimit);
            CHECK(&c, fi[i].used_bits + fi[i].zero_bits == (long) fi[i].frame_bytes * 8);
            pos += fi[i].frame_bytes;
        }
        CHECK(&c, pos == total);
    }

    return CASE_RESULT(c);
}


/* ------------------------------------------------------------------ */
/* case_table_switch: real PCM through the public API, low->high->low  */
/* allocation table switching, joint and stereo output, byte-identical */
/* planar/interleaved and whole/chunked feeding                        */
/* ------------------------------------------------------------------ */

int case_table_switch(void)
{
    case_t c = { 0, 0 };
    twolame_options * o;
    static unsigned char out_planar[OUTCAP], out_chunk[OUTCAP], out_chunk_planar[OUTCAP];
    static unsigned char out_stereo[OUTCAP];
    static frame_info fi[64];
    int total, nframes, i;
    int first_high, saw_low_before, saw_low_after, saw_joint, saw_stereo;
    int saw_idx4, saw_idx10;

    gen_signals();

    /* --- joint stereo full-range encode, fed in one call --- */
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 10.0f, 64, 384);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    total = encode_short(o, NSAMP, 0, 0, outbuf, OUTCAP);
    CHECK(&c, total > 0);
    twolame_close(&o);

    nframes = walk_stream(outbuf, total, fi, 64);
    CHECK(&c, nframes == NFRAMES);

    /* low -> high -> low sblimit (allocation table) switching */
    first_high = -1;
    for (i = 0; i < nframes; i++)
        if (fi[i].sblimit >= 27) {
            first_high = i;
            break;
        }
    CHECK(&c, first_high > 0);      /* some frame switched to a wide table */
    saw_low_before = saw_low_after = 0;
    for (i = 0; i < nframes; i++) {
        if (fi[i].sblimit <= 12 && first_high > 0 && i < first_high)
            saw_low_before = 1;
        if (fi[i].sblimit <= 12 && i > first_high)
            saw_low_after = 1;
    }
    CHECK(&c, saw_low_before);      /* narrow table before the loud part */
    CHECK(&c, saw_low_after);       /* and back down afterwards */
    CHECK(&c, fi[0].sblimit == 8);  /* 64 kbps -> 8-subband table */
    CHECK(&c, fi[nframes - 1].sblimit == 8);

    /* the full bitrate range is really used */
    saw_idx4 = saw_idx10 = 0;
    saw_joint = saw_stereo = 0;
    for (i = 0; i < nframes; i++) {
        if (fi[i].bitrate_index == 4)
            saw_idx4 = 1;
        if (fi[i].bitrate_index >= 10)
            saw_idx10 = 1;
        if (fi[i].mode == 1)
            saw_joint = 1;
        if (fi[i].mode == 0)
            saw_stereo = 1;
        CHECK(&c, p_legal_candidate(fi[i].bitrate));
        CHECK(&c, fi[i].jsbound <= fi[i].sblimit);
        CHECK(&c, fi[i].used_bits + fi[i].zero_bits == (long) fi[i].frame_bytes * 8);
    }
    CHECK(&c, saw_idx4);            /* 64 kbps frames */
    CHECK(&c, saw_idx10);           /* 192+ kbps frames */
    CHECK(&c, saw_joint);           /* joint stereo output produced */
    CHECK(&c, saw_stereo);          /* plain stereo output produced */

    /* --- same sound, planar whole-buffer: byte-identical --- */
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 10.0f, 64, 384);
    CHECK(&c, twolame_init_params(o) == 0);
    {
        int n = encode_short(o, NSAMP, 0, 1, out_planar, OUTCAP);
        CHECK(&c, n == total);
        CHECK(&c, memcmp(outbuf, out_planar, total) == 0);
    }
    twolame_close(&o);

    /* --- same sound, interleaved in 137-sample scraps: byte-identical --- */
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 10.0f, 64, 384);
    CHECK(&c, twolame_init_params(o) == 0);
    {
        int n = encode_short(o, NSAMP, 137, 0, out_chunk, OUTCAP);
        CHECK(&c, n == total);
        CHECK(&c, memcmp(outbuf, out_chunk, total) == 0);
    }
    twolame_close(&o);

    /* --- same sound, planar in 113-sample scraps: byte-identical --- */
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 10.0f, 64, 384);
    CHECK(&c, twolame_init_params(o) == 0);
    {
        int n = encode_short(o, NSAMP, 113, 1, out_chunk_planar, OUTCAP);
        CHECK(&c, n == total);
        CHECK(&c, memcmp(outbuf, out_chunk_planar, total) == 0);
    }
    twolame_close(&o);

    /* --- requested stereo: every frame is plain stereo --- */
    o = make_full_range(44100, TWOLAME_STEREO, 10.0f, 64, 384);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    {
        int n = encode_short(o, NSAMP, 0, 0, out_stereo, OUTCAP);
        static frame_info sfi[64];
        int sn = walk_stream(out_stereo, n, sfi, 64);
        CHECK(&c, n > 0);
        CHECK(&c, sn == NFRAMES);
        for (i = 0; i < sn; i++) {
            CHECK(&c, sfi[i].mode == 0);    /* stereo only, as requested */
            CHECK(&c, sfi[i].mode_ext == 0);
        }
    }
    twolame_close(&o);

    /* stereo run is also chunking-invariant */
    o = make_full_range(44100, TWOLAME_STEREO, 10.0f, 64, 384);
    CHECK(&c, twolame_init_params(o) == 0);
    {
        int n = encode_short(o, NSAMP, 0, 0, outbuf, OUTCAP);
        int m;
        twolame_close(&o);
        o = make_full_range(44100, TWOLAME_STEREO, 10.0f, 64, 384);
        CHECK(&c, twolame_init_params(o) == 0);
        m = encode_short(o, NSAMP, 299, 0, out_chunk, OUTCAP);
        CHECK(&c, m == n);
        CHECK(&c, memcmp(outbuf, out_chunk, n) == 0);
    }
    twolame_close(&o);

    return CASE_RESULT(c);
}


/* ------------------------------------------------------------------ */
/* case_compatibility: with full-range off nothing changes; bounded    */
/* fallback at the upper bound; old-mode byte comparison               */
/* ------------------------------------------------------------------ */

int case_compatibility(void)
{
    case_t c = { 0, 0 };
    twolame_options * o;
    static unsigned char ref[OUTCAP], cmp[OUTCAP];
    static short base_pcm[1152 * 2 * 4];
    int total, n, i;

    /* --- the old VBR behaviour, byte for byte (mirrors devcheck/baseline.c) --- */
    for (i = 0; i < 1152 * 4; i++) {
        base_pcm[2 * i] = (short) ((i * 137) % 20000 - 10000);
        base_pcm[2 * i + 1] = (short) ((i * 251) % 18000 - 9000);
    }

    o = twolame_init();
    CHECK(&c, o != NULL);
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, 44100);
    twolame_set_out_samplerate(o, 44100);
    twolame_set_mode(o, TWOLAME_JOINT_STEREO);
    twolame_set_VBR(o, 1);
    CHECK(&c, twolame_get_VBR_full_range(o) == 0);  /* default off */
    CHECK(&c, twolame_init_params(o) == 0);
    CHECK(&c, twolame_get_mode(o) == TWOLAME_STEREO);   /* old joint->stereo fallback */
    total = 0;
    n = twolame_encode_buffer_interleaved(o, base_pcm, 1152 * 4, ref, OUTCAP);
    CHECK(&c, n > 0);
    total += n;
    n = twolame_encode_flush(o, ref + total, OUTCAP - total);
    CHECK(&c, n >= 0);
    total += n;
    twolame_close(&o);
    CHECK(&c, total == 4176);   /* exactly the old byte count */
    {
        static frame_info fi[8];
        int nf = walk_stream(ref, total, fi, 8);
        CHECK(&c, nf == 4);     /* four frames, exact lengths */
    }

    /* explicitly disabling full-range keeps the exact same bytes */
    o = twolame_init();
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, 44100);
    twolame_set_out_samplerate(o, 44100);
    twolame_set_mode(o, TWOLAME_JOINT_STEREO);
    twolame_set_VBR(o, 1);
    CHECK(&c, twolame_set_VBR_full_range(o, 0) == 0);
    CHECK(&c, twolame_init_params(o) == 0);
    n = twolame_encode_buffer_interleaved(o, base_pcm, 1152 * 4, cmp, OUTCAP);
    CHECK(&c, n >= 0);
    {
        int m = twolame_encode_flush(o, cmp + n, OUTCAP - n);
        CHECK(&c, m >= 0);
        n += m;
    }
    twolame_close(&o);
    CHECK(&c, n == total);
    CHECK(&c, memcmp(ref, cmp, total) == 0);    /* old-mode byte comparison */

    /* old VBR chunked feeding still equals whole-buffer feeding */
    o = twolame_init();
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, 44100);
    twolame_set_out_samplerate(o, 44100);
    twolame_set_mode(o, TWOLAME_JOINT_STEREO);
    twolame_set_VBR(o, 1);
    CHECK(&c, twolame_init_params(o) == 0);
    {
        int pos = 0;
        n = 0;
        while (pos < 1152 * 4) {
            int take = 137;
            int z;
            if (take > 1152 * 4 - pos)
                take = 1152 * 4 - pos;
            z = twolame_encode_buffer_interleaved(o, base_pcm + pos * 2, take,
                                                  cmp + n, OUTCAP - n);
            CHECK(&c, z >= 0);
            n += z;
            pos += take;
        }
        {
            int z = twolame_encode_flush(o, cmp + n, OUTCAP - n);
            CHECK(&c, z >= 0);
            n += z;
        }
    }
    twolame_close(&o);
    CHECK(&c, n == total);
    CHECK(&c, memcmp(ref, cmp, total) == 0);

    /* old VBR keeps its single-table bitrate range: at 44.1 kHz stereo the
       old limits are 192..384 kbps, so a 128 kbps ceiling is rejected and a
       256 kbps ceiling is honoured */
    o = twolame_init();
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, 44100);
    twolame_set_out_samplerate(o, 44100);
    twolame_set_mode(o, TWOLAME_STEREO);
    twolame_set_VBR(o, 1);
    twolame_set_VBR_max_bitrate_kbps(o, 128);
    CHECK(&c, twolame_init_params(o) != 0); /* outside the old range: rejected */
    twolame_close(&o);
    o = twolame_init();
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, 44100);
    twolame_set_out_samplerate(o, 44100);
    twolame_set_mode(o, TWOLAME_STEREO);
    twolame_set_VBR(o, 1);
    twolame_set_VBR_max_bitrate_kbps(o, 256);
    CHECK(&c, twolame_init_params(o) == 0);
    gen_signals();
    n = encode_short(o, NSAMP, 0, 0, cmp, OUTCAP);
    CHECK(&c, n > 0);
    twolame_close(&o);
    {
        static frame_info fi[64];
        int nf = walk_stream(cmp, n, fi, 64);
        CHECK(&c, nf == NFRAMES);
        for (i = 0; i < nf; i++) {
            CHECK(&c, fi[i].bitrate_index >= 10);   /* old lower limit: 192 kbps */
            CHECK(&c, fi[i].bitrate_index <= 12);   /* never above 256 kbps */
        }
    }

    /* CBR is untouched: fixed 192 kbps frames of exactly 522 bytes */
    o = twolame_init();
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, 44100);
    twolame_set_out_samplerate(o, 44100);
    twolame_set_mode(o, TWOLAME_STEREO);
    twolame_set_bitrate(o, 192);
    CHECK(&c, twolame_init_params(o) == 0);
    n = twolame_encode_buffer_interleaved(o, base_pcm, 1152 * 4, cmp, OUTCAP);
    CHECK(&c, n > 0);
    {
        int m = twolame_encode_flush(o, cmp + n, OUTCAP - n);
        CHECK(&c, m >= 0);
        n += m;
    }
    twolame_close(&o);
    CHECK(&c, n == 4 * (144000 * 192 / 44100));
    {
        static frame_info fi[8];
        int nf = walk_stream(cmp, n, fi, 8);
        CHECK(&c, nf == 4);
        for (i = 0; i < nf; i++)
            CHECK(&c, fi[i].bitrate_index == 10);   /* 192 kbps every frame */
    }

    /* --- full-range fallback: upper bound too small for the demand --- */
    gen_loud();
    o = make_full_range(44100, TWOLAME_JOINT_STEREO, 50.0f, 64, 128);
    CHECK(&c, o != NULL);
    CHECK(&c, twolame_init_params(o) == 0);
    n = encode_short(o, NSAMP, 0, 0, cmp, OUTCAP);
    CHECK(&c, n > 0);
    twolame_close(&o);
    {
        static frame_info fi[64];
        int nf = walk_stream(cmp, n, fi, 64);
        CHECK(&c, nf == NFRAMES);
        for (i = 0; i < nf; i++) {
            CHECK(&c, fi[i].bitrate_index == 8);    /* pinned at the 128 kbps ceiling */
            CHECK(&c, fi[i].mode == 0); /* fallback is plain stereo */
            CHECK(&c, fi[i].sblimit == 27); /* 128 kbps -> table 0 */
            CHECK(&c, fi[i].used_bits <= (long) fi[i].frame_bytes * 8); /* no overrun */
            CHECK(&c, fi[i].used_bits + fi[i].zero_bits == (long) fi[i].frame_bytes * 8);
        }
    }

    return CASE_RESULT(c);
}
