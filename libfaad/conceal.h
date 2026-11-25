#ifndef __CONCEAL_H__
#define __CONCEAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "structs.h"

#define CONCEAL_ERROR_UNAVAILABLE 35

void conceal_reset(NeAACDecStruct *hDecoder);
void conceal_invalidate(NeAACDecStruct *hDecoder);
void conceal_decode_begin(NeAACDecStruct *hDecoder);
void conceal_capture_spectrum(NeAACDecStruct *hDecoder,
                              uint8_t channel,
                              const real_t *spectrum,
                              uint8_t window_sequence,
                              uint8_t window_shape);
void conceal_commit(NeAACDecStruct *hDecoder, NeAACDecFrameInfo *hInfo);

#ifdef __cplusplus
}
#endif

#endif
