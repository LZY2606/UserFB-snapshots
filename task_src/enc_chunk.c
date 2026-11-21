/**************************************************************************************
 * Low-latency chunked bitstream output for uavs3e (see task_src/enc_chunk.h).
 **************************************************************************************/

#include "define.h"
#include "task_src/enc_chunk.h"

/* Test observation hook, called from enc_lcu_row() in test builds only.
 * Defined unconditionally so the library never depends on the test driver;
 * the call site itself is compiled only under UAVS3E_TASK_TESTING. */
void (*uavs3e_test_row_observe)(long long poc, int lcu_y) = NULL;

int enc_chunk_cfg_supported(const enc_cfg_t *cfg)
{
    return cfg->bit_depth_input    == 8            &&
           cfg->bit_depth_internal == 8            &&
           cfg->chroma_format      == 1            && /* I420 only */
           cfg->rc_type            == RC_TYPE_NULL && /* fixed QP  */
           cfg->max_b_frames       == 0            &&
           cfg->frm_threads        == 1            &&
           cfg->wpp_threads        == 1            &&
           cfg->adaptive_dqp       == 0            &&
           cfg->alf_enable         == 0            &&
           cfg->sao_enable         == 0            &&
           cfg->use_pic_sign       == 0            ? COM_OK : COM_ERR_UNSUPPORTED;
}

void enc_chunk_stream_begin(enc_chunk_stream_t *cs, u8 *out, int start_code)
{
    cs->out         = out;
    out[0]          = 0x00;
    out[1]          = 0x00;
    out[2]          = 0x01;
    out[3]          = (u8)start_code;
    cs->out_len     = 4;
    cs->emitted     = 0;
    cs->zero_cnt    = (start_code == 0x00) ? 1 : 0;
    cs->bits_offset = 0;
    cs->prev        = (u8)start_code;
}

void enc_chunk_stream_feed(enc_chunk_stream_t *cs, u8 raw)
{
    /* mirrors the inner loop of bs_demulate() for a single input byte */
    for (;;) {
        u8 c = (u8)((cs->prev << (8 - cs->bits_offset)) | (raw >> cs->bits_offset));

        if (cs->zero_cnt >= 2 && (c & 0xfc) == 0) {
            /* two zero bytes followed by 000000xx: insert the two bits '10' */
            cs->out[cs->out_len++] = 0x02;
            cs->bits_offset += 2;
            cs->zero_cnt = 0;
            if (cs->bits_offset >= 8) {
                cs->bits_offset = 0;
                continue; /* reprocess the same input byte, fully consumed */
            }
            break;
        }
        cs->out[cs->out_len++] = c;
        cs->zero_cnt = (c == 0) ? cs->zero_cnt + 1 : 0;
        break;
    }
    cs->prev = raw;
}

void enc_chunk_stream_finish(enc_chunk_stream_t *cs)
{
    /* mirrors the tail of bs_demulate(): the byte produced here is the only
     * one the stop-bit realignment may touch, and it has not been delivered */
    if (cs->bits_offset != 0) {
        cs->out[cs->out_len++] = (u8)(cs->prev << (8 - cs->bits_offset));
    }

    int stuffbitnum = 8 - cs->bits_offset % 8;
    if (stuffbitnum == 8) {
        stuffbitnum = 0;
    }
    if (stuffbitnum > 0) {
        u8 *last = &cs->out[cs->out_len - 1];
        *last = (u8)(((*last) & ((~(1 << (8 - stuffbitnum))) << stuffbitnum)) | (1 << (stuffbitnum - 1)));
    }
}
