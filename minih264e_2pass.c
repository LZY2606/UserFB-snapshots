/*
    minih264e_2pass.c - two-pass measurement and bit-target planning.

    See minih264e_2pass.h for the contract.  This file is plain C11 and uses
    only the C standard library plus minih264e's public measurement hook.
*/

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minih264e_2pass.h"

#define TWO_PASS_MAX_MB      (1 << 22)
#define TWO_PASS_MAX_FRAMES  (1 << 24)

/*****************************************************************************/
/*  Small helpers                                                            */
/*****************************************************************************/

static double tp_clamp_double(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

double H264E_2pass_qscale(int qp)
{
    return pow(2.0, ((double)qp - 26.0) / 6.0);
}

unsigned H264E_2pass_ue_bits(unsigned val)
{
    unsigned n = 0;
    unsigned t = val + 1u;
    do
    {
        n++;
    } while (t >>= 1);
    return 2u*n - 1u;
}

const char *H264E_2pass_strerror(int error)
{
    switch (error)
    {
    case H264E_2PASS_OK:             return "ok";
    case H264E_2PASS_ERR_ARGUMENT:   return "bad argument";
    case H264E_2PASS_ERR_OPEN:       return "cannot open file";
    case H264E_2PASS_ERR_IO:         return "input/output error";
    case H264E_2PASS_ERR_FORMAT:     return "malformed statistics file";
    case H264E_2PASS_ERR_MISSING:    return "missing records";
    case H264E_2PASS_ERR_EXTRA:      return "extra records or frames";
    case H264E_2PASS_ERR_ORDER:      return "records out of order";
    case H264E_2PASS_ERR_IDENTITY:   return "frame identity or type mismatch";
    case H264E_2PASS_ERR_STATE:      return "operation not valid in this state";
    default:                         return "unknown error";
    }
}

/* ANSI C requires the caller to supply a newline to fflush; not needed */
static int tp_line_is_blank(const char *line)
{
    while (*line)
    {
        if (*line != ' ' && *line != '\t' && *line != '\r' && *line != '\n')
        {
            return 0;
        }
        line++;
    }
    return 1;
}

/* Parse one unsigned decimal token, rejecting signs, junk and overflow. */
static int tp_parse_uint(const char *text, unsigned long long *value)
{
    char *end = NULL;
    unsigned long long v;

    if (!text || !*text || *text == '-' || *text == '+')
    {
        return 0;
    }
    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno || !end || *end || end == text)
    {
        return 0;
    }
    *value = v;
    return 1;
}

/* Parse one unsigned hexadecimal token (FNV fingerprints). */
static int tp_parse_hex(const char *text, unsigned long long *value)
{
    char *end = NULL;
    unsigned long long v;

    if (!text || !*text || *text == '-' || *text == '+')
    {
        return 0;
    }
    errno = 0;
    v = strtoull(text, &end, 16);
    if (errno || !end || *end || end == text || strlen(text) > 16)
    {
        return 0;
    }
    *value = v;
    return 1;
}

static int tp_parse_double(const char *text, double *value){
    char *end = NULL;
    double v;

    if (!text || !*text)
    {
        return 0;
    }
    errno = 0;
    v = strtod(text, &end);
    if (errno || !end || *end || end == text || !isfinite(v))
    {
        return 0;
    }
    *value = v;
    return 1;
}

unsigned long long H264E_2pass_fnv1a_i420(
    const H264E_io_yuv_t *frame, int width, int height)
{
    unsigned long long hash = H264E_2PASS_FNV_OFFSET;
    int plane;

    if (!frame || width <= 0 || height <= 0 || (width & 1) || (height & 1))
    {
        return H264E_2PASS_FNV_OFFSET;
    }
    for (plane = 0; plane < 3; plane++)
    {
        int w = plane ? width / 2 : width;
        int h = plane ? height / 2 : height;
        int x, y;
        const unsigned char *base = frame->yuv[plane];
        int stride = frame->stride[plane];

        if (!base || stride < w)
        {
            return H264E_2PASS_FNV_OFFSET;
        }
        for (y = 0; y < h; y++)
        {
            const unsigned char *row = base + (size_t)y*stride;
            for (x = 0; x < w; x++)
            {
                hash ^= row[x];
                hash *= H264E_2PASS_FNV_PRIME;
            }
        }
    }
    return hash;
}

int H264E_2pass_au_is_idr(const unsigned char *data, int size)
{
    int i = 0;

    if (!data || size <= 0)
    {
        return -1;
    }
    while (i + 3 < size)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            int type = data[i + 3] & 0x1f;
            if (type == 5)
            {
                return 1;
            }
            if (type == 1 || type == 20)
            {
                return 0;
            }
            i += 3;
            continue;
        }
        i++;
    }
    return -1;
}

/*****************************************************************************/
/*  Pass-1 writer                                                            */
/*****************************************************************************/

struct H264E_2pass_writer
{
    FILE *file;
    char *path;
    int width, height, gop;

    H264E_2pass_record_t *records;
    int frame_count;
    int frame_cap;

    int active;                 /* between begin() and end()/abort() */
    int index;
    int mb_count;
    int *mb_qp;
    unsigned *mb_bits;
    unsigned long long fnv;

    H264E_2pass_mb_t *mb_public;
    H264E_2pass_frame_stats_t stats;
    int stats_valid;
};

static int tp_writer_reserve(H264E_2pass_writer_t *w)
{
    int cap;
    H264E_2pass_record_t *rec;

    if (w->frame_count < w->frame_cap)
    {
        return H264E_2PASS_OK;
    }
    cap = w->frame_cap ? w->frame_cap*2 : 64;
    if (cap > TWO_PASS_MAX_FRAMES)
    {
        return H264E_2PASS_ERR_IO;
    }
    rec = (H264E_2pass_record_t *)realloc(
        w->records, (size_t)cap*sizeof(*rec));
    if (!rec)
    {
        return H264E_2PASS_ERR_IO;
    }
    w->records = rec;
    w->frame_cap = cap;
    return H264E_2PASS_OK;
}

static void tp_writer_free(H264E_2pass_writer_t *w)
{
    if (!w)
    {
        return;
    }
    free(w->path);
    free(w->records);
    free(w->mb_qp);
    free(w->mb_bits);
    free(w->mb_public);
    free(w);
}

int H264E_2pass_writer_open(
    H264E_2pass_writer_t **out, const char *path,
    int width, int height, int gop)
{
    H264E_2pass_writer_t *w;
    int nmb;

    if (!out || !path || !*path || width <= 0 || height <= 0 ||
        (width & 1) || (height & 1) || gop < 0)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    nmb = ((width + 15) >> 4) * ((height + 15) >> 4);
    if (nmb <= 0 || nmb > TWO_PASS_MAX_MB)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }

    w = (H264E_2pass_writer_t *)calloc(1, sizeof(*w));
    if (!w)
    {
        return H264E_2PASS_ERR_IO;
    }
    w->path = (char *)malloc(strlen(path) + 1);
    if (!w->path)
    {
        tp_writer_free(w);
        return H264E_2PASS_ERR_IO;
    }
    strcpy(w->path, path);
    w->width = width;
    w->height = height;
    w->gop = gop;
    w->mb_count = nmb;
    w->mb_qp = (int *)malloc((size_t)nmb*sizeof(*w->mb_qp));
    w->mb_bits = (unsigned *)malloc((size_t)nmb*sizeof(*w->mb_bits));
    w->mb_public = (H264E_2pass_mb_t *)malloc(
        (size_t)nmb*sizeof(*w->mb_public));
    if (!w->mb_qp || !w->mb_bits || !w->mb_public)
    {
        tp_writer_free(w);
        return H264E_2PASS_ERR_IO;
    }

    /* Create/truncate now so an unwritable path fails before encoding. */
    w->file = fopen(w->path, "wb");
    if (!w->file)
    {
        tp_writer_free(w);
        return H264E_2PASS_ERR_OPEN;
    }
    *out = w;
    return H264E_2PASS_OK;
}

static void tp_writer_on_mb(void *token, int mb_index, int qp, unsigned bits)
{
    H264E_2pass_writer_t *w = (H264E_2pass_writer_t *)token;

    if (!w || !w->active || mb_index < 0 || mb_index >= w->mb_count)
    {
        return;
    }
    if (!w->mb_bits[mb_index] && bits)
    {
        w->mb_qp[mb_index] = qp;
    }
    w->mb_bits[mb_index] += bits;
}

static void tp_writer_on_au(void *token, int au_bytes)
{
    (void)token;
    (void)au_bytes;
}

int H264E_2pass_writer_hook(
    H264E_2pass_writer_t *writer, H264E_mb_stats_hook_t *hook)
{
    if (!writer || !hook)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    memset(hook, 0, sizeof(*hook));
    hook->on_mb = tp_writer_on_mb;
    hook->on_au = tp_writer_on_au;
    hook->token = writer;
    return H264E_2PASS_OK;
}

int H264E_2pass_writer_begin(
    H264E_2pass_writer_t *writer, int input_index,
    const H264E_io_yuv_t *frame)
{
    if (!writer || !frame || input_index < 0)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    if (writer->active)
    {
        return H264E_2PASS_ERR_STATE;
    }
    writer->active = 1;
    writer->stats_valid = 0;
    writer->index = input_index;
    writer->fnv = H264E_2pass_fnv1a_i420(
        frame, writer->width, writer->height);
    memset(writer->mb_bits, 0, (size_t)writer->mb_count*sizeof(*writer->mb_bits));
    memset(writer->mb_qp, 0, (size_t)writer->mb_count*sizeof(*writer->mb_qp));
    return H264E_2PASS_OK;
}

int H264E_2pass_writer_end(
    H264E_2pass_writer_t *writer, int au_bytes, int is_idr,
    H264E_2pass_frame_stats_t *stats)
{
    unsigned long long total_bits = 0;
    double c = 0.0;
    int i, error;
    H264E_2pass_record_t *rec;

    if (!writer || au_bytes <= 0)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    if (!writer->active)
    {
        return H264E_2PASS_ERR_STATE;
    }
    for (i = 0; i < writer->mb_count; i++)
    {
        total_bits += writer->mb_bits[i];
        c += (double)writer->mb_bits[i] * H264E_2pass_qscale(writer->mb_qp[i]);
    }
    if (total_bits > (unsigned long long)au_bytes*8ull)
    {
        /* Measurement must never claim more syntax bits than the AU holds. */
        writer->active = 0;
        return H264E_2PASS_ERR_STATE;
    }

    error = tp_writer_reserve(writer);
    if (error)
    {
        writer->active = 0;
        return error;
    }
    rec = &writer->records[writer->frame_count];
    rec->index = writer->index;
    rec->is_idr = is_idr ? 1 : 0;
    rec->width = writer->width;
    rec->height = writer->height;
    rec->fnv1a = writer->fnv;
    rec->c = c;
    rec->h = (double)((unsigned long long)au_bytes*8ull - total_bits);
    writer->frame_count++;

    for (i = 0; i < writer->mb_count; i++)
    {
        writer->mb_public[i].qp = writer->mb_qp[i];
        writer->mb_public[i].bits = writer->mb_bits[i];
    }
    writer->stats.input_index = rec->index;
    writer->stats.is_idr = rec->is_idr;
    writer->stats.width = rec->width;
    writer->stats.height = rec->height;
    writer->stats.mb_count = writer->mb_count;
    writer->stats.fnv1a = rec->fnv1a;
    writer->stats.c = rec->c;
    writer->stats.h = rec->h;
    writer->stats.mb = writer->mb_public;
    writer->stats_valid = 1;
    writer->active = 0;
    if (stats)
    {
        *stats = writer->stats;
    }
    return H264E_2PASS_OK;
}

int H264E_2pass_writer_abort(H264E_2pass_writer_t *writer)
{
    if (!writer)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    if (!writer->active)
    {
        return H264E_2PASS_ERR_STATE;
    }
    writer->active = 0;
    writer->stats_valid = 0;
    return H264E_2PASS_OK;
}

int H264E_2pass_writer_close(H264E_2pass_writer_t *writer, int *frames)
{
    FILE *file;
    int i;

    if (!writer)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    if (writer->active)
    {
        return H264E_2PASS_ERR_STATE;
    }
    file = writer->file;
    writer->file = NULL;
    if (!file)
    {
        return H264E_2PASS_ERR_STATE;
    }
    if (fseek(file, 0, SEEK_SET) ||
        fprintf(file, "%s %d\n",
            H264E_2PASS_FILE_MAGIC, H264E_2PASS_FILE_VERSION) < 0 ||
        fprintf(file, "width %d\n", writer->width) < 0 ||
        fprintf(file, "height %d\n", writer->height) < 0 ||
        fprintf(file, "gop %d\n", writer->gop) < 0 ||
        fprintf(file, "frames %d\n", writer->frame_count) < 0)
    {
        fclose(file);
        return H264E_2PASS_ERR_IO;
    }
    for (i = 0; i < writer->frame_count; i++)
    {
        const H264E_2pass_record_t *r = &writer->records[i];
        if (fprintf(file,
                "frame %d idr %d width %d height %d fnv %016llx "
                "c %.17g h %.17g\n",
                r->index, r->is_idr, r->width, r->height,
                (unsigned long long)r->fnv1a, r->c, r->h) < 0)
        {
            fclose(file);
            return H264E_2PASS_ERR_IO;
        }
    }
    if (fprintf(file, "end\n") < 0 || fflush(file) || fclose(file))
    {
        return H264E_2PASS_ERR_IO;
    }
    if (frames)
    {
        *frames = writer->frame_count;
    }
    return H264E_2PASS_OK;
}

void H264E_2pass_writer_destroy(H264E_2pass_writer_t *writer)
{
    if (!writer)
    {
        return;
    }
    if (writer->file)
    {
        fclose(writer->file);
        writer->file = NULL;
    }
    tp_writer_free(writer);
}

/*****************************************************************************/
/*  Statistics file reader                                                   */
/*****************************************************************************/

struct H264E_2pass_file
{
    int width, height, gop;
    int count;
    H264E_2pass_record_t *records;
};

static int tp_read_line(FILE *file, char *buf, size_t size)
{
    size_t len;

    if (!fgets(buf, (int)size, file))
    {
        return 0;
    }
    len = strlen(buf);
    if (len + 1 >= size && buf[len - 1] != '\n')
    {
        return 0;       /* overlong line */
    }
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    {
        buf[--len] = 0;
    }
    return 1;
}

/* Strictly tokenise one frame record:
       frame <index> idr <0|1> width <w> height <h> fnv <hex> c <double> h <double>
   All keys are positional and fixed, extra tokens are rejected. */
static int tp_parse_record_line(const char *line, H264E_2pass_record_t *rec)
{
    char tok[16][80];
    int n;
    unsigned long long idx, idr, w, h, fnv;

    n = sscanf(line,
        "%79s %79s %79s %79s %79s %79s %79s %79s %79s %79s %79s "
        "%79s %79s %79s",
        tok[0], tok[1], tok[2], tok[3], tok[4], tok[5], tok[6], tok[7],
        tok[8], tok[9], tok[10], tok[11], tok[12], tok[13]);
    if (n != 14 ||
        strcmp(tok[0], "frame") || strcmp(tok[2], "idr") ||
        strcmp(tok[4], "width") || strcmp(tok[6], "height") ||
        strcmp(tok[8], "fnv") || strcmp(tok[10], "c") ||
        strcmp(tok[12], "h"))
    {
        return 0;
    }
    if (!tp_parse_uint(tok[1], &idx) || !tp_parse_uint(tok[3], &idr) ||
        !tp_parse_uint(tok[5], &w) || !tp_parse_uint(tok[7], &h) ||
        !tp_parse_hex(tok[9], &fnv))
    {
        return 0;
    }
    if (!tp_parse_double(tok[11], &rec->c) ||
        !tp_parse_double(tok[13], &rec->h))
    {
        return 0;
    }
    if (idr > 1 || w == 0 || w > 0x7fffffffULL || h == 0 ||
        h > 0x7fffffffULL || rec->c < 0.0 || rec->h < 0.0)
    {
        return 0;
    }    rec->index = (int)idx;
    rec->is_idr = (int)idr;
    rec->width = (int)w;
    rec->height = (int)h;
    rec->fnv1a = fnv;
    return 1;
}

int H264E_2pass_file_open(const char *path, H264E_2pass_file_t **out)
{
    FILE *file;
    char line[512];
    char magic[64];
    unsigned long long value;
    int version, have_w = 0, have_h = 0, have_g = 0, have_n = 0;
    int width = 0, height = 0, gop = 0, count = 0;
    int i, error = H264E_2PASS_ERR_FORMAT;
    int header_done = 0;
    H264E_2pass_file_t *f = NULL;

    if (!path || !out)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    *out = NULL;
    file = fopen(path, "rb");
    if (!file)
    {
        return H264E_2PASS_ERR_OPEN;
    }
    if (!tp_read_line(file, line, sizeof(line)) ||
        sscanf(line, "%63s %d", magic, &version) != 2 ||
        strcmp(magic, H264E_2PASS_FILE_MAGIC) ||
        version != H264E_2PASS_FILE_VERSION)
    {
        goto fail;
    }

    /* Header keys in any order.  The first "frame " line is read but not
       consumed: it is left in line[] for the record loop below. */
    for (;;)
    {
        char key[64], v1[64];

        if (!tp_read_line(file, line, sizeof(line)))
        {
            goto fail;
        }
        if (tp_line_is_blank(line))
        {
            goto fail;
        }
        if (!strncmp(line, "frame ", 6))
        {
            header_done = 1;
            break;
        }
        if (sscanf(line, "%63s %63s", key, v1) != 2)
        {
            goto fail;
        }
        if (!tp_parse_uint(v1, &value) || value > 0x7fffffffULL)
        {
            goto fail;
        }
        if (!strcmp(key, "width") && !have_w)
        {
            width = (int)value;
            have_w = 1;
        }
        else if (!strcmp(key, "height") && !have_h)
        {
            height = (int)value;
            have_h = 1;
        }
        else if (!strcmp(key, "gop") && !have_g)
        {
            gop = (int)value;
            have_g = 1;
        }
        else if (!strcmp(key, "frames") && !have_n)
        {
            count = (int)value;
            have_n = 1;
        }
        else
        {
            goto fail;
        }
    }
    if (!header_done || !have_w || !have_h || !have_g || !have_n ||
        width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        gop < 0 || count <= 0 || count > TWO_PASS_MAX_FRAMES)
    {
        goto fail;
    }

    f = (H264E_2pass_file_t *)calloc(1, sizeof(*f));
    if (!f)
    {
        error = H264E_2PASS_ERR_IO;
        goto fail;
    }
    f->width = width;
    f->height = height;
    f->gop = gop;
    f->count = count;
    f->records = (H264E_2pass_record_t *)calloc(
        (size_t)count, sizeof(*f->records));
    if (!f->records)
    {
        error = H264E_2PASS_ERR_IO;
        goto fail;
    }

    for (i = 0; i < count; i++)
    {
        if (i)
        {
            if (!tp_read_line(file, line, sizeof(line)))
            {
                error = H264E_2PASS_ERR_MISSING;
                goto fail;
            }
        }
        if (!strcmp(line, "end"))
        {
            /* The file ended early: records declared in the header are absent */
            error = H264E_2PASS_ERR_MISSING;
            goto fail;
        }
        if (!tp_parse_record_line(line, &f->records[i]))
        {
            goto fail;
        }
    }

    /* Nothing but "end" may follow the declared number of records. */
    if (!tp_read_line(file, line, sizeof(line)) || strcmp(line, "end"))
    {
        error = H264E_2PASS_ERR_EXTRA;
        goto fail;
    }
    if (tp_read_line(file, line, sizeof(line)))
    {
        error = H264E_2PASS_ERR_EXTRA;
        goto fail;
    }
    if (ferror(file))
    {
        error = H264E_2PASS_ERR_IO;
        goto fail;
    }
    fclose(file);
    *out = f;
    return H264E_2PASS_OK;

fail:
    if (f)
    {
        free(f->records);
        free(f);
    }
    fclose(file);
    return error;
}

int H264E_2pass_file_frames(const H264E_2pass_file_t *file)
{
    return file ? file->count : 0;
}

int H264E_2pass_file_width(const H264E_2pass_file_t *file)
{
    return file ? file->width : 0;
}

int H264E_2pass_file_height(const H264E_2pass_file_t *file)
{
    return file ? file->height : 0;
}

int H264E_2pass_file_gop(const H264E_2pass_file_t *file)
{
    return file ? file->gop : 0;
}

const H264E_2pass_record_t *H264E_2pass_file_record(
    const H264E_2pass_file_t *file, int index)
{
    if (!file || index < 0 || index >= file->count)
    {
        return NULL;
    }
    return &file->records[index];
}

void H264E_2pass_file_close(H264E_2pass_file_t *file)
{
    if (!file)
    {
        return;
    }
    free(file->records);
    free(file);
}

/*****************************************************************************/
/*  Planner                                                                  */
/*****************************************************************************/

struct H264E_2pass_planner
{
    H264E_2pass_record_t *records;
    int count;
    int cursor;
    int qp_min, qp_max;
    double remaining_bytes;
    double a[2];                /* [0] = P, [1] = IDR */
    int plan_valid;
    H264E_2pass_plan_t plan;
    int diag_valid;
    H264E_2pass_diag_t diag;
};

int H264E_2pass_planner_open(
    H264E_2pass_planner_t **out,
    const H264E_2pass_record_t *records, int count,
    double target_bytes, int qp_min, int qp_max)
{
    H264E_2pass_planner_t *p;
    int i;

    if (!out || !records || count <= 0 || count > TWO_PASS_MAX_FRAMES ||
        !isfinite(target_bytes) || target_bytes <= 0.0 ||
        qp_min < 10 || qp_max > 51 || qp_min > qp_max)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    for (i = 0; i < count; i++)
    {
        if (records[i].index != i || records[i].width <= 0 ||
            records[i].height <= 0 || !isfinite(records[i].c) ||
            !isfinite(records[i].h) || records[i].c < 0.0 ||
            records[i].h < 0.0)
        {
            return H264E_2PASS_ERR_FORMAT;
        }
    }
    if (records[0].is_idr != 1)
    {
        return H264E_2PASS_ERR_FORMAT;
    }

    p = (H264E_2pass_planner_t *)calloc(1, sizeof(*p));
    if (!p)
    {
        return H264E_2PASS_ERR_IO;
    }
    p->records = (H264E_2pass_record_t *)calloc(
        (size_t)count, sizeof(*p->records));
    if (!p->records)
    {
        free(p);
        return H264E_2PASS_ERR_IO;
    }
    memcpy(p->records, records, (size_t)count*sizeof(*p->records));
    p->count = count;
    p->cursor = 0;
    p->qp_min = qp_min;
    p->qp_max = qp_max;
    p->remaining_bytes = target_bytes;
    p->a[0] = 1.0;              /* P */
    p->a[1] = 1.0;              /* IDR */
    *out = p;
    return H264E_2PASS_OK;
}

static int tp_class_index(const H264E_2pass_record_t *r)
{
    return r->is_idr ? 1 : 0;
}

/* predicted_bits(q) = sum(H) + (1/s(q)) * sum(a_class * C) */
static void tp_planner_sums(
    const H264E_2pass_planner_t *p, double *sum_h, double *sum_ac)
{
    double h = 0.0, ac = 0.0;
    int i;

    for (i = p->cursor; i < p->count; i++)
    {
        const H264E_2pass_record_t *r = &p->records[i];
        h += r->h;
        ac += p->a[tp_class_index(r)] * r->c;
    }
    *sum_h = h;
    *sum_ac = ac;
}

int H264E_2pass_planner_next(
    H264E_2pass_planner_t *p, H264E_2pass_plan_t *plan)
{
    double sum_h, sum_ac, limit_bits;
    int q, chosen = -1, saturated = 0;

    if (!p)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    if (p->cursor >= p->count)
    {
        return H264E_2PASS_ERR_MISSING;   /* input has more frames than records */
    }
    if (p->plan_valid)
    {
        if (plan)
        {
            *plan = p->plan;
        }
        return H264E_2PASS_OK;
    }

    tp_planner_sums(p, &sum_h, &sum_ac);
    limit_bits = p->remaining_bytes * 8.0;
    for (q = p->qp_min; q <= p->qp_max; q++)
    {
        double predicted = sum_h + sum_ac / H264E_2pass_qscale(q);
        if (predicted <= limit_bits)
        {
            chosen = q;
            break;
        }
    }
    if (chosen < 0)
    {
        chosen = p->qp_max;
        saturated = 1;
    }

    p->plan.record_index = p->cursor;
    p->plan.q = chosen;
    p->plan.saturated = saturated;
    p->plan.remaining_bytes = p->remaining_bytes;
    p->plan.predicted_bits =
        sum_h + sum_ac / H264E_2pass_qscale(chosen);
    p->plan.frame_predicted_bits =
        p->records[p->cursor].h +
        p->a[tp_class_index(&p->records[p->cursor])] *
        p->records[p->cursor].c / H264E_2pass_qscale(chosen);
    p->plan.a_i = p->a[1];
    p->plan.a_p = p->a[0];
    p->plan_valid = 1;
    if (plan)
    {
        *plan = p->plan;
    }
    return H264E_2PASS_OK;
}

int H264E_2pass_planner_commit(
    H264E_2pass_planner_t *p, long actual_au_bytes)
{
    const H264E_2pass_record_t *r;
    double s, actual_bits, a_old, a_new;
    int cls;

    if (!p || actual_au_bytes <= 0)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    if (p->cursor >= p->count)
    {
        return H264E_2PASS_ERR_STATE;
    }
    if (!p->plan_valid)
    {
        return H264E_2PASS_ERR_STATE;
    }
    r = &p->records[p->cursor];
    cls = tp_class_index(r);
    s = H264E_2pass_qscale(p->plan.q);
    actual_bits = (double)actual_au_bytes * 8.0;

    p->diag.record_index = p->cursor;
    p->diag.q = p->plan.q;
    p->diag.saturated = p->plan.saturated;
    p->diag.predicted_bits = p->plan.predicted_bits;
    p->diag.frame_predicted_bits = p->plan.frame_predicted_bits;
    p->diag.actual_bits = actual_au_bytes * 8l;
    p->diag.committed = 1;

    p->remaining_bytes -= (double)actual_au_bytes;

    if (r->c > 0.0)
    {
        a_old = p->a[cls];
        a_new = (a_old + ((actual_bits - r->h > 0.0 ? actual_bits - r->h : 0.0)
            * s / r->c)) / 2.0;
        p->a[cls] = tp_clamp_double(a_new, 0.25, 4.0);
    }

    p->diag.remaining_bytes = p->remaining_bytes;
    p->diag.a_i = p->a[1];
    p->diag.a_p = p->a[0];
    p->diag_valid = 1;
    p->cursor++;
    p->plan_valid = 0;
    return H264E_2PASS_OK;
}

int H264E_2pass_planner_abort(H264E_2pass_planner_t *p)
{
    if (!p)
    {
        return H264E_2PASS_ERR_ARGUMENT;
    }
    p->plan_valid = 0;
    return H264E_2PASS_OK;
}

int H264E_2pass_planner_done(const H264E_2pass_planner_t *p)
{
    return p && p->cursor >= p->count;
}

int H264E_2pass_planner_frames(const H264E_2pass_planner_t *p)
{
    return p ? p->count : 0;
}

int H264E_2pass_planner_remaining(const H264E_2pass_planner_t *p)
{
    return p ? p->count - p->cursor : 0;
}

int H264E_2pass_planner_diag(
    const H264E_2pass_planner_t *p, H264E_2pass_diag_t *diag)
{
    if (!p || !diag || !p->diag_valid)
    {
        return H264E_2PASS_ERR_STATE;
    }
    *diag = p->diag;
    return H264E_2PASS_OK;
}

void H264E_2pass_planner_close(H264E_2pass_planner_t *p)
{
    if (!p)
    {
        return;
    }
    free(p->records);
    free(p);
}
