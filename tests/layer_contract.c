/* Layer-contract regression test for nested Speex bitstreams.
 *
 * This test only exercises the public Speex API.  It encodes continuous
 * multi-frame wideband / ultra-wideband streams with speex_encode_int(), then
 * builds an *independent* low-layer reference bitstream with its own bit
 * reader: layer headers are parsed with the bit-length tables documented in
 * doc/layer_contract.md, raw payload bits are copied verbatim (no re-encoding,
 * no speex_mode_query length queries) and two persistent low-layer decoders
 * compare every int16 sample, every return status and the decoded frame
 * count.
 *
 * The test never compares the generated PCM against decoded output: the codec
 * is lossy, so only two decoders fed equivalent bitstreams are compared.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <speex/speex_callbacks.h>

#define LC_NFRAMES 12
#define LC_MAX_SAMPLES 640
#define LC_MAX_BYTES 8192
#define LC_MAX_BITS (LC_MAX_BYTES * 8)

/* Narrowband block sizes (bits), indexed by the 4-bit NB submode id.
 * Source: libspeex/modes.c, SpeexSubmode.bits_per_frame.  The values cover
 * the whole NB block *including* its 5-bit header (1 narrowband flag +
 * 4 submode bits). */
static const int lc_nb_bits[16] = {
    0, 43, 119, 160, 220, 300, 364, 492,
    79, 0, 0, 0, 0, 0, 0, 0
};

/* Sub-band (high layer) block sizes (bits), indexed by the 3-bit SB submode
 * id.  Source: libspeex/modes_wb.c (wb_submode1..4 bits_per_frame) and the
 * mirrored wb_skip_table in libspeex/nb_celp.c.  The values cover the whole
 * high block *including* its 4-bit header (1 wideband flag + 3 submode bits). */
static const int lc_sb_bits[8] = {0, 36, 112, 192, 352, 0, 0, 0};

#define LC_NB_HDR_BITS 5   /* 1 narrowband flag + 4 NB mode bits  */
#define LC_SB_HDR_BITS 4   /* 1 wideband flag   + 3 SB mode bits  */

enum {
    LC_MSG_NONE = 0,
    LC_MSG_IGNORE_16,      /* unregistered 16-bit max-bitrate request (id 10) */
    LC_MSG_CHAR_8          /* 8-bit CHAR message (id 8) with custom callback  */
};

enum {
    LC_KIND_TRUNC = 1,     /* decoder status / frame-count failure            */
    LC_KIND_PCM,           /* decoded samples differ                          */
    LC_KIND_RANGE,         /* parsed layer range outside the valid bits       */
    LC_KIND_TERM,          /* stream kept producing frames after terminator   */
    LC_KIND_CALLBACK       /* in-band callback ordering/content failure       */
};

static int lc_failures;

/* MSB-first bit reader over a raw byte buffer, independent of SpeexBits. */
typedef struct {
    const unsigned char *data;
    int nbits;
    int pos;
} lc_reader;

static int lc_read(lc_reader *r, int n)
{
    int v = 0;
    int i;
    if (r->pos + n > r->nbits)
        return -1;
    for (i = 0; i < n; i++) {
        int bit = (r->data[(r->pos) >> 3] >> (7 - (r->pos & 7))) & 1;
        v = (v << 1) | bit;
        r->pos++;
    }
    return v;
}

/* Bit intervals retained for every frame / layer / in-band message. */
typedef struct {
    int frame_start, frame_end;
    int msg_start, msg_end;          /* -1 when the frame carries no message */
    int nb_start, nb_end;
    int wb_start, wb_end;            /* -1 when the layer is absent          */
    int uwb_start, uwb_end;
    int nb_mode, wb_submode, uwb_submode;
} lc_frame_rec;

typedef struct {
    lc_frame_rec frame[LC_NFRAMES];
    int valid_bits;                  /* payload bits, before the terminator  */
    unsigned char bytes[LC_MAX_BYTES];
    int nbytes;
} lc_stream;

static void lc_fail(const char *group, int frame, const char *layer,
                    int bit, int kind, const char *detail)
{
    const char *k;
    lc_failures++;
    switch (kind) {
    case LC_KIND_TRUNC:     k = "TRUNC"; break;
    case LC_KIND_PCM:       k = "PCM"; break;
    case LC_KIND_RANGE:     k = "RANGE"; break;
    case LC_KIND_TERM:      k = "TERM"; break;
    case LC_KIND_CALLBACK:  k = "CALLBACK"; break;
    default:                k = "UNKNOWN"; break;
    }
    printf("FAIL group=%s frame=%d layer=%s bit=%d kind=%s %s\n",
           group, frame, layer, bit, k, detail);
}

/* Deterministic, low-amplitude PCM that varies with the absolute sample
 * index.  No sine wave: decoded output is never compared against this. */
static spx_int16_t lc_pcm_sample(long absolute_index)
{
    unsigned long x = (unsigned long)absolute_index;
    x = x * 1103515245UL + 12345UL;
    return (spx_int16_t)((int)(x % 2001UL) - 1000);
}

static void lc_pack_message(SpeexBits *bits, int msg_kind, int frame_index)
{
    /* In-band request block: narrowband flag (0) + request marker (14) +
     * 4-bit request id, then the payload, packed back-to-back with the audio
     * blocks (no byte padding). */
    speex_bits_pack(bits, 0, 1);
    speex_bits_pack(bits, 14, 4);
    if (msg_kind == LC_MSG_IGNORE_16) {
        speex_bits_pack(bits, SPEEX_INBAND_MAX_BITRATE, 4);
        speex_bits_pack(bits, 0x6D3B, 16);
    } else {
        speex_bits_pack(bits, SPEEX_INBAND_CHAR, 4);
        speex_bits_pack(bits, ('A' + frame_index) & 0xFF, 8);
    }
}

/* Encode one complete multi-frame stream and record every layer interval. */
static int lc_build_stream(const char *group, int src_mode, int changing,
                           int msg_kind, int high_layers, lc_stream *out)
{
    const SpeexMode *mode = speex_lib_get_mode(src_mode);
    void *enc;
    SpeexBits bits;
    spx_int16_t in[LC_MAX_SAMPLES];
    int frame_size = 0, quality = 5, zero = 0;
    int f;
    int enc_end[LC_NFRAMES];
    const int quality_cycle[3] = {1, 3, 8};
    lc_reader r;
    int pos = 0;

    enc = speex_encoder_init(mode);
    if (!enc)
        return -1;
    speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
    speex_encoder_ctl(enc, SPEEX_SET_VBR, &zero);
    speex_encoder_ctl(enc, SPEEX_SET_VAD, &zero);
    speex_encoder_ctl(enc, SPEEX_SET_DTX, &zero);
    speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &quality);

    speex_bits_init(&bits);
    for (f = 0; f < LC_NFRAMES; f++) {
        int i;
        if (changing) {
            quality = quality_cycle[f % 3];
            speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &quality);
        }
        for (i = 0; i < frame_size; i++)
            in[i] = lc_pcm_sample((long)f * frame_size + i);
        if (msg_kind != LC_MSG_NONE)
            lc_pack_message(&bits, msg_kind, f);
        if (speex_encode_int(enc, in, &bits) < 0) {
            lc_fail(group, f, "frame", bits.nbBits, LC_KIND_TRUNC,
                    "speex_encode_int failed");
            speex_bits_destroy(&bits);
            speex_encoder_destroy(enc);
            return -1;
        }
        enc_end[f] = bits.nbBits;
    }
    out->valid_bits = bits.nbBits;

    /* Independent parse of the finished stream with our own reader. */
    r.data = (const unsigned char *)bits.chars;
    r.nbits = bits.nbBits;
    r.pos = 0;
    for (f = 0; f < LC_NFRAMES; f++) {
        lc_frame_rec *rec = &out->frame[f];
        rec->msg_start = rec->msg_end = -1;
        rec->wb_start = rec->wb_end = -1;
        rec->uwb_start = rec->uwb_end = -1;
        rec->frame_start = pos;

        if (msg_kind != LC_MSG_NONE) {
            int flag, marker, id;
            rec->msg_start = pos;
            flag = lc_read(&r, 1);
            marker = lc_read(&r, 4);
            id = lc_read(&r, 4);
            if (flag != 0 || marker != 14 ||
                (msg_kind == LC_MSG_IGNORE_16 && id != SPEEX_INBAND_MAX_BITRATE) ||
                (msg_kind == LC_MSG_CHAR_8 && id != SPEEX_INBAND_CHAR)) {
                lc_fail(group, f, "msg", rec->msg_start, LC_KIND_RANGE,
                        "in-band message header mismatch");
                goto fail;
            }
            if (lc_read(&r, msg_kind == LC_MSG_IGNORE_16 ? 16 : 8) < 0)
                goto oob;
            rec->msg_end = r.pos;
            pos = r.pos;
        }

        /* Narrowband layer: 0 flag + 4-bit mode, then the mode payload. */
        rec->nb_start = r.pos;
        if (lc_read(&r, 1) != 0) {
            lc_fail(group, f, "nb", rec->nb_start, LC_KIND_RANGE,
                    "narrowband flag must be 0");
            goto fail;
        }
        rec->nb_mode = lc_read(&r, 4);
        if (rec->nb_mode < 1 || rec->nb_mode > 8 ||
            lc_nb_bits[rec->nb_mode] == 0) {
            lc_fail(group, f, "nb", rec->nb_start + 1, LC_KIND_RANGE,
                    "invalid narrowband submode id");
            goto fail;
        }
        r.pos = rec->nb_start + lc_nb_bits[rec->nb_mode];
        rec->nb_end = r.pos;

        if (high_layers >= 1) {
            rec->wb_start = pos = r.pos;
            if (lc_read(&r, 1) != 1) {
                lc_fail(group, f, "wb", rec->wb_start, LC_KIND_RANGE,
                        "wideband flag must be 1");
                goto fail;
            }
            rec->wb_submode = lc_read(&r, 3);
            if (lc_sb_bits[rec->wb_submode] == 0) {
                lc_fail(group, f, "wb", rec->wb_start + 1, LC_KIND_RANGE,
                        "invalid wideband submode id");
                goto fail;
            }
            r.pos = rec->wb_start + lc_sb_bits[rec->wb_submode];
            rec->wb_end = r.pos;
        }

        if (high_layers >= 2) {
            rec->uwb_start = pos = r.pos;
            if (lc_read(&r, 1) != 1) {
                lc_fail(group, f, "uwb", rec->uwb_start, LC_KIND_RANGE,
                        "ultra-wideband flag must be 1");
                goto fail;
            }
            rec->uwb_submode = lc_read(&r, 3);
            if (lc_sb_bits[rec->uwb_submode] == 0) {
                lc_fail(group, f, "uwb", rec->uwb_start + 1, LC_KIND_RANGE,
                        "invalid ultra-wideband submode id");
                goto fail;
            }
            r.pos = rec->uwb_start + lc_sb_bits[rec->uwb_submode];
            rec->uwb_end = r.pos;
        }

        rec->frame_end = r.pos;
        pos = r.pos;

        /* Generator self-check: every per-frame/layer range must lie inside
         * the bits actually emitted after this frame's encode call. */
        if (rec->frame_end != enc_end[f] || rec->frame_end > out->valid_bits ||
            rec->nb_end > enc_end[f] ||
            (rec->wb_end >= 0 && rec->wb_end > enc_end[f]) ||
            (rec->uwb_end >= 0 && rec->uwb_end > enc_end[f]) ||
            (rec->msg_end >= 0 && rec->msg_end > enc_end[f])) {
            lc_fail(group, f, "frame", rec->frame_end, LC_KIND_RANGE,
                    "layer range outside the valid encoded bits");
            goto fail;
        }
    }
    if (r.pos != out->valid_bits) {
        lc_fail(group, LC_NFRAMES - 1, "frame", r.pos, LC_KIND_RANGE,
                "parsed length does not match the encoded stream length");
        goto fail;
    }

    speex_bits_insert_terminator(&bits);
    out->nbytes = speex_bits_write(&bits, (char *)out->bytes, LC_MAX_BYTES);
    speex_bits_destroy(&bits);
    speex_encoder_destroy(enc);
    return 0;

oob:
    lc_fail(group, f, "frame", r.pos, LC_KIND_RANGE,
            "reader ran past the valid encoded bits");
fail:
    speex_bits_destroy(&bits);
    speex_encoder_destroy(enc);
    return -1;
}

static void lc_copy_bits(SpeexBits *dst, const unsigned char *src,
                         int from, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        int bit = (src[(from + i) >> 3] >> (7 - ((from + i) & 7))) & 1;
        speex_bits_pack(dst, bit, 1);
    }
}

/* Build an independent low-layer projection by copying raw payload bits.
 * No re-encoding happens here; high layers are simply omitted. */
static int lc_project(const char *group, const lc_stream *src, lc_stream *dst,
                      int keep_wb, int keep_uwb, int keep_messages)
{
    SpeexBits bits;
    int f;

    memset(dst, 0, sizeof(*dst));
    speex_bits_init(&bits);
    for (f = 0; f < LC_NFRAMES; f++) {
        const lc_frame_rec *rec = &src->frame[f];
        if (keep_messages && rec->msg_start >= 0)
            lc_copy_bits(&bits, src->bytes, rec->msg_start,
                         rec->msg_end - rec->msg_start);
        lc_copy_bits(&bits, src->bytes, rec->nb_start,
                     rec->nb_end - rec->nb_start);
        if (keep_wb && rec->wb_start >= 0)
            lc_copy_bits(&bits, src->bytes, rec->wb_start,
                         rec->wb_end - rec->wb_start);
        if (keep_uwb && rec->uwb_start >= 0)
            lc_copy_bits(&bits, src->bytes, rec->uwb_start,
                         rec->uwb_end - rec->uwb_start);
    }
    dst->valid_bits = bits.nbBits;
    speex_bits_insert_terminator(&bits);
    dst->nbytes = speex_bits_write(&bits, (char *)dst->bytes, LC_MAX_BYTES);
    if (dst->nbytes <= 0) {
        lc_fail(group, 0, "frame", 0, LC_KIND_RANGE,
                "empty projected bitstream");
        speex_bits_destroy(&bits);
        return -1;
    }
    speex_bits_destroy(&bits);
    return 0;
}

typedef struct {
    int count;
    int chars[LC_NFRAMES + 1];
} lc_cb_log;

/* Custom CHAR handler: reads exactly the 8-bit character and records order. */
static int lc_char_callback(SpeexBits *bits, void *state, void *data)
{
    lc_cb_log *log = (lc_cb_log *)data;
    unsigned int ch = speex_bits_unpack_unsigned(bits, 8);
    (void)state;
    if (log->count <= LC_NFRAMES)
        log->chars[log->count] = (int)ch;
    log->count++;
    return 0;
}

/* Two persistent decoders with identical initial settings compare the full
 * high-layer stream against the independently extracted low-layer stream. */
static void lc_compare(const char *group, const lc_stream *full,
                       const lc_stream *ref, int decoder_mode,
                       int register_callback,
                       lc_cb_log *log_full, lc_cb_log *log_ref)
{
    const SpeexMode *mode = speex_lib_get_mode(decoder_mode);
    void *dec_full = speex_decoder_init(mode);
    void *dec_ref = speex_decoder_init(mode);
    SpeexBits bf, br;
    spx_int16_t out_full[LC_MAX_SAMPLES], out_ref[LC_MAX_SAMPLES];
    int frame_size = 0, enh_off = 0;
    int frames_full = 0, frames_ref = 0;
    int f, i;

    if (!dec_full || !dec_ref) {
        lc_fail(group, 0, "frame", 0, LC_KIND_TRUNC, "decoder init failed");
        return;
    }
    speex_decoder_ctl(dec_full, SPEEX_GET_FRAME_SIZE, &frame_size);
    /* Enhancement is explicitly disabled on both decoders. */
    speex_decoder_ctl(dec_full, SPEEX_SET_ENH, &enh_off);
    speex_decoder_ctl(dec_ref, SPEEX_SET_ENH, &enh_off);

    if (register_callback) {
        SpeexCallback cb;
        memset(&cb, 0, sizeof(cb));
        cb.callback_id = SPEEX_INBAND_CHAR;
        cb.func = lc_char_callback;
        cb.data = log_full;
        speex_decoder_ctl(dec_full, SPEEX_SET_HANDLER, &cb);
        cb.data = log_ref;
        speex_decoder_ctl(dec_ref, SPEEX_SET_HANDLER, &cb);
    }

    speex_bits_init(&bf);
    speex_bits_init(&br);
    speex_bits_read_from(&bf, (const char *)full->bytes, full->nbytes);
    speex_bits_read_from(&br, (const char *)ref->bytes, ref->nbytes);

    for (f = 0; f < LC_NFRAMES; f++) {
        int before_full = register_callback ? log_full->count : 0;
        int before_ref = register_callback ? log_ref->count : 0;
        int pos_full = speex_bits_remaining(&bf);
        int pos_ref = speex_bits_remaining(&br);
        int sf = speex_decode_int(dec_full, &bf, out_full);
        int sr = speex_decode_int(dec_ref, &br, out_ref);

        if (sf != 0 || sr != 0) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "truncated/corrupt stream status full=%d ref=%d", sf, sr);
            lc_fail(group, f, "frame",
                    sf != 0 ? pos_full : pos_ref, LC_KIND_TRUNC, detail);
            /* A desynced decoder cannot produce comparable audio. */
            goto done;
        }
        frames_full++;
        frames_ref++;

        for (i = 0; i < frame_size; i++) {
            if (out_full[i] != out_ref[i]) {
                char detail[160];
                snprintf(detail, sizeof(detail),
                         "first sample mismatch sample=%d full=%d ref=%d",
                         i, (int)out_full[i], (int)out_ref[i]);
                lc_fail(group, f, "pcm",
                        speex_bits_remaining(&bf), LC_KIND_PCM, detail);
                break;
            }
        }

        if (register_callback) {
            if (log_full->count != before_full + 1 ||
                log_ref->count != before_ref + 1) {
                char detail[160];
                snprintf(detail, sizeof(detail),
                         "callback must fire exactly once per frame full=%d ref=%d",
                         log_full->count - before_full,
                         log_ref->count - before_ref);
                lc_fail(group, f, "msg", 0, LC_KIND_CALLBACK, detail);
            } else if (log_full->chars[before_full] != ('A' + f) ||
                       log_ref->chars[before_ref] != ('A' + f)) {
                char detail[160];
                snprintf(detail, sizeof(detail),
                         "callback char/order mismatch full=0x%02x ref=0x%02x",
                         log_full->chars[before_full] & 0xFF,
                         log_ref->chars[before_ref] & 0xFF);
                lc_fail(group, f, "msg", 0, LC_KIND_CALLBACK, detail);
            }
        }
    }

    if (frames_full != LC_NFRAMES || frames_ref != LC_NFRAMES) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "decoded frame count full=%d ref=%d expected=%d",
                 frames_full, frames_ref, LC_NFRAMES);
        lc_fail(group, LC_NFRAMES - 1, "frame", 0, LC_KIND_TRUNC, detail);
    }

    /* After the terminator the next call must end the stream instead of
     * synthesizing an extra silence frame. */
    {
        int sf = speex_decode_int(dec_full, &bf, out_full);
        int sr = speex_decode_int(dec_ref, &br, out_ref);
        if (sf == 0 || sr == 0) {
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "extra frame produced after terminator status full=%d ref=%d",
                     sf, sr);
            lc_fail(group, LC_NFRAMES, "term", 0, LC_KIND_TERM, detail);
        }
    }

done:
    speex_bits_destroy(&bf);
    speex_bits_destroy(&br);
    speex_decoder_destroy(dec_full);
    speex_decoder_destroy(dec_ref);
}

typedef struct {
    const char *name;
    int src_mode;            /* 1 = wideband, 2 = ultra-wideband */
    int decoder_mode;        /* 0 = narrowband, 1 = wideband      */
    int high_layers;         /* high blocks per source frame      */
    int changing;            /* fixed quality 5 vs 1/3/8 cycling  */
    int msg_kind;
    int keep_messages;       /* whether the projection keeps them */
    int register_callback;
} lc_group;

int main(void)
{
    static const lc_group groups[] = {
        {"wb-nb-fixed",       1, 0, 1, 0, LC_MSG_NONE,     0, 0},
        {"wb-nb-changing",    1, 0, 1, 1, LC_MSG_NONE,     0, 0},
        {"uwb-nb-fixed",      2, 0, 2, 0, LC_MSG_NONE,     0, 0},
        {"uwb-nb-changing",   2, 0, 2, 1, LC_MSG_NONE,     0, 0},
        {"uwb-wb-fixed",      2, 1, 2, 0, LC_MSG_NONE,     0, 0},
        {"uwb-wb-changing",   2, 1, 2, 1, LC_MSG_NONE,     0, 0},
        /* Unregistered 16-bit request: the default handler must skip it, so
         * the audio matches a message-free reference. */
        {"controls-ignored",  2, 0, 2, 0, LC_MSG_IGNORE_16, 0, 0},
        /* 8-bit CHAR request kept in the projection, with an ordered
         * callback registered on both decoders. */
        {"controls-callback", 2, 0, 2, 0, LC_MSG_CHAR_8,    1, 1}
    };
    int g;

    for (g = 0; g < (int)(sizeof(groups) / sizeof(groups[0])); g++) {
        const lc_group *grp = &groups[g];
        lc_stream full, ref;
        lc_cb_log log_full, log_ref;
        int failures_before = lc_failures;

        memset(&full, 0, sizeof(full));
        memset(&ref, 0, sizeof(ref));
        memset(&log_full, 0, sizeof(log_full));
        memset(&log_ref, 0, sizeof(log_ref));

        if (lc_build_stream(grp->name, grp->src_mode, grp->changing,
                            grp->msg_kind, grp->high_layers, &full) != 0)
            continue;
        if (lc_project(grp->name, &full, &ref,
                       grp->decoder_mode >= 1,       /* keep WB layer for WB decoder */
                       0,                            /* UWB top layer always stripped */
                       grp->keep_messages) != 0)
            continue;
        lc_compare(grp->name, &full, &ref, grp->decoder_mode,
                   grp->register_callback, &log_full, &log_ref);

        if (lc_failures == failures_before)
            printf("PASS %s\n", grp->name);
        else
            printf("GROUP FAILED: %s\n", grp->name);
    }

    if (lc_failures == 0)
        printf("LAYER_CONTRACT_OK groups=%d frames_per_group=%d\n",
               (int)(sizeof(groups) / sizeof(groups[0])), LC_NFRAMES);
    else
        printf("LAYER_CONTRACT_FAILURES=%d\n", lc_failures);
    return lc_failures ? 1 : 0;
}
