////////////////////////////////////////////////////////////////////////////
//                           **** ADPCM-XQ ****                           //
//                  Xtreme Quality ADPCM Encoder/Decoder                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#ifndef ADPCMLIB_H_
#define ADPCMLIB_H_

#define NOISE_SHAPING_OFF       0       // flat noise (no shaping)
#define NOISE_SHAPING_STATIC    0x100   // static 1st-order shaping (configurable, highpass default)
#define NOISE_SHAPING_DYNAMIC   0x200   // dynamically tilted noise based on signal

#define LOOKAHEAD_DEPTH         0x0ff   // depth of search
#define LOOKAHEAD_EXHAUSTIVE    0x800   // full breadth of search (all branches taken)
#define LOOKAHEAD_NO_BRANCHING  0x400   // no branches taken (internal use only!)

#if defined(_MSC_VER) && _MSC_VER < 1600
typedef unsigned __int64 uint64_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int8 uint8_t;
typedef __int64 int64_t;
typedef __int32 int32_t;
typedef __int16 int16_t;
typedef __int8  int8_t;
#else
#include <stdint.h>
#endif

#include <stddef.h>

#ifdef __cplusplus 
extern "C" {
#endif

/* adpcm-lib.c */

int adpcm_sample_count_to_block_size (int sample_count, int num_chans, int bps);
int adpcm_block_size_to_sample_count (int block_size, int num_chans, int bps);
int adpcm_align_block_size (int block_size, int num_chans, int bps, int round_up);
void *adpcm_create_context (int num_channels, int sample_rate, int lookahead, int noise_shaping);
void adpcm_set_shaping_weight (void *p, double shaping_weight);
int adpcm_encode_block_ex (void *p, uint8_t *outbuf, size_t *outbufsize, const int16_t *inbuf, int inbufcount, int bps);
int adpcm_encode_block (void *p, uint8_t *outbuf, size_t *outbufsize, const int16_t *inbuf, int inbufcount);

/* Result returned by adpcm_encode_adaptive(). The sse[] values are the per-channel
 * sums of squared errors of the actual reconstructed 16-bit PCM (sse[1] is zero
 * for mono sources).
 */

typedef struct {
    int bps;                    // selected code size in bits (2-5)
    size_t bytes;               // actual size of the encoded block in bytes
    uint64_t sse[2];            // per-channel sum of squared errors
} adpcm_adaptive_result;

int adpcm_encode_adaptive (void *ctx, uint8_t *out, size_t capacity, const int16_t *pcm, int frames,
    unsigned allowed_bps_mask, const uint64_t max_sse[2], adpcm_adaptive_result *result);
int adpcm_decode_block_ex (int16_t *outbuf, const uint8_t *inbuf, size_t inbufsize, int channels, int bps);
int adpcm_decode_block (int16_t *outbuf, const uint8_t *inbuf, size_t inbufsize, int channels);
void adpcm_free_context (void *p);

/* adpcm-dns.c */

void generate_dns_values (const int16_t *samples, int sample_count, int num_chans, int sample_rate,
    int16_t *values, int16_t min_value, int16_t last_value);

#ifdef __cplusplus 
}
#endif


#endif /* ADPCMLIB_H_ */
