/*
    Tests for multiple AVC sample descriptions on a single track.

    The muxer side registers several AVCDecoderConfigurationRecords for one
    'avc1' track and maps every sample to the configuration that was selected
    when the sample was submitted. The demuxer side must expose each of those
    descriptions and answer which one a given sample uses.
*/
#include "minimp4.h"
#include "task_support/case_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define FT(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *data;
    size_t size;
    size_t capacity;
} mem_file_t;

static int mem_write(int64_t offset, const void *buffer, size_t size, void *token)
{
    mem_file_t *f = (mem_file_t *)token;
    if (offset < 0)
        return 1;
    if ((size_t)offset + size > f->capacity)
    {
        size_t cap = (size_t)offset + size + 4096;
        unsigned char *p = (unsigned char *)realloc(f->data, cap);
        if (!p)
            return 1;
        if (cap > f->size)
            memset(p + f->size, 0, cap - f->size);
        f->data = p;
        f->capacity = cap;
    }
    memcpy(f->data + (size_t)offset, buffer, size);
    if ((size_t)offset + size > f->size)
        f->size = (size_t)offset + size;
    return 0;
}

static void mem_file_reset(mem_file_t *f)
{
    free(f->data);
    memset(f, 0, sizeof(*f));
}

typedef struct
{
    const unsigned char *data;
    size_t size;
} mem_input_t;

static int mem_read(int64_t offset, void *buffer, size_t size, void *token)
{
    mem_input_t *in = (mem_input_t *)token;
    if (offset < 0 || (uint64_t)offset > in->size || size > in->size - (size_t)offset)
        return 1;
    memcpy(buffer, in->data + (size_t)offset, size);
    return 0;
}

static unsigned be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* Build one AVCDecoderConfigurationRecord.
   Returns its size in bytes. */
static int build_avcc(unsigned char *dst, const unsigned char *sps, unsigned sps_len,
                      const unsigned char *pps, unsigned pps_len,
                      const unsigned char *ext, unsigned ext_len)
{
    unsigned n = 0;
    dst[n++] = 1;                       /* configurationVersion */
    dst[n++] = 0x64;                    /* AVCProfileIndication */
    dst[n++] = 0x00;                    /* profile_compatibility */
    dst[n++] = 0x1f;                    /* AVCLevelIndication */
    dst[n++] = 0xff;                    /* reserved | lengthSizeMinusOne = 3 */
    dst[n++] = 0xe1;                    /* reserved | numOfSequenceParameterSets = 1 */
    dst[n++] = (unsigned char)(sps_len >> 8);
    dst[n++] = (unsigned char)sps_len;
    memcpy(dst + n, sps, sps_len);
    n += sps_len;
    dst[n++] = 1;                       /* numOfPictureParameterSets */
    dst[n++] = (unsigned char)(pps_len >> 8);
    dst[n++] = (unsigned char)pps_len;
    memcpy(dst + n, pps, pps_len);
    n += pps_len;
    if (ext_len)
    {
        memcpy(dst + n, ext, ext_len);
        n += ext_len;
    }
    return (int)n;
}

static const unsigned char sps_a[] = { 0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0xa0 };
static const unsigned char pps_a[] = { 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0 };
static const unsigned char sps_b[] = { 0x67, 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0xb0, 0x11 };
static const unsigned char pps_b[] = { 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc1, 0x33 };
static const unsigned char sps_c[] = { 0x67, 0x4d, 0x00, 0x1e, 0x9a, 0x66, 0x0a };
static const unsigned char pps_c[] = { 0x68, 0xce, 0x06, 0xe2 };

typedef struct
{
    unsigned char avcc[128];
    int avcc_bytes;
    unsigned width;
    unsigned height;
} description_fixture_t;

static void make_fixture(description_fixture_t *f, unsigned id)
{
    switch (id)
    {
    case 0:
        f->avcc_bytes = build_avcc(f->avcc, sps_a, sizeof(sps_a), pps_a, sizeof(pps_a), NULL, 0);
        f->width = 320;
        f->height = 240;
        break;
    case 1:
        f->avcc_bytes = build_avcc(f->avcc, sps_b, sizeof(sps_b), pps_b, sizeof(pps_b),
                                   (const unsigned char *)"\xde\xad\xbe\xef", 4);
        f->width = 640;
        f->height = 360;
        break;
    default:
        f->avcc_bytes = build_avcc(f->avcc, sps_c, sizeof(sps_c), pps_c, sizeof(pps_c), NULL, 0);
        f->width = 800;
        f->height = 600;
        break;
    }
}

static MP4E_track_t make_video_track(unsigned width, unsigned height)
{
    MP4E_track_t t;
    memset(&t, 0, sizeof(t));
    t.object_type_indication = MP4_OBJECT_TYPE_AVC;
    t.language[0] = 'u';
    t.language[1] = 'n';
    t.language[2] = 'd';
    t.track_media_kind = e_video;
    t.time_scale = 90000;
    t.default_duration = 3000;
    t.u.v.width = (int)width;
    t.u.v.height = (int)height;
    return t;
}

static MP4E_track_t make_audio_track(void)
{
    MP4E_track_t t;
    memset(&t, 0, sizeof(t));
    t.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    t.language[0] = 'u';
    t.language[1] = 'n';
    t.language[2] = 'd';
    t.track_media_kind = e_audio;
    t.time_scale = 48000;
    t.default_duration = 1024;
    t.u.a.channelcount = 2;
    return t;
}

/* Fill a payload with a recognisable pattern. */
static void fill_payload(unsigned char *p, unsigned n, unsigned seed)
{
    unsigned i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char)((seed * 31 + i * 7 + 3) & 0xff);
}

/* Box scanner over a raw buffer: find a top-level box by name.
   Returns a pointer to the box payload, or NULL. */
static const unsigned char *find_box(const unsigned char *p, size_t size, unsigned name,
                                     size_t *payload_size)
{
    size_t pos = 0;
    while (pos + 8 <= size)
    {
        uint64_t box_size = be32(p + pos);
        unsigned box_name = be32(p + pos + 4);
        uint64_t header = 8;
        if (box_size == 1)
        {
            if (pos + 16 > size)
                return NULL;
            box_size = ((uint64_t)be32(p + pos + 8) << 32) | be32(p + pos + 12);
            header = 16;
        }
        if (box_size < header || pos + box_size > size)
            return NULL;
        if (box_name == name)
        {
            *payload_size = (size_t)(box_size - header);
            return p + pos + header;
        }
        pos += (size_t)box_size;
    }
    return NULL;
}

static int is_container(unsigned name)
{
    return name == FT('m','o','o','v') || name == FT('t','r','a','k') ||
           name == FT('m','d','i','a') || name == FT('m','i','n','f') ||
           name == FT('s','t','b','l') || name == FT('s','t','s','d') ||
           name == FT('a','v','c','1') || name == FT('e','d','t','s') ||
           name == FT('d','i','n','f');
}

/* Recursive descent: find a box anywhere inside an already-located payload.
   Returns a pointer to the box payload, or NULL. */
static const unsigned char *find_box_nested(const unsigned char *p, size_t size, unsigned name,
                                            size_t *payload_size)
{
    size_t pos = 0;
    while (pos + 8 <= size)
    {
        uint64_t box_size = be32(p + pos);
        unsigned box_name = be32(p + pos + 4);
        uint64_t header = 8;
        if (box_size == 1)
        {
            if (pos + 16 > size)
                return NULL;
            box_size = ((uint64_t)be32(p + pos + 8) << 32) | be32(p + pos + 12);
            header = 16;
        }
        if (box_size < header || pos + box_size > size)
            return NULL;
        if (box_name == name)
        {
            *payload_size = (size_t)(box_size - header);
            return p + pos + header;
        }
        if (is_container(box_name))
        {
            const unsigned char *found = find_box_nested(p + pos + header, (size_t)(box_size - header),
                                                         name, payload_size);
            if (found)
                return found;
        }
        pos += (size_t)box_size;
    }
    return NULL;
}

/* Independent structural reader of the written file. */
typedef struct
{
    unsigned entry_count;
    struct
    {
        unsigned width;
        unsigned height;
        unsigned avcc_bytes;
        unsigned char avcc[256];
    } entries[8];
} parsed_stsd_t;

typedef struct
{
    unsigned entry_count;
    struct
    {
        unsigned first_chunk;
        unsigned samples_per_chunk;
        unsigned description_index;
    } entries[64];
} parsed_stsc_t;

static int parse_stsd(const unsigned char *file, size_t size, parsed_stsd_t *out)
{
    const unsigned char *p;
    size_t n;
    unsigned i;
    memset(out, 0, sizeof(*out));
    p = find_box_nested(file, size, FT('s','t','s','d'), &n);
    if (!p || n < 8)
        return 0;
    p += 4;   /* version + flags */
    n -= 4;
    out->entry_count = be32(p);
    p += 4;
    n -= 4;
    for (i = 0; i < out->entry_count && i < 8; i++)
    {
        uint64_t box_size;
        unsigned box_name;
        const unsigned char *body;
        size_t body_bytes;
        const unsigned char *avcc;
        size_t avcc_bytes;
        if (n < 8)
            return 0;
        box_size = be32(p);
        box_name = be32(p + 4);
        if (box_size < 8 || box_size > n)
            return 0;
        if (box_name != FT('a','v','c','1'))
            return 0;
        body = p + 8;
        body_bytes = (size_t)(box_size - 8);
        if (body_bytes < 78)
            return 0;
        /* VisualSampleEntry: width/height at offset 6+2+2+2+4*3 = 24 */
        out->entries[i].width = be16(body + 24);
        out->entries[i].height = be16(body + 26);
        avcc = find_box_nested(body + 78, body_bytes - 78, FT('a','v','c','C'), &avcc_bytes);
        if (!avcc)
            return 0;
        if (avcc_bytes > sizeof(out->entries[i].avcc))
            return 0;
        out->entries[i].avcc_bytes = (unsigned)avcc_bytes;
        memcpy(out->entries[i].avcc, avcc, avcc_bytes);
        p += box_size;
        n -= (size_t)box_size;
    }
    return 1;
}

static int parse_stsc(const unsigned char *file, size_t size, parsed_stsc_t *out)
{
    const unsigned char *p;
    size_t n;
    unsigned i;
    memset(out, 0, sizeof(*out));
    p = find_box_nested(file, size, FT('s','t','s','c'), &n);
    if (!p || n < 8)
        return 0;
    p += 4;   /* version + flags */
    n -= 4;
    out->entry_count = be32(p);
    p += 4;
    n -= 4;
    if (out->entry_count > 64)
        return 0;
    if (out->entry_count * 12 > n)
        return 0;
    for (i = 0; i < out->entry_count; i++)
    {
        out->entries[i].first_chunk = be32(p);
        out->entries[i].samples_per_chunk = be32(p + 4);
        out->entries[i].description_index = be32(p + 8);
        p += 12;
    }
    return 1;
}

/* Parse the file with the public demuxer and verify the mapping. */
static int check_demux_mapping(mem_file_t *out, unsigned description_count,
                               const description_fixture_t *fixtures,
                               const unsigned *expected, unsigned expected_samples)
{
    MP4D_demux_t mp4;
    mem_input_t in;
    unsigned i;
    int ok = 0;
    in.data = out->data;
    in.size = out->size;
    memset(&mp4, 0, sizeof(mp4));
    if (!MP4D_open(&mp4, mem_read, &in, (int64_t)in.size))
        return 0;
    if (mp4.track_count != 1 || mp4.track[0].sample_count != expected_samples)
        goto done;
    for (i = 0; i < description_count; i++)
    {
        const MP4D_avc_description_t *d = MP4D_avc_description(&mp4, 0, i + 1);
        if (!d || d->width != fixtures[i].width || d->height != fixtures[i].height)
            goto done;
        if (d->avcc_bytes != (unsigned)fixtures[i].avcc_bytes ||
            memcmp(d->avcc, fixtures[i].avcc, d->avcc_bytes))
            goto done;
    }
    if (MP4D_avc_description(&mp4, 0, description_count + 1))
        goto done;
    if (MP4D_avc_description(&mp4, 0, 0))
        goto done;
    for (i = 0; i < expected_samples; i++)
    {
        unsigned idx = MP4D_sample_description_index(&mp4, 0, i);
        if (idx != expected[i])
            goto done;
    }
    ok = 1;
done:
    MP4D_close(&mp4);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Muxing                                                            */
/* ------------------------------------------------------------------ */

void test_description_mux(void)
{
    mem_file_t out;
    MP4E_mux_t *mux;
    MP4E_track_t vt;
    description_fixture_t fx[3];
    unsigned char payload[64];
    unsigned i;
    static const unsigned schedule[] = { 1, 1, 2, 2, 2, 1, 3, 3, 1, 2 };
    parsed_stsd_t stsd;
    parsed_stsc_t stsc;
    unsigned expected[ARRAY_SIZE(schedule)];

    CHECK_CASE();
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 3; i++)
        make_fixture(&fx[i], i);

    mux = MP4E_open(0, 0, &out, mem_write);
    UNIT_CHECK(mux != NULL);
    vt = make_video_track(fx[0].width, fx[0].height);
    UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
    UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[0].width, fx[0].height, fx[0].avcc, fx[0].avcc_bytes) == 1);
    UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[1].width, fx[1].height, fx[1].avcc, fx[1].avcc_bytes) == 2);
    UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[2].width, fx[2].height, fx[2].avcc, fx[2].avcc_bytes) == 3);
    /* Registration alone must not switch the selected description. */
    UNIT_CHECK(MP4E_select_avc_description(mux, 0, 1) == MP4E_STATUS_OK);

    for (i = 0; i < ARRAY_SIZE(schedule); i++)
    {
        if (schedule[i] != (i ? schedule[i - 1] : 1))
            UNIT_CHECK(MP4E_select_avc_description(mux, 0, schedule[i]) == MP4E_STATUS_OK);
        fill_payload(payload, sizeof(payload), i);
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, (int)sizeof(payload), 3000,
                                   MP4E_SAMPLE_RANDOM_ACCESS) == MP4E_STATUS_OK);
        expected[i] = schedule[i];
    }
    UNIT_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    UNIT_CHECK(check_demux_mapping(&out, 3, fx, expected, ARRAY_SIZE(schedule)));

    /* Independently parse stsd: one entry per registration, own size/avcC. */
    UNIT_CHECK(parse_stsd(out.data, out.size, &stsd));
    UNIT_CHECK(stsd.entry_count == 3);
    for (i = 0; i < 3; i++)
    {
        UNIT_CHECK(stsd.entries[i].width == fx[i].width);
        UNIT_CHECK(stsd.entries[i].height == fx[i].height);
        UNIT_CHECK(stsd.entries[i].avcc_bytes == (unsigned)fx[i].avcc_bytes);
        UNIT_CHECK(!memcmp(stsd.entries[i].avcc, fx[i].avcc, fx[i].avcc_bytes));
    }

    /* Independently parse stsc: one sample per chunk, runs merged. */
    UNIT_CHECK(parse_stsc(out.data, out.size, &stsc));
    {
        unsigned run = 0, saw[64];
        unsigned runs[ARRAY_SIZE(schedule)];
        unsigned run_desc[ARRAY_SIZE(schedule)];
        unsigned run_chunks[ARRAY_SIZE(schedule)];
        memset(saw, 0, sizeof(saw));
        for (i = 0; i < ARRAY_SIZE(schedule); i++)
        {
            if (i == 0 || schedule[i] != schedule[i - 1])
            {
                runs[run] = i + 1;
                run_chunks[run] = 1;
                run_desc[run] = schedule[i];
                run++;
            } else
            {
                run_chunks[run - 1]++;
            }
        }
        UNIT_CHECK(stsc.entry_count == run);
        /* Each stsc entry starts at the run's first chunk, covers it with
           one sample per chunk, and names the run's description. */
        for (i = 0; i < run && i < stsc.entry_count; i++)
        {
            unsigned c, next = (i + 1 < stsc.entry_count) ? stsc.entries[i + 1].first_chunk : ARRAY_SIZE(schedule) + 1;
            UNIT_CHECK(stsc.entries[i].first_chunk == runs[i]);
            UNIT_CHECK(stsc.entries[i].samples_per_chunk == 1);
            UNIT_CHECK(stsc.entries[i].description_index == run_desc[i]);
            for (c = stsc.entries[i].first_chunk; c < next; c++)
                saw[c - 1] = 1;
        }
        for (i = 0; i < ARRAY_SIZE(schedule); i++)
            UNIT_CHECK(saw[i] == 1);
        (void)run_chunks;
    }

    /* mdat must contain every payload verbatim, in order. */
    {
        size_t n;
        const unsigned char *mdat = find_box(out.data, out.size, FT('m','d','a','t'), &n);
        unsigned char expect[64 * ARRAY_SIZE(schedule)];
        UNIT_CHECK(mdat != NULL);
        for (i = 0; i < ARRAY_SIZE(schedule); i++)
            fill_payload(expect + i * sizeof(payload), sizeof(payload), i);
        UNIT_CHECK(n == sizeof(expect));
        UNIT_CHECK(!memcmp(mdat, expect, sizeof(expect)));
    }

    mem_file_reset(&out);

    /* Sequential mode produces the same description mapping. */
    {
        static const unsigned schedule2[] = { 2, 2, 1, 1, 1, 2 };
        unsigned expected2[ARRAY_SIZE(schedule2)];
        mem_input_t in;
        MP4D_demux_t mp4;
        parsed_stsc_t stsc2;
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(1, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(fx[0].width, fx[0].height);
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[0].width, fx[0].height, fx[0].avcc, fx[0].avcc_bytes) == 1);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[1].width, fx[1].height, fx[1].avcc, fx[1].avcc_bytes) == 2);
        for (i = 0; i < ARRAY_SIZE(schedule2); i++)
        {
            if (i == 0 || schedule2[i] != schedule2[i - 1])
                UNIT_CHECK(MP4E_select_avc_description(mux, 0, schedule2[i]) == 0);
            fill_payload(payload, sizeof(payload), 50 + i);
            UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);
            expected2[i] = schedule2[i];
        }
        UNIT_CHECK(MP4E_close(mux) == 0);
        UNIT_CHECK(check_demux_mapping(&out, 2, fx, expected2, ARRAY_SIZE(schedule2)));
        UNIT_CHECK(parse_stsc(out.data, out.size, &stsc2));
        UNIT_CHECK(stsc2.entry_count == 3);
        UNIT_CHECK(stsc2.entries[0].first_chunk == 1 && stsc2.entries[0].description_index == 2);
        UNIT_CHECK(stsc2.entries[1].first_chunk == 3 && stsc2.entries[1].description_index == 1);
        UNIT_CHECK(stsc2.entries[2].first_chunk == 6 && stsc2.entries[2].description_index == 2);
        in.data = out.data;
        in.size = out.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        {
            unsigned k;
            for (k = 0; k < ARRAY_SIZE(schedule2); k++)
            {
                unsigned bytes = 0, ts = 0, dur = 0, q;
                unsigned char pat[64];
                MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, k, &bytes, &ts, &dur);
                fill_payload(pat, sizeof(pat), 50 + k);
                UNIT_CHECK(bytes == sizeof(payload));
                for (q = 0; q < sizeof(payload); q++)
                    UNIT_CHECK(out.data[off + q] == pat[q]);
            }
        }
        MP4D_close(&mp4);
        mem_file_reset(&out);
    }

    /* The caller may reuse the avcc buffer after registration: each entry
       must own a deep copy. */
    {
        unsigned char scratch[128];
        description_fixture_t f0;
        mem_input_t in;
        MP4D_demux_t mp4;
        CHECK_CASE();
        make_fixture(&f0, 0);
        memcpy(scratch, f0.avcc, f0.avcc_bytes);
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(0, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(f0.width, f0.height);
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, f0.width, f0.height, scratch, f0.avcc_bytes) == 1);
        memset(scratch, 0xa5, sizeof(scratch));   /* caller clobbers its buffer */
        fill_payload(payload, sizeof(payload), 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);
        UNIT_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        in.data = out.data;
        in.size = out.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        {
            const MP4D_avc_description_t *d = MP4D_avc_description(&mp4, 0, 1);
            UNIT_CHECK(d != NULL);
            UNIT_CHECK(d->avcc_bytes == (unsigned)f0.avcc_bytes);
            UNIT_CHECK(!memcmp(d->avcc, f0.avcc, f0.avcc_bytes));
        }
        MP4D_close(&mp4);
        mem_file_reset(&out);
    }
}

/* Minimal box writer used to build a file by hand. */
typedef struct
{
    unsigned char *data;
    size_t size;
    size_t cap;
} write_buf_t;

static void wb_bytes(write_buf_t *w, const void *p, size_t n)
{
    if (w->size + n > w->cap)
    {
        size_t cap = w->cap ? w->cap * 2 : 4096;
        while (cap < w->size + n)
            cap *= 2;
        w->data = (unsigned char *)realloc(w->data, cap);
        w->cap = cap;
    }
    memcpy(w->data + w->size, p, n);
    w->size += n;
}

static void wb_be32(write_buf_t *w, unsigned v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
    wb_bytes(w, b, 4);
}

static void wb_be16(write_buf_t *w, unsigned v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)v;
    wb_bytes(w, b, 2);
}

static size_t wb_begin(write_buf_t *w, unsigned name)
{
    size_t at = w->size;
    wb_be32(w, 0);
    wb_be32(w, name);
    return at;
}

static void wb_end(write_buf_t *w, size_t at)
{
    unsigned size = (unsigned)(w->size - at);
    w->data[at + 0] = (unsigned char)(size >> 24);
    w->data[at + 1] = (unsigned char)(size >> 16);
    w->data[at + 2] = (unsigned char)(size >> 8);
    w->data[at + 3] = (unsigned char)size;
}

/* Write an avc1 sample entry with its avcC child. */
static void wb_avc1(write_buf_t *w, unsigned width, unsigned height,
                    const unsigned char *avcc, unsigned avcc_bytes)
{
    size_t at = wb_begin(w, FT('a','v','c','1'));
    unsigned i;
    wb_bytes(w, "\0\0\0\0\0\0", 6);   /* reserved */
    wb_be16(w, 1);                    /* data_reference_index */
    wb_be16(w, 0);                    /* pre_defined */
    wb_be16(w, 0);                    /* reserved */
    wb_be32(w, 0);
    wb_be32(w, 0);
    wb_be32(w, 0);
    wb_be16(w, width);
    wb_be16(w, height);
    wb_be32(w, 0x00480000);
    wb_be32(w, 0x00480000);
    wb_be32(w, 0);
    wb_be16(w, 1);
    for (i = 0; i < 32; i++)
        wb_bytes(w, "\0", 1);
    wb_be16(w, 24);
    wb_be16(w, 0xffff);
    {
        size_t avcc_at = wb_begin(w, FT('a','v','c','C'));
        wb_bytes(w, avcc, avcc_bytes);
        wb_end(w, avcc_at);
    }
    wb_end(w, at);
}

typedef struct
{
    unsigned first_chunk;
    unsigned samples_per_chunk;
    unsigned description_index;
} stsc_entry_t;

/* Build a complete single-video-track MP4 file by hand.
   Samples are given as sizes and laid out contiguously in one mdat; the stsc
   entries group them into chunks (samples_per_chunk samples per chunk).
   forced_chunk_count != 0 fixes the chunk table size independently of stsc
   (used to craft inconsistent mappings).
   stsd_first rotates which fixture is written first in stsd;
   stsc_first_flag places stsc before stsd in stbl. */
static void build_handmade_file(write_buf_t *w,
                                const description_fixture_t *fx, unsigned ndesc,
                                const stsc_entry_t *stsc, unsigned stsc_count,
                                const unsigned *sizes, unsigned nsamples,
                                unsigned stsd_first, int stsc_first_flag,
                                unsigned forced_chunk_count)
{
    unsigned char ftyp[] = { 0,0,0,0x18,'f','t','y','p','m','p','4','2',0,0,0,0,'m','p','4','2','i','s','o','m' };
    size_t i, d;
    size_t mdat_at, moov_at;
    unsigned chunk_count;
    unsigned *chunk_offset;

    /* Number of chunks: the last stsc entry starts at first_chunk and spans
       as many chunks as the trailing samples need. */
    if (stsc_count)
    {
        unsigned remaining = nsamples, c;
        for (c = 0; c + 1 < stsc_count; c++)
            remaining -= (stsc[c + 1].first_chunk - stsc[c].first_chunk) * stsc[c].samples_per_chunk;
        chunk_count = stsc[stsc_count - 1].first_chunk - 1;
        if (stsc[stsc_count - 1].samples_per_chunk)
            chunk_count += (remaining + stsc[stsc_count - 1].samples_per_chunk - 1) /
                           stsc[stsc_count - 1].samples_per_chunk;
    } else
    {
        chunk_count = 0;
    }
    if (forced_chunk_count)
        chunk_count = forced_chunk_count;
    chunk_offset = (unsigned *)calloc(chunk_count ? chunk_count : 1, sizeof(unsigned));

    wb_bytes(w, ftyp, sizeof(ftyp));

    /* mdat with one byte-pattern per sample, grouped into chunks */
    mdat_at = wb_begin(w, FT('m','d','a','t'));
    {
        unsigned c, sample = 0;
        for (c = 0; c < chunk_count; c++)
        {
            unsigned g = 0, k, s;
            for (k = 0; k < stsc_count; k++)
                if (stsc[k].first_chunk <= c + 1)
                    g = k;
            chunk_offset[c] = (unsigned)w->size;
            for (s = 0; s < stsc[g].samples_per_chunk && sample < nsamples; s++, sample++)
            {
                unsigned char pattern[64];
                unsigned q;
                for (q = 0; q < sizes[sample]; q++)
                    pattern[q] = (unsigned char)((sample * 13 + q * 3 + 1) & 0xff);
                wb_bytes(w, pattern, sizes[sample]);
            }
        }
    }
    wb_end(w, mdat_at);

    moov_at = wb_begin(w, FT('m','o','o','v'));
    {
        /* mvhd */
        size_t at = wb_begin(w, FT('m','v','h','d'));
        wb_be32(w, 0);
        wb_be32(w, 0); wb_be32(w, 0);
        wb_be32(w, 1000);
        wb_be32(w, 1000);
        wb_be32(w, 0x00010000);
        wb_be16(w, 0x0100); wb_be16(w, 0);
        wb_be32(w, 0); wb_be32(w, 0);
        wb_be32(w, 0x00010000); wb_be32(w, 0); wb_be32(w, 0);
        wb_be32(w, 0); wb_be32(w, 0x00010000); wb_be32(w, 0);
        wb_be32(w, 0); wb_be32(w, 0); wb_be32(w, 0x40000000);
        for (i = 0; i < 6; i++) wb_be32(w, 0);
        wb_be32(w, 2);
        wb_end(w, at);
    }
    {
        size_t trak_at = wb_begin(w, FT('t','r','a','k'));
        /* tkhd */
        {
            size_t at = wb_begin(w, FT('t','k','h','d'));
            wb_be32(w, 7);
            wb_be32(w, 0); wb_be32(w, 0);
            wb_be32(w, 1); wb_be32(w, 0);
            wb_be32(w, 1000);
            wb_be32(w, 0); wb_be32(w, 0);
            wb_be16(w, 0); wb_be16(w, 0);
            wb_be16(w, 0); wb_be16(w, 0);
            wb_be32(w, 0x00010000); wb_be32(w, 0); wb_be32(w, 0);
            wb_be32(w, 0); wb_be32(w, 0x00010000); wb_be32(w, 0);
            wb_be32(w, 0); wb_be32(w, 0); wb_be32(w, 0x40000000);
            wb_be32(w, fx[0].width << 16);
            wb_be32(w, fx[0].height << 16);
            wb_end(w, at);
        }
        {
            size_t mdia_at = wb_begin(w, FT('m','d','i','a'));
            {
                size_t at = wb_begin(w, FT('m','d','h','d'));
                wb_be32(w, 0);
                wb_be32(w, 0); wb_be32(w, 0);
                wb_be32(w, 90000);
                wb_be32(w, 90000);
                wb_be16(w, 0x55c4);
                wb_be16(w, 0);
                wb_end(w, at);
            }
            {
                size_t at = wb_begin(w, FT('h','d','l','r'));
                wb_be32(w, 0);
                wb_be32(w, FT('v','i','d','e'));
                wb_be32(w, 0); wb_be32(w, 0); wb_be32(w, 0);
                wb_bytes(w, "VideoHandler\0", 13);
                wb_end(w, at);
            }
            {
                size_t minf_at = wb_begin(w, FT('m','i','n','f'));
                {
                    size_t at = wb_begin(w, FT('v','m','h','d'));
                    wb_be32(w, 1);
                    wb_be16(w, 0); wb_be16(w, 0); wb_be16(w, 0); wb_be16(w, 0);
                    wb_end(w, at);
                }
                {
                    size_t dinf_at = wb_begin(w, FT('d','i','n','f'));
                    size_t dref_at = wb_begin(w, FT('d','r','e','f'));
                    wb_be32(w, 0);
                    wb_be32(w, 1);
                    {
                        size_t url_at = wb_begin(w, FT('u','r','l',' '));
                        wb_be32(w, 1);
                        wb_end(w, url_at);
                    }
                    wb_end(w, dref_at);
                    wb_end(w, dinf_at);
                }
                {
                    size_t stbl_at = wb_begin(w, FT('s','t','b','l'));
                    unsigned stage;
                    for (stage = 0; stage < 2; stage++)
                    {
                        unsigned stsc_stage = stsc_first_flag ? 0 : 1;
                        if (stage == stsc_stage)
                        {
                            size_t at = wb_begin(w, FT('s','t','s','c'));
                            wb_be32(w, 0);
                            wb_be32(w, stsc_count);
                            for (i = 0; i < stsc_count; i++)
                            {
                                wb_be32(w, stsc[i].first_chunk);
                                wb_be32(w, stsc[i].samples_per_chunk);
                                wb_be32(w, stsc[i].description_index);
                            }
                            wb_end(w, at);
                        } else
                        {
                            /* stsd must come before stts/stsz/stco */
                            size_t stsd_at = wb_begin(w, FT('s','t','s','d'));
                            wb_be32(w, 0);   /* version + flags */
                            wb_be32(w, ndesc);
                            for (d = 0; d < ndesc; d++)
                            {
                                unsigned idx = (d + stsd_first) % ndesc;
                                wb_avc1(w, fx[idx].width, fx[idx].height, fx[idx].avcc, (unsigned)fx[idx].avcc_bytes);
                            }
                            wb_end(w, stsd_at);
                            {
                                size_t at = wb_begin(w, FT('s','t','t','s'));
                                wb_be32(w, 0);
                                wb_be32(w, 1);
                                wb_be32(w, nsamples);
                                wb_be32(w, 3000);
                                wb_end(w, at);
                            }
                        }
                    }
                    {
                        size_t at = wb_begin(w, FT('s','t','s','z'));
                        wb_be32(w, 0);          /* version + flags */
                        wb_be32(w, 0);          /* sample_size: 0 = per-sample table */
                        wb_be32(w, nsamples);   /* sample_count */
                        for (i = 0; i < nsamples; i++)
                            wb_be32(w, sizes[i]);
                        wb_end(w, at);
                    }
                    {
                        size_t at = wb_begin(w, FT('s','t','c','o'));
                        wb_be32(w, 0);
                        wb_be32(w, chunk_count);
                        for (i = 0; i < chunk_count; i++)
                            wb_be32(w, chunk_offset[i]);
                        wb_end(w, at);
                    }
                    wb_end(w, stbl_at);
                }
                wb_end(w, minf_at);
            }
            wb_end(w, mdia_at);
        }
        wb_end(w, trak_at);
    }
    wb_end(w, moov_at);
    free(chunk_offset);
}

void test_description_demux(void)
{
    write_buf_t w;
    description_fixture_t fx[3];
    mem_input_t in;
    MP4D_demux_t mp4;
    unsigned i;

    CHECK_CASE();
    memset(&w, 0, sizeof(w));
    for (i = 0; i < 3; i++)
        make_fixture(&fx[i], i);

    /* Two descriptions, one chunk holding several samples. */
    {
        stsc_entry_t stsc[2];
        unsigned sizes[5] = { 10, 11, 12, 13, 14 };
        unsigned expected[5] = { 1, 1, 1, 2, 2 };
        CHECK_CASE();

        w.size = 0;
        stsc[0].first_chunk = 1; stsc[0].samples_per_chunk = 3; stsc[0].description_index = 1;
        stsc[1].first_chunk = 2; stsc[1].samples_per_chunk = 2; stsc[1].description_index = 2;
        build_handmade_file(&w, fx, 2, stsc, 2, sizes, 5, 0, 1, 0);

        in.data = w.data;
        in.size = w.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        UNIT_CHECK(mp4.track_count == 1);
        UNIT_CHECK(mp4.track[0].sample_count == 5);
        UNIT_CHECK(MP4D_avc_description(&mp4, 0, 1)->width == fx[0].width);
        UNIT_CHECK(MP4D_avc_description(&mp4, 0, 2)->width == fx[1].width);
        UNIT_CHECK(MP4D_avc_description(&mp4, 0, 3) == NULL);
        for (i = 0; i < 5; i++)
            UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, i) == expected[i]);
        /* Random access order must not matter. */
        for (i = 5; i-- > 0;)
            UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, i) == expected[i]);
        UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, 5) == 0);

        /* Frame offsets stay the same as before and agree with the mdat
           contents laid out by the builder. */
        {
            size_t mdat_n = 0;
            const unsigned char *mdat = find_box(w.data, w.size, FT('m','d','a','t'), &mdat_n);
            unsigned running = 0;
            UNIT_CHECK(mdat != NULL);
            for (i = 0; i < 5; i++)
            {
                unsigned bytes = 0, ts = 0, dur = 0;
                MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, i, &bytes, &ts, &dur);
                unsigned k;
                UNIT_CHECK(bytes == sizes[i]);
                UNIT_CHECK(dur == 3000);
                UNIT_CHECK((size_t)off >= (size_t)(mdat - w.data) && (size_t)off - (size_t)(mdat - w.data) == running);
                for (k = 0; k < bytes; k++)
                    UNIT_CHECK(w.data[off + k] == (unsigned char)((i * 13 + k * 3 + 1) & 0xff));
                running += bytes;
            }
            (void)mdat_n;
        }
        MP4D_close(&mp4);
    }

    /* Interleaved queries: alternate the two APIs and go backwards. */
    {
        stsc_entry_t stsc[3];
        unsigned sizes[6] = { 5, 5, 5, 5, 5, 5 };
        unsigned expected[6] = { 2, 2, 1, 1, 2, 2 };
        CHECK_CASE();

        w.size = 0;
        stsc[0].first_chunk = 1; stsc[0].samples_per_chunk = 1; stsc[0].description_index = 2;
        stsc[1].first_chunk = 3; stsc[1].samples_per_chunk = 1; stsc[1].description_index = 1;
        stsc[2].first_chunk = 5; stsc[2].samples_per_chunk = 1; stsc[2].description_index = 2;
        build_handmade_file(&w, fx, 2, stsc, 3, sizes, 6, 0, 0, 0);

        in.data = w.data;
        in.size = w.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        {
            unsigned order[6] = { 3, 0, 5, 1, 4, 2 };
            unsigned bytes, ts, dur;
            for (i = 0; i < 6; i++)
            {
                unsigned s = order[i];
                UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, s) == expected[s]);
                MP4D_frame_offset(&mp4, 0, s, &bytes, &ts, &dur);
                UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, s) == expected[s]);
            }
            /* Negative and cross-track queries. */
            UNIT_CHECK(MP4D_sample_description_index(&mp4, 1, 0) == 0);
            UNIT_CHECK(MP4D_avc_description(&mp4, 1, 1) == NULL);
        }
        MP4D_close(&mp4);
    }

    /* The legacy view (dsi, read_sps/pps, SampleDescription) is entry 1
       even when a query for another entry ran first. */
    {
        stsc_entry_t stsc[1];
        unsigned sizes[2] = { 7, 7 };
        const void *sps;
        int sps_bytes = 0;
        CHECK_CASE();
        w.size = 0;
        stsc[0].first_chunk = 1; stsc[0].samples_per_chunk = 2; stsc[0].description_index = 2;
        build_handmade_file(&w, fx, 2, stsc, 1, sizes, 2, 0, 0, 0);
        in.data = w.data;
        in.size = w.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        UNIT_CHECK(mp4.track[0].SampleDescription.video.width == fx[0].width);
        UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, 0) == 2);
        UNIT_CHECK(MP4D_avc_description(&mp4, 0, 2)->width == fx[1].width);
        sps = MP4D_read_sps(&mp4, 0, 0, &sps_bytes);
        UNIT_CHECK(sps && sps_bytes == (int)sizeof(sps_a) && !memcmp(sps, sps_a, sizeof(sps_a)));
        UNIT_CHECK(mp4.track[0].SampleDescription.video.width == fx[0].width);
        MP4D_close(&mp4);
    }

    /* Corrupted mappings must make the open fail rather than silently
       fall back to the first description. Each variant is a file whose
       chunk table is laid out consistently but whose stsc is wrong. */
    {
        stsc_entry_t stsc[2];
        unsigned sizes[4] = { 6, 6, 6, 6 };
        int variant;
        CHECK_CASE();
        for (variant = 0; variant < 7; variant++)
        {
            w.size = 0;
            stsc[0].first_chunk = 1; stsc[0].samples_per_chunk = 2; stsc[0].description_index = 1;
            stsc[1].first_chunk = 3; stsc[1].samples_per_chunk = 2; stsc[1].description_index = 2;
            switch (variant)
            {
            case 0: stsc[0].first_chunk = 2; break;              /* does not start at chunk 1 */
            case 1: stsc[1].first_chunk = 1; break;              /* not strictly increasing */
            case 2: stsc[0].samples_per_chunk = 0; break;        /* zero samples per chunk */
            case 3: stsc[1].description_index = 9; break;        /* index out of range */
            case 4: stsc[1].first_chunk = 6; break;              /* references past the last chunk */
            case 5: stsc[1].description_index = 0; break;        /* non-1-based index */
            case 6: stsc[0].first_chunk = 1; stsc[1].first_chunk = 3;
                    stsc[0].samples_per_chunk = 4; stsc[1].samples_per_chunk = 4; break; /* over-covers samples */
            }
            build_handmade_file(&w, fx, 2, stsc, 2, sizes, 4, 0, 0, 0);
            in.data = w.data;
            in.size = w.size;
            memset(&mp4, 0, sizeof(mp4));
            UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 0);
        }
    }

    /* A truncated description record must fail the open, not fall back. */
    {
        stsc_entry_t stsc[1];
        unsigned sizes[2] = { 8, 8 };
        CHECK_CASE();
        w.size = 0;
        stsc[0].first_chunk = 1; stsc[0].samples_per_chunk = 1; stsc[0].description_index = 1;
        build_handmade_file(&w, fx, 2, stsc, 1, sizes, 2, 0, 0, 0);
        /* Drop the tail of the file so the last avcC is truncated. */
        in.data = w.data;
        in.size = w.size - 12;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 0);
    }

    free(w.data);
}

/* ------------------------------------------------------------------ */
/*  Continuation behaviour                                            */
/* ------------------------------------------------------------------ */

static int write_video_file(mem_file_t *out, int sequential,
                            const description_fixture_t *fx, unsigned ndesc,
                            const unsigned *samples)
{
    MP4E_mux_t *mux;
    MP4E_track_t vt;
    unsigned char payload[40];
    unsigned i;
    mux = MP4E_open(sequential, 0, out, mem_write);
    if (!mux)
        return 0;
    vt = make_video_track(fx[0].width, fx[0].height);
    if (MP4E_add_track(mux, &vt) != 0)
        return 0;
    for (i = 0; i < ndesc; i++)
        if (MP4E_add_avc_description(mux, 0, fx[i].width, fx[i].height, fx[i].avcc, fx[i].avcc_bytes) != (int)(i + 1))
            return 0;
    for (i = 0; samples[i] != 0xffffffffu; i++)
    {
        unsigned current = i ? samples[i - 1] : 1;
        fill_payload(payload, sizeof(payload), i);
        if (samples[i] != current)
            if (MP4E_select_avc_description(mux, 0, samples[i]) != MP4E_STATUS_OK)
                return 0;
        if (MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_RANDOM_ACCESS) != MP4E_STATUS_OK)
            return 0;
    }
    return MP4E_close(mux) == MP4E_STATUS_OK;
}

void test_description_continuation(void)
{
    description_fixture_t fx[2];
    mem_file_t out;
    unsigned i;

    CHECK_CASE();
    for (i = 0; i < 2; i++)
        make_fixture(&fx[i], i);

    /* Continuations merge into the preceding sample; A/A/B/A switching keeps
       the mdat bytes and durations intact. */
    {
        static const unsigned samples[] = { 1, 1, 2, 1, 0xffffffffu };
        mem_input_t in;
        MP4D_demux_t mp4;
        static const unsigned expected_desc[] = { 1, 1, 2, 1 };
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        UNIT_CHECK(write_video_file(&out, 0, fx, 2, samples));
        in.data = out.data;
        in.size = out.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        UNIT_CHECK(mp4.track[0].sample_count == 4);
        for (i = 0; i < 4; i++)
            UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, i) == expected_desc[i]);
        MP4D_close(&mp4);
        mem_file_reset(&out);
    }

    /* A continuation submitted in sequential mode must stay attached to the
       pending sample it extends; registering a new description in between
       must not change its identity. */
    {
        MP4E_mux_t *mux;
        MP4E_track_t vt;
        unsigned char payload[16];
        mem_input_t in;
        MP4D_demux_t mp4;
        description_fixture_t fx3;
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        make_fixture(&fx3, 2);
        mux = MP4E_open(1, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(fx[0].width, fx[0].height);
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[0].width, fx[0].height, fx[0].avcc, fx[0].avcc_bytes) == 1);
        fill_payload(payload, sizeof(payload), 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);
        /* Register while the first sample is still pending. */
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[1].width, fx[1].height, fx[1].avcc, fx[1].avcc_bytes) == 2);
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 2) == 0);
        /* Select back to 1 so the pending sample can be extended. */
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 1) == 0);
        fill_payload(payload, sizeof(payload), 1);
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 0, MP4E_SAMPLE_CONTINUATION) == 0);
        /* A second sample switches configuration. */
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 2) == 0);
        fill_payload(payload, sizeof(payload), 2);
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);
        UNIT_CHECK(MP4E_close(mux) == 0);
        in.data = out.data;
        in.size = out.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        UNIT_CHECK(mp4.track[0].sample_count == 2);
        UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, 0) == 1);
        UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, 1) == 2);
        {
            unsigned bytes, ts, dur;
            unsigned k;
            MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, 0, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == 2 * sizeof(payload));
            for (k = 0; k < sizeof(payload); k++)
                UNIT_CHECK(out.data[off + k] == (unsigned char)((0 * 31 + k * 7 + 3) & 0xff));
            for (k = 0; k < sizeof(payload); k++)
                UNIT_CHECK(out.data[off + sizeof(payload) + k] == (unsigned char)((1 * 31 + k * 7 + 3) & 0xff));
        }
        MP4D_close(&mp4);
        mem_file_reset(&out);
    }

    /* Rejected continuations must not consume bytes, and writing may
       continue after the rejection. Checks:
         - no predecessor
         - changed configuration
         - another track's sample committed in between
       and that the rejected call left the track's pending state untouched. */
    {
        MP4E_mux_t *mux;
        MP4E_track_t vt, at;
        unsigned char payload[16], cont[8];
        description_fixture_t fx3;
        mem_input_t in;
        MP4D_demux_t mp4;
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        make_fixture(&fx3, 2);
        mux = MP4E_open(0, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(fx[0].width, fx[0].height);
        at = make_audio_track();
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_track(mux, &at) == 1);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[0].width, fx[0].height, fx[0].avcc, fx[0].avcc_bytes) == 1);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[1].width, fx[1].height, fx[1].avcc, fx[1].avcc_bytes) == 2);
        UNIT_CHECK(MP4E_set_dsi(mux, 1, "\x12\x10", 2) == 0);

        fill_payload(payload, sizeof(payload), 0);
        fill_payload(cont, sizeof(cont), 1);

        /* No predecessor. */
        UNIT_CHECK(MP4E_put_sample(mux, 0, cont, sizeof(cont), 0, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);

        /* First sample of a new description must be random access. */
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);

        /* Configuration changed; continuation is then rejected, and
           selecting the right configuration back lets writing resume. */
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 2) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, cont, sizeof(cont), 0, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 1) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, cont, sizeof(cont), 0, MP4E_SAMPLE_CONTINUATION) == 0);

        /* Another track's sample committed in between also rejects. */
        UNIT_CHECK(MP4E_put_sample(mux, 1, payload, 4, 1024, MP4E_SAMPLE_DEFAULT) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, cont, sizeof(cont), 0, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);

        /* A fresh frame (same configuration) re-establishes the
           predecessor, and a continuation on it is accepted again. */
        UNIT_CHECK(MP4E_put_sample(mux, 0, payload, sizeof(payload), 3000, MP4E_SAMPLE_DEFAULT) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, cont, sizeof(cont), 0, MP4E_SAMPLE_CONTINUATION) == 0);
        UNIT_CHECK(MP4E_close(mux) == 0);

        in.data = out.data;
        in.size = out.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        UNIT_CHECK(mp4.track_count == 2);
        UNIT_CHECK(mp4.track[0].sample_count == 2);
        {
            unsigned bytes = 0, ts = 0, dur = 0;
            MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, 0, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == sizeof(payload) + sizeof(cont));
            UNIT_CHECK(out.data[off + sizeof(payload)] == cont[0]);
            off = MP4D_frame_offset(&mp4, 0, 1, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == sizeof(payload) + sizeof(cont));
            UNIT_CHECK(out.data[off + sizeof(payload)] == cont[0]);
        }
        MP4D_close(&mp4);
        mem_file_reset(&out);
    }

    /* Two interleaved tracks keep their own byte streams and durations. */
    {
        MP4E_mux_t *mux;
        MP4E_track_t vt, at;
        unsigned char v1[20], v2[20], a1[12], a2[12];
        mem_input_t in;
        MP4D_demux_t mp4;
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(0, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(fx[0].width, fx[0].height);
        at = make_audio_track();
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_track(mux, &at) == 1);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[0].width, fx[0].height, fx[0].avcc, fx[0].avcc_bytes) == 1);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[1].width, fx[1].height, fx[1].avcc, fx[1].avcc_bytes) == 2);
        UNIT_CHECK(MP4E_set_dsi(mux, 1, "\x12\x10", 2) == 0);
        fill_payload(v1, sizeof(v1), 100);
        fill_payload(v2, sizeof(v2), 101);
        fill_payload(a1, sizeof(a1), 102);
        fill_payload(a2, sizeof(a2), 103);
        UNIT_CHECK(MP4E_put_sample(mux, 0, v1, sizeof(v1), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 1, a1, sizeof(a1), 1024, MP4E_SAMPLE_DEFAULT) == 0);
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 2) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, v2, sizeof(v2), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 1, a2, sizeof(a2), 1024, MP4E_SAMPLE_DEFAULT) == 0);
        UNIT_CHECK(MP4E_close(mux) == 0);
        in.data = out.data;
        in.size = out.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        UNIT_CHECK(mp4.track_count == 2);
        UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, 0) == 1);
        UNIT_CHECK(MP4D_sample_description_index(&mp4, 0, 1) == 2);
        {
            unsigned bytes, ts, dur, k;
            MP4D_file_offset_t off;
            off = MP4D_frame_offset(&mp4, 0, 0, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == sizeof(v1));
            for (k = 0; k < sizeof(v1); k++)
                UNIT_CHECK(out.data[off + k] == v1[k]);
            off = MP4D_frame_offset(&mp4, 0, 1, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == sizeof(v2));
            for (k = 0; k < sizeof(v2); k++)
                UNIT_CHECK(out.data[off + k] == v2[k]);
            off = MP4D_frame_offset(&mp4, 1, 0, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == sizeof(a1));
            for (k = 0; k < sizeof(a1); k++)
                UNIT_CHECK(out.data[off + k] == a1[k]);
            off = MP4D_frame_offset(&mp4, 1, 1, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == sizeof(a2));
            for (k = 0; k < sizeof(a2); k++)
                UNIT_CHECK(out.data[off + k] == a2[k]);
        }
        MP4D_close(&mp4);
        mem_file_reset(&out);
    }

    /* Same-frame multiple slices: continuation bytes are appended and the
       duration belongs to the frame, not to the slice. */
    {
        MP4E_mux_t *mux;
        MP4E_track_t vt;
        unsigned char s1[30], s2[25], s3[35];
        mem_input_t in;
        MP4D_demux_t mp4;
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(0, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(fx[0].width, fx[0].height);
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[0].width, fx[0].height, fx[0].avcc, fx[0].avcc_bytes) == 1);
        fill_payload(s1, sizeof(s1), 200);
        fill_payload(s2, sizeof(s2), 201);
        fill_payload(s3, sizeof(s3), 202);
        UNIT_CHECK(MP4E_put_sample(mux, 0, s1, sizeof(s1), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, s2, sizeof(s2), 0, MP4E_SAMPLE_CONTINUATION) == 0);
        UNIT_CHECK(MP4E_put_sample(mux, 0, s3, sizeof(s3), 0, MP4E_SAMPLE_CONTINUATION) == 0);
        UNIT_CHECK(MP4E_close(mux) == 0);
        in.data = out.data;
        in.size = out.size;
        memset(&mp4, 0, sizeof(mp4));
        UNIT_CHECK(MP4D_open(&mp4, mem_read, &in, (int64_t)in.size) == 1);
        UNIT_CHECK(mp4.track[0].sample_count == 1);
        {
            unsigned bytes = 0, ts = 0, dur = 0, k;
            MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, 0, &bytes, &ts, &dur);
            UNIT_CHECK(bytes == sizeof(s1) + sizeof(s2) + sizeof(s3));
            UNIT_CHECK(dur == 3000);
            for (k = 0; k < sizeof(s1); k++)
                UNIT_CHECK(out.data[off + k] == s1[k]);
            for (k = 0; k < sizeof(s2); k++)
                UNIT_CHECK(out.data[off + sizeof(s1) + k] == s2[k]);
            for (k = 0; k < sizeof(s3); k++)
                UNIT_CHECK(out.data[off + sizeof(s1) + sizeof(s2) + k] == s3[k]);
        }
        MP4D_close(&mp4);
        mem_file_reset(&out);
    }
}

/* ------------------------------------------------------------------ */
/*  Invalid arguments and rejected operations                         */
/* ------------------------------------------------------------------ */

void test_description_invalid(void)
{
    mem_file_t out;
    MP4E_mux_t *mux;
    MP4E_track_t vt, at;
    description_fixture_t fx[3];
    unsigned char bad[64];
    unsigned char payload[8];
    unsigned i;

    CHECK_CASE();
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 3; i++)
        make_fixture(&fx[i], i);

    /* Argument validation on a clean muxer. */
    {
        MP4E_track_t hevc;
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(0, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(fx[0].width, fx[0].height);
        at = make_audio_track();
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_track(mux, &at) == 1);
        memset(&hevc, 0, sizeof(hevc));
        hevc.track_media_kind = e_video;
        hevc.object_type_indication = MP4_OBJECT_TYPE_HEVC;
        hevc.time_scale = 90000;
        hevc.u.v.width = 320;
        hevc.u.v.height = 240;
        UNIT_CHECK(MP4E_add_track(mux, &hevc) == 2);

        /* Audio / HEVC / bad track / null input / bad sizes. */
        UNIT_CHECK(MP4E_add_avc_description(mux, 1, 320, 240, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, 2, 320, 240, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, 99, 320, 240, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, -1, 320, 240, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 0, 240, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 65536, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, NULL, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, fx[0].avcc, 0) == MP4E_STATUS_BAD_ARGUMENTS);

        /* Malformed configuration records. */
        memcpy(bad, fx[0].avcc, fx[0].avcc_bytes);
        bad[0] = 2;  /* configurationVersion != 1 */
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        memcpy(bad, fx[0].avcc, fx[0].avcc_bytes);
        bad[4] = 0xff & ~3;  /* lengthSizeMinusOne != 3 */
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        memcpy(bad, fx[0].avcc, fx[0].avcc_bytes);
        bad[5] = 0xe0;  /* zero SPS */
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        memcpy(bad, fx[0].avcc, fx[0].avcc_bytes);
        bad[17] = 0x7f;  /* PPS length runs past the record */
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);

        /* Truncated SPS length. */
        memcpy(bad, fx[0].avcc, fx[0].avcc_bytes);
        bad[6] = 0x7f;
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);

        /* Zero PPS. */
        memcpy(bad, fx[0].avcc, fx[0].avcc_bytes);
        bad[16] = 0;    /* numOfPictureParameterSets */
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);

        /* Zero-length PPS entry. */
        memcpy(bad, fx[0].avcc, fx[0].avcc_bytes);
        bad[17] = 0; bad[18] = 0;
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);

        /* A failed registration must not consume an index. */
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[0].width, fx[0].height, fx[0].avcc, fx[0].avcc_bytes) == 1);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, bad, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, fx[1].width, fx[1].height, fx[1].avcc, fx[1].avcc_bytes) == 2);
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 3) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_select_avc_description(mux, 0, 0) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_select_avc_description(mux, 1, 1) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_select_avc_description(mux, 99, 1) == MP4E_STATUS_BAD_ARGUMENTS);

        /* Explicit mode rejects the legacy setters. */
        UNIT_CHECK(MP4E_set_sps(mux, 0, sps_a, sizeof(sps_a)) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_set_pps(mux, 0, pps_a, sizeof(pps_a)) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        mem_file_reset(&out);
    }

    /* Fragmentation mode rejects descriptions. */
    {
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(0, 1, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(320, 240);
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        mem_file_reset(&out);
    }

    /* Entering explicit mode is only allowed before legacy sets/samples. */
    {
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(0, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(320, 240);
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_set_sps(mux, 0, sps_a, sizeof(sps_a)) == MP4E_STATUS_OK);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, fx[0].avcc, fx[0].avcc_bytes) == MP4E_STATUS_BAD_ARGUMENTS);
        UNIT_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        mem_file_reset(&out);
    }
    {
        CHECK_CASE();
        memset(&out, 0, sizeof(out));
        mux = MP4E_open(0, 0, &out, mem_write);
        UNIT_CHECK(mux != NULL);
        vt = make_video_track(320, 240);
        UNIT_CHECK(MP4E_add_track(mux, &vt) == 0);
        UNIT_CHECK(MP4E_add_avc_description(mux, 0, 320, 240, fx[0].avcc, fx[0].avcc_bytes) == 1);
        UNIT_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        mem_file_reset(&out);
    }
    (void)payload;
    (void)at;
}

/* The harness has no per-file main; nothing else to emit. */
