/*
 * bwf_join_test.c - timeline / metadata / error tests for bwf_join.
 *
 * Every input WAV is assembled here from raw bytes (independent RIFF
 * writer, no dr_wav on the producing side) and every expected PCM stream
 * is written out by hand.  The production modules are driven through the
 * same bwf_join_run() entry point the command line tool uses.
 */
#include "tools/bwf.h"
#include "check.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* tiny byte builder and hand-rolled RIFF/WAVE assembly                */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *p;
    size_t n, cap;
} bb;

static void bb_init(bb *b)
{
    b->cap = 256;
    b->n = 0;
    b->p = (unsigned char *)malloc(b->cap);
}

static void bb_need(bb *b, size_t extra)
{
    if (b->n + extra > b->cap) {
        while (b->n + extra > b->cap) b->cap *= 2;
        b->p = (unsigned char *)realloc(b->p, b->cap);
    }
}

static void bb_bytes(bb *b, const void *data, size_t n)
{
    bb_need(b, n);
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void bb_u8(bb *b, unsigned v)
{
    bb_need(b, 1);
    b->p[b->n++] = (unsigned char)v;
}

static void bb_u16(bb *b, unsigned v)
{
    unsigned char d[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    bb_bytes(b, d, 2);
}

static void bb_u32(bb *b, drwav_uint32 v)
{
    unsigned char d[4];
    d[0] = (unsigned char)v;
    d[1] = (unsigned char)(v >> 8);
    d[2] = (unsigned char)(v >> 16);
    d[3] = (unsigned char)(v >> 24);
    bb_bytes(b, d, 4);
}

static void bb_zero(bb *b, size_t n)
{
    bb_need(b, n);
    memset(b->p + b->n, 0, n);
    b->n += n;
}

static void bb_strz(bb *b, const char *s)
{
    bb_bytes(b, s, strlen(s) + 1);
}

/* Begin a chunk; returns the position of the size field. */
static size_t bb_chunk_begin(bb *b, const char id[4])
{
    size_t sizePos;
    bb_bytes(b, id, 4);
    sizePos = b->n;
    bb_u32(b, 0);
    return sizePos;
}

static void bb_chunk_end(bb *b, size_t sizePos)
{
    drwav_uint32 size = (drwav_uint32)(b->n - sizePos - 4);
    b->p[sizePos + 0] = (unsigned char)size;
    b->p[sizePos + 1] = (unsigned char)(size >> 8);
    b->p[sizePos + 2] = (unsigned char)(size >> 16);
    b->p[sizePos + 3] = (unsigned char)(size >> 24);
    if (size & 1u) bb_u8(b, 0);   /* RIFF word alignment */
}

static void bb_fmt(bb *b, drwav_uint16 channels, drwav_uint32 rate, drwav_uint16 bits)
{
    size_t pos = bb_chunk_begin(b, "fmt ");
    bb_u16(b, DR_WAVE_FORMAT_PCM);
    bb_u16(b, channels);
    bb_u32(b, rate);
    bb_u32(b, rate * channels * bits / 8);
    bb_u16(b, (unsigned short)(channels * bits / 8));
    bb_u16(b, bits);
    bb_chunk_end(b, pos);
}

/* Minimal bext: description, then zeroed fixed fields and the time reference. */
static void bb_bext(bb *b, drwav_uint64 timeRef, const char *desc)
{
    size_t pos = bb_chunk_begin(b, "bext");
    size_t i;
    bb_zero(b, 256);
    if (desc != NULL) {
        size_t len = strlen(desc);
        if (len > 255) len = 255;
        memcpy(b->p + b->n - 256, desc, len);
    }
    bb_zero(b, 32 + 32 + 10 + 8);              /* originator / ref / date / time */
    bb_u32(b, (drwav_uint32)timeRef);
    bb_u32(b, (drwav_uint32)(timeRef >> 32));
    bb_u16(b, 1);                              /* version 1 */
    bb_zero(b, 64);                            /* UMID */
    for (i = 0; i < 5; ++i) bb_u16(b, 0);      /* loudness fields */
    bb_zero(b, 180);                           /* reserved */
    bb_chunk_end(b, pos);
}

typedef struct
{
    drwav_uint32 id;
    drwav_uint32 offset;
} test_cue_pt;

static void bb_cue(bb *b, const test_cue_pt *pts, drwav_uint32 count)
{
    drwav_uint32 i;
    size_t pos = bb_chunk_begin(b, "cue ");
    bb_u32(b, count);
    for (i = 0; i < count; ++i) {
        bb_u32(b, pts[i].id);
        bb_u32(b, 0);                          /* playOrderPosition */
        bb_bytes(b, "data", 4);
        bb_u32(b, 0);                          /* chunkStart */
        bb_u32(b, 0);                          /* blockStart */
        bb_u32(b, pts[i].offset);
    }
    bb_chunk_end(b, pos);
}

#define TEST_LABL 0
#define TEST_NOTE 1
#define TEST_LTXT 2

typedef struct
{
    int kind;
    drwav_uint32 cueId;
    const char *text;
    drwav_uint32 length;                       /* ltxt only */
} test_adtl;

static void bb_adtl(bb *b, const test_adtl *entries, drwav_uint32 count)
{
    drwav_uint32 i;
    size_t listPos = bb_chunk_begin(b, "LIST");
    bb_bytes(b, "adtl", 4);
    for (i = 0; i < count; ++i) {
        size_t pos;
        if (entries[i].kind == TEST_LTXT) {
            pos = bb_chunk_begin(b, "ltxt");
            bb_u32(b, entries[i].cueId);
            bb_u32(b, entries[i].length);
            bb_bytes(b, "rgn ", 4);
            bb_u16(b, 0); bb_u16(b, 0); bb_u16(b, 0); bb_u16(b, 0);
            bb_strz(b, entries[i].text);
            bb_chunk_end(b, pos);
        } else {
            pos = bb_chunk_begin(b, entries[i].kind == TEST_LABL ? "labl" : "note");
            bb_u32(b, entries[i].cueId);
            bb_strz(b, entries[i].text);
            bb_chunk_end(b, pos);
        }
    }
    bb_chunk_end(b, listPos);
}

typedef struct
{
    drwav_uint32 cueId, type, first, last, fraction, playCount;
} test_loop;

static void bb_smpl(bb *b, drwav_uint32 unityNote, const test_loop *loops,
                    drwav_uint32 loopCount, const char *specific, drwav_uint32 specificSize)
{
    drwav_uint32 i;
    size_t pos = bb_chunk_begin(b, "smpl");
    bb_u32(b, 7);                    /* manufacturer */
    bb_u32(b, 8);                    /* product */
    bb_u32(b, 1000);                 /* sample period ns */
    bb_u32(b, unityNote);
    bb_u32(b, 1);                    /* pitch fraction */
    bb_u32(b, 0); bb_u32(b, 0);      /* SMPTE */
    bb_u32(b, loopCount);
    bb_u32(b, specificSize);
    for (i = 0; i < loopCount; ++i) {
        bb_u32(b, loops[i].cueId);
        bb_u32(b, loops[i].type);
        bb_u32(b, loops[i].first);
        bb_u32(b, loops[i].last);
        bb_u32(b, loops[i].fraction);
        bb_u32(b, loops[i].playCount);
    }
    if (specificSize) bb_bytes(b, specific, specificSize);
    bb_chunk_end(b, pos);
}

static void bb_data(bb *b, const unsigned char *pcm, size_t n)
{
    size_t pos = bb_chunk_begin(b, "data");
    bb_bytes(b, pcm, n);
    bb_chunk_end(b, pos);
}

static void bb_junk(bb *b)
{
    static const unsigned char junk[5] = { 1, 2, 3, 4, 5 };
    size_t pos = bb_chunk_begin(b, "junk");
    bb_bytes(b, junk, sizeof(junk));
    bb_chunk_end(b, pos);
}

static int write_wav(const char *path, const bb *body)
{
    FILE *f = fopen(path, "wb");
    drwav_uint32 riffSize = (drwav_uint32)body->n + 4;
    TASK_CHECK(f != NULL);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite(body->p, 1, body->n, f);
    TASK_CHECK(fclose(f) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* temp directory and join driver                                      */
/* ------------------------------------------------------------------ */

static char g_tmp[260];

static void tmp_path(char *out, size_t n, const char *name)
{
    static int ready = 0;
    if (!ready) {
        const char *base = getenv("TMPDIR");
        snprintf(g_tmp, sizeof(g_tmp), "%s/bwfjoin_test_XXXXXX",
                 (base != NULL && *base) ? base : ".");
        if (mkdtemp(g_tmp) == NULL) {
            perror("mkdtemp");
            exit(2);
        }
        ready = 1;
    }
    snprintf(out, n, "%s/%s", g_tmp, name);
}

static int run_join(const char *outName, const char **inNames, int n, char *err, size_t errSize)
{
    char *argv[12];
    int i;
    static char outBuf[260];
    TASK_CHECK(n + 2 <= (int)(sizeof(argv) / sizeof(argv[0])));
    tmp_path(outBuf, sizeof(outBuf), outName);
    argv[0] = "bwf_join";
    argv[1] = outBuf;
    for (i = 0; i < n; ++i) {
        argv[i + 2] = (char *)malloc(260);
        tmp_path(argv[i + 2], 260, inNames[i]);
    }
    {
        int rc = bwf_join_run(n + 2, argv, err, errSize);
        for (i = 0; i < n; ++i) free(argv[i + 2]);
        return rc;
    }
}

static int file_exists(const char *name)
{
    char buf[260];
    FILE *f;
    tmp_path(buf, sizeof(buf), name);
    f = fopen(buf, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

static drwav_metadata *find_md(drwav *w, drwav_metadata_type type)
{
    drwav_uint32 i;
    for (i = 0; i < w->metadataCount; ++i)
        if (w->pMetadata[i].type == type) return &w->pMetadata[i];
    return NULL;
}

static int open_output(const char *name, drwav *w)
{
    char buf[260];
    tmp_path(buf, sizeof(buf), name);
    return drwav_init_file_with_metadata(w, buf, 0, NULL);
}

/* Read the whole output as raw little-endian PCM bytes. */
static int read_output_bytes(const char *name, unsigned char *out, size_t n)
{
    drwav w;
    size_t got;
    TASK_CHECK(open_output(name, &w));
    TASK_CHECK(w.totalPCMFrameCount * w.fmt.blockAlign == n);
    got = drwav_read_raw(&w, n, out);
    TASK_CHECK(got == n);
    TASK_CHECK(drwav_uninit(&w) == DRWAV_SUCCESS);
    return 0;
}

/* ------------------------------------------------------------------ */
/* timeline group                                                      */
/* ------------------------------------------------------------------ */

static int case_out_of_order(void)
{
    /* A: frames 0..5  B: frames 10..13  C: frames 12..15, passed as C A B. */
    static const drwav_int16 aVals[6] = { 10, 11, 12, 13, 14, 15 };
    static const drwav_int16 bVals[4] = { 20, 21, 22, 23 };
    static const drwav_int16 cVals[4] = { 22, 23, 30, 31 };
    static const drwav_int16 expect[16] = {
        10, 11, 12, 13, 14, 15,
        0, 0, 0, 0,
        20, 21, 22, 23, 30, 31
    };
    bb ba, bb_, bc;
    char pa[260], pb[260], pc[260], err[BWF_ERR_SIZE];
    const char *names[3];
    drwav_int16 got[16];
    drwav w;

    bb_init(&ba); bb_init(&bb_); bb_init(&bc);
    bb_fmt(&ba, 1, 48000, 16); bb_bext(&ba, 0, "volume A"); bb_data(&ba, (const unsigned char *)aVals, sizeof(aVals));
    bb_fmt(&bb_, 1, 48000, 16); bb_bext(&bb_, 10, "volume B"); bb_data(&bb_, (const unsigned char *)bVals, sizeof(bVals));
    bb_fmt(&bc, 1, 48000, 16); bb_bext(&bc, 12, "volume C"); bb_data(&bc, (const unsigned char *)cVals, sizeof(cVals));
    tmp_path(pa, sizeof(pa), "oo_a.wav");
    tmp_path(pb, sizeof(pb), "oo_b.wav");
    tmp_path(pc, sizeof(pc), "oo_c.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_); write_wav(pc, &bc);

    names[0] = "oo_c.wav"; names[1] = "oo_a.wav"; names[2] = "oo_b.wav";
    TASK_CHECK(run_join("oo_out.wav", names, 3, err, sizeof(err)) == 0);
    TASK_CHECK(open_output("oo_out.wav", &w));
    TASK_CHECK(w.channels == 1 && w.sampleRate == 48000 && w.bitsPerSample == 16);
    TASK_CHECK(w.totalPCMFrameCount == 16);
    TASK_CHECK(find_md(&w, drwav_metadata_type_bext)->data.bext.timeReference == 0);
    TASK_CHECK(strcmp(find_md(&w, drwav_metadata_type_bext)->data.bext.pDescription, "volume C") == 0);
    TASK_CHECK(drwav_read_pcm_frames_le(&w, 16, got) == 16);
    TASK_CHECK(memcmp(got, expect, sizeof(expect)) == 0);
    TASK_CHECK(drwav_uninit(&w) == DRWAV_SUCCESS);

    free(ba.p); free(bb_.p); free(bc.p);
    return 0;
}

static int case_nested_overlap(void)
{
    /* X covers 0..9, Y 2..7, Z 4..7; all shared frames carry identical bytes. */
    static const drwav_int16 xVals[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const drwav_int16 yVals[6]  = { 2, 3, 4, 5, 6, 7 };
    static const drwav_int16 zVals[4]  = { 4, 5, 6, 7 };
    bb bx, by, bz;
    char px[260], py[260], pz[260], err[BWF_ERR_SIZE];
    const char *names[3] = { "no_z.wav", "no_y.wav", "no_x.wav" };
    drwav_int16 got[10];
    drwav w;

    bb_init(&bx); bb_init(&by); bb_init(&bz);
    bb_fmt(&bx, 1, 48000, 16); bb_bext(&bx, 0, NULL); bb_data(&bx, (const unsigned char *)xVals, sizeof(xVals));
    bb_fmt(&by, 1, 48000, 16); bb_bext(&by, 2, NULL); bb_data(&by, (const unsigned char *)yVals, sizeof(yVals));
    bb_fmt(&bz, 1, 48000, 16); bb_bext(&bz, 4, NULL); bb_data(&bz, (const unsigned char *)zVals, sizeof(zVals));
    tmp_path(px, sizeof(px), "no_x.wav");
    tmp_path(py, sizeof(py), "no_y.wav");
    tmp_path(pz, sizeof(pz), "no_z.wav");
    write_wav(px, &bx); write_wav(py, &by); write_wav(pz, &bz);

    TASK_CHECK(run_join("no_out.wav", names, 3, err, sizeof(err)) == 0);
    TASK_CHECK(open_output("no_out.wav", &w));
    TASK_CHECK(w.totalPCMFrameCount == 10);
    TASK_CHECK(drwav_read_pcm_frames_le(&w, 10, got) == 10);
    TASK_CHECK(memcmp(got, xVals, sizeof(xVals)) == 0);
    TASK_CHECK(drwav_uninit(&w) == DRWAV_SUCCESS);

    free(bx.p); free(by.p); free(bz.p);
    return 0;
}

static int case_stereo_conflict(void)
{
    /* Stereo 16-bit; the last shared frame (absolute frame 5) differs on the right channel. */
    static const drwav_int16 aVals[12] = { 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6 };
    static const drwav_int16 bVals[8]  = { 3, 3, 4, 4, 5, 5, 6, 600 };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "sc_a.wav", "sc_b.wav" };

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 2, 48000, 16); bb_bext(&ba, 0, NULL); bb_data(&ba, (const unsigned char *)aVals, sizeof(aVals));
    bb_fmt(&bb_, 2, 48000, 16); bb_bext(&bb_, 2, NULL); bb_data(&bb_, (const unsigned char *)bVals, sizeof(bVals));
    tmp_path(pa, sizeof(pa), "sc_a.wav");
    tmp_path(pb, sizeof(pb), "sc_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("sc_out.wav", names, 2, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "frame 5") != NULL);
    TASK_CHECK(!file_exists("sc_out.wav"));

    free(ba.p); free(bb_.p);
    return 0;
}

static int case_eight_bit_gap(void)
{
    static const unsigned char aVals[3] = { 0x00, 0x81, 0xFF };
    static const unsigned char bVals[2] = { 0x40, 0x7F };
    static const unsigned char expect[7] = { 0x00, 0x81, 0xFF, 0x80, 0x80, 0x40, 0x7F };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "8b_a.wav", "8b_b.wav" };
    unsigned char got[7];

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 1, 22050, 8); bb_bext(&ba, 0, NULL); bb_data(&ba, aVals, sizeof(aVals));
    bb_fmt(&bb_, 1, 22050, 8); bb_bext(&bb_, 5, NULL); bb_data(&bb_, bVals, sizeof(bVals));
    tmp_path(pa, sizeof(pa), "8b_a.wav");
    tmp_path(pb, sizeof(pb), "8b_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("8b_out.wav", names, 2, err, sizeof(err)) == 0);
    TASK_CHECK(read_output_bytes("8b_out.wav", got, sizeof(got)) == 0);
    TASK_CHECK(memcmp(got, expect, sizeof(expect)) == 0);

    free(ba.p); free(bb_.p);
    return 0;
}

static int case_24bit_raw(void)
{
    /* 24-bit little-endian samples must survive byte-for-byte, overlap included. */
    static const unsigned char aVals[9] = {
        0x33, 0x22, 0x11,  0x00, 0x00, 0x00,  0xEF, 0xCD, 0xAB
    };
    static const unsigned char bVals[6] = {
        0xEF, 0xCD, 0xAB,  0x03, 0x02, 0x01
    };
    static const unsigned char expect[12] = {
        0x33, 0x22, 0x11,  0x00, 0x00, 0x00,  0xEF, 0xCD, 0xAB,  0x03, 0x02, 0x01
    };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "24_a.wav", "24_b.wav" };
    unsigned char got[12];

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 1, 48000, 24); bb_bext(&ba, 0, NULL); bb_data(&ba, aVals, sizeof(aVals));
    bb_fmt(&bb_, 1, 48000, 24); bb_bext(&bb_, 2, NULL); bb_data(&bb_, bVals, sizeof(bVals));
    tmp_path(pa, sizeof(pa), "24_a.wav");
    tmp_path(pb, sizeof(pb), "24_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("24_out.wav", names, 2, err, sizeof(err)) == 0);
    TASK_CHECK(read_output_bytes("24_out.wav", got, sizeof(got)) == 0);
    TASK_CHECK(memcmp(got, expect, sizeof(expect)) == 0);

    free(ba.p); free(bb_.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* metadata group                                                      */
/* ------------------------------------------------------------------ */

static int case_cue_adtl_remap(void)
{
    /* Both volumes reuse local ID 5; vol 2 carries tail metadata and a junk chunk. */
    static const drwav_int16 aVals[6] = { 1, 2, 3, 4, 5, 6 };
    static const drwav_int16 bVals[4] = { 7, 8, 9, 10 };
    const test_cue_pt aCue[2] = { { 5, 1 }, { 9, 4 } };
    const test_cue_pt bCue[2] = { { 5, 0 }, { 2, 3 } };
    const test_adtl aAdtl[2] = {
        { TEST_LABL, 5, "mark", 0 },
        { TEST_NOTE, 9, "note", 0 }
    };
    const test_adtl bAdtl[1] = {
        { TEST_LTXT, 5, "region", 2 }
    };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "cm_a.wav", "cm_b.wav" };
    drwav w;
    drwav_metadata *cue, *m;
    drwav_uint32 i, sawLabl = 0, sawNote = 0, sawLtxt = 0;
    static const drwav_uint32 expectId[4]     = { 1, 2, 3, 4 };
    static const drwav_uint32 expectOffset[4] = { 1, 4, 6, 9 };

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 1, 48000, 16);
    bb_bext(&ba, 0, "first");
    bb_cue(&ba, aCue, 2);
    bb_adtl(&ba, aAdtl, 2);
    bb_data(&ba, (const unsigned char *)aVals, sizeof(aVals));

    bb_fmt(&bb_, 1, 48000, 16);
    bb_bext(&bb_, 6, "second");
    bb_data(&bb_, (const unsigned char *)bVals, sizeof(bVals));   /* metadata after data */
    bb_cue(&bb_, bCue, 2);
    bb_adtl(&bb_, bAdtl, 1);
    bb_junk(&bb_);

    tmp_path(pa, sizeof(pa), "cm_a.wav");
    tmp_path(pb, sizeof(pb), "cm_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("cm_out.wav", names, 2, err, sizeof(err)) == 0);
    TASK_CHECK(open_output("cm_out.wav", &w));
    TASK_CHECK(w.totalPCMFrameCount == 10);

    cue = find_md(&w, drwav_metadata_type_cue);
    TASK_CHECK(cue != NULL && cue->data.cue.cuePointCount == 4);
    for (i = 0; i < 4; ++i) {
        drwav_cue_point *cp = &cue->data.cue.pCuePoints[i];
        TASK_CHECK(cp->id == expectId[i]);
        TASK_CHECK(cp->sampleOffset == expectOffset[i]);
        TASK_CHECK(cp->playOrderPosition == 0);
        TASK_CHECK(cp->chunkStart == 0 && cp->blockStart == 0);
        TASK_CHECK(memcmp(cp->dataChunkId, "data", 4) == 0);
    }

    for (i = 0; i < w.metadataCount; ++i) {
        m = &w.pMetadata[i];
        if (m->type == drwav_metadata_type_list_label) {
            if (strcmp(m->data.labelOrNote.pString, "mark") == 0) {
                TASK_CHECK(m->data.labelOrNote.cuePointId == 1);
                ++sawLabl;
            }
        } else if (m->type == drwav_metadata_type_list_note) {
            TASK_CHECK(m->data.labelOrNote.cuePointId == 2);
            TASK_CHECK(strcmp(m->data.labelOrNote.pString, "note") == 0);
            ++sawNote;
        } else if (m->type == drwav_metadata_type_list_labelled_cue_region) {
            TASK_CHECK(m->data.labelledCueRegion.cuePointId == 3);
            TASK_CHECK(m->data.labelledCueRegion.sampleLength == 2);
            TASK_CHECK(strcmp(m->data.labelledCueRegion.pString, "region") == 0);
            ++sawLtxt;
        }
    }
    TASK_CHECK(sawLabl == 1 && sawNote == 1 && sawLtxt == 1);
    TASK_CHECK(find_md(&w, drwav_metadata_type_unknown) == NULL);   /* junk not copied */
    TASK_CHECK(drwav_uninit(&w) == DRWAV_SUCCESS);

    free(ba.p); free(bb_.p);
    return 0;
}

static int case_smpl_loops(void)
{
    /* Closed-interval loops shift wholesale; cue IDs remap; type/fraction/count kept. */
    static const drwav_int16 aVals[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const drwav_int16 bVals[8] = { 9, 10, 11, 12, 13, 14, 15, 16 };
    const test_cue_pt aCue[1] = { { 1, 2 } };
    const test_cue_pt bCue[1] = { { 1, 1 } };
    const test_loop aLoops[1] = { { 1, 0, 2, 5, 9, 3 } };
    const test_loop bLoops[1] = { { 1, 1, 1, 6, 0, 0 } };
    static const char specific[4] = { 'a', 'b', 'c', 'd' };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "sm_a.wav", "sm_b.wav" };
    drwav w;
    drwav_metadata *smpl;

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 1, 48000, 16); bb_bext(&ba, 0, NULL);
    bb_cue(&ba, aCue, 1);
    bb_smpl(&ba, 60, aLoops, 1, specific, 4);
    bb_data(&ba, (const unsigned char *)aVals, sizeof(aVals));

    bb_fmt(&bb_, 1, 48000, 16); bb_bext(&bb_, 8, NULL);
    bb_cue(&bb_, bCue, 1);
    bb_smpl(&bb_, 60, bLoops, 1, specific, 4);
    bb_data(&bb_, (const unsigned char *)bVals, sizeof(bVals));

    tmp_path(pa, sizeof(pa), "sm_a.wav");
    tmp_path(pb, sizeof(pb), "sm_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("sm_out.wav", names, 2, err, sizeof(err)) == 0);
    TASK_CHECK(open_output("sm_out.wav", &w));
    smpl = find_md(&w, drwav_metadata_type_smpl);
    TASK_CHECK(smpl != NULL);
    TASK_CHECK(smpl->data.smpl.sampleLoopCount == 2);
    TASK_CHECK(smpl->data.smpl.midiUnityNote == 60);
    TASK_CHECK(smpl->data.smpl.manufacturerId == 7 && smpl->data.smpl.productId == 8);
    TASK_CHECK(smpl->data.smpl.samplerSpecificDataSizeInBytes == 4);
    TASK_CHECK(memcmp(smpl->data.smpl.pSamplerSpecificData, specific, 4) == 0);

    TASK_CHECK(smpl->data.smpl.pLoops[0].cuePointId == 1);
    TASK_CHECK(smpl->data.smpl.pLoops[0].firstSampleOffset == 2);
    TASK_CHECK(smpl->data.smpl.pLoops[0].lastSampleOffset == 5);   /* inclusive */
    TASK_CHECK(smpl->data.smpl.pLoops[0].type == 0);
    TASK_CHECK(smpl->data.smpl.pLoops[0].sampleFraction == 9);
    TASK_CHECK(smpl->data.smpl.pLoops[0].playCount == 3);

    TASK_CHECK(smpl->data.smpl.pLoops[1].cuePointId == 2);
    TASK_CHECK(smpl->data.smpl.pLoops[1].firstSampleOffset == 9);
    TASK_CHECK(smpl->data.smpl.pLoops[1].lastSampleOffset == 14);  /* 8 + 6, inclusive */
    TASK_CHECK(smpl->data.smpl.pLoops[1].type == 1);
    TASK_CHECK(smpl->data.smpl.pLoops[1].sampleFraction == 0);
    TASK_CHECK(smpl->data.smpl.pLoops[1].playCount == 0);
    TASK_CHECK(drwav_uninit(&w) == DRWAV_SUCCESS);

    free(ba.p); free(bb_.p);
    return 0;
}

static int case_no_metadata_input(void)
{
    /* A volume without cue or smpl must still join; only bext is mandatory. */
    static const drwav_int16 aVals[2] = { 41, 42 };
    static const drwav_int16 bVals[2] = { 43, 44 };
    const test_cue_pt aCue[1] = { { 7, 0 } };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "nm_a.wav", "nm_b.wav" };
    drwav w;
    drwav_metadata *cue;

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 1, 48000, 16); bb_bext(&ba, 0, NULL);
    bb_cue(&ba, aCue, 1);
    bb_data(&ba, (const unsigned char *)aVals, sizeof(aVals));
    bb_fmt(&bb_, 1, 48000, 16); bb_bext(&bb_, 2, NULL);
    bb_data(&bb_, (const unsigned char *)bVals, sizeof(bVals));
    tmp_path(pa, sizeof(pa), "nm_a.wav");
    tmp_path(pb, sizeof(pb), "nm_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("nm_out.wav", names, 2, err, sizeof(err)) == 0);
    TASK_CHECK(open_output("nm_out.wav", &w));
    TASK_CHECK(w.totalPCMFrameCount == 4);
    cue = find_md(&w, drwav_metadata_type_cue);
    TASK_CHECK(cue != NULL && cue->data.cue.cuePointCount == 1);
    TASK_CHECK(cue->data.cue.pCuePoints[0].id == 1);
    TASK_CHECK(find_md(&w, drwav_metadata_type_smpl) == NULL);
    TASK_CHECK(drwav_uninit(&w) == DRWAV_SUCCESS);

    free(ba.p); free(bb_.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* errors group                                                        */
/* ------------------------------------------------------------------ */

static int case_dup_cue_id(void)
{
    static const drwav_int16 vals[4] = { 1, 2, 3, 4 };
    const test_cue_pt pts[2] = { { 5, 0 }, { 5, 2 } };
    bb b;
    char path[260], err[BWF_ERR_SIZE];
    const char *names[1] = { "du.wav" };

    bb_init(&b);
    bb_fmt(&b, 1, 48000, 16); bb_bext(&b, 0, NULL);
    bb_cue(&b, pts, 2);
    bb_data(&b, (const unsigned char *)vals, sizeof(vals));
    tmp_path(path, sizeof(path), "du.wav");
    write_wav(path, &b);

    TASK_CHECK(run_join("du_out.wav", names, 1, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "duplicate local cue ID") != NULL);
    TASK_CHECK(!file_exists("du_out.wav"));
    free(b.p);
    return 0;
}

static int case_dangling_adtl(void)
{
    static const drwav_int16 vals[4] = { 1, 2, 3, 4 };
    const test_cue_pt pts[1] = { { 1, 0 } };
    const test_adtl adtl[1] = { { TEST_LABL, 99, "orphan", 0 } };
    bb b;
    char path[260], err[BWF_ERR_SIZE];
    const char *names[1] = { "da.wav" };

    bb_init(&b);
    bb_fmt(&b, 1, 48000, 16); bb_bext(&b, 0, NULL);
    bb_cue(&b, pts, 1);
    bb_adtl(&b, adtl, 1);
    bb_data(&b, (const unsigned char *)vals, sizeof(vals));
    tmp_path(path, sizeof(path), "da.wav");
    write_wav(path, &b);

    TASK_CHECK(run_join("da_out.wav", names, 1, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "unknown cue ID") != NULL);
    TASK_CHECK(!file_exists("da_out.wav"));
    free(b.p);
    return 0;
}

static int case_output_exists(void)
{
    static const drwav_int16 vals[2] = { 1, 2 };
    static const unsigned char marker[7] = { 'M', 'A', 'R', 'K', 'E', 'R', '!' };
    bb b;
    char in[260], out[260], err[BWF_ERR_SIZE];
    const char *names[1] = { "oe.wav" };
    FILE *f;
    unsigned char got[7];

    bb_init(&b);
    bb_fmt(&b, 1, 48000, 16); bb_bext(&b, 0, NULL);
    bb_data(&b, (const unsigned char *)vals, sizeof(vals));
    tmp_path(in, sizeof(in), "oe.wav");
    write_wav(in, &b);

    tmp_path(out, sizeof(out), "oe_out.wav");
    f = fopen(out, "wb");
    TASK_CHECK(f != NULL);
    fwrite(marker, 1, sizeof(marker), f);
    TASK_CHECK(fclose(f) == 0);

    TASK_CHECK(run_join("oe_out.wav", names, 1, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "already exists") != NULL);

    f = fopen(out, "rb");
    TASK_CHECK(f != NULL);
    TASK_CHECK(fread(got, 1, sizeof(got), f) == sizeof(got));
    TASK_CHECK(memcmp(got, marker, sizeof(marker)) == 0);
    TASK_CHECK(fclose(f) == 0);
    free(b.p);
    return 0;
}

static int case_format_mismatch(void)
{
    static const drwav_int16 vals[4] = { 1, 2, 3, 4 };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "fm_a.wav", "fm_b.wav" };

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 1, 48000, 16); bb_bext(&ba, 0, NULL); bb_data(&ba, (const unsigned char *)vals, sizeof(vals));
    bb_fmt(&bb_, 2, 48000, 16); bb_bext(&bb_, 2, NULL); bb_data(&bb_, (const unsigned char *)vals, sizeof(vals));
    tmp_path(pa, sizeof(pa), "fm_a.wav");
    tmp_path(pb, sizeof(pb), "fm_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("fm_out.wav", names, 2, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "differs") != NULL);
    TASK_CHECK(!file_exists("fm_out.wav"));
    free(ba.p); free(bb_.p);
    return 0;
}

static int case_non_integer_data(void)
{
    static const unsigned char vals[3] = { 1, 2, 3 };   /* odd size, blockAlign = 2 */
    bb b;
    char path[260], err[BWF_ERR_SIZE];
    const char *names[1] = { "ni.wav" };

    bb_init(&b);
    bb_fmt(&b, 1, 48000, 16); bb_bext(&b, 0, NULL);
    bb_data(&b, vals, sizeof(vals));
    tmp_path(path, sizeof(path), "ni.wav");
    write_wav(path, &b);

    TASK_CHECK(run_join("ni_out.wav", names, 1, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "whole number of frames") != NULL);
    TASK_CHECK(!file_exists("ni_out.wav"));
    free(b.p);
    return 0;
}

static int case_cue_out_of_range(void)
{
    static const drwav_int16 vals[2] = { 1, 2 };   /* 2 frames */
    const test_cue_pt pts[1] = { { 1, 2 } };     /* offset == frame count */
    bb b;
    char path[260], err[BWF_ERR_SIZE];
    const char *names[1] = { "co.wav" };

    bb_init(&b);
    bb_fmt(&b, 1, 48000, 16); bb_bext(&b, 0, NULL);
    bb_cue(&b, pts, 1);
    bb_data(&b, (const unsigned char *)vals, sizeof(vals));
    tmp_path(path, sizeof(path), "co.wav");
    write_wav(path, &b);

    TASK_CHECK(run_join("co_out.wav", names, 1, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "outside the volume") != NULL);
    TASK_CHECK(!file_exists("co_out.wav"));
    free(b.p);
    return 0;
}

static int case_ltxt_out_of_range(void)
{
    static const drwav_int16 vals[4] = { 1, 2, 3, 4 };   /* 4 frames */
    const test_cue_pt pts[1] = { { 1, 2 } };
    const test_adtl adtl[1] = { { TEST_LTXT, 1, "long", 3 } };  /* 2 + 3 > 4 frames */
    bb b;
    char path[260], err[BWF_ERR_SIZE];
    const char *names[1] = { "lo.wav" };

    bb_init(&b);
    bb_fmt(&b, 1, 48000, 16); bb_bext(&b, 0, NULL);
    bb_cue(&b, pts, 1);
    bb_adtl(&b, adtl, 1);
    bb_data(&b, (const unsigned char *)vals, sizeof(vals));
    tmp_path(path, sizeof(path), "lo.wav");
    write_wav(path, &b);

    TASK_CHECK(run_join("lo_out.wav", names, 1, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "extends past the volume") != NULL);
    TASK_CHECK(!file_exists("lo_out.wav"));
    free(b.p);
    return 0;
}

static int case_smpl_param_mismatch(void)
{
    static const drwav_int16 vals[8] = { 1, 2, 3, 4 };
    const test_cue_pt pts[1] = { { 1, 0 } };
    const test_loop loops[1] = { { 1, 0, 0, 1, 0, 1 } };
    bb ba, bb_;
    char pa[260], pb[260], err[BWF_ERR_SIZE];
    const char *names[2] = { "sp_a.wav", "sp_b.wav" };

    bb_init(&ba); bb_init(&bb_);
    bb_fmt(&ba, 1, 48000, 16); bb_bext(&ba, 0, NULL);
    bb_cue(&ba, pts, 1); bb_smpl(&ba, 60, loops, 1, NULL, 0);
    bb_data(&ba, (const unsigned char *)vals, sizeof(vals));
    bb_fmt(&bb_, 1, 48000, 16); bb_bext(&bb_, 8, NULL);
    bb_cue(&bb_, pts, 1); bb_smpl(&bb_, 72, loops, 1, NULL, 0);
    bb_data(&bb_, (const unsigned char *)vals, sizeof(vals));
    tmp_path(pa, sizeof(pa), "sp_a.wav");
    tmp_path(pb, sizeof(pb), "sp_b.wav");
    write_wav(pa, &ba); write_wav(pb, &bb_);

    TASK_CHECK(run_join("sp_out.wav", names, 2, err, sizeof(err)) != 0);
    TASK_CHECK(strstr(err, "smpl non-loop parameters differ") != NULL);
    TASK_CHECK(!file_exists("sp_out.wav"));
    free(ba.p); free(bb_.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "timeline", "out_of_order",        case_out_of_order },
    { "timeline", "nested_overlap",      case_nested_overlap },
    { "timeline", "stereo_last_frame",   case_stereo_conflict },
    { "timeline", "eight_bit_gap",       case_eight_bit_gap },
    { "timeline", "24bit_raw_bytes",     case_24bit_raw },

    { "metadata", "cue_adtl_remap_tail", case_cue_adtl_remap },
    { "metadata", "smpl_closed_loops",   case_smpl_loops },
    { "metadata", "input_without_marks", case_no_metadata_input },

    { "errors", "duplicate_cue_id",      case_dup_cue_id },
    { "errors", "dangling_adtl_ref",     case_dangling_adtl },
    { "errors", "output_already_exists", case_output_exists },
    { "errors", "format_mismatch",       case_format_mismatch },
    { "errors", "non_integer_data",      case_non_integer_data },
    { "errors", "cue_out_of_range",      case_cue_out_of_range },
    { "errors", "ltxt_out_of_range",     case_ltxt_out_of_range },
    { "errors", "smpl_param_mismatch",   case_smpl_param_mismatch }
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
