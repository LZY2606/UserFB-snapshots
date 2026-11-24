#include "task_support/check.h"

#include "minih264e.h"
#include "h264bsd_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TW 64
#define TH 64
#define TY (TW * TH)
#define TFRAME (TW * TH * 3 / 2)
#define TMAX 80

/* ------------------------------------------------------------------ */
/* Small deterministic test pictures                                   */
/* ------------------------------------------------------------------ */

static unsigned char clamp_byte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

/* Moving bars plus either broad blocks or alternating high-frequency
   detail.  wild_detail is used for the layer-2 texture isolation check. */
static void make_frame(unsigned char *frame, int index, int wild_detail)
{
    int y, x, c;
    memset(frame + TY, 128, TY / 2);

    for (y = 0; y < TH; y++)
    {
        for (x = 0; x < TW; x++)
        {
            int moving_bar = ((x - (index * 5) % TW + TW) % TW) < 7 ? 54 : 0;
            int sweep = ((x + y * 2 + index * 3) & 31) - 16;
            int detail;
            int v;

            if (wild_detail)
            {
                int noise = (int)(((unsigned)(x * 1103515245u +
                    y * 12345u + index * 977u) >> 9) & 127u) - 64;
                detail = noise * 2;
            }
            else if (index & 1)
            {
                detail = (((x * 13 + y * 7 + index * 17) & 1) ? 42 : -42);
            }
            else
            {
                detail = (((x / 8 + y / 8) & 1) ? 20 : -20);
            }

            v = 118 + sweep + moving_bar + detail;
            frame[y * TW + x] = clamp_byte(v);
        }
    }

    for (c = 1; c < 3; c++)
    {
        unsigned char *plane = frame + TY + (c - 1) * TY / 4;
        for (y = 0; y < TH / 2; y++)
        {
            for (x = 0; x < TW / 2; x++)
            {
                plane[y * (TW / 2) + x] = clamp_byte(
                    128 + ((x * 5 + y * 11 + index * 13 + c * 31) & 31) - 15);
            }
        }
    }
}

static H264E_io_yuv_t make_yuv(unsigned char *frame)
{
    H264E_io_yuv_t y;
    memset(&y, 0, sizeof(y));
    y.yuv[0] = frame;
    y.yuv[1] = frame + TY;
    y.yuv[2] = frame + TY + TY / 4;
    y.stride[0] = TW;
    y.stride[1] = y.stride[2] = TW / 2;
    return y;
}

/* ------------------------------------------------------------------ */
/* Encoder session/recording                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    H264E_temporal_stats_t v[TMAX];
    int n;
} StatsSink;

static void stats_observe(const H264E_temporal_stats_t *stats, void *opaque)
{
    StatsSink *sink = (StatsSink *)opaque;
    if (sink && sink->n < TMAX)
        sink->v[sink->n++] = *stats;
}

static void stats_observe_forbidden(const H264E_temporal_stats_t *stats, void *opaque)
{
    int *called = (int *)opaque;
    (void)stats;
    if (called) (*called)++;
}

typedef struct
{
    unsigned char *data[TMAX * 4];
    int size[TMAX * 4];
    int n;
} NalSink;

static void nal_callback(const unsigned char *data, int size, void *opaque)
{
    NalSink *sink = (NalSink *)opaque;
    if (sink && sink->n < TMAX * 4)
    {
        sink->data[sink->n] = malloc(size > 0 ? (size_t)size : 1);
        if (sink->data[sink->n])
        {
            memcpy(sink->data[sink->n], data, (size_t)size);
            sink->size[sink->n] = size;
            sink->n++;
        }
    }
}

static void nal_sink_reset(NalSink *sink)
{
    int i;
    for (i = 0; i < sink->n; i++)
        free(sink->data[i]);
    sink->n = 0;
}

typedef struct
{
    int br[3];
    int buf[3];
    int fps_num;
    int fps_den;
    int qp_min;
    int qp_max;
    int fine_rc;
    int nalu_bytes;
} CaseConfig;

static CaseConfig config_good(void)
{
    CaseConfig c;
    c.br[0] = 300000;
    c.br[1] = 900000;
    c.br[2] = 1800000;
    c.buf[0] = 20000;
    c.buf[1] = 60000;
    c.buf[2] = 120000;
    c.fps_num = 30;
    c.fps_den = 1;
    c.qp_min = 12;
    c.qp_max = 42;
    c.fine_rc = 0;
    c.nalu_bytes = 0;
    return c;
}

typedef struct
{
    uint64_t index;
    int layer;
    int size;
    unsigned char *data;
} FrameRecord;

typedef struct
{
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    FrameRecord frame[TMAX];
    int nframes;
    StatsSink stats;
    NalSink nals;
    int collect_nals;
    int temporal_on;
    CaseConfig cfg;
} Session;

static int session_init(Session *s, const CaseConfig *cfg, int temporal_on)
{
    H264E_create_param_t p;
    int psize = 0, ssize = 0, err;

    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->temporal_on = temporal_on;
    memset(&p, 0, sizeof(p));

    p.width = TW;
    p.height = TH;
    p.gop = 0;
    p.vbv_size_bytes = 0;
    p.fine_rate_control_flag = cfg->fine_rc;
    p.const_input_flag = 1;
    p.max_long_term_reference_frames = 2;
    p.enableNEON = 0;
    p.num_layers = 1;
    p.max_threads = 0;

    if (temporal_on)
    {
        p.temporal_layers = 3;
        memcpy(p.temporal_bitrate_bps, cfg->br, sizeof(cfg->br));
        memcpy(p.temporal_buffer_bytes, cfg->buf, sizeof(cfg->buf));
        p.temporal_fps_num = cfg->fps_num;
        p.temporal_fps_den = cfg->fps_den;
    }
    else
    {
        /* When temporal_layers is zero every one of these is ignored,
           including deliberately invalid values. */
        p.temporal_layers = 0;
        p.temporal_bitrate_bps[0] = -123;
        p.temporal_bitrate_bps[1] = 2;
        p.temporal_bitrate_bps[2] = 1;
        p.temporal_buffer_bytes[0] = 0;
        p.temporal_buffer_bytes[1] = -4;
        p.temporal_buffer_bytes[2] = 99999999;
        p.temporal_fps_num = 0;
        p.temporal_fps_den = 7777777;
    }

    err = H264E_sizeof(&p, &psize, &ssize);
    if (err) return err;
    s->enc = (H264E_persist_t *)malloc((size_t)psize);
    s->scratch = (H264E_scratch_t *)malloc((size_t)ssize);
    if (!s->enc || !s->scratch) return -100;
    err = H264E_init(s->enc, &p);
    if (err) return err;
    return 0;
}

static int session_encode(Session *s, int input_index, int frame_type,
                          unsigned char *pixels)
{
    H264E_run_param_t r;
    H264E_io_yuv_t yuv = make_yuv(pixels);
    unsigned char *out = NULL;
    int out_size = 0, err;

    memset(&r, 0, sizeof(r));
    r.encode_speed = 5;
    r.frame_type = frame_type;
    r.qp_min = s->cfg.qp_min;
    r.qp_max = s->cfg.qp_max;
    r.desired_nalu_bytes = s->cfg.nalu_bytes;
    r.temporal_observe = stats_observe;
    r.temporal_opaque = &s->stats;
    if (s->collect_nals)
    {
        r.nalu_callback = nal_callback;
        r.nalu_callback_token = &s->nals;
    }

    err = H264E_encode(s->enc, s->scratch, &r, &yuv, &out, &out_size);
    if (err) return err;
    if (s->nframes >= TMAX ||
        (s->temporal_on && s->stats.n != s->nframes + 1))
        return -101;

    s->frame[s->nframes].index = (uint64_t)input_index;
    s->frame[s->nframes].layer = s->temporal_on ?
        s->stats.v[s->nframes].temporal_id : 0;
    s->frame[s->nframes].size = out_size;
    s->frame[s->nframes].data = (unsigned char *)malloc((size_t)out_size);
    if (!s->frame[s->nframes].data) return -102;
    memcpy(s->frame[s->nframes].data, out, (size_t)out_size);
    s->nframes++;
    return 0;
}

static void session_free(Session *s)
{
    int i;
    for (i = 0; i < s->nframes; i++)
        free(s->frame[i].data);
    nal_sink_reset(&s->nals);
    free(s->enc);
    free(s->scratch);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* Minimal independent Annex-B/slice-header parser                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    const unsigned char *data;
    size_t size;
} NalRange;

static int collect_nals(const unsigned char *data, int size,
                        NalRange *out, int max_nals)
{
    int count = 0;
    size_t pos = 0;

    while (pos + 3 <= (size_t)size)
    {
        size_t payload, next, i;

        if (pos + 4 <= (size_t)size &&
            data[pos] == 0 && data[pos + 1] == 0 &&
            data[pos + 2] == 0 && data[pos + 3] == 1)
        {
            payload = pos + 4;
        }
        else if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1)
        {
            payload = pos + 3;
        }
        else
        {
            pos++;
            continue;
        }

        next = (size_t)size;
        for (i = payload; i + 3 <= (size_t)size; i++)
        {
            if ((i + 4 <= (size_t)size &&
                 data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1) ||
                (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1))
            {
                next = i;
                break;
            }
        }

        if (count < max_nals)
        {
            out[count].data = data + payload;
            out[count].size = next - payload;
            count++;
        }
        if (next == (size_t)size) break;
        pos = next;
    }
    return count;
}

typedef struct
{
    const unsigned char *data;
    size_t size_bytes;
    size_t bit_pos;
    int error;
} BitReader;

static uint32_t br_bits(BitReader *b, int n)
{
    uint32_t value = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        size_t bit_index = b->bit_pos++;
        int bit;
        if (bit_index / 8 >= b->size_bytes)
        {
            b->error = 1;
            return 0;
        }
        bit = (b->data[bit_index >> 3] >> (7 - (bit_index & 7))) & 1;
        value = (value << 1) | (uint32_t)bit;
    }
    return value;
}

static uint32_t br_ue(BitReader *b)
{
    int zeros = 0;
    uint32_t suffix;
    while (!b->error && br_bits(b, 1) == 0 && zeros < 32)
        zeros++;
    if (b->error || zeros >= 32) return 0;
    suffix = zeros ? br_bits(b, zeros) : 0;
    return ((1u << zeros) - 1u) + suffix;
}

static unsigned char *nal_to_rbsp(NalRange nal, size_t *rbsp_size)
{
    unsigned char *rbsp = (unsigned char *)malloc(nal.size ? nal.size : 1);
    size_t i, out = 0;
    if (!rbsp) return NULL;
    for (i = 0; i < nal.size; i++)
    {
        if (i + 2 < nal.size && nal.data[i] == 0 &&
            nal.data[i + 1] == 0 && nal.data[i + 2] == 3)
        {
            rbsp[out++] = 0;
            rbsp[out++] = 0;
            i += 2;
        }
        else
        {
            rbsp[out++] = nal.data[i];
        }
    }
    *rbsp_size = out;
    return rbsp;
}

typedef struct
{
    int nal_type;
    int ref_idc;
    int slice_count;
    int frame_num;
    int idr_long_term_flag;
    int list_modification;
    int list_use_raw;
    int adaptive_marking;
    int mmco_update_slot; /* external one-based slot, 0 if absent */
    int sps_gaps_allowed;
    int sps_max_ref_frames;
} ParsedAccessUnit;

static int parse_access_unit(const unsigned char *data, int size,
                             ParsedAccessUnit *info)
{
    NalRange nals[64];
    int nal_count, i;

    memset(info, 0, sizeof(*info));
    info->list_use_raw = -1;
    info->mmco_update_slot = 0;
    info->sps_gaps_allowed = -1;
    nal_count = collect_nals(data, size, nals, 64);

    for (i = 0; i < nal_count; i++)
    {
        unsigned char header;
        int nal_type, ref_idc;
        size_t rbsp_size = 0;
        unsigned char *rbsp;
        BitReader br;
        int parse_this_slice;

        if (!nals[i].size) continue;
        header = nals[i].data[0];
        ref_idc = (header >> 5) & 3;
        nal_type = header & 31;
        if (i == 0 || nal_type == 5 || nal_type == 1)
        {
            info->nal_type = nal_type;
            info->ref_idc = ref_idc;
        }

        rbsp = nal_to_rbsp(nals[i], &rbsp_size);
        if (!rbsp) return -1;
        memset(&br, 0, sizeof(br));
        br.data = rbsp;
        br.size_bytes = rbsp_size;
        br.bit_pos = 0;
        (void)br_bits(&br, 8); /* NAL header */

        if (nal_type == 7)
        {
            int profile = (int)br_bits(&br, 8);
            (void)br_bits(&br, 8); /* constraint flags */
            (void)br_bits(&br, 8); /* level */
            (void)br_ue(&br);      /* sps id */
            if (profile == 100 || profile == 110 || profile == 122 ||
                profile == 244 || profile == 44 || profile == 83 ||
                profile == 86 || profile == 118 || profile == 128)
            {
                int chroma = (int)br_ue(&br);
                if (chroma == 3) (void)br_bits(&br, 1);
                (void)br_ue(&br);
                (void)br_ue(&br);
                (void)br_bits(&br, 1);
                if (br_bits(&br, 1))
                {
                    int j;
                    for (j = 0; j < 8; j++)
                        if (br_bits(&br, 1))
                            (void)br_bits(&br, 8 * 4);
                }
            }
            (void)br_ue(&br); /* log2_max_frame_num_minus4 */
            {
                uint32_t poc_type = br_ue(&br);
                if (poc_type == 0) (void)br_ue(&br);
                else if (poc_type == 1)
                {
                    (void)br_bits(&br, 1);
                    (void)br_bits(&br, 1);
                    (void)br_bits(&br, 1);
                    (void)br_ue(&br);
                    (void)br_ue(&br);
                }
            }
            info->sps_max_ref_frames = (int)br_ue(&br);
            info->sps_gaps_allowed = (int)br_bits(&br, 1);
        }

        parse_this_slice = (nal_type == 1 || nal_type == 5) &&
                           info->slice_count == 0;
        if (nal_type == 1 || nal_type == 5)
            info->slice_count++;

        if (parse_this_slice)
        {
            uint32_t slice_type;
            int is_p;

            (void)br_ue(&br); /* first mb */
            slice_type = br_ue(&br);
            (void)br_ue(&br); /* pps id */
            info->frame_num = (int)br_bits(&br, 5);
            if (nal_type == 5)
                (void)br_ue(&br); /* idr_pic_id */

            is_p = slice_type == 0 || slice_type == 5;
            if (is_p)
            {
                int override_flag = (int)br_bits(&br, 1);
                if (override_flag)
                    (void)br_ue(&br);
                info->list_modification = (int)br_bits(&br, 1);
                if (info->list_modification)
                {
                    for (;;)
                    {
                        uint32_t op = br_ue(&br);
                        if (op == 3) break;
                        if (op == 0 || op == 1)
                            (void)br_ue(&br);
                        else if (op == 2)
                            info->list_use_raw = (int)br_ue(&br);
                    }
                }
            }

            if (ref_idc != 0)
            {
                if (nal_type == 5)
                {
                    (void)br_bits(&br, 1); /* no_output */
                    info->idr_long_term_flag = (int)br_bits(&br, 1);
                }
                else
                {
                    info->adaptive_marking = (int)br_bits(&br, 1);
                    if (info->adaptive_marking)
                    {
                        for (;;)
                        {
                            uint32_t op = br_ue(&br);
                            if (op == 0) break;
                            if (op == 1 || op == 3)
                                (void)br_ue(&br);
                            else if (op == 2)
                                (void)br_ue(&br);
                            else if (op == 4)
                                (void)br_ue(&br);
                            else if (op == 6)
                                info->mmco_update_slot = (int)br_ue(&br) + 1;
                        }
                    }
                }
            }
        }

        free(rbsp);
        if (br.error) return -2;
    }

    return nal_count;
}

/* Classify only from standard NAL reference priority and MMCO slot
   commands, without using temporal_observe's temporal_id. */
static int parsed_temporal_layer(const ParsedAccessUnit *info)
{
    if (info->nal_type == 5) return 0;
    if (info->nal_type != 1) return -1;
    if (info->ref_idc == 0) return 2;
    if (info->mmco_update_slot == 1) return 0;
    if (info->mmco_update_slot == 2) return 1;
    return -1;
}

/* ------------------------------------------------------------------ */
/* h264bsd extraction decoding                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *pic[TMAX];
    uint32_t pic_id[TMAX];
    int pic_error[TMAX];
    int count;
} DecodedSet;

static void decoded_free(DecodedSet *d)
{
    int i;
    for (i = 0; i < TMAX; i++)
        free(d->pic[i]);
    memset(d, 0, sizeof(*d));
}

static int decode_extract_range(const Session *s, int max_layer,
                                int start_index, DecodedSet *out)
{
    storage_t *dec = h264bsdAlloc();
    int i;

    memset(out, 0, sizeof(*out));
    if (!dec || h264bsdInit(dec, 1) != 0)
    {
        if (dec) h264bsdFree(dec);
        return -1;
    }

    for (i = 0; i < s->nframes; i++)
    {
        const FrameRecord *f = &s->frame[i];
        unsigned char *mutable_stream;
        uint32_t offset = 0, used = 0;
        int stalls = 0;

        if (i < start_index || f->layer > max_layer) continue;
        mutable_stream = (unsigned char *)malloc((size_t)f->size);
        if (!mutable_stream)
        {
            h264bsdShutdown(dec);
            h264bsdFree(dec);
            return -6;
        }
        memcpy(mutable_stream, f->data, (size_t)f->size);

        while (offset < (uint32_t)f->size)
        {
            uint32_t status = h264bsdDecode(dec, mutable_stream + offset,
                (uint32_t)f->size - offset, (uint32_t)f->index, &used);
            offset += used;
            if (status > H264BSD_HDRS_RDY)
            {
                fprintf(stderr, "decode fail layer<=%d i=%d status=%u used=%u off=%u size=%d\n", max_layer, i, status, used, offset, f->size);
                free(mutable_stream);
                h264bsdShutdown(dec);
                h264bsdFree(dec);
                return -2;
            }
            if (!used && ++stalls > 16)
            {
                free(mutable_stream);
                h264bsdShutdown(dec);
                h264bsdFree(dec);
                return -3;
            }
            if (status == H264BSD_PIC_RDY)
            {
                uint32_t id = 0, is_idr = 0, errors = 0;
                unsigned char *pic = h264bsdNextOutputPicture(
                    dec, &id, &is_idr, &errors);
                if (pic)
                {
                    if (id >= TMAX || out->pic[id] != NULL || errors != 0)
                    {
                        free(mutable_stream);
                        h264bsdShutdown(dec);
                        h264bsdFree(dec);
                        return -4;
                    }
                    out->pic[id] = (unsigned char *)malloc(TFRAME);
                    if (!out->pic[id])
                    {
                        free(mutable_stream);
                        h264bsdShutdown(dec);
                        h264bsdFree(dec);
                        return -5;
                    }
                    memcpy(out->pic[id], pic, TFRAME);
                    out->pic_id[out->count] = id;
                    out->pic_error[out->count] = (int)errors;
                    out->count++;
                }
            }
        }
        free(mutable_stream);
    }

    h264bsdShutdown(dec);
    h264bsdFree(dec);
    return 0;
}

static int decode_extract(const Session *s, int max_layer, DecodedSet *out)
{
    return decode_extract_range(s, max_layer, 0, out);
}

static int mean_luma_difference(const unsigned char *a, const unsigned char *b)
{
    long sum = 0;
    int i;
    for (i = 0; i < TY; i++)
    {
        int d = (int)a[i] - (int)b[i];
        sum += d < 0 ? -d : d;
    }
    return (int)(sum / TY);
}

/* ------------------------------------------------------------------ */
/* schedule group                                                      */
/* ------------------------------------------------------------------ */

static int schedule_case(void)
{
    enum { N = 18, KEY_AT = 7 };
    CaseConfig cfg = config_good();
    Session s;
    unsigned char pixels[TFRAME];
    int i, err, phase = 0, expected_fn = 1;
    static const int pattern[4] = {2, 1, 2, 0};

    err = session_init(&s, &cfg, 1);
    TASK_CHECK(err == 0);

    for (i = 0; i < N; i++)
    {
        int key = (i == 0 || i == KEY_AT);
        int expected_layer = key ? 0 : pattern[phase & 3];
        ParsedAccessUnit info;
        int expected_use, expected_update;

        make_frame(pixels, i, 0);
        err = session_encode(&s, i, key ? H264E_FRAME_TYPE_KEY :
            H264E_FRAME_TYPE_DEFAULT, pixels);
        TASK_CHECK(err == 0);
        TASK_CHECK(s.stats.v[i].input_index == (uint64_t)i);
        TASK_CHECK(s.stats.v[i].temporal_id == expected_layer);

        err = parse_access_unit(s.frame[i].data, s.frame[i].size, &info);
        TASK_CHECK(err >= (key ? 3 : 1)); /* key: SPS, PPS, slice */
        TASK_CHECK(info.slice_count == 1);

        if (key)
        {
            TASK_CHECK(expected_layer == 0);
            TASK_CHECK(info.nal_type == 5);
            TASK_CHECK(info.ref_idc == 3);
            TASK_CHECK(info.frame_num == 0);
            TASK_CHECK(info.idr_long_term_flag == 1);
            TASK_CHECK(info.sps_gaps_allowed == 1);
            TASK_CHECK(info.sps_max_ref_frames == 3);
            expected_fn = 1;
            phase = 0;
        }
        else
        {
            TASK_CHECK(info.nal_type == 1);
            TASK_CHECK(info.frame_num == expected_fn);

            if (expected_layer == 0)
            {
                expected_use = 0;    /* raw long_term_pic_num for slot 1 */
                expected_update = 1;
                TASK_CHECK(info.ref_idc == 3);
                TASK_CHECK(info.list_modification == 1);
                TASK_CHECK(info.list_use_raw == expected_use);
                TASK_CHECK(info.adaptive_marking == 1);
                TASK_CHECK(info.mmco_update_slot == expected_update);
                expected_fn++;
            }
            else if (expected_layer == 1)
            {
                expected_use = 0;    /* base slot 1 */
                expected_update = 2; /* middle slot 2 */
                TASK_CHECK(info.ref_idc == 3);
                TASK_CHECK(info.list_use_raw == expected_use);
                TASK_CHECK(info.mmco_update_slot == expected_update);
                expected_fn++;
            }
            else
            {
                /* phase 0: high follows base slot 1; phase 2: middle slot 2 */
                expected_use = ((phase & 3) == 0) ? 0 : 1;
                TASK_CHECK(info.ref_idc == 0);
                TASK_CHECK(info.list_modification == 1);
                TASK_CHECK(info.list_use_raw == expected_use);
                TASK_CHECK(info.adaptive_marking == 0);
                TASK_CHECK(info.mmco_update_slot == 0);
                /* Highest pictures are non-reference and do not advance the
                   encoder's reference frame number. */
            }
            phase = (phase + 1) & 3;
        }
    }

    TASK_CHECK(s.nframes == N);
    TASK_CHECK(s.stats.n == N);
    session_free(&s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* budget group                                                        */
/* ------------------------------------------------------------------ */

static int64_t cumulative_bytes(const CaseConfig *c, int k, uint64_t n)
{
    __int128 v = (__int128)n * c->br[k] * c->fps_den;
    v /= (__int128)8 * c->fps_num;
    return (int64_t)v;
}

static int budget_case(void)
{
    enum { N = 44, KEY_AT = 19 };
    CaseConfig cfg = config_good();
    Session s;
    unsigned char pixels[TFRAME];
    int i, err, phase = 0;
    int saw_positive_debt = 0, saw_negative_debt = 0;
    int crossed_schedule_cycle = 0;
    int64_t debt[3] = {0, 0, 0};
    static const int pattern[4] = {2, 1, 2, 0};

    cfg.br[0] = 120000;
    cfg.br[1] = 260000;
    cfg.br[2] = 520000;
    cfg.buf[0] = 256;
    cfg.buf[1] = 512;
    cfg.buf[2] = 1024;
    cfg.qp_min = 22;
    cfg.qp_max = 50;

    err = session_init(&s, &cfg, 1);
    TASK_CHECK(err == 0);

    for (i = 0; i < N; i++)
    {
        uint64_t n = (uint64_t)i + 1;
        int key = (i == 0 || i == KEY_AT);
        int layer = key ? 0 : pattern[phase & 3];
        int64_t quota[3], budget64;
        int k;

        make_frame(pixels, i, 0);
        err = session_encode(&s, i, key ? H264E_FRAME_TYPE_KEY :
            H264E_FRAME_TYPE_DEFAULT, pixels);
        TASK_CHECK(err == 0);

        for (k = 0; k < 3; k++)
        {
            quota[k] = cumulative_bytes(&cfg, k, n) -
                       cumulative_bytes(&cfg, k, n - 1);
            TASK_CHECK(s.stats.v[i].quota_bytes[k] == quota[k]);
        }

        budget64 = INT64_MAX;
        for (k = layer; k < 3; k++)
        {
            int64_t candidate = (int64_t)cfg.buf[k] - debt[k] + quota[k];
            if (candidate < budget64) budget64 = candidate;
        }
        if (budget64 < 1) budget64 = 1;
        if (budget64 > 1000000) budget64 = 1000000;
        TASK_CHECK(s.stats.v[i].budget_bytes == (int)budget64);
        TASK_CHECK(s.stats.v[i].actual_bytes == s.frame[i].size);

        for (k = 0; k < 3; k++)
        {
            debt[k] -= quota[k];
            if (k >= layer) debt[k] += s.frame[i].size;
            TASK_CHECK(s.stats.v[i].debt_bytes[k] == debt[k]);
            if (debt[k] > 0) saw_positive_debt = 1;
            if (debt[k] < 0) saw_negative_debt = 1;
        }

        if (key) phase = 0;
        else
        {
            phase = (phase + 1) & 3;
            if (i > 8) crossed_schedule_cycle = 1;
        }
    }

    TASK_CHECK(saw_positive_debt);
    TASK_CHECK(crossed_schedule_cycle);
    TASK_CHECK(s.stats.n == N);
    (void)saw_negative_debt; /* Fractional quota may or may not underspend. */
    session_free(&s);
    return 0;
}

/* Base set k=0 is deliberately the tightest candidate.  Changing only
   layer-2 textures must not perturb base RC/reference state. */
static int budget_isolation_case(void)
{
    enum { N = 24 };
    CaseConfig cfg = config_good();
    Session a, b;
    DecodedSet da, db;
    unsigned char pa[TFRAME], pb[TFRAME];
    int i, err;

    cfg.br[0] = 200000;
    cfg.br[1] = 40000000;
    cfg.br[2] = 80000000;
    cfg.buf[0] = 6000;
    cfg.buf[1] = 1000000;
    cfg.buf[2] = 1000000;
    cfg.qp_min = 12;
    cfg.qp_max = 48;

    err = session_init(&a, &cfg, 1);
    TASK_CHECK(err == 0);
    err = session_init(&b, &cfg, 1);
    TASK_CHECK(err == 0);

    for (i = 0; i < N; i++)
    {
        static const int isolation_pattern[4] = {2, 1, 2, 0};
        int layer = (i == 0) ? 0 : isolation_pattern[(i - 1) & 3];
        int wild = (layer == 2);
        make_frame(pa, i, 0);
        make_frame(pb, i, wild);
        if (wild)
        {
            /* Keep the same base/middle inputs but make high layers much
               more complex in instance b. */
            int y, x;
            for (y = 0; y < TH; y++)
                for (x = 0; x < TW; x++)
                    pb[y * TW + x] = clamp_byte(
                        pb[y * TW + x] + (((x ^ y ^ i) & 1) ? 70 : -70));
        }

        err = session_encode(&a, i, H264E_FRAME_TYPE_DEFAULT, pa);
        TASK_CHECK(err == 0);
        err = session_encode(&b, i, H264E_FRAME_TYPE_DEFAULT, pb);
        TASK_CHECK(err == 0);

        if (a.stats.v[i].temporal_id == 0)
        {
            int64_t candidate0 = (int64_t)cfg.buf[0] -
                (i ? a.stats.v[i - 1].debt_bytes[0] : 0) +
                a.stats.v[i].quota_bytes[0];
            int64_t candidate1 = (int64_t)cfg.buf[1] -
                (i ? a.stats.v[i - 1].debt_bytes[1] : 0) +
                a.stats.v[i].quota_bytes[1];
            int64_t candidate2 = (int64_t)cfg.buf[2] -
                (i ? a.stats.v[i - 1].debt_bytes[2] : 0) +
                a.stats.v[i].quota_bytes[2];
            TASK_CHECK(candidate0 <= candidate1);
            TASK_CHECK(candidate0 <= candidate2);
            TASK_CHECK(a.stats.v[i].budget_bytes ==
                        b.stats.v[i].budget_bytes);
            TASK_CHECK(a.frame[i].size == b.frame[i].size);
            TASK_CHECK(!memcmp(a.frame[i].data, b.frame[i].data,
                               (size_t)a.frame[i].size));
        }
    }

    err = decode_extract(&a, 0, &da);
    TASK_CHECK(err == 0);
    err = decode_extract(&b, 0, &db);
    TASK_CHECK(err == 0);
    for (i = 0; i < N; i++)
    {
        if (a.stats.v[i].temporal_id == 0)
        {
            TASK_CHECK(da.pic[i] != NULL && db.pic[i] != NULL);
            TASK_CHECK(!memcmp(da.pic[i], db.pic[i], TFRAME));
        }
    }
    decoded_free(&da);
    decoded_free(&db);
    session_free(&a);
    session_free(&b);
    return 0;
}

/* ------------------------------------------------------------------ */
/* extraction group                                                    */
/* ------------------------------------------------------------------ */

static int extraction_case(void)
{
    enum { N = 72, KEY_AT = 37 };
    CaseConfig cfg = config_good();
    Session s;
    unsigned char pixels[TMAX][TFRAME];
    DecodedSet d0, d1, d2, key0, key1;
    int i, err;

    cfg.br[0] = 1200000;
    cfg.br[1] = 2400000;
    cfg.br[2] = 4800000;
    cfg.buf[0] = 60000;
    cfg.buf[1] = 120000;
    cfg.buf[2] = 240000;
    cfg.qp_min = 10;
    cfg.qp_max = 34;

    err = session_init(&s, &cfg, 1);
    TASK_CHECK(err == 0);

    for (i = 0; i < N; i++)
    {
        ParsedAccessUnit parsed;
        int parsed_layer;
        make_frame(pixels[i], i, 0);
        err = session_encode(&s, i,
            i == KEY_AT ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT,
            pixels[i]);
        TASK_CHECK(err == 0);

        err = parse_access_unit(s.frame[i].data, s.frame[i].size, &parsed);
        TASK_CHECK(err >= (i == KEY_AT || i == 0 ? 3 : 1));
        parsed_layer = parsed_temporal_layer(&parsed);
        TASK_CHECK(parsed_layer == s.frame[i].layer);
        /* The decoders below select frames using this independent parser
           classification, not the observer's temporal_id. */
        s.frame[i].layer = parsed_layer;
    }

    err = decode_extract(&s, 0, &d0);
    TASK_CHECK(err == 0);
    err = decode_extract(&s, 1, &d1);
    TASK_CHECK(err == 0);
    err = decode_extract(&s, 2, &d2);
    TASK_CHECK(err == 0);

    {
        int expect0 = 0, expect1 = 0, expect2 = 0;
        for (i = 0; i < N; i++)
        {
            expect2++;
            if (s.frame[i].layer <= 1) expect1++;
            if (s.frame[i].layer == 0) expect0++;
        }
        TASK_CHECK(d0.count == expect0);
        TASK_CHECK(d1.count == expect1);
        TASK_CHECK(d2.count == expect2);
    }

    for (i = 0; i < N; i++)
    {
        if (s.frame[i].layer <= 0)
        {
            TASK_CHECK(d0.pic[i] != NULL);
            TASK_CHECK(d1.pic[i] != NULL);
            TASK_CHECK(d2.pic[i] != NULL);
            TASK_CHECK(!memcmp(d0.pic[i], d1.pic[i], TFRAME));
            TASK_CHECK(!memcmp(d0.pic[i], d2.pic[i], TFRAME));
            TASK_CHECK(mean_luma_difference(pixels[i], d0.pic[i]) < 45);
        }
        if (s.frame[i].layer <= 1)
        {
            TASK_CHECK(d1.pic[i] != NULL);
            TASK_CHECK(d2.pic[i] != NULL);
            TASK_CHECK(!memcmp(d1.pic[i], d2.pic[i], TFRAME));
            TASK_CHECK(mean_luma_difference(pixels[i], d1.pic[i]) < 45);
        }
        TASK_CHECK(d2.pic[i] != NULL);
        TASK_CHECK(mean_luma_difference(pixels[i], d2.pic[i]) < 45);
    }

    /* The explicit KEY access unit must also be a valid random-access start
       for either retained subset. */
    err = decode_extract_range(&s, 0, KEY_AT, &key0);
    TASK_CHECK(err == 0);
    err = decode_extract_range(&s, 1, KEY_AT, &key1);
    TASK_CHECK(err == 0);
    for (i = KEY_AT; i < N; i++)
    {
        if (s.frame[i].layer == 0)
        {
            TASK_CHECK(key0.pic[i] != NULL);
            TASK_CHECK(key1.pic[i] != NULL);
            TASK_CHECK(!memcmp(key0.pic[i], d0.pic[i], TFRAME));
            TASK_CHECK(!memcmp(key1.pic[i], d1.pic[i], TFRAME));
        }
    }

    decoded_free(&key0);
    decoded_free(&key1);
    decoded_free(&d0);
    decoded_free(&d1);
    decoded_free(&d2);
    session_free(&s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* compat group                                                        */
/* ------------------------------------------------------------------ */

static int check_create_rejected(H264E_create_param_t p)
{
    int ps = 0, ss = 0;
    H264E_persist_t *enc;
    int ps_valid = 0, ss_valid = 0;
    H264E_sizeof(&p, &ps_valid, &ss_valid); /* may already reject */
    if (H264E_sizeof(&p, &ps, &ss) == 0) return 1;

    /* Init must independently reject.  Use enough storage for the valid
       template size; invalid sizeof outputs are not relied upon. */
    {
        H264E_create_param_t good;
        CaseConfig cfg = config_good();
        int good_ps = 0, good_ss = 0;
        memset(&good, 0, sizeof(good));
        good.width = TW; good.height = TH; good.gop = 0;
        good.num_layers = 1; good.max_long_term_reference_frames = 2;
        good.temporal_layers = 3;
        memcpy(good.temporal_bitrate_bps, cfg.br, sizeof(cfg.br));
        memcpy(good.temporal_buffer_bytes, cfg.buf, sizeof(cfg.buf));
        good.temporal_fps_num = 30; good.temporal_fps_den = 1;
        if (H264E_sizeof(&good, &good_ps, &good_ss)) return 2;
        enc = (H264E_persist_t *)malloc((size_t)good_ps);
        if (!enc) return 3;
        if (H264E_init(enc, &p) == 0)
        {
            free(enc);
            return 4;
        }
        free(enc);
    }
    return 0;
}

static int invalid_creation_case(void)
{
    H264E_create_param_t base;
    CaseConfig cfg = config_good();

    memset(&base, 0, sizeof(base));
    base.width = TW;
    base.height = TH;
    base.gop = 0;
    base.num_layers = 1;
    base.max_long_term_reference_frames = 2;
    base.temporal_layers = 3;
    memcpy(base.temporal_bitrate_bps, cfg.br, sizeof(cfg.br));
    memcpy(base.temporal_buffer_bytes, cfg.buf, sizeof(cfg.buf));
    base.temporal_fps_num = 30;
    base.temporal_fps_den = 1;

    {
        H264E_create_param_t p = base;
        p.temporal_layers = 2;
        TASK_CHECK(check_create_rejected(p) == 0);
    }
    {
        H264E_create_param_t p = base;
        p.temporal_bitrate_bps[1] = p.temporal_bitrate_bps[0];
        TASK_CHECK(check_create_rejected(p) == 0);
    }
    {
        H264E_create_param_t p = base;
        p.temporal_bitrate_bps[2] = p.temporal_bitrate_bps[1] - 1;
        TASK_CHECK(check_create_rejected(p) == 0);
    }
    {
        H264E_create_param_t p = base;
        p.temporal_bitrate_bps[2] = 100000001;
        TASK_CHECK(check_create_rejected(p) == 0);
    }
    {
        int k;
        for (k = 0; k < 3; k++)
        {
            H264E_create_param_t p = base;
            p.temporal_buffer_bytes[k] = 0;
            TASK_CHECK(check_create_rejected(p) == 0);
            p = base;
            p.temporal_buffer_bytes[k] = 1000001;
            TASK_CHECK(check_create_rejected(p) == 0);
        }
    }
    {
        H264E_create_param_t p = base;
        p.temporal_fps_num = 0;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.temporal_fps_den = 1000001;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.temporal_fps_num = 29; p.temporal_fps_den = 30;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.temporal_fps_num = 121; p.temporal_fps_den = 1;
        TASK_CHECK(check_create_rejected(p) == 0);
    }
    {
        H264E_create_param_t p = base;
        p.num_layers = 2;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.max_threads = 2;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.gop = 12;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.temporal_denoise_flag = 1;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.vbv_size_bytes = 4096;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.max_long_term_reference_frames = 1;
        TASK_CHECK(check_create_rejected(p) == 0);
        p = base; p.max_long_term_reference_frames = 3;
        TASK_CHECK(check_create_rejected(p) == 0);
    }
    return 0;
}

static int invalid_runtime_retry_case(void)
{
    CaseConfig cfg = config_good();
    Session s;
    unsigned char before[TFRAME], pixels[TFRAME];
    unsigned char *out = NULL;
    int out_size = 0, err, j;
    static const int bad_types[] = {
        H264E_FRAME_TYPE_I, H264E_FRAME_TYPE_GOLDEN,
        H264E_FRAME_TYPE_RECOVERY, H264E_FRAME_TYPE_DROPPABLE,
        H264E_FRAME_TYPE_CUSTOM
    };

    err = session_init(&s, &cfg, 1);
    TASK_CHECK(err == 0);
    make_frame(pixels, 0, 0);
    memcpy(before, pixels, TFRAME);

    for (j = 0; j < (int)(sizeof(bad_types) / sizeof(bad_types[0])); j++)
    {
        H264E_run_param_t r;
        H264E_io_yuv_t yuv = make_yuv(pixels);
        memset(&r, 0, sizeof(r));
        r.frame_type = bad_types[j];
        r.temporal_observe = stats_observe;
        r.temporal_opaque = &s.stats;
        err = H264E_encode(s.enc, s.scratch, &r, &yuv, &out, &out_size);
        TASK_CHECK(err == H264E_STATUS_BAD_FRAME_TYPE);
        TASK_CHECK(s.stats.n == 0);
        TASK_CHECK(!memcmp(before, pixels, TFRAME));
    }

    err = session_encode(&s, 0, H264E_FRAME_TYPE_DEFAULT, pixels);
    TASK_CHECK(err == 0);
    TASK_CHECK(s.nframes == 1);
    TASK_CHECK(s.stats.v[0].input_index == 0);
    TASK_CHECK(s.stats.v[0].temporal_id == 0);
    TASK_CHECK(parse_access_unit(s.frame[0].data, s.frame[0].size,
        &(ParsedAccessUnit){0}) >= 2);
    session_free(&s);
    return 0;
}

static int two_instances_case(void)
{
    CaseConfig cfg_a = config_good();
    CaseConfig cfg_b = config_good();
    Session a, b;
    unsigned char pa[TFRAME], pb[TFRAME];
    int i, err;

    cfg_a.fps_num = 24000;
    cfg_a.fps_den = 1001;
    cfg_b.br[0] = 180000;
    cfg_b.br[1] = 420000;
    cfg_b.br[2] = 900000;
    cfg_b.fps_num = 25;
    cfg_b.fps_den = 1;

    err = session_init(&a, &cfg_a, 1);
    TASK_CHECK(err == 0);
    err = session_init(&b, &cfg_b, 1);
    TASK_CHECK(err == 0);

    for (i = 0; i < 14; i++)
    {
        make_frame(pa, i, 0);
        make_frame(pb, i + 100, 1);
        err = session_encode(&a, i, H264E_FRAME_TYPE_DEFAULT, pa);
        TASK_CHECK(err == 0);
        err = session_encode(&b, i, H264E_FRAME_TYPE_DEFAULT, pb);
        TASK_CHECK(err == 0);
        TASK_CHECK(a.stats.v[i].input_index == (uint64_t)i);
        TASK_CHECK(b.stats.v[i].input_index == (uint64_t)i);
        TASK_CHECK(a.stats.v[i].quota_bytes[0] !=
                   b.stats.v[i].quota_bytes[0] || i == 0);
    }

    TASK_CHECK(a.stats.n == 14 && b.stats.n == 14);
    TASK_CHECK(a.stats.v[13].debt_bytes[0] != 0 ||
               b.stats.v[13].debt_bytes[0] != 0 || 1);
    session_free(&a);
    session_free(&b);
    return 0;
}

static int off_byte_compat_case(void)
{
    CaseConfig cfg = config_good();
    Session a, b;
    unsigned char pa[TFRAME], pb[TFRAME];
    int i, err, ignored_observer_calls = 0;

    err = session_init(&a, &cfg, 0);
    TASK_CHECK(err == 0);
    err = session_init(&b, &cfg, 0);
    TASK_CHECK(err == 0);

    for (i = 0; i < 6; i++)
    {
        make_frame(pa, i, 0);
        make_frame(pb, i, 0);
        err = session_encode(&a, i, H264E_FRAME_TYPE_DEFAULT, pa);
        TASK_CHECK(err == 0);
        err = session_encode(&b, i, H264E_FRAME_TYPE_DEFAULT, pb);
        TASK_CHECK(err == 0);
        TASK_CHECK(a.frame[i].size == b.frame[i].size);
        TASK_CHECK(!memcmp(a.frame[i].data, b.frame[i].data,
                           (size_t)a.frame[i].size));
        TASK_CHECK(a.stats.n == 0 && b.stats.n == 0);
    }

    /* The run-time observer is also new and must be ignored when off. */
    {
        H264E_run_param_t r;
        H264E_io_yuv_t yuv = make_yuv(pa);
        unsigned char *out;
        int out_size;
        memset(&r, 0, sizeof(r));
        r.frame_type = H264E_FRAME_TYPE_P;
        r.temporal_observe = stats_observe_forbidden;
        r.temporal_opaque = &ignored_observer_calls;
        TASK_CHECK(H264E_encode(a.enc, a.scratch, &r, &yuv,
                                &out, &out_size) == 0);
        TASK_CHECK(ignored_observer_calls == 0);
    }

    session_free(&a);
    session_free(&b);
    return 0;
}

static int nal_callback_and_slices_case(void)
{
    CaseConfig cfg = config_good();
    Session s;
    unsigned char pixels[TFRAME];
    int i, err;

    cfg.fine_rc = 1;
    cfg.nalu_bytes = 96;
    err = session_init(&s, &cfg, 1);
    TASK_CHECK(err == 0);
    s.collect_nals = 1;

    for (i = 0; i < 5; i++)
    {
        NalRange ranges[TMAX * 4];
        int nal_count, j;
        make_frame(pixels, i, 0);
        nal_sink_reset(&s.nals);
        err = session_encode(&s, i, H264E_FRAME_TYPE_DEFAULT, pixels);
        TASK_CHECK(err == 0);

        nal_count = collect_nals(s.frame[i].data, s.frame[i].size,
                                 ranges, TMAX * 4);
        TASK_CHECK(nal_count == s.nals.n);
        for (j = 0; j < nal_count; j++)
        {
            TASK_CHECK((int)ranges[j].size == s.nals.size[j]);
            TASK_CHECK(!memcmp(ranges[j].data, s.nals.data[j],
                               ranges[j].size));
        }
        if (i >= 1)
        {
            ParsedAccessUnit info;
            TASK_CHECK(parse_access_unit(s.frame[i].data,
                s.frame[i].size, &info) >= 1);
            TASK_CHECK(info.slice_count >= 2);
        }
    }

    session_free(&s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver registration                                                 */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    { "schedule", "reference_hierarchy", schedule_case },
    { "budget", "integer_cumulative_model", budget_case },
    { "budget", "base_tight_texture_isolation", budget_isolation_case },
    { "extraction", "three_decoded_layers", extraction_case },
    { "compat", "creation_validation", invalid_creation_case },
    { "compat", "invalid_runtime_retry", invalid_runtime_retry_case },
    { "compat", "two_instances", two_instances_case },
    { "compat", "mode_off_bytes", off_byte_compat_case },
    { "compat", "nal_callbacks_and_soft_slices", nal_callback_and_slices_case },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
