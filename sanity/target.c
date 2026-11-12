/*
 * target.c - regression tests for the SAV import chain: long-string
 * segmentation, row compression, column projection and row windows.
 *
 * Four groups, registered through sanity/cases.h:
 *
 *   SegmentLayout    - long strings of width 255/256/257/510/511 in a file
 *                      whose dictionary and data area are emitted byte by
 *                      byte by the independent oracle in savkit.h (no
 *                      ReadStat writer).  Asserts land on variable names,
 *                      logical variable count, storage widths and the exact
 *                      reassembled string bytes delivered to the callbacks.
 *
 *   CompressedRows   - a hand-crafted row-compressed stream containing
 *                      integer opcodes, a 253 literal, a 254 space run, a
 *                      255 SYSMIS, zero padding opcodes and the 252
 *                      terminator, with control groups straddling logical
 *                      row boundaries.  sav_decompress_row() output is
 *                      compared against independently expanded 8-byte
 *                      units; readstat_parse_sav() output against logical
 *                      row values.  A stream truncated in the middle of a
 *                      literal must be rejected.
 *
 *   ProjectionWindow - column projection (skipping numeric columns before,
 *                      between and after a long string) combined with
 *                      row_offset/row_limit windows, original index vs
 *                      index_after_skipping, abort from the value callback,
 *                      zero rows, all-columns-skipped and windows past EOF.
 *
 *   IndependentOracle- one logical model materialized through the
 *                      independent byte-level oracle (uncompressed and
 *                      compressed) and through the ReadStat writer
 *                      (uncompressed and compressed); all four parses must
 *                      deliver the model's values exactly.
 *
 * Each group returns 0 when every assertion passes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "readstat.h"
#include "cases.h"
#include "savkit.h"

#include "spss/readstat_sav_compress.h"

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static cap_t g_cap;

static const char GLYPHS[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/* Position-dependent, non-repeating printable content: any shift or loss
 * across a segment boundary shows up as a mismatch at a known offset. */
static void fill_pattern(char *out, size_t len, uint32_t seed) {
    uint32_t x = seed ? seed : 0x9e3779b9u;
    size_t i;
    for (i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = GLYPHS[x % 62];
    }
}

static int str_diff_at(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    return (int)i;
}

#define FAIL(...) do { \
        printf("ASSERT %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        goto cleanup; \
    } while (0)

#define CHECK(cond) do { if (!(cond)) FAIL("%s", #cond); } while (0)

#define CHECK_STR(got, exp, what) do { \
        const char *g_ = (got), *e_ = (exp); \
        if (strcmp(g_, e_) != 0) { \
            FAIL("%s mismatch at byte %d (got %zu bytes, want %zu bytes)", \
                    what, str_diff_at(g_, e_), strlen(g_), strlen(e_)); \
        } \
    } while (0)

/* ------------------------------------------------------------------ */
/* SegmentLayout                                                       */
/* ------------------------------------------------------------------ */

static const om_var_t SEG_VARS[] = {
    { "id",     0, 0   },
    { "txt255", 1, 255 },
    { "txt256", 1, 256 },
    { "txt257", 1, 257 },
    { "txt510", 1, 510 },
    { "txt511", 1, 511 },
    { "weight", 0, 0   },
};
#define SEG_NVARS 7
#define SEG_NROWS 3

static char seg_str[SEG_NROWS][SEG_NVARS][520];
static om_cell_t seg_cells[SEG_NROWS][SEG_NVARS];

/* Stored content bytes for string variable vi (1..5) of the given row. */
static size_t seg_content(int vi, int row, char *buf) {
    uint32_t seed = 0x1000u + (uint32_t)vi * 131u + (uint32_t)row;
    switch (vi) {
        case 1: /* txt255: full-width pattern; row 2 is a blank cell */
            if (row == 2)
                return 0;
            fill_pattern(buf, 255, seed);
            return 255;
        case 2: /* txt256: U+4E2D straddling the 255/256 segment cut */
            fill_pattern(buf, 253, seed);
            memcpy(buf + 253, "\xE4\xB8\xAD", 3);
            return 256;
        case 3: /* txt257: content ends in spaces before the padding */
            fill_pattern(buf, 190, seed);
            memcpy(buf + 190, "END", 3);
            memset(buf + 193, ' ', 3);
            return 196;
        case 4: /* txt510: exactly two full 255-byte segments */
            fill_pattern(buf, 510, seed);
            return 510;
        case 5: /* txt511: U+4E2D across the first cut, U+00E9 across the second */
            fill_pattern(buf, 254, seed);
            memcpy(buf + 254, "\xE4\xB8\xAD", 3);
            fill_pattern(buf + 257, 252, seed ^ 0x5a5au);
            memcpy(buf + 509, "\xC3\xA9", 2);
            return 511;
    }
    return 0;
}

/* What the reader must deliver: the stored content with the trailing
 * space padding (and the content's own trailing spaces) trimmed. */
static void seg_expect_str(int vi, int row, char *out) {
    char buf[520];
    size_t len = seg_content(vi, row, buf);
    while (len && buf[len - 1] == ' ')
        len--;
    memcpy(out, buf, len);
    out[len] = '\0';
}

static double seg_num(int vi, int row) {
    return vi == 0 ? (double)(row + 1) : (row + 1) * 1.5;
}

static int test_segment_layout(void) {
    char dir[1024], path[1200];
    cap_t *cap = &g_cap;
    int rc = 1, i, r, vi, k;
    readstat_error_t prc;

    if (!sk_mkdtemp(dir, sizeof(dir))) {
        printf("ASSERT: mkdtemp failed\n");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/segments.sav", dir);

    for (r = 0; r < SEG_NROWS; r++) {
        for (vi = 0; vi < SEG_NVARS; vi++) {
            om_cell_t *cell = &seg_cells[r][vi];
            cell->sysmis = 0;
            cell->num = 0;
            cell->str = NULL;
            if (SEG_VARS[vi].is_string) {
                size_t len = seg_content(vi, r, seg_str[r][vi]);
                seg_str[r][vi][len] = '\0';
                cell->str = seg_str[r][vi];
            } else {
                cell->num = seg_num(vi, r);
            }
        }
    }

    /* Dictionary and data area written byte-by-byte by the oracle. */
    if (om_write_sav(path, SEG_VARS, SEG_NVARS, &seg_cells[0][0], SEG_NROWS, 0))
        FAIL("oracle writer failed");

    cap_reset(cap);
    prc = cap_parse(cap, path, 0, 0);
    if (prc != READSTAT_OK)
        FAIL("readstat_parse_sav rc=%d", prc);

    CHECK(cap->meta_seen);
    CHECK(cap->meta_rows == SEG_NROWS);
    CHECK(cap->meta_vars == SEG_NVARS);
    CHECK(cap->meta_compression == READSTAT_COMPRESS_NONE);

    CHECK(cap->nvars == SEG_NVARS);
    for (i = 0; i < SEG_NVARS; i++) {
        CHECK(cap->vars[i].index == i);
        CHECK(cap->vars[i].index_after_skipping == i);
        CHECK(cap->vars[i].is_string == SEG_VARS[i].is_string);
        CHECK_STR(cap->vars[i].name, SEG_VARS[i].name, "variable name");
        CHECK(cap->vars[i].storage_width ==
                (SEG_VARS[i].is_string ? SEG_VARS[i].width : 8));
    }

    CHECK(cap->nvals == SEG_NROWS * SEG_NVARS);
    k = 0;
    for (r = 0; r < SEG_NROWS; r++) {
        for (vi = 0; vi < SEG_NVARS; vi++) {
            cap_val_t *cv = &cap->vals[k++];
            CHECK(cv->row == r);
            CHECK(cv->var == vi);
            CHECK(cv->is_string == SEG_VARS[vi].is_string);
            if (cv->is_string) {
                char expect[520];
                seg_expect_str(vi, r, expect);
                CHECK_STR(cv->str, expect, "long string content");
            } else {
                CHECK(!cv->sysmis);
                CHECK(cv->num == seg_num(vi, r));
            }
        }
    }
    rc = 0;
cleanup:
    unlink(path);
    rmdir(dir);
    return rc;
}

/* ------------------------------------------------------------------ */
/* CompressedRows                                                      */
/* ------------------------------------------------------------------ */

static const om_var_t CR_VARS[] = {
    { "c0", 0, 0 },
    { "c1", 1, 8 },
    { "c2", 0, 0 },
};
#define CR_NVARS 3
#define CR_NROWS 4
#define CR_ROW_BYTES 24
#define CR_TRUNC_LEN 36 /* cuts the "XYZ" literal in half, mid row 2 */

/* Logical values carried by the hand-crafted stream below. */
static const double CR_NUM[CR_NROWS][2] = {
    { 5.0, 2.5 }, { -3.0, 0.0 }, { 42.0, 7.0 }, { 100.5, 0.0 }
};
static const int CR_SYSMIS[CR_NROWS] = { 0, 1, 0, 0 }; /* applies to c2 */
static const char *CR_STR[CR_NROWS] = { "AB", "", "XYZ", "Q" };

static int cr_check_capture(cap_t *cap) {
    int k;
    if (cap->nvals != CR_NROWS * CR_NVARS) {
        printf("ASSERT: cell count %d != %d\n", cap->nvals, CR_NROWS * CR_NVARS);
        return 1;
    }
    for (k = 0; k < cap->nvals; k++) {
        cap_val_t *cv = &cap->vals[k];
        int row = k / CR_NVARS, col = k % CR_NVARS;
        if (cv->row != row || cv->var != col) {
            printf("ASSERT: cell %d at row=%d var=%d\n", k, cv->row, cv->var);
            return 1;
        }
        if (col == 1) {
            if (!cv->is_string || strcmp(cv->str, CR_STR[row]) != 0) {
                printf("ASSERT: row %d c1=\"%s\" want \"%s\"\n", row, cv->str, CR_STR[row]);
                return 1;
            }
        } else if (col == 2 && CR_SYSMIS[row]) {
            if (!cv->sysmis) {
                printf("ASSERT: row %d c2 not system-missing\n", row);
                return 1;
            }
        } else {
            if (cv->is_string || cv->sysmis || cv->num != CR_NUM[row][col / 2]) {
                printf("ASSERT: row %d c%d=%f want %f\n", row, col, cv->num, CR_NUM[row][col / 2]);
                return 1;
            }
        }
    }
    return 0;
}

static int test_compressed_rows(void) {
    char dir[1024] = "", path_comp[1200] = "", path_plain[1200] = "", path_trunc[1200] = "";
    cap_t *cap = &g_cap;
    int rc = 1, terminated = 0, rows, k;
    readstat_error_t prc;
    ob_t stream = { 0 }, units = { 0 };
    unsigned char rowbuf[CR_ROW_BYTES];
    struct sav_row_stream_s st;

    /* Hand-crafted stream.  Control group 1 ends one unit into logical
     * row 2, so control groups straddle row boundaries.  Opcodes used:
     * 105/97/142/107/100 (integers - bias), 253 literal, 254 spaces,
     * 255 SYSMIS, 0 padding (mid-stream, slot 6 of group 1), 252 end. */
    {
        unsigned char g1[8] = { 105, 253, 253, 97, 254, 255, 0, 142 };
        unsigned char g2[8] = { 253, 107, 253, 253, 100, 252, 0, 0 };
        ob_put(&stream, g1, 8);
        ob_put(&stream, "AB      ", 8);   /* literal for row 0 c1 */
        ob_f64(&stream, 2.5);             /* literal for row 0 c2 */
        ob_put(&stream, g2, 8);
        ob_put(&stream, "XYZ     ", 8);   /* literal for row 2 c1 */
        ob_f64(&stream, 100.5);           /* literal for row 3 c0 */
        ob_put(&stream, "Q       ", 8);   /* literal for row 3 c1 */
    }
    CHECK(stream.len == 56);

    /* Reference: independent expansion into 8-byte units. */
    CHECK(ok_expand(stream.data, stream.len, &units, &terminated) == 0);
    CHECK(terminated);
    CHECK(units.len == CR_NROWS * CR_ROW_BYTES);

    /* sav_decompress_row must reproduce the expanded units row by row. */
    memset(&st, 0, sizeof(st));
    st.next_in = stream.data;
    st.avail_in = stream.len;
    st.missing_value = OM_SYSMIS;
    st.bias = 100.0;
    st.bswap = 0;
    rows = 0;
    while (1) {
        st.next_out = rowbuf;
        st.avail_out = sizeof(rowbuf);
        sav_decompress_row(&st);
        if (st.status == SAV_ROW_STREAM_FINISHED_ROW) {
            CHECK(rows < CR_NROWS);
            CHECK(memcmp(rowbuf, units.data + rows * CR_ROW_BYTES, CR_ROW_BYTES) == 0);
            rows++;
            continue;
        }
        break;
    }
    CHECK(st.status == SAV_ROW_STREAM_FINISHED_ALL);
    CHECK(rows == CR_NROWS);

    /* The truncated stream stops producing rows mid-literal. */
    memset(&st, 0, sizeof(st));
    st.next_in = stream.data;
    st.avail_in = CR_TRUNC_LEN;
    st.missing_value = OM_SYSMIS;
    st.bias = 100.0;
    st.bswap = 0;
    rows = 0;
    while (1) {
        st.next_out = rowbuf;
        st.avail_out = sizeof(rowbuf);
        sav_decompress_row(&st);
        if (st.status == SAV_ROW_STREAM_FINISHED_ROW) {
            rows++;
            continue;
        }
        break;
    }
    CHECK(st.status == SAV_ROW_STREAM_NEED_DATA);
    CHECK(rows == 2);

    if (!sk_mkdtemp(dir, sizeof(dir)))
        FAIL("mkdtemp failed");
    snprintf(path_comp, sizeof(path_comp), "%s/comp.sav", dir);
    snprintf(path_plain, sizeof(path_plain), "%s/plain.sav", dir);
    snprintf(path_trunc, sizeof(path_trunc), "%s/trunc.sav", dir);

    /* Same dictionary three ways: hand-crafted compressed data, the
     * independently expanded uncompressed data, and the truncated stream. */
    if (om_write_sav_raw(path_comp, CR_VARS, CR_NVARS, CR_NROWS, 1, stream.data, stream.len))
        FAIL("oracle writer failed (comp)");
    if (om_write_sav_raw(path_plain, CR_VARS, CR_NVARS, CR_NROWS, 0, units.data, units.len))
        FAIL("oracle writer failed (plain)");
    if (om_write_sav_raw(path_trunc, CR_VARS, CR_NVARS, CR_NROWS, 1, stream.data, CR_TRUNC_LEN))
        FAIL("oracle writer failed (trunc)");

    cap_reset(cap);
    prc = cap_parse(cap, path_comp, 0, 0);
    if (prc != READSTAT_OK)
        FAIL("parse(comp) rc=%d", prc);
    CHECK(cap->meta_rows == CR_NROWS);
    CHECK(cap->meta_vars == CR_NVARS);
    CHECK(cap->meta_compression == READSTAT_COMPRESS_ROWS);
    CHECK(cap->nvars == CR_NVARS);
    CHECK_STR(cap->vars[0].name, "c0", "variable name");
    CHECK_STR(cap->vars[1].name, "c1", "variable name");
    CHECK_STR(cap->vars[2].name, "c2", "variable name");
    CHECK(cap->vars[1].storage_width == 8);
    if (cr_check_capture(cap))
        goto cleanup;

    /* The uncompressed rendering of the same data must deliver the same
     * logical rows through the same public entry point. */
    {
        cap_t plain;
        cap_reset(&plain);
        prc = cap_parse(&plain, path_plain, 0, 0);
        if (prc != READSTAT_OK)
            FAIL("parse(plain) rc=%d", prc);
        CHECK(plain.meta_compression == READSTAT_COMPRESS_NONE);
        if (cr_check_capture(&plain))
            goto cleanup;
        CHECK(plain.nvals == cap->nvals);
        for (k = 0; k < plain.nvals; k++) {
            CHECK(plain.vals[k].row == cap->vals[k].row);
            CHECK(plain.vals[k].var == cap->vals[k].var);
            CHECK(plain.vals[k].is_string == cap->vals[k].is_string);
            CHECK(plain.vals[k].sysmis == cap->vals[k].sysmis);
            if (plain.vals[k].is_string)
                CHECK_STR(plain.vals[k].str, cap->vals[k].str, "plain vs comp string");
            else if (!plain.vals[k].sysmis)
                CHECK(plain.vals[k].num == cap->vals[k].num);
        }
    }

    /* Truncated in the middle of a literal: the parser must fail. */
    cap_reset(cap);
    prc = cap_parse(cap, path_trunc, 0, 0);
    if (prc != READSTAT_ERROR_PARSE)
        FAIL("parse(trunc) rc=%d, want READSTAT_ERROR_PARSE", prc);

    rc = 0;
cleanup:
    if (path_comp[0])
        unlink(path_comp);
    if (path_plain[0])
        unlink(path_plain);
    if (path_trunc[0])
        unlink(path_trunc);
    if (dir[0])
        rmdir(dir);
    ob_free(&stream);
    ob_free(&units);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ProjectionWindow                                                    */
/* ------------------------------------------------------------------ */

static const om_var_t PW_VARS[] = {
    { "n0", 0, 0   },
    { "s1", 1, 300 },
    { "n2", 0, 0   },
    { "n3", 0, 0   },
};
#define PW_NVARS 4
#define PW_NROWS 5

static char pw_str[PW_NROWS][288];
static om_cell_t pw_cells[PW_NROWS][PW_NVARS];

static size_t pw_s1_content(int row, char *buf) {
    int p = snprintf(buf, 8, "R%d:", row);
    fill_pattern(buf + p, 280 - (size_t)p, 0x2000u + (uint32_t)row);
    return 280;
}

static double pw_num(int vi, int row) {
    return (vi == 0 ? 10.0 : vi == 2 ? 20.0 : 30.0) + row;
}

/* Check a capture against the model: rows first_model_row..+nrows-1
 * (delivered numbered from 0), columns outside skip_mask in order. */
static int pw_check(cap_t *cap, unsigned skip_mask, long first_model_row, long nrows) {
    long dr;
    int vi, k = 0;
    for (dr = 0; dr < nrows; dr++) {
        long mr = first_model_row + dr;
        for (vi = 0; vi < PW_NVARS; vi++) {
            cap_val_t *cv;
            if (skip_mask & (1u << vi))
                continue;
            if (k >= cap->nvals) {
                printf("ASSERT: missing cell for model row %ld var %d\n", mr, vi);
                return 1;
            }
            cv = &cap->vals[k++];
            if (cv->row != dr || cv->var != vi) {
                printf("ASSERT: cell %d row=%d var=%d, want row=%ld var=%d\n",
                        k - 1, cv->row, cv->var, dr, vi);
                return 1;
            }
            if (vi == 1) {
                char expect[288];
                size_t len = pw_s1_content((int)mr, expect);
                expect[len] = '\0';
                if (!cv->is_string || strcmp(cv->str, expect) != 0) {
                    printf("ASSERT: s1 row %ld mismatch at byte %d\n",
                            mr, str_diff_at(cv->str, expect));
                    return 1;
                }
            } else {
                if (cv->is_string || cv->sysmis || cv->num != pw_num(vi, (int)mr)) {
                    printf("ASSERT: n%d row %ld = %f, want %f\n",
                            vi, mr, cv->num, pw_num(vi, (int)mr));
                    return 1;
                }
            }
        }
    }
    if (k != cap->nvals) {
        printf("ASSERT: %d cells delivered, %d expected\n", cap->nvals, k);
        return 1;
    }
    return 0;
}

static int pw_check_vars(cap_t *cap) {
    int i;
    if (cap->nvars != PW_NVARS) {
        printf("ASSERT: %d variables, want %d\n", cap->nvars, PW_NVARS);
        return 1;
    }
    for (i = 0; i < PW_NVARS; i++) {
        if (cap->vars[i].index != i || strcmp(cap->vars[i].name, PW_VARS[i].name) != 0) {
            printf("ASSERT: variable %d is \"%s\" index %d\n",
                    i, cap->vars[i].name, cap->vars[i].index);
            return 1;
        }
        if (cap->vars[i].storage_width != (PW_VARS[i].is_string ? PW_VARS[i].width : 8)) {
            printf("ASSERT: variable %d storage width %ld\n", i, cap->vars[i].storage_width);
            return 1;
        }
    }
    return 0;
}

static int test_projection_window(void) {
    char dir[1024], path[1200], path0[1200];
    cap_t *cap = &g_cap;
    int rc = 1, r, vi;
    readstat_error_t prc;

    for (r = 0; r < PW_NROWS; r++) {
        for (vi = 0; vi < PW_NVARS; vi++) {
            om_cell_t *cell = &pw_cells[r][vi];
            cell->sysmis = 0;
            cell->num = 0;
            cell->str = NULL;
            if (PW_VARS[vi].is_string) {
                size_t len = pw_s1_content(r, pw_str[r]);
                pw_str[r][len] = '\0';
                cell->str = pw_str[r];
            } else {
                cell->num = pw_num(vi, r);
            }
        }
    }

    if (!sk_mkdtemp(dir, sizeof(dir))) {
        printf("ASSERT: mkdtemp failed\n");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/proj.sav", dir);
    snprintf(path0, sizeof(path0), "%s/proj0.sav", dir);

    /* Row-compressed so projection is exercised through the decompressor. */
    if (om_write_sav(path, PW_VARS, PW_NVARS, &pw_cells[0][0], PW_NROWS, 1))
        FAIL("oracle writer failed");
    /* Zero-row material: dictionary only, empty data area. */
    if (om_write_sav(path0, PW_VARS, PW_NVARS, &pw_cells[0][0], 0, 0))
        FAIL("oracle writer failed (zero rows)");

    /* (a) skip the numeric column before the long string */
    cap_reset(cap);
    cap->skip_mask = 0x1;
    if ((prc = cap_parse(cap, path, 0, 0)) != READSTAT_OK)
        FAIL("parse(a) rc=%d", prc);
    if (pw_check_vars(cap) || pw_check(cap, 0x1, 0, PW_NROWS))
        goto cleanup;

    /* (b) skip the numeric column right after the long string */
    cap_reset(cap);
    cap->skip_mask = 0x4;
    if ((prc = cap_parse(cap, path, 0, 0)) != READSTAT_OK)
        FAIL("parse(b) rc=%d", prc);
    if (pw_check_vars(cap) || pw_check(cap, 0x4, 0, PW_NROWS))
        goto cleanup;

    /* (c) skip the trailing numeric column */
    cap_reset(cap);
    cap->skip_mask = 0x8;
    if ((prc = cap_parse(cap, path, 0, 0)) != READSTAT_OK)
        FAIL("parse(c) rc=%d", prc);
    if (pw_check_vars(cap) || pw_check(cap, 0x8, 0, PW_NROWS))
        goto cleanup;

    /* (d) skip every numeric column; the long string must still arrive whole */
    cap_reset(cap);
    cap->skip_mask = 0xD;
    if ((prc = cap_parse(cap, path, 0, 0)) != READSTAT_OK)
        FAIL("parse(d) rc=%d", prc);
    if (pw_check_vars(cap) || pw_check(cap, 0xD, 0, PW_NROWS))
        goto cleanup;

    /* (e) original index vs index_after_skipping with n0 and n2 skipped */
    cap_reset(cap);
    cap->skip_mask = 0x5;
    if ((prc = cap_parse(cap, path, 0, 0)) != READSTAT_OK)
        FAIL("parse(e) rc=%d", prc);
    if (pw_check_vars(cap))
        goto cleanup;
    CHECK(cap->vars[1].index_after_skipping == 0);
    CHECK(cap->vars[3].index_after_skipping == 1);
    if (pw_check(cap, 0x5, 0, PW_NROWS))
        goto cleanup;

    /* (f) row window: offset 1, limit 2 -> model rows 1..2 as rows 0..1 */
    cap_reset(cap);
    if ((prc = cap_parse(cap, path, 1, 2)) != READSTAT_OK)
        FAIL("parse(f) rc=%d", prc);
    CHECK(cap->meta_rows == 2);
    if (pw_check(cap, 0, 1, 2))
        goto cleanup;

    /* (g) abort from the value callback keeps the delivered prefix; a
     * fresh parser still reads the same material completely */
    cap_reset(cap);
    cap->abort_after = 3;
    prc = cap_parse(cap, path, 0, 0);
    if (prc != READSTAT_ERROR_USER_ABORT)
        FAIL("parse(g) rc=%d, want USER_ABORT", prc);
    CHECK(cap->nvals == 3);
    CHECK(cap->vals[0].row == 0 && cap->vals[0].var == 0 && cap->vals[0].num == 10.0);
    CHECK(cap->vals[1].row == 0 && cap->vals[1].var == 1 && cap->vals[1].is_string);
    {
        char expect[288];
        size_t len = pw_s1_content(0, expect);
        expect[len] = '\0';
        CHECK_STR(cap->vals[1].str, expect, "aborted prefix s1");
    }
    CHECK(cap->vals[2].row == 0 && cap->vals[2].var == 2 && cap->vals[2].num == 20.0);
    cap_reset(cap);
    if ((prc = cap_parse(cap, path, 0, 0)) != READSTAT_OK)
        FAIL("parse(g-retry) rc=%d", prc);
    if (pw_check(cap, 0, 0, PW_NROWS))
        goto cleanup;

    /* (h) zero rows: parses cleanly, delivers nothing */
    cap_reset(cap);
    if ((prc = cap_parse(cap, path0, 0, 0)) != READSTAT_OK)
        FAIL("parse(h) rc=%d", prc);
    CHECK(cap->meta_rows == 0);
    CHECK(cap->nvars == PW_NVARS);
    CHECK(cap->nvals == 0);

    /* (i) all columns skipped: no value callbacks, still a clean parse */
    cap_reset(cap);
    cap->skip_mask = 0xF;
    if ((prc = cap_parse(cap, path, 0, 0)) != READSTAT_OK)
        FAIL("parse(i) rc=%d", prc);
    CHECK(cap->nvars == PW_NVARS);
    CHECK(cap->nvals == 0);

    /* (j) windows past the end of the data */
    cap_reset(cap);
    if ((prc = cap_parse(cap, path, 10, 0)) != READSTAT_OK)
        FAIL("parse(j-offset) rc=%d", prc);
    CHECK(cap->nvals == 0);
    cap_reset(cap);
    if ((prc = cap_parse(cap, path, 3, 100)) != READSTAT_OK)
        FAIL("parse(j-window) rc=%d", prc);
    CHECK(cap->meta_rows == 2);
    if (pw_check(cap, 0, 3, 2))
        goto cleanup;

    rc = 0;
cleanup:
    unlink(path);
    unlink(path0);
    rmdir(dir);
    return rc;
}

/* ------------------------------------------------------------------ */
/* IndependentOracle                                                   */
/* ------------------------------------------------------------------ */

static const om_var_t IO_VARS[] = {
    { "num1",    0, 0   },
    { "txt8",    1, 8   },
    { "longtext",1, 300 },
    { "num2",    0, 0   },
};
#define IO_NVARS 4
#define IO_NROWS 4

static const double IO_NUM1[IO_NROWS] = { 0.5, 1.5, 0.0, -3.25 };
static const int IO_SYSMIS[IO_NROWS] = { 0, 0, 1, 0 };
static const double IO_NUM2[IO_NROWS] = { 0, 7, 14, 21 };

static char io_txt8[IO_NROWS][16];
static char io_long[IO_NROWS][288];
static om_cell_t io_cells[IO_NROWS][IO_NVARS];

static size_t io_long_content(int row, char *buf) {
    int p = snprintf(buf, 8, "L%d#", row);
    fill_pattern(buf + p, 280 - (size_t)p, 0x3000u + (uint32_t)row);
    return 280;
}

static ssize_t io_file_write(const void *data, size_t len, void *ctx) {
    return fwrite(data, 1, len, (FILE *)ctx);
}

/* Materialize the same model through the ReadStat writer (comparison
 * material only; the oracle files above are the independent reference). */
static int io_write_with_readstat(const char *path, int compressed) {
    FILE *f = fopen(path, "wb");
    readstat_writer_t *w;
    readstat_variable_t *rv[IO_NVARS];
    readstat_error_t e;
    int i;
    long r;

    if (!f)
        return 1;
    w = readstat_writer_init();
    readstat_set_data_writer(w, io_file_write);
    for (i = 0; i < IO_NVARS; i++) {
        rv[i] = readstat_add_variable(w, IO_VARS[i].name,
                IO_VARS[i].is_string ? READSTAT_TYPE_STRING : READSTAT_TYPE_DOUBLE,
                IO_VARS[i].is_string ? (size_t)IO_VARS[i].width : 0);
    }
    if (compressed)
        readstat_writer_set_compression(w, READSTAT_COMPRESS_ROWS);
    e = readstat_begin_writing_sav(w, f, IO_NROWS);
    for (r = 0; e == READSTAT_OK && r < IO_NROWS; r++) {
        e = readstat_begin_row(w);
        for (i = 0; e == READSTAT_OK && i < IO_NVARS; i++) {
            const om_cell_t *cell = &io_cells[r][i];
            if (IO_VARS[i].is_string)
                e = readstat_insert_string_value(w, rv[i], cell->str);
            else if (cell->sysmis)
                e = readstat_insert_missing_value(w, rv[i]);
            else
                e = readstat_insert_double_value(w, rv[i], cell->num);
        }
        if (e == READSTAT_OK)
            e = readstat_end_row(w);
    }
    if (e == READSTAT_OK)
        e = readstat_end_writing(w);
    readstat_writer_free(w);
    fclose(f);
    return e != READSTAT_OK;
}

static int io_check(cap_t *cap, int want_compression) {
    int r, k = 0;
    if (!cap->meta_seen || cap->meta_rows != IO_NROWS || cap->meta_vars != IO_NVARS) {
        printf("ASSERT: metadata rows=%ld vars=%d\n", cap->meta_rows, cap->meta_vars);
        return 1;
    }
    if (cap->meta_compression != want_compression) {
        printf("ASSERT: compression %d, want %d\n", cap->meta_compression, want_compression);
        return 1;
    }
    if (cap->nvars != IO_NVARS) {
        printf("ASSERT: %d variables\n", cap->nvars);
        return 1;
    }
    for (r = 0; r < IO_NVARS; r++) {
        if (cap->vars[r].index != r || strcmp(cap->vars[r].name, IO_VARS[r].name) != 0) {
            printf("ASSERT: variable %d \"%s\"\n", r, cap->vars[r].name);
            return 1;
        }
        if (cap->vars[r].storage_width != (IO_VARS[r].is_string ? IO_VARS[r].width : 8)) {
            printf("ASSERT: variable %d width %ld\n", r, cap->vars[r].storage_width);
            return 1;
        }
    }
    if (cap->nvals != IO_NROWS * IO_NVARS) {
        printf("ASSERT: %d cells\n", cap->nvals);
        return 1;
    }
    for (r = 0; r < IO_NROWS; r++) {
        cap_val_t *cv;
        char expect[288];
        size_t len;

        cv = &cap->vals[k++];
        if (cv->row != r || cv->var != 0 || cv->is_string)
            return printf("ASSERT: row %d num1 slot\n", r), 1;
        if (IO_SYSMIS[r]) {
            if (!cv->sysmis)
                return printf("ASSERT: row %d num1 not sysmis\n", r), 1;
        } else if (cv->sysmis || cv->num != IO_NUM1[r]) {
            return printf("ASSERT: row %d num1=%f\n", r, cv->num), 1;
        }

        cv = &cap->vals[k++];
        snprintf(expect, sizeof(expect), "item%d", r);
        if (cv->row != r || cv->var != 1 || !cv->is_string || strcmp(cv->str, expect) != 0)
            return printf("ASSERT: row %d txt8=\"%s\"\n", r, cv->str), 1;

        cv = &cap->vals[k++];
        len = io_long_content(r, expect);
        expect[len] = '\0';
        if (cv->row != r || cv->var != 2 || !cv->is_string || strcmp(cv->str, expect) != 0)
            return printf("ASSERT: row %d longtext mismatch at byte %d\n",
                    r, str_diff_at(cv->str, expect)), 1;

        cv = &cap->vals[k++];
        if (cv->row != r || cv->var != 3 || cv->is_string || cv->sysmis || cv->num != IO_NUM2[r])
            return printf("ASSERT: row %d num2=%f\n", r, cv->num), 1;
    }
    return 0;
}

static int test_independent_oracle(void) {
    char dir[1024], pa[1200], pb[1200], pc[1200], pd[1200];
    cap_t *cap = &g_cap;
    int rc = 1, r;
    readstat_error_t prc;

    for (r = 0; r < IO_NROWS; r++) {
        size_t len;
        snprintf(io_txt8[r], sizeof(io_txt8[r]), "item%d", r);
        len = io_long_content(r, io_long[r]);
        io_long[r][len] = '\0';
        io_cells[r][0].num = IO_NUM1[r];
        io_cells[r][0].sysmis = IO_SYSMIS[r];
        io_cells[r][0].str = NULL;
        io_cells[r][1].str = io_txt8[r];
        io_cells[r][1].num = 0;
        io_cells[r][1].sysmis = 0;
        io_cells[r][2].str = io_long[r];
        io_cells[r][2].num = 0;
        io_cells[r][2].sysmis = 0;
        io_cells[r][3].num = IO_NUM2[r];
        io_cells[r][3].sysmis = 0;
        io_cells[r][3].str = NULL;
    }

    if (!sk_mkdtemp(dir, sizeof(dir))) {
        printf("ASSERT: mkdtemp failed\n");
        return 1;
    }
    snprintf(pa, sizeof(pa), "%s/oracle_plain.sav", dir);
    snprintf(pb, sizeof(pb), "%s/oracle_comp.sav", dir);
    snprintf(pc, sizeof(pc), "%s/writer_plain.sav", dir);
    snprintf(pd, sizeof(pd), "%s/writer_comp.sav", dir);

    /* Independent material, both storage forms. */
    if (om_write_sav(pa, IO_VARS, IO_NVARS, &io_cells[0][0], IO_NROWS, 0))
        FAIL("oracle writer failed (plain)");
    if (om_write_sav(pb, IO_VARS, IO_NVARS, &io_cells[0][0], IO_NROWS, 1))
        FAIL("oracle writer failed (comp)");
    /* Comparison material through the ReadStat writer. */
    if (io_write_with_readstat(pc, 0))
        FAIL("readstat writer failed (plain)");
    if (io_write_with_readstat(pd, 1))
        FAIL("readstat writer failed (comp)");

    cap_reset(cap);
    if ((prc = cap_parse(cap, pa, 0, 0)) != READSTAT_OK)
        FAIL("parse(oracle plain) rc=%d", prc);
    if (io_check(cap, READSTAT_COMPRESS_NONE))
        goto cleanup;

    cap_reset(cap);
    if ((prc = cap_parse(cap, pb, 0, 0)) != READSTAT_OK)
        FAIL("parse(oracle comp) rc=%d", prc);
    if (io_check(cap, READSTAT_COMPRESS_ROWS))
        goto cleanup;

    cap_reset(cap);
    if ((prc = cap_parse(cap, pc, 0, 0)) != READSTAT_OK)
        FAIL("parse(writer plain) rc=%d", prc);
    if (io_check(cap, READSTAT_COMPRESS_NONE))
        goto cleanup;

    cap_reset(cap);
    if ((prc = cap_parse(cap, pd, 0, 0)) != READSTAT_OK)
        FAIL("parse(writer comp) rc=%d", prc);
    if (io_check(cap, READSTAT_COMPRESS_ROWS))
        goto cleanup;

    rc = 0;
cleanup:
    unlink(pa);
    unlink(pb);
    unlink(pc);
    unlink(pd);
    rmdir(dir);
    return rc;
}

/* ------------------------------------------------------------------ */

void task_register_tests(void) {
    task_case("SegmentLayout", test_segment_layout);
    task_case("CompressedRows", test_compressed_rows);
    task_case("ProjectionWindow", test_projection_window);
    task_case("IndependentOracle", test_independent_oracle);
}
