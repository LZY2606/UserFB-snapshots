/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder
**
** Error concealment support: keeps the most recent complete access unit's
** post-tool spectrum so a missing access unit can be synthesised without
** touching the bitstream parser.
*/

#ifndef __CONCEAL_H__
#define __CONCEAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "structs.h"

/* full reset: used by re-initialisation */
void faad_conceal_reset(NeAACDecStruct *hDecoder);
/* Init2 completed: enables the fixed-format concealment cache */
void faad_conceal_set_initialized(NeAACDecStruct *hDecoder);
/* start of every decode attempt: committed cache is invalidated, pending
   capture is cleared.  A successful access unit commits again. */
void faad_conceal_begin_au(NeAACDecStruct *hDecoder);
/* capture one channel's spectrum after PNS/MS/IS/TNS/DRC and before the
   inverse filter bank */
void faad_conceal_capture(NeAACDecStruct *hDecoder, uint8_t channel,
                          uint8_t window_sequence, uint8_t window_shape,
                          const real_t *spec, uint16_t frame_len);
/* a complete access unit finished successfully */
void faad_conceal_commit_au(NeAACDecStruct *hDecoder);
/* explicit invalidation (PostSeekReset and friends) */
void faad_conceal_invalidate(NeAACDecStruct *hDecoder);

/* Read-only observation hook for the task's tests: the spectrum that would be
   handed to the inverse filter bank for the next concealed access unit, plus
   the cached window info, the running missing-AU counter, the sequence the
   next synthesis will use and the current overlap/pre-window-shape state */
uint8_t faad_conceal_test_peek(NeAACDecStruct *hDecoder, uint8_t channel,
                               const real_t **spectrum, uint8_t *window_sequence,
                               uint8_t *window_shape, uint32_t *lost,
                               uint8_t *next_sequence,
                               const real_t **overlap, uint8_t *window_shape_prev,
                               uint32_t *frame_counter,
                               uint32_t *rng1, uint32_t *rng2);

/* Test aid: an independently held instance of the real inverse filter bank.
   It keeps its own overlap and window-shape history and is driven only by the
   caller's spectrum, so tests can compute expected PCM without touching the
   decoder's state.  Nothing here is used by the decode paths. */
void *task_test_fb_create(uint16_t frame_len);
void task_test_fb_run(void *fb_state, uint8_t window_sequence,
                      uint8_t window_shape, uint8_t window_shape_prev,
                      const real_t *spectrum, real_t *time_out,
                      real_t *overlap, uint16_t frame_len);
void task_test_fb_end(void *fb_state);

#ifdef __cplusplus
}
#endif
#endif
