#ifndef __CONCEAL_H__
#define __CONCEAL_H__

#include "neaacdec.h"
#include "structs.h"

#define CONCEAL_ERR_NO_HISTORY 35
#define CONCEAL_ERR_UNSUPPORTED 36

void conceal_reinit(NeAACDecStruct *hDecoder);
void conceal_post_seek_reset(NeAACDecStruct *hDecoder);
void conceal_decode_begin(NeAACDecStruct *hDecoder);
void conceal_capture_spectrum(NeAACDecStruct *hDecoder, uint8_t channel,
                              uint8_t window_sequence, uint8_t window_shape,
                              const real_t *spectrum);
void conceal_commit_good_au(NeAACDecStruct *hDecoder);

/* Defined in decoder.c; fills the same channel-position metadata as normal
   frame decoding. */
void create_channel_config(NeAACDecStruct *hDecoder,
                           NeAACDecFrameInfo *hInfo);

NEAACDECAPI void* NeAACDecConceal(NeAACDecHandle hDecoder,
                                  NeAACDecFrameInfo *hInfo);
NEAACDECAPI void* NeAACDecConceal2(NeAACDecHandle hDecoder,
                                   NeAACDecFrameInfo *hInfo,
                                   void **sample_buffer,
                                   unsigned long sample_buffer_size);

#endif
