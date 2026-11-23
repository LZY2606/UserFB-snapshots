/*
 * Recovery point (SEI payload type 6) output gating tests.
 *
 * The bit writers from task_support/fixture.h are reused and extended here;
 * the production baseline fixture itself is untouched. Every stream is built
 * from auditable C bit writers, no external media files.
 */

#include "H264SwDecApi.h"
#include "task_support/check.h"
#include "task_support/fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* local stream builders                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned pocType;    /* 0 or 2 */
    unsigned widthMB;
    unsigned heightMB;
    unsigned frameBits;  /* log2(MaxFrameNum) */
    unsigned maxFrameNum;
} rv_cfg;

static size_t rv_sps(unsigned char *out, const rv_cfg *cfg)
{
    task_bits b = {{0}, 0};
    ts_put(&b, 66, 8);             /* profile_idc = Baseline */
    ts_put(&b, 0, 8);              /* constraint flags + reserved */
    ts_put(&b, 10, 8);             /* level_idc */
    ts_ue(&b, 0);                  /* sps id */
    ts_ue(&b, cfg->frameBits - 4); /* log2_max_frame_num_minus4 */
    ts_ue(&b, cfg->pocType);       /* pic_order_cnt_type */
    if (cfg->pocType == 0)
        ts_ue(&b, 0);              /* log2_max_pic_order_cnt_lsb_minus4 */
    ts_ue(&b, 1);                  /* num_ref_frames */
    ts_put(&b, 0, 1);              /* gaps_in_frame_num_allowed_flag */
    ts_ue(&b, cfg->widthMB - 1);
    ts_ue(&b, cfg->heightMB - 1);
    ts_put(&b, 1, 1);              /* frame_mbs_only_flag */
    ts_put(&b, 0, 1);              /* direct_8x8_inference_flag */
    ts_put(&b, 0, 1);              /* frame_cropping_flag */
    ts_put(&b, 0, 1);              /* vui_parameters_present_flag */
    ts_tail(&b);
    return ts_nal(&b, 0x67, out);
}

/* redundant_pic_cnt_present_flag is the only knob over the baseline PPS */
static size_t rv_pps(unsigned char *out, unsigned redundantPresent)
{
    task_bits b = {{0}, 0};
    ts_ue(&b, 0);                  /* pps id */
    ts_ue(&b, 0);                  /* sps id */
    ts_put(&b, 0, 1);              /* entropy_coding_mode_flag */
    ts_put(&b, 0, 1);              /* pic_order_present_flag */
    ts_ue(&b, 0);                  /* num_slice_groups_minus1 */
    ts_ue(&b, 0);                  /* num_ref_idx_l0_active_minus1 */
    ts_ue(&b, 0);                  /* num_ref_idx_l1_active_minus1 */
    ts_put(&b, 0, 1);              /* weighted_pred_flag */
    ts_put(&b, 0, 2);              /* weighted_bipred_idc */
    ts_se(&b, 0);                  /* pic_init_qp_minus26 */
    ts_se(&b, 0);                  /* pic_init_qs_minus26 */
    ts_se(&b, 0);                  /* chroma_qp_index_offset */
    ts_put(&b, 1, 1);              /* deblocking_filter_control_present */
    ts_put(&b, 0, 1);              /* constrained_intra_pred_flag */
    ts_put(&b, redundantPresent, 1);
    ts_tail(&b);
    return ts_nal(&b, 0x68, out);
}

typedef struct
{
    unsigned frameNum;
    int idr;
    int ref;
    unsigned slices;      /* number of primary slices */
    unsigned mbCount;     /* macroblocks in the picture */
    unsigned pocType;
    unsigned frameBits;
    unsigned redundantPresent; /* PPS carries redundant_pic_cnt_present */
    unsigned redundantCnt;     /* redundant_pic_cnt for every slice */
} rv_pic;

static void rv_pic_slice(task_bits *b, const rv_pic *p, unsigned firstMb,
    unsigned mbInSlice)
{
    ts_ue(b, firstMb);
    ts_ue(b, p->idr ? 2 : 0);    /* I for IDR, P otherwise */
    ts_ue(b, 0);                 /* pps id */
    ts_put(b, p->frameNum, p->frameBits);
    if (p->idr)
    {
        ts_ue(b, 0);             /* idr_pic_id */
        if (p->pocType == 0)
            ts_put(b, 0, p->frameBits); /* pic_order_cnt_lsb */
        if (p->redundantPresent)
            ts_ue(b, p->redundantCnt);
        ts_put(b, 0, 1);         /* no_output_of_prior_pics_flag */
        ts_put(b, 0, 1);         /* long_term_reference_flag */
    }
    else
    {
        if (p->pocType == 0)
            ts_put(b, p->frameNum, p->frameBits);
        if (p->redundantPresent)
            ts_ue(b, p->redundantCnt);
        ts_put(b, 0, 1);         /* num_ref_idx_active_override_flag */
        ts_put(b, 0, 1);         /* ref_pic_list_reordering_flag_l0 */
        if (p->ref)
            ts_put(b, 0, 1);     /* adaptive_ref_pic_marking_mode_flag */
    }
    ts_se(b, 0);                 /* slice_qp_delta */
    ts_ue(b, 1);                 /* disable_deblocking_filter_idc = 1 */

    if (p->idr)
    {
        unsigned i, j;
        for (j = 0; j < mbInSlice; j++)
        {
            ts_ue(b, 25);        /* mb_type I_PCM */
            ts_align(b);         /* pcm_alignment_zero_bit */
            for (i = 0; i < 384; i++)
                ts_put(b, 128, 8);
        }
    }
    else
    {
        ts_ue(b, mbInSlice);     /* skip_run covers the whole slice */
    }
    ts_tail(b);
}

static size_t rv_picture(unsigned char *out, const rv_pic *p)
{
    task_bits b = {{0}, 0};
    unsigned s;
    unsigned mbPerSlice = p->mbCount / p->slices;
    unsigned header;

    for (s = 0; s < p->slices; s++)
        rv_pic_slice(&b, p, s * mbPerSlice, mbPerSlice);

    if (p->idr)
        header = 0x65;
    else if (p->ref)
        header = 0x41;
    else
        header = 0x01;           /* nal_ref_idc == 0 */
    return ts_nal(&b, header, out);
}

/* one SEI payload descriptor; raw payload bytes are given explicitly so that
 * both legal and deliberately damaged NALs can be built. declared payload
 * size is encoded from typeExt/sizeExt/size, while dataBytes (when nonzero)
 * controls how many payload bytes are actually written, allowing truncated
 * NALs. When dataBytes is zero the number of written bytes equals size. */
typedef struct
{
    unsigned type;
    unsigned size;
    const unsigned char *data;
    unsigned typeExt;   /* extra 0xFF bytes preceding the type byte */
    unsigned sizeExt;   /* extra 0xFF bytes preceding the size byte */
    unsigned dataBytes; /* actual bytes written; 0 means use size */
} rv_sei_pl;

static size_t rv_sei(unsigned char *out, const rv_sei_pl *pl, unsigned count,
    int writeTrailing)
{
    task_bits b = {{0}, 0};
    unsigned i, j;
    unsigned writeN;

    for (i = 0; i < count; i++)
    {
        for (j = 0; j < pl[i].typeExt; j++)
            ts_put(&b, 0xFF, 8);
        ts_put(&b, pl[i].type & 0xFF, 8);
        for (j = 0; j < pl[i].sizeExt; j++)
            ts_put(&b, 0xFF, 8);
        ts_put(&b, pl[i].size & 0xFF, 8);
        writeN = pl[i].dataBytes ? pl[i].dataBytes : pl[i].size;
        for (j = 0; j < writeN && pl[i].data; j++)
            ts_put(&b, pl[i].data[j], 8);
    }
    if (writeTrailing)
        ts_tail(&b);
    else if (b.bits & 7)
        ts_align(&b);
    return ts_nal(&b, 0x06, out);
}

/* build a legal recovery_point payload: ue(cnt) exact broken changing */
static size_t rv_recovery_payload(unsigned char *out, unsigned cnt,
    unsigned exactMatch, unsigned brokenLink, unsigned changing)
{
    task_bits b = {{0}, 0};
    ts_ue(&b, cnt);
    ts_put(&b, exactMatch, 1);
    ts_put(&b, brokenLink, 1);
    ts_put(&b, changing, 2);
    ts_tail(&b);
    memcpy(out, b.bytes, (b.bits + 7) / 8);
    return (b.bits + 7) / 8;
}

/* ------------------------------------------------------------------ */
/* decoder helpers                                                    */
/* ------------------------------------------------------------------ */

#define RV_BUF 65536

static const rv_cfg CFG_1MB_POC2 = {2, 1, 1, 4, 16};
static const rv_cfg CFG_1MB_POC0 = {0, 1, 1, 4, 16};
static const rv_cfg CFG_2MB_POC0 = {0, 2, 1, 4, 16};

static H264SwDecRet rv_feed(H264SwDecInst dec, const unsigned char *bytes,
    size_t len)
{
    H264SwDecInput in;
    H264SwDecOutput out;
    H264SwDecRet ret = H264SWDEC_OK;
    unsigned tries = 0;

    in.pStream = (u8 *)bytes;
    in.dataLen = (u32)len;
    in.picId = 0;
    in.intraConcealmentMethod = 0;

    while (in.dataLen)
    {
        size_t used;
        ret = H264SwDecDecode(dec, &in, &out);
        if (ret < 0 || !out.pStrmCurrPos)
            return ret;
        used = (size_t)(out.pStrmCurrPos - in.pStream);
        if (used > in.dataLen || ++tries > 32)
            return H264SWDEC_STRM_ERR;
        in.pStream += used;
        in.dataLen -= (u32)used;
    }
    return ret;
}

typedef struct
{
    u32 picId;
    u32 isIdr;
    unsigned char yuv[768]; /* up to 2 macroblocks */
} rv_out_pic;

typedef struct
{
    rv_out_pic pics[40];
    unsigned count;
} rv_outputs;

static H264SwDecRet rv_drain(H264SwDecInst dec, unsigned eos,
    rv_outputs *outs, unsigned yuvSize)
{
    H264SwDecPicture pic;
    H264SwDecRet ret;

    while ((ret = H264SwDecNextPicture(dec, &pic, eos)) == H264SWDEC_PIC_RDY)
    {
        if (outs->count >= sizeof(outs->pics) / sizeof(outs->pics[0]))
            return H264SWDEC_STRM_ERR;
        outs->pics[outs->count].picId = pic.picId;
        outs->pics[outs->count].isIdr = pic.isIdrPicture;
        memcpy(outs->pics[outs->count].yuv, pic.pOutputPicture, yuvSize);
        outs->count++;
        if (!eos)
            break;
    }
    return ret;
}

static unsigned rv_state(H264SwDecInst dec)
{
    u32 state = 99;
    TASK_CHECK(H264SwDecGetRecoveryState(dec, &state) == H264SWDEC_OK);
    return state;
}

static H264SwDecInst rv_new(unsigned noReordering)
{
    H264SwDecInst dec = NULL;
    if (H264SwDecInit(&dec, noReordering) != H264SWDEC_OK)
        return NULL;
    return dec;
}

static int rv_feed_headers(H264SwDecInst dec, const rv_cfg *cfg,
    unsigned redundantPresent)
{
    unsigned char buf[RV_BUF];
    size_t n;
    n = rv_sps(buf, cfg);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    n = rv_pps(buf, redundantPresent);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    return 0;
}

static rv_pic rv_mk_pic(unsigned frameNum, int idr, int ref,
    const rv_cfg *cfg)
{
    rv_pic p;
    memset(&p, 0, sizeof(p));
    p.frameNum = frameNum;
    p.idr = idr;
    p.ref = ref;
    p.slices = 1;
    p.mbCount = cfg->widthMB * cfg->heightMB;
    p.pocType = cfg->pocType;
    p.frameBits = cfg->frameBits;
    return p;
}

static int rv_feed_pic(H264SwDecInst dec, const rv_pic *p, u32 picId)
{
    unsigned char buf[RV_BUF];
    H264SwDecInput in;
    H264SwDecOutput out;
    size_t n = rv_picture(buf, p);
    unsigned tries = 0;

    in.pStream = buf;
    in.dataLen = (u32)n;
    in.picId = picId;
    in.intraConcealmentMethod = 0;
    while (in.dataLen)
    {
        size_t used;
        H264SwDecRet ret = H264SwDecDecode(dec, &in, &out);
        TASK_CHECK(ret >= 0);
        TASK_CHECK(out.pStrmCurrPos != NULL);
        used = (size_t)(out.pStrmCurrPos - in.pStream);
        TASK_CHECK(used <= in.dataLen);
        TASK_CHECK(++tries <= 32);
        in.pStream += used;
        in.dataLen -= (u32)used;
    }
    return 0;
}

/* parameter sets only become active when the first slice picture is
 * decoded, so an IDR is required before BeginRecovery can succeed */
static int rv_feed_idr(H264SwDecInst dec, const rv_cfg *cfg,
    unsigned redundantPresent)
{
    rv_pic p = rv_mk_pic(0, 1, 1, cfg);
    p.redundantPresent = redundantPresent;
    return rv_feed_pic(dec, &p, 0);
}

/* ================================================================== */
/* sei group                                                          */
/* ================================================================== */

static int sei_valid_marker_transitions(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_state(dec) == 0);

    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    TASK_CHECK(rv_state(dec) == 1);

    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 2);

    /* bound picture frame 1, target frame 2 */
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 2);
    p = rv_mk_pic(2, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 2) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int sei_unsupported_combinations_ignored(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    /* exact_match_flag = 0 */
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 0, 0, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 1);

    /* changing_slice_group_idc = 1 */
    pl.size = (unsigned)rv_recovery_payload(payload, 0, 1, 0, 1);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 1);

    /* recovery_frame_cnt == MaxFrameNum is out of range */
    pl.size = (unsigned)rv_recovery_payload(payload, 16, 1, 0, 0);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 1);

    /* broken_link_flag = 1 is accepted */
    pl.size = (unsigned)rv_recovery_payload(payload, 0, 1, 1, 0);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 2);
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int sei_corrupt_marker_is_stream_error(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    /* truncated payload: declares 2 bytes but only one is present */
    pl.type = 6;
    pl.size = 2;
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    payload[0] = (unsigned char)rv_recovery_payload(payload, 0, 1, 0, 0);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) == H264SWDEC_STRM_ERR);
    TASK_CHECK(rv_state(dec) == 1);

    /* all-zero payload byte: exp-golomb code runs past the payload */
    payload[0] = 0x00;
    pl.size = 1;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) == H264SWDEC_STRM_ERR);
    TASK_CHECK(rv_state(dec) == 1);

    /* legal syntax bits but a zero payload alignment bit */
    payload[0] = 0xC0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) == H264SWDEC_STRM_ERR);
    TASK_CHECK(rv_state(dec) == 1);

    /* a valid marker still gets through afterwards */
    pl.size = (unsigned)rv_recovery_payload(payload, 0, 1, 0, 0);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 2);
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int sei_partial_marker_not_committed(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], good[16], bad[16];
    rv_sei_pl pl[2] = {{0}};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    /* first payload valid, second one truncated: the whole NAL must fail and
     * the first marker must not be committed */
    pl[0].type = 6;
    pl[0].size = (unsigned)rv_recovery_payload(good, 1, 1, 0, 0);
    pl[0].data = good;
    pl[0].typeExt = pl[0].sizeExt = 0;
    bad[0] = 0x00;
    pl[1].type = 6;
    pl[1].size = 1;
    pl[1].data = bad;
    pl[1].typeExt = pl[1].sizeExt = 0;
    n = rv_sei(buf, pl, 2, 1);
    TASK_CHECK(rv_feed(dec, buf, n) == H264SWDEC_STRM_ERR);
    TASK_CHECK(rv_state(dec) == 1);

    /* picture without a marker does not bind or complete anything */
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 1);

    H264SwDecRelease(dec);
    return 0;
}

static int sei_last_marker_wins_and_extensions(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], pay1[16], pay2[16], filler[257];
    rv_sei_pl pl[3] = {{0}};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    /* unknown filler payload exercising both extension byte loops:
     * payloadType  = 0xFF + 0x10 = 265, payloadSize = 0xFF + 0x02 = 257;
     * it must be skipped over its declared boundary */
    memset(filler, 0x5A, sizeof(filler));
    pl[0].type = 16;
    pl[0].size = 2;       /* final payloadSize byte, after one 0xFF -> 257 */
    pl[0].data = filler;
    pl[0].typeExt = 1;
    pl[0].sizeExt = 1;
    pl[0].dataBytes = 257;

    /* first recovery point: cnt 3, target would be frame 4 */
    pl[1].type = 6;
    pl[1].size = (unsigned)rv_recovery_payload(pay1, 3, 1, 0, 0);
    pl[1].data = pay1;
    pl[1].typeExt = 0;
    pl[1].sizeExt = 0;

    /* last recovery point wins: cnt 1 instead of 3 */
    pl[2].type = 6;
    pl[2].size = (unsigned)rv_recovery_payload(pay2, 1, 1, 0, 0);
    pl[2].data = pay2;
    pl[2].typeExt = 0;
    pl[2].sizeExt = 0;

    n = rv_sei(buf, pl, 3, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 2);

    /* bound frame 1; with cnt 1 the target is frame 2, with cnt 3 it would
     * be frame 4 */
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);
    TASK_CHECK(rv_state(dec) == 2);
    p = rv_mk_pic(2, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 2) == 0);
    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int sei_marker_ignored_after_completion(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 0, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 3);

    /* state 3 ignores further markers until BeginRecovery is called again */
    pl.size = (unsigned)rv_recovery_payload(payload, 2, 1, 0, 0);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 3);

    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    TASK_CHECK(rv_state(dec) == 1);

    H264SwDecRelease(dec);
    return 0;
}

/* ================================================================== */
/* window group                                                       */
/* ================================================================== */

static int window_count_zero_and_nonzero(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    /* count 0: the bound picture itself is the target */
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 0, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 3);

    /* re-arm and verify a nonzero count */
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    TASK_CHECK(rv_state(dec) == 1);
    pl.size = (unsigned)rv_recovery_payload(payload, 2, 1, 0, 0);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    p = rv_mk_pic(2, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 2) == 0);    TASK_CHECK(rv_state(dec) == 2);
    p = rv_mk_pic(3, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 3) == 0);    TASK_CHECK(rv_state(dec) == 2);
    p = rv_mk_pic(4, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 4) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int window_multi_slice_binds_once(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    /* two macroblocks, two slices per picture */
    TASK_CHECK(rv_feed_headers(dec, &CFG_2MB_POC0, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_2MB_POC0, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);

    p = rv_mk_pic(1, 0, 1, &CFG_2MB_POC0);
    p.slices = 2;
    /* first slice does not finish the picture */
    {
        rv_pic one = p;
        unsigned char sbuf[RV_BUF];
        task_bits bb = {{0}, 0};
        size_t sn;
        one.slices = 1;
        rv_pic_slice(&bb, &one, 0, 1);
        sn = ts_nal(&bb, 0x41, sbuf);
        TASK_CHECK(rv_feed(dec, sbuf, sn) >= 0);
    }
    TASK_CHECK(rv_state(dec) == 2);
    /* second slice completes the picture, marker binds to frame 1 */
    {
        unsigned char sbuf[RV_BUF];
        task_bits bb = {{0}, 0};
        size_t sn;
        rv_pic_slice(&bb, &p, 1, 1);
        sn = ts_nal(&bb, 0x41, sbuf);
        TASK_CHECK(rv_feed(dec, sbuf, sn) >= 0);
    }
    TASK_CHECK(rv_state(dec) == 2);

    /* target frame 2 completes recovery */
    p = rv_mk_pic(2, 0, 1, &CFG_2MB_POC0);
    p.slices = 2;
    n = rv_picture(buf, &p);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int window_wrap_with_non_reference(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    unsigned f;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    /* IDR 0 and reference frames 1..14 */
    p = rv_mk_pic(0, 1, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 0) == 0);    for (f = 1; f <= 14; f++)
    {
        p = rv_mk_pic(f, 0, 1, &CFG_1MB_POC2);
        TASK_CHECK(rv_feed_pic(dec, &p, f) == 0);    }

    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    /* bind frame 15, target (15+1) mod 16 = 0 */
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);

    p = rv_mk_pic(15, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 15) == 0);    TASK_CHECK(rv_state(dec) == 2);

    /* non-reference picture carrying the target number 0 must not finish */
    p = rv_mk_pic(0, 0, 0, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 16) == 0);    TASK_CHECK(rv_state(dec) == 2);

    /* the reference frame 0 completes the recovery */
    p = rv_mk_pic(0, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 17) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int window_replaced_marker_rebinds(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    /* first marker: bind frame 1, target frame 4 */
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 3, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 2);

    /* replacement marker while waiting: rebinds the next picture, frame 2,
     * target frame 3 */
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    p = rv_mk_pic(2, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 2) == 0);    TASK_CHECK(rv_state(dec) == 2);
    p = rv_mk_pic(3, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 3) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int window_idr_completes_recovery(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 5, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 2);

    /* IDR ends the recovery immediately and revokes the old marker */
    p = rv_mk_pic(0, 1, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 2) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

static int window_repeated_begin_and_busy(void)
{
    H264SwDecInst dec0 = NULL;
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    u32 state;

    TASK_CHECK(dec != NULL);

    /* null instance and null output pointer */
    TASK_CHECK(H264SwDecBeginRecovery(dec0) == H264SWDEC_PARAM_ERR);
    TASK_CHECK(H264SwDecGetRecoveryState(dec0, &state) ==
               H264SWDEC_PARAM_ERR);
    TASK_CHECK(H264SwDecGetRecoveryState(dec, NULL) ==
               H264SWDEC_PARAM_ERR);

    /* before parameter sets: headers not ready, state stays 0 */
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_HDRS_NOT_RDY);
    TASK_CHECK(rv_state(dec) == 0);

    TASK_CHECK(rv_feed_headers(dec, &CFG_2MB_POC0, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_2MB_POC0, 0) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    TASK_CHECK(rv_state(dec) == 1);

    /* repeatable: re-arming keeps state 1 and drops any pending marker */
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 2, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    TASK_CHECK(rv_state(dec) == 2);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    TASK_CHECK(rv_state(dec) == 1);

    /* half-finished picture (first slice of two) rejects the call */
    p = rv_mk_pic(1, 0, 1, &CFG_2MB_POC0);
    {
        task_bits bb = {{0}, 0};
        unsigned char sbuf[RV_BUF];
        size_t sn;
        rv_pic_slice(&bb, &p, 0, 1);
        sn = ts_nal(&bb, 0x41, sbuf);
        TASK_CHECK(rv_feed(dec, sbuf, sn) >= 0);
    }
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_PARAM_ERR);
    TASK_CHECK(rv_state(dec) == 1);
    {
        task_bits bb = {{0}, 0};
        unsigned char sbuf[RV_BUF];
        size_t sn;
        rv_pic_slice(&bb, &p, 1, 1);
        sn = ts_nal(&bb, 0x41, sbuf);
        TASK_CHECK(rv_feed(dec, sbuf, sn) >= 0);
    }

    H264SwDecRelease(dec);
    return 0;
}

static int window_redundant_slice_no_rebind(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;

    TASK_CHECK(dec != NULL);
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 1) == 0); /* redundant_pic_cnt present */
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 1) == 0);
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);

    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);

    /* primary picture frame 1 binds the marker */
    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    p.redundantPresent = 1;
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    TASK_CHECK(rv_state(dec) == 2);

    /* redundant slice of the same picture must not restart the binding */
    {
        task_bits bb = {{0}, 0};
        unsigned char sbuf[RV_BUF];
        size_t sn;
        rv_pic red = p;
        red.redundantCnt = 1;
        rv_pic_slice(&bb, &red, 0, 1);
        sn = ts_nal(&bb, 0x41, sbuf);
        TASK_CHECK(rv_feed(dec, sbuf, sn) >= 0);
    }
    TASK_CHECK(rv_state(dec) == 2);

    /* target remains frame 2 */
    p = rv_mk_pic(2, 0, 1, &CFG_1MB_POC2);
    p.redundantPresent = 1;
    TASK_CHECK(rv_feed_pic(dec, &p, 2) == 0);    TASK_CHECK(rv_state(dec) == 3);

    H264SwDecRelease(dec);
    return 0;
}

/* ================================================================== */
/* output group                                                       */
/* ================================================================== */

static const rv_out_pic *rv_find(const rv_outputs *outs, u32 picId)
{
    unsigned i;
    for (i = 0; i < outs->count; i++)
        if (outs->pics[i].picId == picId)
            return &outs->pics[i];
    return NULL;
}

/* reordering DPB (noOutputReordering=0), POC type 0: only target and later
 * pictures may be delivered, EOS flush included */
static int output_gating_and_eos(void)
{
    H264SwDecInst dec = rv_new(0);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    rv_outputs outs;
    unsigned f;
    const unsigned yuvSize = 384;

    TASK_CHECK(dec != NULL);
    memset(&outs, 0, sizeof(outs));
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC0, 0) == 0);
    p = rv_mk_pic(0, 1, 1, &CFG_1MB_POC0);
    TASK_CHECK(rv_feed_pic(dec, &p, 0) == 0);    for (f = 1; f <= 5; f++)
    {
        p = rv_mk_pic(f, 0, 1, &CFG_1MB_POC0);
        TASK_CHECK(rv_feed_pic(dec, &p, f) == 0);    }

    /* arm after frame 1, marker before frame 2 with cnt 2: target frame 4 */
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 2, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    for (f = 2; f <= 5; f++)
    {
        p = rv_mk_pic(f, 0, 1, &CFG_1MB_POC0);
        TASK_CHECK(rv_feed_pic(dec, &p, f) == 0);    }
    TASK_CHECK(rv_state(dec) == 3);

    /* nothing pulled while decoding; EOS drain must not leak frames 0..3 */
    TASK_CHECK(rv_drain(dec, 1, &outs, yuvSize) == H264SWDEC_OK);
    TASK_CHECK(outs.count == 2);
    TASK_CHECK(outs.pics[0].picId == 4);
    TASK_CHECK(outs.pics[1].picId == 5);

    H264SwDecRelease(dec);
    return 0;
}

/* noReordering mode: suppressed pictures never reach the immediate output */
static int output_no_reordering_mode(void)
{
    H264SwDecInst dec = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    rv_outputs outs;
    H264SwDecPicture pic;

    TASK_CHECK(dec != NULL);
    memset(&outs, 0, sizeof(outs));
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    p = rv_mk_pic(0, 1, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 0) == 0);    TASK_CHECK(rv_drain(dec, 0, &outs, 384) == H264SWDEC_PIC_RDY);

    /* arming revokes the already queued output as well */
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    TASK_CHECK(H264SwDecNextPicture(dec, &pic, 0) == H264SWDEC_OK);

    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);

    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 1) == 0);    /* frame 1 is suppressed */
    TASK_CHECK(H264SwDecNextPicture(dec, &pic, 0) == H264SWDEC_OK);
    TASK_CHECK(rv_state(dec) == 2);

    p = rv_mk_pic(2, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(dec, &p, 2) == 0);    TASK_CHECK(rv_state(dec) == 3);
    TASK_CHECK(H264SwDecNextPicture(dec, &pic, 0) == H264SWDEC_PIC_RDY);
    TASK_CHECK(pic.picId == 2);

    H264SwDecRelease(dec);
    return 0;
}

/* eligible YUV is byte identical to the ungated reference and normal mode is
 * unchanged */
static int output_pixels_match_reference(void)
{
    H264SwDecInst gated = rv_new(0);
    H264SwDecInst plain = rv_new(0);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    rv_outputs gouts, pouts;
    unsigned f;
    const unsigned yuvSize = 384;

    TASK_CHECK(gated != NULL && plain != NULL);
    memset(&gouts, 0, sizeof(gouts));
    memset(&pouts, 0, sizeof(pouts));

    TASK_CHECK(rv_feed_headers(gated, &CFG_1MB_POC0, 0) == 0);    TASK_CHECK(rv_feed_headers(plain, &CFG_1MB_POC0, 0) == 0);
    for (f = 0; f <= 5; f++)
    {
        p = rv_mk_pic(f, f == 0, 1, &CFG_1MB_POC0);
        TASK_CHECK(rv_feed_pic(gated, &p, f) == 0);        TASK_CHECK(rv_feed_pic(plain, &p, f) == 0);        TASK_CHECK(rv_drain(plain, 0, &pouts, yuvSize) == H264SWDEC_OK);
    }

    TASK_CHECK(H264SwDecBeginRecovery(gated) == H264SWDEC_OK);
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(gated, buf, n) >= 0);
    TASK_CHECK(rv_feed(plain, buf, n) >= 0);

    for (f = 1; f <= 4; f++)
    {
        p = rv_mk_pic(f, 0, 1, &CFG_1MB_POC0);
        TASK_CHECK(rv_feed_pic(gated, &p, f + 10) == 0);        TASK_CHECK(rv_feed_pic(plain, &p, f + 10) == 0);        TASK_CHECK(rv_drain(plain, 0, &pouts, yuvSize) == H264SWDEC_OK);
    }
    TASK_CHECK(rv_state(gated) == 3);

    TASK_CHECK(rv_drain(gated, 1, &gouts, yuvSize) == H264SWDEC_OK);
    TASK_CHECK(rv_drain(plain, 1, &pouts, yuvSize) == H264SWDEC_OK);

    /* gated run delivers the target and everything after it */
    TASK_CHECK(gouts.count == 3);
    TASK_CHECK(gouts.pics[0].picId == 12);
    TASK_CHECK(gouts.pics[1].picId == 13);
    TASK_CHECK(gouts.pics[2].picId == 14);

    /* every delivered picture is byte identical to the ungated run */
    for (f = 0; f < gouts.count; f++)
    {
        const rv_out_pic *ref = rv_find(&pouts, gouts.pics[f].picId);
        TASK_CHECK(ref != NULL);
        TASK_CHECK(memcmp(gouts.pics[f].yuv, ref->yuv, yuvSize) == 0);
    }

    H264SwDecRelease(gated);
    H264SwDecRelease(plain);
    return 0;
}

/* plain run without BeginRecovery keeps pixel, order and status behavior */
static int output_normal_mode_unchanged(void)
{
    H264SwDecInst dec = rv_new(0);
    rv_pic p;
    rv_outputs outs;
    unsigned f, i;

    TASK_CHECK(dec != NULL);
    memset(&outs, 0, sizeof(outs));
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC0, 0) == 0);
    TASK_CHECK(rv_state(dec) == 0);

    for (f = 0; f < 4; f++)
    {
        p = rv_mk_pic(f, f == 0, 1, &CFG_1MB_POC0);
        TASK_CHECK(rv_feed_pic(dec, &p, f) == 0);
    }
    TASK_CHECK(rv_drain(dec, 1, &outs, 384) == H264SWDEC_OK);
    TASK_CHECK(outs.count == 4);
    for (f = 0; f < 4; f++)
    {
        TASK_CHECK(outs.pics[f].picId == f);
        for (i = 0; i < 384; i++)
            TASK_CHECK(outs.pics[f].yuv[i] == 128);
    }
    TASK_CHECK(rv_state(dec) == 0);

    H264SwDecRelease(dec);
    return 0;
}

/* two instances never share recovery state or gating decisions */
static int output_instance_isolation(void)
{
    H264SwDecInst gated = rv_new(1);
    H264SwDecInst plain = rv_new(1);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    rv_outputs gouts, pouts;
    H264SwDecPicture pic;

    TASK_CHECK(gated != NULL && plain != NULL);
    memset(&gouts, 0, sizeof(gouts));
    memset(&pouts, 0, sizeof(pouts));

    TASK_CHECK(rv_feed_headers(gated, &CFG_1MB_POC2, 0) == 0);    TASK_CHECK(rv_feed_headers(plain, &CFG_1MB_POC2, 0) == 0);
    p = rv_mk_pic(0, 1, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(gated, &p, 0) == 0);    TASK_CHECK(rv_feed_pic(plain, &p, 0) == 0);    TASK_CHECK(rv_drain(plain, 0, &pouts, 384) == H264SWDEC_PIC_RDY);

    TASK_CHECK(H264SwDecBeginRecovery(gated) == H264SWDEC_OK);
    TASK_CHECK(rv_state(plain) == 0);

    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 1, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(gated, buf, n) >= 0);
    TASK_CHECK(rv_feed(plain, buf, n) >= 0);

    p = rv_mk_pic(1, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(gated, &p, 1) == 0);    TASK_CHECK(rv_feed_pic(plain, &p, 1) == 0);    TASK_CHECK(H264SwDecNextPicture(gated, &pic, 0) == H264SWDEC_OK);
    TASK_CHECK(rv_drain(plain, 0, &pouts, 384) == H264SWDEC_PIC_RDY);
    TASK_CHECK(rv_state(gated) == 2);
    TASK_CHECK(rv_state(plain) == 0);

    p = rv_mk_pic(2, 0, 1, &CFG_1MB_POC2);
    TASK_CHECK(rv_feed_pic(gated, &p, 2) == 0);    TASK_CHECK(rv_feed_pic(plain, &p, 2) == 0);    TASK_CHECK(H264SwDecNextPicture(gated, &pic, 0) == H264SWDEC_PIC_RDY);
    TASK_CHECK(pic.picId == 2);
    TASK_CHECK(rv_drain(plain, 0, &pouts, 384) == H264SWDEC_PIC_RDY);
    TASK_CHECK(rv_state(gated) == 3);

    TASK_CHECK(pouts.count == 3);
    TASK_CHECK(gouts.count == 0);

    H264SwDecRelease(gated);
    H264SwDecRelease(plain);
    return 0;
}

/* decode the whole gated sequence first, only then pull; pictures queued
 * before the target stay suppressed even though recovery has completed */
static int output_decode_first_pull_later(void)
{
    H264SwDecInst dec = rv_new(0);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    rv_outputs outs;
    unsigned f;

    TASK_CHECK(dec != NULL);
    memset(&outs, 0, sizeof(outs));
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC0, 0) == 0);
    for (f = 0; f <= 3; f++)
    {
        p = rv_mk_pic(f, f == 0, 1, &CFG_1MB_POC0);
        TASK_CHECK(rv_feed_pic(dec, &p, f) == 0);    }
    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 0, 1, 0, 0);
    pl.data = payload;
    pl.typeExt = pl.sizeExt = 0;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);
    for (f = 1; f <= 3; f++)
    {
        p = rv_mk_pic(f, 0, 1, &CFG_1MB_POC0);
        TASK_CHECK(rv_feed_pic(dec, &p, f + 10) == 0);    }
    TASK_CHECK(rv_state(dec) == 3);

    TASK_CHECK(rv_drain(dec, 1, &outs, 384) == H264SWDEC_OK);
    /* count 0: first picture after the marker is eligible, frames 0..3 that
     * were queued before BeginRecovery stay suppressed */
    TASK_CHECK(outs.count == 3);
    TASK_CHECK(outs.pics[0].picId == 11);
    TASK_CHECK(outs.pics[1].picId == 12);
    TASK_CHECK(outs.pics[2].picId == 13);

    H264SwDecRelease(dec);
    return 0;
}

/* long gated run in reordering mode: far more pictures than the DPB
 * reference capacity are decoded without any pull, exercising sliding
 * window marking while the gate is closed. No deadlock, no leak, and only
 * the target picture and later ones are delivered at EOS */
static int output_long_gated_run(void)
{
    H264SwDecInst dec = rv_new(0);
    unsigned char buf[RV_BUF], payload[16];
    rv_sei_pl pl = {0};
    size_t n;
    rv_pic p;
    rv_outputs outs;
    unsigned f;
    u32 frameNum;

    TASK_CHECK(dec != NULL);
    memset(&outs, 0, sizeof(outs));
    TASK_CHECK(rv_feed_headers(dec, &CFG_1MB_POC2, 0) == 0);
    TASK_CHECK(rv_feed_idr(dec, &CFG_1MB_POC2, 0) == 0);

    /* a few reference frames before arming */
    for (f = 1; f <= 3; f++)
    {
        p = rv_mk_pic(f, 0, 1, &CFG_1MB_POC2);
        TASK_CHECK(rv_feed_pic(dec, &p, f) == 0);
    }

    TASK_CHECK(H264SwDecBeginRecovery(dec) == H264SWDEC_OK);
    /* bind frame 4, target frame 6 */
    pl.type = 6;
    pl.size = (unsigned)rv_recovery_payload(payload, 2, 1, 0, 0);
    pl.data = payload;
    n = rv_sei(buf, &pl, 1, 1);
    TASK_CHECK(rv_feed(dec, buf, n) >= 0);

    /* decode frame numbers 4..15 then wrap 0..5, all without pulling */
    for (f = 0; f < 18; f++)
    {
        frameNum = (4u + f) % 16u;
        p = rv_mk_pic(frameNum, 0, 1, &CFG_1MB_POC2);
        TASK_CHECK(rv_feed_pic(dec, &p, 100 + f) == 0);
    }
    TASK_CHECK(rv_state(dec) == 3);

    TASK_CHECK(rv_drain(dec, 1, &outs, 384) == H264SWDEC_OK);
    /* the gate never leaks: every delivered picture is the target (frame 6,
     * picId 102) or a later one. Whether the target itself still survives
     * depends on the ordinary DPB sliding-window capacity, exactly as in an
     * ungated run; the long run mainly proves no deadlock or accounting
     * drift while the gate stays closed across DPB pressure and frame_num
     * wrap */
    TASK_CHECK(outs.count > 0);
    {
        unsigned i;
        for (i = 0; i < outs.count; i++)
            TASK_CHECK(outs.pics[i].picId >= 102);
    }

    H264SwDecRelease(dec);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    /* sei */
    {"sei", "valid_marker_transitions", sei_valid_marker_transitions},
    {"sei", "unsupported_combinations_ignored",
        sei_unsupported_combinations_ignored},
    {"sei", "corrupt_marker_is_stream_error",
        sei_corrupt_marker_is_stream_error},
    {"sei", "partial_marker_not_committed",
        sei_partial_marker_not_committed},
    {"sei", "last_marker_wins_and_extensions",
        sei_last_marker_wins_and_extensions},
    {"sei", "marker_ignored_after_completion",
        sei_marker_ignored_after_completion},

    /* window */
    {"window", "count_zero_and_nonzero", window_count_zero_and_nonzero},
    {"window", "multi_slice_binds_once", window_multi_slice_binds_once},
    {"window", "wrap_with_non_reference", window_wrap_with_non_reference},
    {"window", "replaced_marker_rebinds", window_replaced_marker_rebinds},
    {"window", "idr_completes_recovery", window_idr_completes_recovery},
    {"window", "repeated_begin_and_busy", window_repeated_begin_and_busy},
    {"window", "redundant_slice_no_rebind",
        window_redundant_slice_no_rebind},

    /* output */
    {"output", "gating_and_eos", output_gating_and_eos},
    {"output", "no_reordering_mode", output_no_reordering_mode},
    {"output", "pixels_match_reference", output_pixels_match_reference},
    {"output", "normal_mode_unchanged", output_normal_mode_unchanged},
    {"output", "instance_isolation", output_instance_isolation},
    {"output", "decode_first_pull_later", output_decode_first_pull_later},
    {"output", "long_gated_run", output_long_gated_run},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
