/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/**
 * LC3 Repair Window
 *
 * The repair window is a bounded decode window for pre-roll audio preview
 * with late packet correction. Frames are appended in sequence order; a
 * frame first appended as lost (PLC) can be replaced later, as long as no
 * sample of it has been read for playback yet. Every preview or read
 * produces the same PCM as decoding the final packet sequence in order
 * with ordinary `lc3_decode()` calls, starting from a fresh decoder state.
 *
 * The window keeps at most `capacity` frames. Memory and retained
 * compressed data grow only with the configuration, the channel count and
 * `capacity`; already played frames are released and the whole played
 * history is never stored.
 *
 * Units
 *   - Frame sizes (`nbytes`) are in bytes, per channel.
 *   - `capacity` and frame identifiers count frames.
 *   - `capacity_samples` and the value returned by `lc3_repair_read()`
 *     count PCM samples *per channel*. A frame holds
 *     `lc3_frame_samples(dt_us, sr_pcm_hz)` samples per channel.
 *   - Output PCM is interleaved signed 16 bits: frame `f`, sample `s`,
 *     channel `c` is stored at index
 *     `(f * frame_samples + s) * nchannels + c`.
 *
 * Repair range
 *   Any frame still held by the window whose playback has not started can
 *   be replaced, repeatedly and out of order. Replacing a frame also
 *   recomputes every later frame still in the window (independent per
 *   channel loss, consecutive PLC runs and loss-after-recovery included).
 *   A frame from which at least one sample has been read is immutable;
 *   once a frame is fully read, its slot is recycled. Frames no longer
 *   held by the window (fully read or cleared by a reset) cannot be
 *   addressed by their old identifier.
 */

#ifndef __LC3_REPAIR_H
#define __LC3_REPAIR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "lc3.h"


/**
 * Opaque repair window handle
 */

typedef struct lc3_repair lc3_repair_t;


/**
 * Per-channel frame descriptor
 * data     Pointer to the compressed frame, NULL for a lost channel.
 *          Input packets are copied internally; the caller may reuse or
 *          overwrite the source buffers once the call returns.
 * nbytes   Size of the frame in bytes, 0 together with a NULL `data`
 *          marks the channel as lost. A non-NULL payload must fit the
 *          frame byte range of the window configuration.
 */

struct lc3_repair_frame {
    const void *data;
    int nbytes;
};


/**
 * Create a repair window
 * dt_us          Frame duration in us: 7500 or 10000
 * sr_hz          Bitstream sample rate in Hz:
 *                8000, 16000, 24000, 32000 or 48000
 * sr_pcm_hz      Output sample rate in Hz, greater than or equal to
 *                `sr_hz`; 0 falls back to `sr_hz`
 * nchannels      Number of channels: 1 or 2
 * capacity       Maximum number of frames held by the window,
 *                from 2 to 32 frames
 * return         Window handle, NULL on invalid parameters or
 *                allocation failure
 */
LC3_EXPORT lc3_repair_t *lc3_repair_create(
    int dt_us, int sr_hz, int sr_pcm_hz, int nchannels, int capacity);

/**
 * Destroy a repair window
 * ctx            Window handle, NULL is accepted
 */
LC3_EXPORT void lc3_repair_destroy(lc3_repair_t *ctx);

/**
 * Reset a repair window
 * ctx            Window handle
 *
 * All retained frames, PCM and decoder history are cleared. Frame
 * numbering keeps increasing afterwards, so identifiers handed out before
 * the reset are permanently rejected.
 */
LC3_EXPORT void lc3_repair_reset(lc3_repair_t *ctx);

/**
 * Append the next frame of the sequence
 * ctx            Window handle
 * frames         Array of `nchannels` frame descriptors
 * id             On success, set to the frame identifier, numbered from 0
 *                and strictly consecutive
 * return         0 on success, -1 on invalid parameters or when the
 *                window already holds `capacity` frames
 *
 * All channels are validated before any state is updated: a failure never
 * consumes a frame identifier, never changes retained audio and never
 * writes `*id`. A payload with a legal size rejected by the decoder is
 * accepted and decoded as PLC, matching `lc3_decode()` behavior.
 */
LC3_EXPORT int lc3_repair_append(
    lc3_repair_t *ctx, const struct lc3_repair_frame *frames, uint64_t *id);

/**
 * Replace a frame that has not started playing
 * ctx            Window handle
 * id             Frame identifier returned by `lc3_repair_append()`
 * frames         New array of `nchannels` frame descriptors
 * return         0 on success, -1 on invalid parameters or when the
 *                frame is unknown, already playing/played or fully read
 *
 * Replacement is allowed repeatedly while the frame is still mutable.
 * Later frames are recomputed on the next preview or read, exactly as if
 * the final packet sequence had been decoded in order from the start.
 * On failure no retained audio or state is modified.
 */
LC3_EXPORT int lc3_repair_replace(
    lc3_repair_t *ctx, uint64_t id, const struct lc3_repair_frame *frames);

/**
 * Preview a whole frame without consuming it
 * ctx            Window handle
 * id             Frame identifier returned by `lc3_repair_append()`
 * pcm            Output buffer for one interleaved frame, i.e.
 *                `nchannels * frame_samples` int16_t values
 * return         0 on success, -1 on invalid parameters or an unknown
 *                frame identifier
 *
 * Preview is not playback: it may target any retained frame, in any
 * order, any number of times, and never consumes window capacity.
 */
LC3_EXPORT int lc3_repair_preview(
    lc3_repair_t *ctx, uint64_t id, int16_t *pcm);

/**
 * Read decoded PCM for playback
 * ctx            Window handle
 * pcm            Output interleaved S16 buffer
 * capacity_samples  Buffer size in samples per channel (the buffer must
 *                hold `capacity_samples * nchannels` int16_t values)
 * return         Number of samples per channel actually written, from 0
 *                up to `capacity_samples`; 0 when the window is
 *                temporarily empty
 *
 * Reads may span several frames and may consume part of a frame. As soon
 * as one sample of a frame has been delivered, that frame can no longer
 * be replaced, but its remaining PCM is always delivered in full. The
 * frame slot is recycled only once the whole frame has been read.
 */
LC3_EXPORT int lc3_repair_read(
    lc3_repair_t *ctx, int16_t *pcm, int capacity_samples);


#ifdef __cplusplus
}
#endif

#endif /* __LC3_REPAIR_H */
