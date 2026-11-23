/*
 * Streaming PCM to audio-only OP1a MXF wrapper
 *
 * Public C interface. The writer takes a caller owned, writable and seekable
 * MXFFile and wraps a stream of 48 kHz little-endian signed integer PCM into
 * BWFFrameWrapped audio edit units inside an audio-only OP1a MXF file.
 *
 * Supported PCM: 48 kHz, 16 or 24 bit, 1 or 2 channels.
 * Supported edit rates: 25/1 and 30000/1001.
 */

#ifndef PCM_WRAP_H_
#define PCM_WRAP_H_


#include <stdint.h>

#include <mxf/mxf.h>


#ifdef __cplusplus
extern "C"
{
#endif


/* PCM sample formats: 48 kHz little-endian signed integer PCM */
typedef enum
{
    PCM_WRAP_PCM_S16LE_1CH = 0,
    PCM_WRAP_PCM_S16LE_2CH,
    PCM_WRAP_PCM_S24LE_1CH,
    PCM_WRAP_PCM_S24LE_2CH
} PCMWrapPCMFormat;

/* Track edit rates */
typedef enum
{
    PCM_WRAP_EDIT_RATE_25_1 = 0,      /* 25/1, 1920 sample frames per edit unit */
    PCM_WRAP_EDIT_RATE_30000_1001     /* 30000/1001, 1602/1601 sample frame cadence */
} PCMWrapEditRate;


typedef struct PCMWrapWriter PCMWrapWriter;


/*
 * Begin wrapping PCM into a new audio-only OP1a MXF file.
 *
 * mxfFile is owned by the caller, must be writable and seekable, must be
 * empty and its cursor must be at position 0. The writer does not close the
 * MXFFile, even on failure.
 *
 * sequenceOffset selects the starting position in the audio edit unit
 * cadence: it must be 0 for 25/1 and in the range 0-4 for 30000/1001.
 * maxEditUnitsPerBodyPartition (1-1024) limits the number of completed edit
 * units written to a single body partition.
 *
 * Returns a writer on success, or NULL on invalid arguments or if the
 * MXFFile is not empty, not at position 0, not seekable or a write fails.
 */
PCMWrapWriter *pcm_wrap_begin(MXFFile *mxfFile,
                              PCMWrapPCMFormat pcmFormat,
                              PCMWrapEditRate editRate,
                              uint32_t sequenceOffset,
                              uint32_t maxEditUnitsPerBodyPartition);

/*
 * Feed an arbitrary byte fragment of PCM. Bytes are buffered until a
 * complete edit unit is available; PCM bytes are written through verbatim.
 * Returns 1 on success and 0 on failure, after which the writer is in a
 * failed state and further push/finish calls write nothing and return 0.
 */
int pcm_wrap_push(PCMWrapWriter *writer, const uint8_t *data, uint64_t length);

/*
 * Flush the completed MXF file. Fails if no complete edit unit was written or
 * if bytes for an incomplete edit unit remain buffered, or if any write or
 * seek fails. On failure the writer enters (or stays in) the failed state.
 */
int pcm_wrap_finish(PCMWrapWriter *writer);

/*
 * Free the writer. The underlying MXFFile is never closed. Safe to call
 * after a failure.
 */
void pcm_wrap_destroy(PCMWrapWriter *writer);


#ifdef __cplusplus
}
#endif


#endif
