/*
 * savkit.h - independent SAV material generator and capture harness for the
 * sanity/target.c regression tests.
 *
 * Everything in this file is written directly from the SAV format
 * description.  It never calls into the ReadStat writer, reader or row
 * compressor, so a correlated bug in those cannot cancel out here:
 *
 *   - om_write_sav()/om_write_sav_raw() emit a whole .sav file (dictionary
 *     and data area) byte by byte through explicit little-endian putters.
 *   - ok_expand() expands a row-compressed byte stream into 8-byte units
 *     using only the opcode semantics (0, 1..251, 252, 253, 254, 255).
 *
 * The capture harness (cap_t + cap_parse) drives the public
 * readstat_parse_sav() entry point and records everything the callbacks
 * deliver, so tests can assert on concrete content.
 *
 * Header-only: run.sh compiles only sanity/main.c and sanity/target.c, so
 * all functions here are static.
 */

#ifndef SAVKIT_H
#define SAVKIT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "readstat.h"

/* ------------------------------------------------------------------ */
/* Logical row model                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;      /* logical variable name (UTF-8) */
    int         is_string; /* 1 => READSTAT_TYPE_STRING, 0 => double */
    long        width;     /* declared string width; unused for numerics */
} om_var_t;

typedef struct {
    const char *str;       /* string cell content (bytes); NULL/"" => blank */
    double      num;       /* numeric cell value */
    int         sysmis;    /* numeric cell is system-missing */
} om_cell_t;

/* ------------------------------------------------------------------ */
/* Growable byte buffer with explicit little-endian putters            */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *data;
    size_t         len, cap;
    int            oom;
} ob_t;

static void ob_put(ob_t *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = 2 * (b->len + n) + 16;
        unsigned char *nd = realloc(b->data, nc);
        if (!nd) { b->oom = 1; return; }
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void ob_byte(ob_t *b, unsigned char c) { ob_put(b, &c, 1); }

static void ob_u32(ob_t *b, uint32_t v) {
    unsigned char p[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
    ob_put(b, p, 4);
}

static void ob_u64(ob_t *b, uint64_t v) {
    unsigned char p[8];
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (v >> (8 * i)) & 0xff;
    ob_put(b, p, 8);
}

static void ob_f64(ob_t *b, double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    ob_u64(b, u);
}

static void ob_fill(ob_t *b, unsigned char c, size_t n) {
    unsigned char tmp[32];
    memset(tmp, c, sizeof(tmp));
    while (n) {
        size_t k = n > sizeof(tmp) ? sizeof(tmp) : n;
        ob_put(b, tmp, k);
        n -= k;
    }
}

static void ob_free(ob_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* SAV constants (from the format description) */
#define OM_SYSMIS   0xFFEFFFFFFFFFFFFFULL
#define OM_HIGHEST  0x7FEFFFFFFFFFFFFFULL
#define OM_LOWEST   0xFFEFFFFFFFFFFFFEULL

/* ------------------------------------------------------------------ */
/* Segment geometry for long strings                                   */
/*                                                                     */
/* A string of declared width W > 255 is stored in ceil((W+4)/252)     */
/* segments.  Every full segment holds 255 data bytes in 256 bytes of  */
/* storage (the 256th byte is padding).  The last segment holds the    */
/* remaining W - 252*(nseg-1) bytes, rounded up to a multiple of 8.    */
/* ------------------------------------------------------------------ */

static int om_nseg(long w) {
    return w > 255 ? (int)((w + 251) / 252) : 1;
}

static long om_seg_storage(long w, int seg, int nseg) {
    if (nseg == 1)
        return (w + 7) / 8 * 8;
    if (seg < nseg - 1)
        return 256;
    return (w - 252L * (nseg - 1) + 7) / 8 * 8;
}

static long om_var_columns(const om_var_t *v) {
    if (!v->is_string)
        return 1;
    int nseg = om_nseg(v->width);
    long cols = 0;
    int s;
    for (s = 0; s < nseg; s++)
        cols += om_seg_storage(v->width, s, nseg) / 8;
    return cols;
}

/* ------------------------------------------------------------------ */
/* Short (8-byte, upper-case) dictionary names and ghost segment names */
/* ------------------------------------------------------------------ */

#define OM_MAX_SEG 8

typedef struct {
    char shortname[9];
    char ghost[OM_MAX_SEG][9]; /* entry s>=1 names segment s */
    int  nseg;
} om_names_t;

static void om_upcase(char *dst, const char *src, size_t max) {
    size_t i;
    for (i = 0; i < max && src[i]; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
    }
    dst[i] = '\0';
}

static int om_name_used(const om_names_t *names, int count, const char *cand) {
    int i, s;
    for (i = 0; i < count; i++) {
        if (names[i].shortname[0] && strcmp(names[i].shortname, cand) == 0)
            return 1;
        for (s = 1; s < names[i].nseg && s < OM_MAX_SEG; s++) {
            if (names[i].ghost[s][0] && strcmp(names[i].ghost[s], cand) == 0)
                return 1;
        }
    }
    return 0;
}

static void om_assign_names(const om_var_t *vars, int nvars, om_names_t *names) {
    int i, s;
    unsigned int fallback = 1;
    memset(names, 0, sizeof(om_names_t) * nvars);
    for (i = 0; i < nvars; i++) {
        om_upcase(names[i].shortname, vars[i].name, 8);
        if (om_name_used(names, i, names[i].shortname))
            snprintf(names[i].shortname, 9, "V%07u", fallback++);
        names[i].nseg = vars[i].is_string ? om_nseg(vars[i].width) : 1;
    }
    /* Ghost names: first five characters of the logical name, upper-cased,
     * plus a counter digit, as SPSS expects (LONGNAME -> LONGN0, LONGN1..) */
    for (i = 0; i < nvars; i++) {
        char stem[6];
        unsigned int counter = 0;
        if (names[i].nseg <= 1)
            continue;
        om_upcase(stem, vars[i].name, 5);
        for (s = 1; s < names[i].nseg && s < OM_MAX_SEG; s++) {
            char cand[9];
            do {
                snprintf(cand, sizeof(cand), "%.5s%u", stem, counter);
                if (strlen(cand) > 8)
                    snprintf(cand, sizeof(cand), "G%07u", fallback);
                counter++;
            } while (om_name_used(names, nvars, cand) && counter < 100);
            snprintf(names[i].ghost[s], 9, "%s", cand);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Dictionary emission (byte-by-byte, no ReadStat writer)              */
/* ------------------------------------------------------------------ */

static void om_emit_header(ob_t *b, long ncols, long nrows, int compressed) {
    ob_put(b, "$FL2", 4);                                   /* rec_type */
    ob_fill(b, ' ', 60);                                    /* prod_name */
    ob_u32(b, 2);                                           /* layout_code */
    ob_u32(b, (uint32_t)ncols);                             /* nominal_case_size */
    ob_u32(b, compressed ? 1 : 0);                          /* compression */
    ob_u32(b, 0);                                           /* weight_index */
    ob_u32(b, (uint32_t)nrows);                             /* ncases */
    ob_f64(b, 100.0);                                       /* bias */
    ob_put(b, "12 Nov 26", 9);                              /* creation_date */
    ob_put(b, "00:00:00", 8);                               /* creation_time */
    ob_fill(b, ' ', 64);                                    /* file_label */
    ob_fill(b, 0, 3);                                       /* padding */
}

static void om_emit_var_record(ob_t *b, int32_t type, const char *name8, int32_t fmt) {
    char nm[8];
    memset(nm, ' ', sizeof(nm));
    if (name8) {
        size_t l = strlen(name8);
        if (l > 8) l = 8;
        memcpy(nm, name8, l);
    }
    ob_u32(b, 2);              /* rec_type: variable */
    ob_u32(b, (uint32_t)type); /* type: 0 numeric, >0 string width, -1 continuation */
    ob_u32(b, 0);              /* has_var_label */
    ob_u32(b, 0);              /* n_missing_values */
    ob_u32(b, (uint32_t)fmt);  /* print format */
    ob_u32(b, (uint32_t)fmt);  /* write format */
    ob_put(b, nm, 8);
}

#define OM_FMT_A(w)  ((1 << 16) | (((w) & 0xff) << 8))     /* A<w>   */
#define OM_FMT_NUM   ((5 << 16) | (8 << 8) | 2)            /* F8.2   */
#define OM_FMT_CONT  0x011d01                              /* as SPSS writes on continuation records */

static void om_emit_variable_records(ob_t *b, const om_var_t *vars, int nvars,
        const om_names_t *names) {
    int i, s;
    long k;
    for (i = 0; i < nvars; i++) {
        const om_var_t *v = &vars[i];
        if (!v->is_string) {
            om_emit_var_record(b, 0, names[i].shortname, OM_FMT_NUM);
            continue;
        }
        int nseg = names[i].nseg;
        for (s = 0; s < nseg; s++) {
            long seg_user = (s < nseg - 1) ? 255 : (v->width > 255 ? v->width - 252L * (nseg - 1) : v->width);
            long storage = om_seg_storage(v->width, s, nseg);
            const char *nm = s == 0 ? names[i].shortname : names[i].ghost[s];
            om_emit_var_record(b, (int32_t)seg_user, nm, OM_FMT_A(seg_user));
            for (k = 0; k < storage / 8 - 1; k++)
                om_emit_var_record(b, -1, NULL, OM_FMT_CONT);
        }
    }
}

static void om_emit_info_header(ob_t *b, int32_t subtype, int32_t size, int32_t count) {
    ob_u32(b, 7);            /* rec_type: info record */
    ob_u32(b, (uint32_t)subtype);
    ob_u32(b, (uint32_t)size);
    ob_u32(b, (uint32_t)count);
}

static void om_emit_dictionary(ob_t *b, const om_var_t *vars, int nvars,
        long nrows, int compressed) {
    om_names_t *names = calloc(nvars, sizeof(om_names_t));
    long ncols = 0;
    int i, need_long_names = 0;
    ob_t long_names = { 0 }, vls = { 0 };

    om_assign_names(vars, nvars, names);
    for (i = 0; i < nvars; i++)
        ncols += om_var_columns(&vars[i]);

    om_emit_header(b, ncols, nrows, compressed);
    om_emit_variable_records(b, vars, nvars, names);

    /* machine integer info: little-endian IEEE, character code UTF-8 */
    om_emit_info_header(b, 3, 4, 8);
    ob_u32(b, 20); ob_u32(b, 0); ob_u32(b, 0); ob_u32(b, (uint32_t)-1);
    ob_u32(b, 1);  ob_u32(b, 1); ob_u32(b, 2); ob_u32(b, 65001);

    /* machine floating-point info: SYSMIS / HIGHEST / LOWEST */
    om_emit_info_header(b, 4, 8, 3);
    ob_u64(b, OM_SYSMIS);
    ob_u64(b, OM_HIGHEST);
    ob_u64(b, OM_LOWEST);

    /* character encoding record */
    om_emit_info_header(b, 20, 1, 5);
    ob_put(b, "UTF-8", 5);

    /* long variable names, when a logical name is not its short name */
    for (i = 0; i < nvars; i++) {
        if (strcmp(vars[i].name, names[i].shortname) != 0) {
            if (need_long_names)
                ob_byte(&long_names, '\t');
            ob_put(&long_names, names[i].shortname, strlen(names[i].shortname));
            ob_byte(&long_names, '=');
            ob_put(&long_names, vars[i].name, strlen(vars[i].name));
            need_long_names = 1;
        }
    }
    if (need_long_names) {
        om_emit_info_header(b, 13, 1, (int32_t)long_names.len);
        ob_put(b, long_names.data, long_names.len);
    }

    /* very long string record: "SHORT=00511\0\t" per wide variable */
    for (i = 0; i < nvars; i++) {
        char kv[32];
        if (!vars[i].is_string || vars[i].width <= 255)
            continue;
        snprintf(kv, sizeof(kv), "%s=%05lu", names[i].shortname, (unsigned long)vars[i].width);
        ob_put(&vls, kv, strlen(kv));
        ob_byte(&vls, 0);
        ob_byte(&vls, '\t');
    }
    if (vls.len) {
        om_emit_info_header(b, 14, 1, (int32_t)vls.len);
        ob_put(b, vls.data, vls.len);
    }

    /* extended number of cases */
    om_emit_info_header(b, 16, 8, 2);
    ob_u64(b, 1);
    ob_u64(b, (uint64_t)nrows);

    /* dictionary termination */
    ob_u32(b, 999);
    ob_u32(b, 0);

    ob_free(&long_names);
    ob_free(&vls);
    free(names);
}

/* ------------------------------------------------------------------ */
/* Data area emission                                                  */
/* ------------------------------------------------------------------ */

/* One uncompressed row: for each variable, its 8-byte units in order. */
static void om_emit_row_units(ob_t *b, const om_var_t *vars, int nvars,
        const om_cell_t *cells) {
    int i, s;
    long k;
    for (i = 0; i < nvars; i++) {
        const om_var_t *v = &vars[i];
        const om_cell_t *cell = &cells[i];
        if (!v->is_string) {
            if (cell->sysmis)
                ob_u64(b, OM_SYSMIS);
            else
                ob_f64(b, cell->num);
            continue;
        }
        const char *c = cell->str ? cell->str : "";
        size_t clen = strlen(c);
        int nseg = om_nseg(v->width);
        for (s = 0; s < nseg; s++) {
            long storage = om_seg_storage(v->width, s, nseg);
            long base = s * 255L;
            for (k = 0; k < storage; k++) {
                long idx = base + k;
                /* the 256th byte of a full segment is padding, as is any
                 * storage past the declared width */
                ob_byte(b, (size_t)idx < clen ? (unsigned char)c[idx] : ' ');
            }
        }
    }
}

/* Row compression, written from the opcode semantics: 254 for eight
 * spaces, 255 for SYSMIS, value+100 for integral doubles in (-100,152),
 * 253 followed by a literal 8-byte unit otherwise.  Opcodes and literal
 * bytes are collected in two streams and interleaved in groups of eight
 * opcodes at the end; 252 terminates the data after the last row. */
typedef struct {
    ob_t ops;
    ob_t lits;
} om_comp_t;

static void om_comp_row(om_comp_t *c, const om_var_t *vars, int nvars,
        const unsigned char *row) {
    static const char eight_spaces[8] = { ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    size_t off = 0;
    int i;
    for (i = 0; i < nvars; i++) {
        const om_var_t *v = &vars[i];
        long cols = om_var_columns(v);
        long k;
        for (k = 0; k < cols; k++, off += 8) {
            const unsigned char *unit = row + off;
            if (v->is_string) {
                if (memcmp(unit, eight_spaces, 8) == 0) {
                    ob_byte(&c->ops, 254);
                } else {
                    ob_byte(&c->ops, 253);
                    ob_put(&c->lits, unit, 8);
                }
            } else {
                uint64_t bits;
                double d;
                memcpy(&bits, unit, 8);
                memcpy(&d, unit, 8);
                if (bits == OM_SYSMIS) {
                    ob_byte(&c->ops, 255);
                } else if (d > -100 && d < 152 && (double)(long long)d == d) {
                    ob_byte(&c->ops, (unsigned char)((int)d + 100));
                } else {
                    ob_byte(&c->ops, 253);
                    ob_put(&c->lits, unit, 8);
                }
            }
        }
    }
}

static void om_comp_flush(om_comp_t *c, ob_t *out, int terminate) {
    size_t p = 0, lit = 0;
    if (terminate)
        ob_byte(&c->ops, 252);
    while (p < c->ops.len) {
        size_t g = c->ops.len - p > 8 ? 8 : c->ops.len - p;
        size_t i;
        ob_put(out, c->ops.data + p, g);
        ob_fill(out, 0, 8 - g);
        for (i = 0; i < g; i++) {
            if (c->ops.data[p + i] == 253) {
                ob_put(out, c->lits.data + lit, 8);
                lit += 8;
            }
        }
        p += g;
    }
}

static void om_emit_data(ob_t *b, const om_var_t *vars, int nvars,
        const om_cell_t *cells, long nrows, int compressed) {
    long r;
    if (!compressed) {
        for (r = 0; r < nrows; r++)
            om_emit_row_units(b, vars, nvars, cells + r * nvars);
        return;
    }
    om_comp_t comp;
    memset(&comp, 0, sizeof(comp));
    for (r = 0; r < nrows; r++) {
        ob_t row = { 0 };
        om_emit_row_units(&row, vars, nvars, cells + r * nvars);
        om_comp_row(&comp, vars, nvars, row.data);
        ob_free(&row);
    }
    om_comp_flush(&comp, b, nrows > 0);
    ob_free(&comp.ops);
    ob_free(&comp.lits);
}

static int om_write_file(const char *path, const ob_t *b) {
    if (b->oom)
        return 1;
    FILE *f = fopen(path, "wb");
    if (!f)
        return 1;
    if (b->len && fwrite(b->data, 1, b->len, f) != b->len) {
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/* Whole file from the logical model. */
static int om_write_sav(const char *path, const om_var_t *vars, int nvars,
        const om_cell_t *cells, long nrows, int compressed) {
    ob_t b = { 0 };
    om_emit_dictionary(&b, vars, nvars, nrows, compressed);
    om_emit_data(&b, vars, nvars, cells, nrows, compressed);
    int rc = om_write_file(path, &b);
    ob_free(&b);
    return rc;
}

/* Dictionary from the model, caller-supplied raw data area (used for the
 * hand-crafted compressed streams). */
static int om_write_sav_raw(const char *path, const om_var_t *vars, int nvars,
        long nrows, int compressed, const unsigned char *data, size_t dlen) {
    ob_t b = { 0 };
    om_emit_dictionary(&b, vars, nvars, nrows, compressed);
    ob_put(&b, data, dlen);
    int rc = om_write_file(path, &b);
    ob_free(&b);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Independent expansion of a row-compressed stream (reference side)   */
/* ------------------------------------------------------------------ */

/* Expand src into 8-byte units appended to out.  Returns 0 on success
 * (sets *terminated if opcode 252 was seen), 1 if the stream is
 * truncated (partial control group or partial literal). */
static int ok_expand(const unsigned char *src, size_t n, ob_t *out, int *terminated) {
    size_t p = 0;
    *terminated = 0;
    while (p < n) {
        unsigned char ctrl[8];
        int i;
        if (n - p < 8)
            return 1;
        memcpy(ctrl, src + p, 8);
        p += 8;
        for (i = 0; i < 8; i++) {
            unsigned char c = ctrl[i];
            if (c == 0) {
                continue;                       /* padding: no output */
            } else if (c == 252) {
                *terminated = 1;
                return 0;
            } else if (c == 253) {
                if (n - p < 8)
                    return 1;
                ob_put(out, src + p, 8);
                p += 8;
            } else if (c == 254) {
                ob_fill(out, ' ', 8);
            } else if (c == 255) {
                ob_u64(out, OM_SYSMIS);
            } else {
                ob_f64(out, (double)c - 100.0);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Capture harness: record everything readstat_parse_sav delivers      */
/* ------------------------------------------------------------------ */

#define CAP_MAXVARS 16
#define CAP_MAXVALS 512

typedef struct {
    char name[65];
    int  index;
    int  index_after_skipping;
    int  is_string;
    long storage_width;
} cap_var_t;

typedef struct {
    int    row;
    int    var;          /* logical variable index */
    int    is_string;
    int    sysmis;
    double num;
    char   str[520];
} cap_val_t;

typedef struct {
    cap_var_t vars[CAP_MAXVARS];
    int       nvars;
    cap_val_t vals[CAP_MAXVALS];
    int       nvals;
    int       meta_seen;
    long      meta_rows;
    int       meta_vars;
    int       meta_compression;
    unsigned  skip_mask;   /* bit i: return SKIP_VARIABLE for variable i */
    int       abort_after; /* if >0: value handler aborts on the Nth cell */
} cap_t;

static void cap_reset(cap_t *c) {
    c->nvars = 0;
    c->nvals = 0;
    c->meta_seen = 0;
    c->meta_rows = -99;
    c->meta_vars = -99;
    c->meta_compression = -99;
    c->skip_mask = 0;
    c->abort_after = 0;
}

static int cap_metadata(readstat_metadata_t *m, void *ctx) {
    cap_t *c = ctx;
    c->meta_seen = 1;
    c->meta_rows = readstat_get_row_count(m);
    c->meta_vars = readstat_get_var_count(m);
    c->meta_compression = readstat_get_compression(m);
    return READSTAT_HANDLER_OK;
}

static int cap_variable(int idx, readstat_variable_t *v, const char *label, void *ctx) {
    cap_t *c = ctx;
    (void)label;
    if (c->nvars < CAP_MAXVARS) {
        cap_var_t *cv = &c->vars[c->nvars++];
        snprintf(cv->name, sizeof(cv->name), "%s", readstat_variable_get_name(v));
        cv->index = readstat_variable_get_index(v);
        cv->index_after_skipping = readstat_variable_get_index_after_skipping(v);
        cv->is_string = readstat_variable_get_type(v) == READSTAT_TYPE_STRING;
        cv->storage_width = (long)readstat_variable_get_storage_width(v);
    }
    if (idx >= 0 && idx < 32 && (c->skip_mask & (1u << idx)))
        return READSTAT_HANDLER_SKIP_VARIABLE;
    return READSTAT_HANDLER_OK;
}

static int cap_value(int row, readstat_variable_t *v, readstat_value_t x, void *ctx) {
    cap_t *c = ctx;
    if (c->nvals < CAP_MAXVALS) {
        cap_val_t *cv = &c->vals[c->nvals++];
        cv->row = row;
        cv->var = readstat_variable_get_index(v);
        cv->is_string = readstat_value_type(x) == READSTAT_TYPE_STRING;
        cv->sysmis = readstat_value_is_system_missing(x);
        cv->num = 0;
        cv->str[0] = '\0';
        if (cv->is_string) {
            const char *s = readstat_string_value(x);
            snprintf(cv->str, sizeof(cv->str), "%s", s ? s : "");
        } else {
            cv->num = readstat_double_value(x);
        }
    }
    if (c->abort_after > 0 && c->nvals >= c->abort_after)
        return READSTAT_HANDLER_ABORT;
    return READSTAT_HANDLER_OK;
}

static readstat_error_t cap_parse(cap_t *c, const char *path, long row_offset, long row_limit) {
    readstat_parser_t *p = readstat_parser_init();
    readstat_set_metadata_handler(p, cap_metadata);
    readstat_set_variable_handler(p, cap_variable);
    readstat_set_value_handler(p, cap_value);
    if (row_offset > 0)
        readstat_set_row_offset(p, row_offset);
    if (row_limit > 0)
        readstat_set_row_limit(p, row_limit);
    readstat_error_t rc = readstat_parse_sav(p, path, c);
    readstat_parser_free(p);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Temp directory helper                                               */
/* ------------------------------------------------------------------ */

static char *sk_mkdtemp(char *buf, size_t n) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base)
        base = "/tmp";
    snprintf(buf, n, "%s/savkit-XXXXXX", base);
    return mkdtemp(buf);
}

#endif /* SAVKIT_H */
