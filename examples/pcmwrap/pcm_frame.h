/*
 * PCM audio edit unit framing
 *
 * Groups a stream of 48 kHz little-endian signed integer PCM bytes into
 * fixed-cadence audio edit units. Only one edit unit worth of bytes is
 * buffered at a time; completed units are delivered through a callback.
 */

#ifndef PCM_FRAME_H_
#define PCM_FRAME_H_


#include <stdint.h>

#include "pcm_wrap.h"


#ifdef __cplusplus
extern "C"
{
#endif


/* 30000/1001 sample frame counts cycled per edit unit */
#define PCM_FRAME_NTSC_CADENCE_LEN     5
#define PCM_FRAME_NTSC_CADENCE         {1602, 1601, 1602, 1601, 1602}
#define PCM_FRAME_PAL_UNIT_FRAMES      1920
#define PCM_FRAME_SAMPLE_RATE          48000


typedef struct
{
    PCMWrapPCMFormat pcmFormat;
    PCMWrapEditRate editRate;
    uint32_t sequenceOffset;
    uint32_t bitsPerSample;
    uint32_t channelCount;
    uint32_t blockAlign;            /* bytes per sample frame (all channels) */
    uint32_t startCadencePosition;  /* index of the first edit unit */
} PCMFrameConfig;

typedef struct PCMFramer PCMFramer;

/*
 * Called once for every completed edit unit, in order. The unit buffer is
 * only valid for the duration of the callback. Return 0 to fail the framing
 * operation.
 */
typedef int (*PCMFrameUnitCallback)(void *context, const uint8_t *unit,
                                    uint32_t byteLength, uint32_t sampleFrames);


/* Validate the format, edit rate and sequence offset combination. */
int pcm_frame_config_init(PCMFrameConfig *config,
                          PCMWrapPCMFormat pcmFormat,
                          PCMWrapEditRate editRate,
                          uint32_t sequenceOffset);

/* Number of sample frames in the edit unit at the given cadence position. */
uint32_t pcm_frame_unit_sample_frames(const PCMFrameConfig *config, uint32_t cadencePosition);

/* Largest edit unit size in bytes. */
uint32_t pcm_frame_max_unit_bytes(const PCMFrameConfig *config);

/*
 * Create a framer. Returns NULL on allocation failure. The framer buffers at
 * most one (largest) edit unit.
 */
PCMFramer *pcm_framer_create(const PCMFrameConfig *config);
void pcm_framer_destroy(PCMFramer *framer);

/*
 * Feed an arbitrary PCM byte fragment. Each completed edit unit is delivered
 * through the callback. Returns 0 if a callback fails or memory is invalid.
 */
int pcm_framer_push(PCMFramer *framer, const uint8_t *data, uint64_t length,
                    PCMFrameUnitCallback callback, void *context);

/* Number of completed edit units delivered so far. */
uint64_t pcm_framer_completed_units(const PCMFramer *framer);

/* Number of bytes buffered for the not yet completed edit unit. */
uint32_t pcm_framer_buffered_bytes(const PCMFramer *framer);

/* Cadence position of the next edit unit to be delivered. */
uint32_t pcm_framer_next_cadence_position(const PCMFramer *framer);


#ifdef __cplusplus
}
#endif


#endif
