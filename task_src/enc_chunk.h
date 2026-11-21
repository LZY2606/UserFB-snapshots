/**************************************************************************************
 * Low-latency chunked bitstream output for uavs3e.
 *
 * Incremental, row-granular streaming of the entropy re-write stage: raw
 * slice bytes are consumed as they become stable in the encoder's bitstream
 * buffer and the AVS3 emulation-prevention transform (see bs_demulate()) is
 * applied on the fly, so finished prefixes can be handed to the user
 * callback while later CTU rows are still being encoded.
 *
 * The transform is causal: every output byte produced by
 * enc_chunk_stream_feed() is final. Only the very last byte of a picture may
 * be adjusted (stop-bit realignment), and that byte is produced inside
 * enc_chunk_stream_finish(), so nothing already delivered is ever revised.
 **************************************************************************************/

#ifndef _ENC_CHUNK_H_
#define _ENC_CHUNK_H_

#include "define.h"

typedef struct uavs3e_enc_chunk_stream_t {
    u8  *out;          /* staging buffer for demulated bytes (bs demulation scratch) */
    int  out_len;      /* demulated bytes produced so far (incl. the 4-byte unit header) */
    int  emitted;      /* bytes already handed to the chunk callback */
    int  zero_cnt;     /* consecutive zero bytes seen in the demulated domain */
    int  bits_offset;  /* bit shift introduced by two-bit emulation-prevention insertions */
    u8   prev;         /* previous raw byte fed to the stream */
} enc_chunk_stream_t;

/* COM_OK when the (refined) configuration supports chunked streaming output */
int  enc_chunk_cfg_supported(const enc_cfg_t *cfg);

/* start a new patch unit: emit the 00 00 01 <start_code> header verbatim */
void enc_chunk_stream_begin (enc_chunk_stream_t *cs, u8 *out, int start_code);
/* consume one raw slice byte, appending final demulated bytes to cs->out */
void enc_chunk_stream_feed  (enc_chunk_stream_t *cs, u8 raw);
/* flush the tail bits and apply the stop-bit realignment of the last byte */
void enc_chunk_stream_finish(enc_chunk_stream_t *cs);

#endif /* _ENC_CHUNK_H_ */
